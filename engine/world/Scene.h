#pragma once
// 场景：一组实体。每个实体有「上一步 / 当前步」两份变换（固定步长模拟），
// 渲染时按插值因子 alpha 混合 —— 主循环的「物理固定步长、渲染插值」在这里落地。
//
// 刻意不做 ECS：现阶段实体只有「变换 + 可选的网格/材质」，一个结构体数组最直白。
// EXTEND: M4 给实体加 PhysicsWorld 的刚体句柄；实体类型变多时再考虑组件化。

#include "math/Transform.h"
#include "render/Material.h"
#include "render/Mesh.h"
#include "render/Model.h"
#include "render/RenderTypes.h"

#include <memory>
#include <string>
#include <vector>

namespace eng {

namespace rhi { class Device; }

using EntityId = uint32_t;

struct Entity {
    std::string name;
    Transform previous; // 上一个固定步的状态
    Transform current;  // 当前固定步的状态
    mat4 meshOffset{1.0f}; // 网格相对实体的固定偏移（来自 glTF 节点层级）
    const GpuMesh* mesh = nullptr;
    const Material* material = nullptr;
    bool visible = true;
};

class Scene {
public:
    EntityId add(Entity e);
    Entity& entity(EntityId id) { return m_entities[id]; }
    std::vector<Entity>& entities() { return m_entities; }

    // 加载 glTF 并为其每个实例创建实体；模型资源归场景所有。失败返回 false。
    bool addModel(rhi::Device& device, const std::filesystem::path& path, const Transform& root);
    // 场景持有的程序化网格与材质（指针在场景生命周期内稳定）
    const GpuMesh* addMesh(rhi::Device& device, const MeshData& data, const std::string& name);
    Material* addMaterial(const Material& m);

    // 每个固定步开始时调用：current → previous
    void beginFixedStep();
    // 生成渲染列表，变换按 alpha 在 previous 与 current 之间插值
    void buildDrawList(DrawList& out, float alpha) const;

    // 所有可编辑材质（ImGui 材质面板用）
    std::vector<Material*> materials();

    void destroy(rhi::Device& device);

private:
    std::vector<Entity> m_entities;
    std::vector<std::unique_ptr<Model>> m_models;       // unique_ptr：保证 Model 内部指针稳定
    std::vector<std::unique_ptr<GpuMesh>> m_meshes;
    std::vector<std::unique_ptr<Material>> m_materials;
};

} // namespace eng
