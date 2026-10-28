#include "physics/PhysicsWorld.h"

#include "core/Log.h"

// Jolt.h 必须是第一个 Jolt 头文件：它定义了平台宏和内存分配钩子
#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <thread>
#include <vector>

namespace eng::physics {
namespace {

// ---- 碰撞层 ------------------------------------------------------------------------------
// 对象层（ObjectLayer）决定「谁和谁碰」；宽相层（BroadPhaseLayer）决定「放在哪棵加速树里」。
// 只分两层：静态物体彼此永远不会碰撞，单独放一棵很少更新的树，宽相查询最便宜。
// EXTEND: 加「角色」「触发器」「投射物」等层时，在这里扩充两张表。
namespace Layers {
constexpr JPH::ObjectLayer kStatic = 0;
constexpr JPH::ObjectLayer kMoving = 1;
} // namespace Layers
namespace BroadLayers {
constexpr JPH::BroadPhaseLayer kStatic{0};
constexpr JPH::BroadPhaseLayer kMoving{1};
constexpr uint32_t kCount = 2;
} // namespace BroadLayers

class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return BroadLayers::kCount; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        return layer == Layers::kStatic ? BroadLayers::kStatic : BroadLayers::kMoving;
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
        return layer == BroadLayers::kStatic ? "Static" : "Moving";
    }
#endif
};

class ObjectVsBroadPhase final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer broad) const override {
        return layer == Layers::kStatic ? broad == BroadLayers::kMoving : true;
    }
};

class ObjectPairs final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        return a == Layers::kMoving || b == Layers::kMoving; // 静态 vs 静态不碰
    }
};

// ---- 类型转换（只在这个文件里出现 Jolt 类型） ------------------------------------------------
JPH::Vec3 toJ(const vec3& v) { return {v.x, v.y, v.z}; }
JPH::Quat toJ(const quat& q) { return {q.x, q.y, q.z, q.w}; }
vec3 fromJ(JPH::Vec3Arg v) { return {v.GetX(), v.GetY(), v.GetZ()}; }
quat fromJ(JPH::QuatArg q) { return quat(q.GetW(), q.GetX(), q.GetY(), q.GetZ()); }
JPH::BodyID toJ(BodyId id) { return JPH::BodyID(id.value); }
BodyId fromJ(const JPH::BodyID& id) { return {id.GetIndexAndSequenceNumber()}; }

// ---- Jolt 的日志与断言接到引擎日志 ----------------------------------------------------------
void joltTrace(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    LOG_INFO("[Jolt] {}", buf);
}

#ifdef JPH_ENABLE_ASSERTS
bool joltAssert(const char* expr, const char* msg, const char* file, JPH::uint line) {
    LOG_ERROR("[Jolt] 断言失败 {}:{}: {} {}", file, line, expr, msg ? msg : "");
    return false; // false = 不触发断点，继续执行（错误日志会让冒烟测试失败）
}
#endif

// Jolt 的全局状态（分配器、类型工厂）只初始化一次；多个 PhysicsWorld 共享
int g_joltUsers = 0;

void acquireJolt() {
    if (g_joltUsers++ > 0) return;
    JPH::RegisterDefaultAllocator();
    JPH::Trace = joltTrace;
    JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = joltAssert;)
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();
}

void releaseJolt() {
    if (--g_joltUsers > 0) return;
    JPH::UnregisterTypes();
    delete JPH::Factory::sInstance;
    JPH::Factory::sInstance = nullptr;
}

} // namespace

struct PhysicsWorld::Impl {
    PhysicsSettings settings;
    std::unique_ptr<JPH::TempAllocatorImpl> temp;
    std::unique_ptr<JPH::JobSystemThreadPool> jobs;
    BroadPhaseLayers broadLayers;
    ObjectVsBroadPhase objectVsBroad;
    ObjectPairs objectPairs;
    JPH::PhysicsSystem system;
    std::vector<JPH::RefConst<JPH::Shape>> shapes; // ShapeId.index → 形状（释放后置空）

    struct Character {
        JPH::Ref<JPH::CharacterVirtual> ch;
        CharacterDesc desc;
    };
    std::vector<Character> characters; // CharacterId.index → 角色（销毁后 ch 为空）
    float lastStepMs = 0.0f;
    bool warnedUpdateError = false;

