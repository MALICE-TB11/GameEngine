#pragma once
// 模型：网格 + 材质 + 贴图 + 节点层级（展开成实例列表）。
//
// 两种形态：
//   ModelData —— 纯 CPU 数据，不碰 GPU。glTF 导入器产出它，资源烘焙器读写它（可以在后台线程处理）。
//   Model     —— 已上传到 GPU 的形态，渲染直接使用。由 uploadModel(ModelData) 得到（必须在主线程）。
//
// glTF 的场景是一棵节点树，每个节点有局部变换，可能引用一个网格（网格又有多个 primitive，
// 每个 primitive 一种材质）。渲染时不需要树结构，所以导入时直接展开成
// 「(primitive, 材质, 相对模型根的矩阵) 实例」的扁平列表。
// EXTEND: 需要骨骼动画或运行时移动子节点时，再保留节点树。

#include "render/Material.h"
#include "render/Mesh.h"
#include "rhi/Resources.h"

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace eng {

namespace rhi { class Device; }

// 材质贴图槽位（顺序与 ForwardPass 的 set 1 绑定号、pbr_standard.hlsl 一致）
enum MaterialSlot : uint32_t { kSlotBaseColor = 0, kSlotNormal, kSlotMetalRough, kSlotOcclusion, kSlotEmissive, kMaterialSlotCount };

// 贴图的用途决定烘焙方式：颜色 → BC7 sRGB；法线 → BC5（只存 XY，Z 在 shader 里重建）；数据 → BC7 线性
enum class TextureUsage : uint8_t { Color, Normal, Data };

struct TextureData {
    std::string name;
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    uint32_t width = 0, height = 0;
    std::vector<std::vector<uint8_t>> mips; // 只有 1 级且为 RGBA8 时，上传时由 GPU 生成 mip
    TextureUsage usage = TextureUsage::Data;
};

struct MaterialDesc {
    std::string name;
    MaterialParams params;
    bool doubleSided = false;
    std::array<int32_t, kMaterialSlotCount> textures{-1, -1, -1, -1, -1}; // 指向 ModelData::textures，-1 = 无
};

struct ModelInstance {
    uint32_t mesh = 0;
    uint32_t material = UINT32_MAX; // UINT32_MAX = 默认材质
    mat4 transform{1.0f};           // 相对模型根节点
    std::string name;               // glTF 节点名（编辑器大纲里显示）
};

struct ModelData {
    std::string name;
    std::vector<std::string> meshNames;
    std::vector<MeshData> meshes;
    std::vector<MaterialDesc> materials;
    std::vector<TextureData> textures;
    std::vector<ModelInstance> instances;
};

// 网格的 CPU 端碰撞几何（模型空间）：物理模块据此建三角网格（静态）或凸包（动态）碰撞体
struct CollisionMesh {
    std::vector<vec3> positions;
    std::vector<uint32_t> indices;
};

struct Model {
    using Instance = ModelInstance;

    std::string name;
    std::vector<GpuMesh> meshes;
    std::vector<CollisionMesh> collision; // 与 meshes 一一对应
    std::vector<Material> materials;
    std::vector<rhi::Image> textures; // 加载完成后不再增删：Material 里存的是指向它的指针
    std::vector<Instance> instances;
    Aabb bounds;
};

// 读取 .gltf / .glb 为 CPU 数据（贴图解码为 RGBA8 单级）。可以在后台线程调用。
// 失败返回 nullopt 并写日志（缺失贴图等可恢复问题只警告，用默认贴图代替）。
std::optional<ModelData> loadGltfData(const std::filesystem::path& path);
// 上传到 GPU（必须在主线程）。
Model uploadModel(rhi::Device& device, const ModelData& data);
// 两步合一：直接加载 glTF（不经过资源烘焙，例如工具或测试里用）
std::optional<Model> loadGltf(rhi::Device& device, const std::filesystem::path& path);
void destroyModel(rhi::Device& device, Model& model);

} // namespace eng
