#pragma once
// 场景：一组实体。每个实体有「上一步 / 当前步」两份变换（固定步长模拟），
// 渲染时按插值因子 alpha 混合 —— 主循环的「物理固定步长、渲染插值」在这里落地。
//
// 实体不直接持有网格/材质指针，而是「模型句柄 + 实例号」：
//   - 模型热重载后（GPU 资源整个换掉）实体自动用上新数据，不会悬空
//   - 场景文件只需要记录「哪个资产（GUID）的第几个实例」
// 材质参数可以按实体覆盖（materialOverride），不影响其他使用同一模型的实体。
//
// 刻意不做 ECS：现阶段实体只有「变换 + 模型引用 + 材质覆盖」，一个结构体数组最直白。
// EXTEND: M4 给实体加 PhysicsWorld 的刚体句柄；实体类型变多时再考虑组件化。

#include "asset/AssetManager.h"
#include "math/Transform.h"
#include "render/RenderTypes.h"
#include "world/SceneDesc.h"

#include <optional>
#include <string>
#include <vector>

namespace eng {

using EntityId = uint32_t;
using asset::ModelHandle;

struct Entity {
    std::string name;
    Transform previous; // 上一个固定步的状态
    Transform current;  // 当前固定步的状态
    ModelHandle model;
    uint32_t instance = 0; // 模型中的第几个实例（glTF 的节点 × primitive）
    std::optional<MaterialParams> materialOverride;
    bool visible = true;
};

// LOD 选择需要的相机信息
struct LodContext {
    vec3 cameraPos{0.0f};
    float pixelsPerRadian = 1000.0f; // 屏幕高度 / (2·tan(fov/2))：一弧度张角对应多少像素
    float thresholdPx = 1.0f;        // 误差投影到屏幕上小于这么多像素就可以用更粗的 LOD
};

class Scene {
public:
    void init(asset::AssetManager& assets) { m_assets = &assets; }

    EntityId add(Entity e);
    void remove(EntityId id);
    void clear() { m_entities.clear(); }
    Entity& entity(EntityId id) { return m_entities[id]; }
    const Entity& entity(EntityId id) const { return m_entities[id]; }
    std::vector<Entity>& entities() { return m_entities; }
    const std::vector<Entity>& entities() const { return m_entities; }
    std::optional<EntityId> findByName(const std::string& name) const;

    // 为模型的每个实例创建一个实体（名字为「前缀/节点名」），返回第一个实体
    EntityId instantiate(ModelHandle model, const Transform& root, const std::string& prefix);

    // 每个固定步开始时调用：current → previous
    void beginFixedStep();
    // 生成渲染列表：变换按 alpha 插值、按屏幕误差选 LOD、选中的实体带高亮
    void buildDrawList(DrawList& out, float alpha, const LodContext& lod, std::optional<EntityId> highlighted = std::nullopt,
                       float highlightStrength = 0.0f) const;

    // 解析实体引用：当前（可能是兜底）模型中的网格、材质、实例
    const GpuMesh* mesh(EntityId id) const;
    const Material* material(EntityId id) const;
    mat4 worldMatrix(EntityId id, float alpha = 1.0f) const;
    bool worldBounds(EntityId id, vec3& outMin, vec3& outMax) const;
    uint64_t lastTriangleCount() const { return m_lastTriangles; }

    // ---- 序列化 ----
    SceneDesc toDesc() const;
    // 清空当前场景并按描述重建（模型通过 AssetManager 异步加载）
    void fromDesc(const SceneDesc& desc);

private:
    const ModelInstance* resolveInstance(const Entity& e) const;

    asset::AssetManager* m_assets = nullptr;
    std::vector<Entity> m_entities;
    mutable uint64_t m_lastTriangles = 0;
};

} // namespace eng