    ShapeId store(const JPH::ShapeSettings::ShapeResult& result, const char* what) {
        if (result.HasError()) {
            LOG_ERROR("创建碰撞形状（{}）失败: {}", what, result.GetError().c_str());
            return {};
        }
        shapes.push_back(result.Get());
        return {uint32_t(shapes.size() - 1)};
    }
    const JPH::Shape* shape(ShapeId id) const { return id.index < shapes.size() ? shapes[id.index].GetPtr() : nullptr; }
    JPH::BodyInterface& bodies() { return system.GetBodyInterface(); }
    const JPH::BodyInterface& bodies() const { return system.GetBodyInterface(); }
    JPH::CharacterVirtual* character(CharacterId id) const {
        return id.index < characters.size() ? characters[id.index].ch.GetPtr() : nullptr;
    }
};

PhysicsWorld::PhysicsWorld() = default;
PhysicsWorld::~PhysicsWorld() { shutdown(); }

bool PhysicsWorld::init(const PhysicsSettings& settings) {
    shutdown();
    acquireJolt();
    m_impl = std::make_unique<Impl>();
    Impl& w = *m_impl;
    w.settings = settings;
    // 临时分配器：step 期间的碰撞对、接触点等短命数据从这里线性分配，每步结束整体回收
    w.temp = std::make_unique<JPH::TempAllocatorImpl>(settings.tempMemoryMB * 1024 * 1024);
    uint32_t threads = settings.threads;
    if (threads == 0) threads = std::clamp<uint32_t>(std::thread::hardware_concurrency(), 2u, 5u) - 1u;
    w.jobs = std::make_unique<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, int(threads));

    // 后三个数字是 Jolt 内部数组的容量上限：body 互斥锁数量（0 = 默认）、最大碰撞对、最大接触约束
    const JPH::uint maxBodyPairs = settings.maxBodies * 2, maxContacts = settings.maxBodies * 2;
    w.system.Init(settings.maxBodies, 0, maxBodyPairs, maxContacts, w.broadLayers, w.objectVsBroad, w.objectPairs);
    w.system.SetGravity(toJ(settings.gravity));
    LOG_INFO("物理世界初始化：Jolt {}.{}.{}，{} 个工作线程，刚体上限 {}", JPH_VERSION_MAJOR, JPH_VERSION_MINOR, JPH_VERSION_PATCH,
             threads, settings.maxBodies);
    return true;
}

void PhysicsWorld::shutdown() {
    if (!m_impl) return;
    // 角色和形状引用必须在 PhysicsSystem 销毁前释放；unique_ptr 析构顺序与声明顺序相反，这里显式清理更清楚
    m_impl->characters.clear();
    JPH::BodyIDVector ids;
    m_impl->system.GetBodies(ids);
    for (const JPH::BodyID& id : ids) {
        m_impl->bodies().RemoveBody(id);
        m_impl->bodies().DestroyBody(id);
    }
    m_impl->shapes.clear();
    m_impl.reset();
    releaseJolt();
}

// ---- 形状 ------------------------------------------------------------------------------------

ShapeId PhysicsWorld::createBox(vec3 halfExtents) {
    // 凸半径（圆角）不能超过最短半边长：小盒子要相应减小
    const float radius = std::min(JPH::cDefaultConvexRadius, std::min({halfExtents.x, halfExtents.y, halfExtents.z}) * 0.5f);
    return m_impl->store(JPH::BoxShapeSettings(toJ(glm::max(halfExtents, vec3(1e-3f))), radius).Create(), "盒");
}

ShapeId PhysicsWorld::createSphere(float radius) {
    return m_impl->store(JPH::SphereShapeSettings(std::max(radius, 1e-3f)).Create(), "球");
}

ShapeId PhysicsWorld::createCapsule(float halfHeight, float radius) {
    return m_impl->store(JPH::CapsuleShapeSettings(std::max(halfHeight, 1e-3f), std::max(radius, 1e-3f)).Create(), "胶囊");
}

ShapeId PhysicsWorld::createConvexHull(std::span<const vec3> points) {
    JPH::Array<JPH::Vec3> pts;
    pts.reserve(points.size());
    for (const vec3& p : points) pts.push_back(toJ(p));
    return m_impl->store(JPH::ConvexHullShapeSettings(pts).Create(), "凸包");
}

ShapeId PhysicsWorld::createMesh(std::span<const vec3> positions, std::span<const uint32_t> indices) {
    JPH::VertexList verts;
    verts.reserve(positions.size());
    for (const vec3& p : positions) verts.push_back(JPH::Float3(p.x, p.y, p.z));
    JPH::IndexedTriangleList tris;
    tris.reserve(indices.size() / 3);
    for (size_t i = 0; i + 2 < indices.size(); i += 3) tris.push_back(JPH::IndexedTriangle(indices[i], indices[i + 1], indices[i + 2]));
    // 构造函数会自动剔除退化三角形（Sanitize）
    return m_impl->store(JPH::MeshShapeSettings(std::move(verts), std::move(tris)).Create(), "三角网格");
}

