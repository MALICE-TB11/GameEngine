#pragma once
// 物理模块的公开类型。只用引擎自己的数学类型 —— 玩法和场景代码永远看不到 Jolt 的类型，
// 将来换物理引擎（或者给 Jolt 升级大版本）只需要改 PhysicsWorld.cpp 一个文件。

#include "math/MathTypes.h"

#include <cstdint>

namespace eng::physics {

// 碰撞形状。同一个形状可以被很多刚体共用（例如 14 个材质球共用一个球形凸包）。
struct ShapeId {
    uint32_t index = UINT32_MAX;
    bool valid() const { return index != UINT32_MAX; }
    bool operator==(const ShapeId&) const = default;
};

// 刚体。value 是 Jolt BodyID 的「下标 + 序号」：刚体删除后槽位会复用，但序号不同，旧句柄自然失效。
struct BodyId {
    uint32_t value = UINT32_MAX;
    bool valid() const { return value != UINT32_MAX; }
    bool operator==(const BodyId&) const = default;
};

struct CharacterId {
    uint32_t index = UINT32_MAX;
    bool valid() const { return index != UINT32_MAX; }
};

enum class Motion : uint8_t {
    Static,    // 永远不动：地形、建筑。可以用三角网格形状
    Dynamic,   // 受力、重力、碰撞驱动。必须是凸形状（凸包、盒、球、胶囊）或它们的组合
    Kinematic, // 由代码设置速度/位置，能推开动态刚体但自己不受力：移动平台、电梯
};

struct BodyDesc {
    ShapeId shape;
    vec3 scale{1.0f}; // 非均匀缩放（球/胶囊只支持均匀缩放，会取平均）
    Motion motion = Motion::Static;
    vec3 position{0.0f};
    quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    float friction = 0.6f;
    float restitution = 0.05f; // 弹性：0 完全不弹，1 完全弹性
    float density = 600.0f;    // kg/m³（动态刚体的质量 = 密度 × 形状体积）；木头约 600，石头约 2500
    uint64_t userData = 0;     // 调用者自定义（ScenePhysics 不用它；射线检测结果会带回来）
};

struct RayHit {
    vec3 point{0.0f};
    vec3 normal{0.0f, 1.0f, 0.0f};
    float distance = 0.0f;
    BodyId body;
    uint64_t userData = 0;
};

// 角色控制器：胶囊体 + 「虚拟角色」（不是刚体，由代码逐步推进，自动沿墙滑动、上台阶、贴地下坡）
struct CharacterDesc {
    float radius = 0.35f;
    float height = 1.8f;       // 总高度（含两端半球）
    float maxSlopeDeg = 50.0f; // 能走上去的最大坡度
    float stepUp = 0.35f;      // 自动跨上的台阶高度
    float stickDown = 0.5f;    // 下坡时贴地的最大距离（避免走下坡时「飞起来」）
    float mass = 70.0f;        // 推动态刚体时使用
    float maxPushForce = 500.0f;
};

struct CharacterState {
    vec3 position{0.0f}; // 脚底位置
    vec3 velocity{0.0f};
    bool grounded = false;     // 站在能行走的地面上
    bool onSteepGround = false; // 站在太陡的坡上（会滑下去）
    vec3 groundNormal{0.0f, 1.0f, 0.0f};
};

struct PhysicsSettings {
    vec3 gravity{0.0f, -9.81f, 0.0f};
    uint32_t maxBodies = 8192;
    int collisionSteps = 1;  // 每次 step 内部的碰撞子步数：高速物体穿透时调大
    uint32_t threads = 0;    // 物理工作线程数，0 = 自动（CPU 核数 - 1，最多 4）
    uint32_t tempMemoryMB = 16;
};

struct PhysicsStats {
    uint32_t bodies = 0;
    uint32_t activeBodies = 0;
    uint32_t shapes = 0;
    uint32_t characters = 0;
    float stepMs = 0.0f; // 最近一次 step 的 CPU 耗时
};

} // namespace eng::physics
