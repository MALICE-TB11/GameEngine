#include "world/ScenePhysics.h"

#include "core/Log.h"

namespace eng {
namespace {

bool nearlyEqual(const Transform& a, const Transform& b) {
    // 编辑器拖动通常是厘米级以上的变化；阈值太小会因为浮点往返误差每步都瞬移
    return glm::length(a.position - b.position) < 1e-4f && std::abs(glm::dot(a.rotation, b.rotation)) > 1.0f - 1e-6f &&
           glm::length(a.scale - b.scale) < 1e-4f;
}

} // namespace

void ScenePhysics::init(physics::PhysicsWorld& world, asset::AssetManager& assets) {
    m_world = &world;
    m_assets = &assets;
}

void ScenePhysics::shutdown() {
    if (!m_world) return;
    for (uint32_t i = 0; i < m_records.size(); ++i)
        if (m_records[i].alive) release(i);
    for (const auto& [key, shape] : m_shapes) m_world->releaseShape(shape);
    m_records.clear();
    m_free.clear();
    m_shapes.clear();
    m_world = nullptr;
}

uint32_t ScenePhysics::allocRecord() {
    if (!m_free.empty()) {
        const uint32_t slot = m_free.back();
        m_free.pop_back();
        m_records[slot] = Record{};
        return slot;
    }
    m_records.emplace_back();
    return uint32_t(m_records.size() - 1);
}

void ScenePhysics::release(uint32_t slot) {
    Record& r = m_records[slot];
    if (!r.alive) return;
    m_world->removeBody(r.body);
    r = Record{};
    m_free.push_back(slot);
}

bool ScenePhysics::matches(const Record& r, const Entity& e, uint32_t version) const {
    return r.mode == e.physics && r.model == e.model && r.instance == e.instance && r.version == version &&
           glm::length(r.scale - e.current.scale) < 1e-4f;
}

physics::ShapeId ScenePhysics::shapeFor(const Model& model, ModelHandle handle, uint32_t version, uint32_t instance, bool convex) {
    const ShapeKey key{handle.index, version, instance, convex};
    if (auto it = m_shapes.find(key); it != m_shapes.end()) return it->second;

    // 同一模型的旧版本形状（热重载前）不会再被用到：释放（已创建的刚体仍持有引用，直到它们被重建）
    for (auto it = m_shapes.begin(); it != m_shapes.end();) {
        const auto& [m, v, inst, cvx] = it->first;
        if (m == handle.index && v != version) {
            m_world->releaseShape(it->second);
            it = m_shapes.erase(it);
        } else {
            ++it;
        }
    }

    physics::ShapeId shape;
    if (instance < model.instances.size()) {
        const ModelInstance& inst = model.instances[instance];
        if (inst.mesh < model.collision.size()) {
            // 实例相对模型根的变换（glTF 节点矩阵）直接烘进顶点：刚体的变换就和实体变换完全一致，写回时不用换算
            const CollisionMesh& cm = model.collision[inst.mesh];
            std::vector<vec3> pts;
            pts.reserve(cm.positions.size());
            for (const vec3& p : cm.positions) pts.push_back(vec3(inst.transform * vec4(p, 1.0f)));
            shape = convex ? m_world->createConvexHull(pts) : m_world->createMesh(pts, cm.indices);
        }
    }
    if (!shape.valid()) LOG_WARN("模型 {} 的实例 {} 没有可用的碰撞几何", model.name, instance);
    m_shapes[key] = shape; // 失败也缓存（无效句柄），避免每步重试并刷屏
    return shape;
}

void ScenePhysics::syncBeforeStep(Scene& scene) {
    if (!m_world) return;
    ++m_syncId;
    m_stats = {};

    for (Entity& e : scene.entities()) {
        // 认领：槽位有效、活着、并且这一轮还没被别的实体认领（复制出来的实体会走到「新建」分支）
        uint32_t slot = e.physicsSlot;
        const bool owned = slot < m_records.size() && m_records[slot].alive && m_records[slot].claimedAt != m_syncId;
        if (!owned) slot = UINT32_MAX;

        if (e.physics == PhysicsMode::None) {
            if (slot != UINT32_MAX) release(slot);
            e.physicsSlot = UINT32_MAX;
            continue;
        }
        // 模型还在加载：此时只有兜底立方体，等真正的几何到了再建碰撞体
        const asset::AssetStatus status = m_assets->status(e.model);
        if (status != asset::AssetStatus::Ready) {
            if (slot != UINT32_MAX) m_records[slot].claimedAt = m_syncId; // 保留旧刚体（热重载期间）
            e.physicsSlot = slot;
            if (status == asset::AssetStatus::Loading) ++m_stats.waiting;
            continue;
        }

        const uint32_t version = m_assets->version(e.model);
        if (slot != UINT32_MAX && !matches(m_records[slot], e, version)) {
            release(slot);
            slot = UINT32_MAX;
        }
        if (slot == UINT32_MAX) {
            // 新建刚体
            slot = allocRecord();
            Record& r = m_records[slot];
            r.alive = true;
            r.mode = e.physics;
            r.model = e.model;
            r.instance = e.instance;
            r.version = version;
            r.scale = e.current.scale;
            r.written = e.current;
            const bool dynamic = e.physics == PhysicsMode::Dynamic;
            const physics::ShapeId shape = shapeFor(m_assets->model(e.model), e.model, version, e.instance, dynamic);
            if (shape.valid()) {
                physics::BodyDesc d;
                d.shape = shape;
                d.scale = e.current.scale;
                d.motion = dynamic ? physics::Motion::Dynamic : physics::Motion::Static;
                d.position = e.current.position;
                d.rotation = e.current.rotation;
                r.body = m_world->addBody(d);
            }
        } else if (!nearlyEqual(m_records[slot].written, e.current)) {
            // 实体被外部（编辑器、脚本）移动了 → 刚体跟着瞬移
            m_world->setTransform(m_records[slot].body, e.current.position, e.current.rotation);
            m_records[slot].written = e.current;
        }

        Record& r = m_records[slot];
        r.claimedAt = m_syncId;
        e.physicsSlot = slot;
        if (r.mode == PhysicsMode::Dynamic) {
            ++m_stats.dynamicBodies;
            if (!m_world->isActive(r.body)) ++m_stats.sleeping;
        } else {
            ++m_stats.staticBodies;
        }
    }

    // 没被认领的记录 = 实体已被删除或场景已被替换
    for (uint32_t i = 0; i < m_records.size(); ++i)
        if (m_records[i].alive && m_records[i].claimedAt != m_syncId) release(i);
}

void ScenePhysics::syncAfterStep(Scene& scene) {
    if (!m_world) return;
    for (Entity& e : scene.entities()) {
        if (e.physics != PhysicsMode::Dynamic || e.physicsSlot >= m_records.size()) continue;
        Record& r = m_records[e.physicsSlot];
        if (!r.alive || !r.body.valid()) continue;
        if (m_world->getTransform(r.body, e.current.position, e.current.rotation)) r.written = e.current;
    }
}

} // namespace eng