ShapeId PhysicsWorld::createHeightfield(std::span<const float> samples, uint32_t resolution, vec2 originXZ, float spacing) {
    if (samples.size() < size_t(resolution) * resolution) {
        LOG_ERROR("高度场采样数不足：需要 {}²，实际 {}", resolution, samples.size());
        return {};
    }
    // 表面 = offset + scale × (x, h[x,z], z)。Jolt 会把采样数向上取整到 blockSize 的倍数，补的采样视为「无碰撞」
    JPH::HeightFieldShapeSettings hs(samples.data(), JPH::Vec3(originXZ.x, 0.0f, originXZ.y), JPH::Vec3(spacing, 1.0f, spacing),
                                     resolution);
    return m_impl->store(hs.Create(), "高度场");
}

void PhysicsWorld::releaseShape(ShapeId shape) {
    if (shape.index < m_impl->shapes.size()) m_impl->shapes[shape.index] = nullptr;
}

// ---- 刚体 ------------------------------------------------------------------------------------

BodyId PhysicsWorld::addBody(const BodyDesc& desc) {
    Impl& w = *m_impl;
    JPH::RefConst<JPH::Shape> shape = w.shape(desc.shape);
    if (!shape) {
        LOG_ERROR("addBody：无效的形状句柄 {}", desc.shape.index);
        return {};
    }
    // 缩放：包一层 ScaledShape。MakeScaleValid 处理「球只能均匀缩放」这类限制
    if (glm::any(glm::greaterThan(glm::abs(desc.scale - vec3(1.0f)), vec3(1e-4f)))) {
        const JPH::Vec3 scale = shape->MakeScaleValid(toJ(desc.scale));
        auto scaled = JPH::ScaledShapeSettings(shape, scale).Create();
        if (scaled.HasError()) {
            LOG_ERROR("缩放碰撞形状失败: {}", scaled.GetError().c_str());
            return {};
        }
        shape = scaled.Get();
    }

    Motion motion = desc.motion;
    if (motion != Motion::Static && shape->MustBeStatic()) {
        // 三角网格/高度场没有体积，无法计算质量和惯量 → 只能静态
        LOG_WARN("三角网格形状不能用于动态刚体，已改为静态（动态物体请用凸包）");
        motion = Motion::Static;
    }
    const JPH::EMotionType type = motion == Motion::Static    ? JPH::EMotionType::Static
                                  : motion == Motion::Dynamic ? JPH::EMotionType::Dynamic
                                                              : JPH::EMotionType::Kinematic;
    JPH::BodyCreationSettings bcs(shape, toJ(desc.position), toJ(glm::normalize(desc.rotation)), type,
                                  motion == Motion::Static ? Layers::kStatic : Layers::kMoving);
    bcs.mFriction = desc.friction;
    bcs.mRestitution = desc.restitution;
    bcs.mUserData = desc.userData;
    if (motion == Motion::Dynamic) {
        // Jolt 形状的默认密度是 1000 kg/m³（水）。按我们的密度换算质量，惯量由形状自动计算
        bcs.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        bcs.mMassPropertiesOverride.mMass = std::max(shape->GetMassProperties().mMass * desc.density / 1000.0f, 0.01f);
    }
    const JPH::BodyID id =
        w.bodies().CreateAndAddBody(bcs, motion == Motion::Static ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
    if (id.IsInvalid()) {
        LOG_ERROR("创建刚体失败：已达到刚体数量上限 {}", w.settings.maxBodies);
        return {};
    }
    return fromJ(id);
}

void PhysicsWorld::removeBody(BodyId body) {
    if (!contains(body)) return;
    m_impl->bodies().RemoveBody(toJ(body));
    m_impl->bodies().DestroyBody(toJ(body));
}

bool PhysicsWorld::contains(BodyId body) const {
    return m_impl && body.valid() && m_impl->bodies().IsAdded(toJ(body));
}

void PhysicsWorld::setTransform(BodyId body, const vec3& position, const quat& rotation, bool resetVelocity) {
    if (!contains(body)) return;
    JPH::BodyInterface& bi = m_impl->bodies();
    const bool isStatic = bi.GetMotionType(toJ(body)) == JPH::EMotionType::Static;
    bi.SetPositionAndRotation(toJ(body), toJ(position), toJ(glm::normalize(rotation)),
                              isStatic ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
    if (resetVelocity && !isStatic) bi.SetLinearAndAngularVelocity(toJ(body), JPH::Vec3::sZero(), JPH::Vec3::sZero());
}

bool PhysicsWorld::getTransform(BodyId body, vec3& position, quat& rotation) const {
    if (!contains(body)) return false;
    JPH::RVec3 p;
    JPH::Quat q;
    m_impl->bodies().GetPositionAndRotation(toJ(body), p, q);
    position = fromJ(JPH::Vec3(p));
    rotation = fromJ(q);
    return true;
}

void PhysicsWorld::setLinearVelocity(BodyId body, const vec3& v) {
    if (contains(body)) m_impl->bodies().SetLinearVelocity(toJ(body), toJ(v));
}

vec3 PhysicsWorld::linearVelocity(BodyId body) const {
    return contains(body) ? fromJ(m_impl->bodies().GetLinearVelocity(toJ(body))) : vec3(0.0f);
}

void PhysicsWorld::addImpulse(BodyId body, const vec3& impulse) {
    if (contains(body)) m_impl->bodies().AddImpulse(toJ(body), toJ(impulse));
}

bool PhysicsWorld::isActive(BodyId body) const {
    return contains(body) && m_impl->bodies().IsActive(toJ(body));
}

// ---- 模拟 ------------------------------------------------------------------------------------

void PhysicsWorld::step(float dt) {
    Impl& w = *m_impl;
    const auto t0 = std::chrono::steady_clock::now();
    const JPH::EPhysicsUpdateError err = w.system.Update(dt, w.settings.collisionSteps, w.temp.get(), w.jobs.get());
    w.lastStepMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (err != JPH::EPhysicsUpdateError::None && !w.warnedUpdateError) {
        // 通常是碰撞对/接触数超过 Init 时给的容量：模拟仍会继续，但部分碰撞被忽略
        LOG_WARN("物理更新报告容量不足（错误码 {}），请调大 PhysicsSettings.maxBodies", uint32_t(err));
        w.warnedUpdateError = true;
    }
}

vec3 PhysicsWorld::gravity() const { return fromJ(m_impl->system.GetGravity()); }
void PhysicsWorld::setGravity(const vec3& g) { m_impl->system.SetGravity(toJ(g)); }

std::optional<RayHit> PhysicsWorld::castRay(const vec3& origin, const vec3& dir, float maxDistance) const {
    const float len = glm::length(dir);
    if (!m_impl || len < 1e-6f || maxDistance <= 0.0f) return std::nullopt;
    const vec3 n = dir / len;
    // Jolt 的射线方向向量自带长度：命中点 = 起点 + 方向 × fraction
    const JPH::RRayCast ray{toJ(origin), toJ(n * maxDistance)};
    JPH::RayCastResult hit;
    if (!m_impl->system.GetNarrowPhaseQuery().CastRay(ray, hit)) return std::nullopt;

    RayHit r;
    r.distance = hit.mFraction * maxDistance;
    r.point = origin + n * r.distance;
    r.body = fromJ(hit.mBodyID);
    JPH::BodyLockRead lock(m_impl->system.GetBodyLockInterface(), hit.mBodyID);
    if (lock.Succeeded()) {
        const JPH::Body& body = lock.GetBody();
        r.normal = fromJ(body.GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, ray.GetPointOnRay(hit.mFraction)));
        r.userData = body.GetUserData();
    }
    return r;
}

// ---- 角色 ------------------------------------------------------------------------------------

CharacterId PhysicsWorld::createCharacter(const CharacterDesc& desc, const vec3& feetPosition) {
    Impl& w = *m_impl;
    const float radius = std::max(desc.radius, 0.05f);
    const float halfHeight = std::max(desc.height * 0.5f - radius, 0.01f); // 胶囊中间圆柱的半高
    // 胶囊原点在中心；角色的「位置」约定在脚底 → 把胶囊整体上移
    auto shape = JPH::RotatedTranslatedShapeSettings(JPH::Vec3(0.0f, halfHeight + radius, 0.0f), JPH::Quat::sIdentity(),
                                                     new JPH::CapsuleShape(halfHeight, radius))
                     .Create();
    if (shape.HasError()) {
        LOG_ERROR("创建角色胶囊失败: {}", shape.GetError().c_str());
        return {};
    }
    JPH::Ref<JPH::CharacterVirtualSettings> s = new JPH::CharacterVirtualSettings();
    s->mShape = shape.Get();
    s->mMaxSlopeAngle = glm::radians(desc.maxSlopeDeg);
    s->mMass = desc.mass;
    s->mMaxStrength = desc.maxPushForce;
    // 支撑平面：只有接触点在胶囊下半球（离脚底一个半径以内）才算「站在上面」，侧面蹭墙不算
    s->mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -radius);

    Impl::Character c;
    c.ch = new JPH::CharacterVirtual(s, toJ(feetPosition), JPH::Quat::sIdentity(), 0, &w.system);
    c.desc = desc;
    w.characters.push_back(std::move(c));
    return {uint32_t(w.characters.size() - 1)};
}

