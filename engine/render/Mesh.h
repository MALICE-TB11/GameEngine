#pragma once
// 网格：统一的顶点格式 + GPU 缓冲。
//
// 只用一种顶点格式（位置/法线/UV/切线，48 字节）：所有材质的 VS 输入都一样，
// 任意网格可以配任意材质，不需要「顶点格式 × 材质」的组合爆炸。
// EXTEND: 蒙皮角色（M7 之后）需要骨骼索引/权重，届时增加第二种顶点格式。

#include "math/MathTypes.h"
#include "rhi/Pipeline.h"
#include "rhi/Resources.h"

#include <string>
#include <vector>

namespace eng {

namespace rhi { class Device; }

struct Vertex {
    vec3 position;
    vec3 normal;
    vec2 uv;
    vec4 tangent; // xyz = 切线方向，w = ±1 副切线符号（glTF 约定：bitangent = cross(normal, tangent) * w）
};
static_assert(sizeof(Vertex) == 48, "Vertex 布局必须与 shaders/common/mesh.hlsli 的 VSIn 一致");

// 一级 LOD = 索引缓冲中的一段。所有 LOD 共用同一份顶点缓冲（简化只删三角形、不新增顶点），
// 所以切换 LOD 只是换一段索引，不需要换缓冲。
struct MeshLod {
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    float error = 0.0f; // 这一级相对原网格的几何误差（模型空间，米）；LOD0 为 0
};

struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<MeshLod> lods; // 为空表示只有一级：全部索引
};

struct Aabb {
    vec3 min{1e30f};
    vec3 max{-1e30f};
    void expand(const vec3& p) {
        min = glm::min(min, p);
        max = glm::max(max, p);
    }
};

struct GpuMesh {
    rhi::Buffer vertexBuffer;
    rhi::Buffer indexBuffer;
    uint32_t indexCount = 0; // LOD0 的索引数
    std::vector<MeshLod> lods;
    Aabb bounds;
    std::string name;
};

GpuMesh uploadMesh(rhi::Device& device, const MeshData& data, const std::string& name);
void destroyMesh(rhi::Device& device, GpuMesh& mesh);
void drawMesh(VkCommandBuffer cmd, const GpuMesh& mesh, uint32_t lod = 0);

// 把 Vertex 的顶点输入描述填进管线状态
void setMeshVertexInput(rhi::GraphicsState& state);

// ---- 程序化网格 ----
MeshData makeCube(float halfExtent = 0.5f);
MeshData makeUvSphere(float radius, uint32_t segments, uint32_t rings);
MeshData makePlane(float halfSize, float uvScale = 1.0f);
// 胶囊：底部在 y = 0、顶部在 y = height（角色用）
MeshData makeCapsule(float radius, float height, uint32_t segments = 32, uint32_t rings = 16);
// 由位置/法线/UV 计算切线（glTF 没给切线时使用）
void computeTangents(MeshData& mesh);

} // namespace eng
