#pragma once
// 物理世界：Jolt Physics 的薄封装。
//
// 为什么包一层：
//   1. 隔离第三方类型 —— 头文件里没有任何 Jolt 类型（PIMPL），玩法代码只认识 BodyId / ShapeId；
//   2. 统一约定 —— 单位（米、千克、秒）、Y 轴向上、碰撞层、线程数都在一处决定；
//   3. 容错 —— 形状创建失败（退化网格、凸包点太少）只打日志、返回无效句柄，不会崩溃。
//
// 用法（每个固定步）：
//   world.moveCharacter(...)  // 先推进角色（它不是刚体，由代码驱动）
//   world.step(fixedDt)       // 再推进刚体
//   world.getTransform(...)   // 读回动态刚体的位置，写回场景
//
// 线程：所有函数只在主线程调用；step 内部 Jolt 会用自己的线程池并行求解。
// EXTEND: 把 Jolt 的 JobSystem 接到引擎的 JobSystem 上，共用一个线程池。

#include "physics/PhysicsTypes.h"

#include <memory>
#include <optional>
#include <span>

namespace eng::physics {

class PhysicsWorld {
public:
    PhysicsWorld();
    ~PhysicsWorld();
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    bool init(const PhysicsSettings& settings = {});
    void shutdown();
    bool initialized() const { return m_impl != nullptr; }

    // ---- 形状（失败返回无效 ShapeId 并打日志） ----
    ShapeId createBox(vec3 halfExtents);
    ShapeId createSphere(float radius);
    ShapeId createCapsule(float halfHeight, float radius); // 沿 Y 轴，原点在中心
    ShapeId createConvexHull(std::span<const vec3> points);
    // 三角网格：只能用于静态/运动学刚体
    ShapeId createMesh(std::span<const vec3> positions, std::span<const uint32_t> indices);
    // 高度场：samples 行优先（index = z * resolution + x），采样 (0,0) 位于世界 (originXZ.x, h, originXZ.y)
    ShapeId createHeightfield(std::span<const float> samples, uint32_t resolution, vec2 originXZ, float spacing);
    void releaseShape(ShapeId shape); // 已经创建的刚体仍然持有形状引用，不受影响

    // ---- 刚体 ----
    BodyId addBody(const BodyDesc& desc);
    void removeBody(BodyId body);
    bool contains(BodyId body) const;
    // 瞬移（编辑器拖动、重生点）。resetVelocity：动态刚体同时清零速度
    void setTransform(BodyId body, const vec3& position, const quat& rotation, bool resetVelocity = true);
    bool getTransform(BodyId body, vec3& position, quat& rotation) const;
    void setLinearVelocity(BodyId body, const vec3& v);
    vec3 linearVelocity(BodyId body) const;
    void addImpulse(BodyId body, const vec3& impulse);
    bool isActive(BodyId body) const; // false = 静止休眠中（Jolt 不再求解它，省 CPU）

    // ---- 模拟 ----
    void step(float dt);
    vec3 gravity() const;
    void setGravity(const vec3& g);

    // ---- 查询 ----
    // dir 不需要归一化；返回最近的命中
    std::optional<RayHit> castRay(const vec3& origin, const vec3& dir, float maxDistance) const;

    // ---- 角色控制器 ----
    CharacterId createCharacter(const CharacterDesc& desc, const vec3& feetPosition);
    void destroyCharacter(CharacterId id);
    // 推进一个固定步：wishVelocity 是期望的水平速度（米/秒），jumpSpeed > 0 且站在地上时起跳。
    // 重力、贴地、上台阶、沿墙滑动、推开动态刚体都在里面处理。
    void moveCharacter(CharacterId id, const vec3& wishVelocity, float jumpSpeed, float dt);
    void teleportCharacter(CharacterId id, const vec3& feetPosition);
    CharacterState character(CharacterId id) const;

    PhysicsStats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace eng::physics