void PhysicsWorld::destroyCharacter(CharacterId id) {
    if (id.index < m_impl->characters.size()) m_impl->characters[id.index].ch = nullptr;
}

void PhysicsWorld::moveCharacter(CharacterId id, const vec3& wishVelocity, float jumpSpeed, float dt) {
    Impl& w = *m_impl;
    JPH::CharacterVirtual* c = w.character(id);
    if (!c || dt <= 0.0f) return;
    const CharacterDesc& desc = w.characters[id.index].desc;

    // 速度合成（Jolt 官方角色示例的做法）：
    //   站在地上 → 竖直速度取地面的速度（站在移动平台上会跟着走），可以起跳
    //   在空中   → 保留当前竖直速度（上抛 / 下落）
    //   两种情况都再加上重力 × dt 与玩家输入的水平速度
    c->UpdateGroundVelocity();
    const JPH::Vec3 up = c->GetUp();
    const JPH::Vec3 vertical = up * c->GetLinearVelocity().Dot(up);
    const JPH::Vec3 groundVelocity = c->GetGroundVelocity();
    const bool movingTowardsGround = (vertical.GetY() - groundVelocity.GetY()) < 0.1f;
    JPH::Vec3 v;
    if (c->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround && movingTowardsGround) {
        v = groundVelocity;
        if (jumpSpeed > 0.0f) v += up * jumpSpeed;
    } else {
        v = vertical;
    }
    const JPH::Vec3 g = w.system.GetGravity();
    v += g * dt;
    v += toJ(vec3(wishVelocity.x, 0.0f, wishVelocity.z));
    c->SetLinearVelocity(v);

    JPH::CharacterVirtual::ExtendedUpdateSettings eu;
    eu.mStickToFloorStepDown = JPH::Vec3(0.0f, -desc.stickDown, 0.0f);
    eu.mWalkStairsStepUp = JPH::Vec3(0.0f, desc.stepUp, 0.0f);
    c->ExtendedUpdate(dt, g, eu, w.system.GetDefaultBroadPhaseLayerFilter(Layers::kMoving),
                      w.system.GetDefaultLayerFilter(Layers::kMoving), {}, {}, *w.temp);
}

