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

struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
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
    uint32_t indexCount = 0;
    Aabb bounds;
    std::string name;
};

GpuMesh uploadMesh(rhi::Device& device, const MeshData& data, const std::string& name);
void destroyMesh(rhi::Device& device, GpuMesh& mesh);
void drawMesh(VkCommandBuffer cmd, const GpuMesh& mesh);

// 把 Vertex 的顶点输入描述填进管线状态
void setMeshVertexInput(rhi::GraphicsState& state);

// ---- 程序化网格 ----
MeshData makeCube(float halfExtent = 0.5f);
MeshData makeUvSphere(float radius, uint32_t segments, uint32_t rings);
MeshData makePlane(float halfSize, float uvScale = 1.0f);
// 由位置/法线/UV 计算切线（glTF 没给切线时使用）
void computeTangents(MeshData& mesh);

} // namespace eng
