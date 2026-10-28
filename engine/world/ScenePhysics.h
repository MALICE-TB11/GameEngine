#pragma once
// 场景 ↔ 物理世界的同步桥。
//
// 实体只声明 physics = None / Static / Dynamic，这个类负责剩下的一切：
//   - 模型加载完成后自动生成碰撞体（静态 → 三角网格，动态 → 凸包），同一模型实例的形状只建一次
//   - 模型热重载（版本号变化）、实体改了模式或缩放 → 重建刚体
//   - 编辑器里拖动/修改了实体变换 → 瞬移对应刚体
//   - 实体被删除、场景被替换 → 对应刚体自动移除（每次同步时没被任何实体认领的刚体就是孤儿）
//   - 每步模拟后把动态刚体的位置写回实体的 current（previous 不动 → 渲染插值照常平滑）
//
// 为什么不在 Scene 里直接持有 BodyId：实体数组会增删、复制（编辑器的「复制实体」），
// 由同步桥按「每帧认领」的方式维护映射，复制出来的实体会自动拿到自己的新刚体，不会两个实体抢一个刚体。

#include "physics/PhysicsWorld.h"
#include "world/Scene.h"

#include <map>
#include <tuple>
#include <vector>

namespace eng {

class ScenePhysics {
public:
    void init(physics::PhysicsWorld& world, asset::AssetManager& assets);
    void shutdown(); // 移除所有由它创建的刚体与形状

    // 每个固定步、world.step 之前调用
    void syncBeforeStep(Scene& scene);
    // world.step 之后调用：动态刚体的位置 → 实体
    void syncAfterStep(Scene& scene);

    struct Stats {
        uint32_t staticBodies = 0;
        uint32_t dynamicBodies = 0;
        uint32_t sleeping = 0; // 已静止休眠的动态刚体
        uint32_t waiting = 0;  // 模型还在加载、暂时没有刚体的实体
    };
    const Stats& stats() const { return m_stats; }

private:
    struct Record {
        physics::BodyId body;
        ModelHandle model;
        uint32_t instance = 0;
        uint32_t version = 0;
        vec3 scale{1.0f};
        PhysicsMode mode = PhysicsMode::None;
        Transform written;       // 最近一次同步时刚体与实体一致的变换
        uint64_t claimedAt = 0;  // 被哪一次同步认领
        bool alive = false;
    };
    // 形状缓存的键：(模型槽位, 模型版本, 实例号, 是否凸包)
    using ShapeKey = std::tuple<uint32_t, uint32_t, uint32_t, bool>;

    physics::ShapeId shapeFor(const Model& model, ModelHandle handle, uint32_t version, uint32_t instance, bool convex);
    uint32_t allocRecord();
    void release(uint32_t slot);
    bool matches(const Record& r, const Entity& e, uint32_t version) const;

    physics::PhysicsWorld* m_world = nullptr;
    asset::AssetManager* m_assets = nullptr;
    std::vector<Record> m_records;
    std::vector<uint32_t> m_free;
    std::map<ShapeKey, physics::ShapeId> m_shapes;
    uint64_t m_syncId = 0;
    Stats m_stats;
};

} // namespace eng