void PhysicsWorld::teleportCharacter(CharacterId id, const vec3& feetPosition) {
    if (JPH::CharacterVirtual* c = m_impl->character(id)) {
        c->SetPosition(toJ(feetPosition));
        c->SetLinearVelocity(JPH::Vec3::sZero());
    }
}

CharacterState PhysicsWorld::character(CharacterId id) const {
    CharacterState s;
    const JPH::CharacterVirtual* c = m_impl ? m_impl->character(id) : nullptr;
    if (!c) return s;
    s.position = fromJ(JPH::Vec3(c->GetPosition()));
    s.velocity = fromJ(c->GetLinearVelocity());
    s.grounded = c->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround;
    s.onSteepGround = c->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnSteepGround;
    s.groundNormal = fromJ(c->GetGroundNormal());
    return s;
}

PhysicsStats PhysicsWorld::stats() const {
    PhysicsStats s;
    if (!m_impl) return s;
    s.bodies = m_impl->system.GetNumBodies();
    s.activeBodies = m_impl->system.GetNumActiveBodies(JPH::EBodyType::RigidBody);
    s.shapes = uint32_t(std::count_if(m_impl->shapes.begin(), m_impl->shapes.end(), [](const auto& p) { return p != nullptr; }));
    s.characters = uint32_t(std::count_if(m_impl->characters.begin(), m_impl->characters.end(), [](const auto& c) { return c.ch != nullptr; }));
    s.stepMs = m_impl->lastStepMs;
    return s;
}

} // namespace eng::physics
