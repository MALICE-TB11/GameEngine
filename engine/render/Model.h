#pragma once
// 模型：从 glTF 加载的网格 + 材质 + 贴图 + 节点层级（展开成实例列表）。
//
// glTF 的场景是一棵节点树，每个节点有局部变换，可能引用一个网格（网格又有多个 primitive，
// 每个 primitive 一种材质）。渲染时不需要树结构，所以加载时直接展开成
// 「(primitive, 材质, 世界矩阵) 实例」的扁平列表。
// EXTEND: 需要骨骼动画或运行时移动子节点时，再保留节点树。

#include "render/Material.h"
#include "render/Mesh.h"
#include "rhi/Resources.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace eng {

namespace rhi { class Device; }

struct Model {
    struct Instance {
        uint32_t mesh = 0;
        uint32_t material = UINT32_MAX; // UINT32_MAX = 默认材质
        mat4 transform{1.0f};           // 相对模型根节点
        std::string name;               // glTF 节点名（编辑器大纲里显示）
    };

    std::string name;
    std::vector<GpuMesh> meshes;        // 每个 glTF primitive 一个
    std::vector<Material> materials;
    std::vector<rhi::Image> textures;   // 加载完成后不再增删：Material 里存的是指向它的指针
    std::vector<Instance> instances;
    Aabb bounds;
};

// 加载 .gltf / .glb。失败返回 nullopt 并写日志（缺失贴图等可恢复问题只警告，用默认贴图代替）。
std::optional<Model> loadGltf(rhi::Device& device, const std::filesystem::path& path);
void destroyModel(rhi::Device& device, Model& model);

} // namespace eng
