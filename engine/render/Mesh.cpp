#include "render/Mesh.h"

#include "rhi/Upload.h"

#include <algorithm>
#include <cstddef>

namespace eng {

GpuMesh uploadMesh(rhi::Device& device, const MeshData& data, const std::string& name) {
    GpuMesh m;
    m.name = name;
    m.lods = data.lods;
    if (m.lods.empty()) m.lods.push_back({0, static_cast<uint32_t>(data.indices.size()), 0.0f});
    m.indexCount = m.lods[0].indexCount;
    for (const Vertex& v : data.vertices) m.bounds.expand(v.position);
    m.vertexBuffer = rhi::createBufferWithData(device, data.vertices.data(), data.vertices.size() * sizeof(Vertex),
                                               VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, (name + "_VB").c_str());
    m.indexBuffer = rhi::createBufferWithData(device, data.indices.data(), data.indices.size() * sizeof(uint32_t),
                                              VK_BUFFER_USAGE_INDEX_BUFFER_BIT, (name + "_IB").c_str());
    return m;
}

void destroyMesh(rhi::Device& device, GpuMesh& mesh) {
    rhi::destroyBuffer(device, mesh.vertexBuffer);
    rhi::destroyBuffer(device, mesh.indexBuffer);
    mesh.indexCount = 0;
}

void drawMesh(VkCommandBuffer cmd, const GpuMesh& mesh, uint32_t lod) {
    const MeshLod& l = mesh.lods[std::min<size_t>(lod, mesh.lods.size() - 1)];
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &mesh.vertexBuffer.buffer, &offset);
    vkCmdBindIndexBuffer(cmd, mesh.indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, l.indexCount, 1, l.firstIndex, 0, 0);
}

void setMeshVertexInput(rhi::GraphicsState& s) {
    s.vertexBindings = {{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX}};
    // location 必须与 HLSL 里的 [[vk::location(n)]] 对应
    s.vertexAttributes = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)},
        {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, tangent)},
    };
}

MeshData makeCube(float h) {
    MeshData m;
    // 6 个面各自 4 个顶点：面与面之间法线不同，不能共享顶点
    struct Face {
        vec3 n, u, v;
    };
    const Face faces[6] = {
        {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}},  {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}}, {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}},
        {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}}, {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},  {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}},
    };
    for (const Face& f : faces) {
        const uint32_t base = static_cast<uint32_t>(m.vertices.size());
        const vec2 corners[4] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        for (const vec2& c : corners) {
            Vertex v;
            v.position = (f.n + f.u * c.x + f.v * c.y) * h;
            v.normal = f.n;
            v.uv = vec2(c.x * 0.5f + 0.5f, 0.5f - c.y * 0.5f);
            v.tangent = vec4(f.u, 1.0f);
            m.vertices.push_back(v);
        }
        // 逆时针（从外面看）为正面
        m.indices.insert(m.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    return m;
}

MeshData makeUvSphere(float radius, uint32_t segments, uint32_t rings) {
    MeshData m;
    for (uint32_t r = 0; r <= rings; ++r) {
        const float v = float(r) / float(rings);
        const float phi = v * kPi; // 从北极(0)到南极(π)
        for (uint32_t s = 0; s <= segments; ++s) {
            const float u = float(s) / float(segments);
            const float theta = u * 2.0f * kPi;
            const vec3 n{std::sin(phi) * std::cos(theta), std::cos(phi), -std::sin(phi) * std::sin(theta)};
            Vertex vert;
            vert.position = n * radius;
            vert.normal = n;
            vert.uv = {u, v};
            // 切线沿 u 增大方向（theta 的导数）
            vert.tangent = vec4(-std::sin(theta), 0.0f, -std::cos(theta), 1.0f);
            m.vertices.push_back(vert);
        }
    }
    const uint32_t stride = segments + 1;
    for (uint32_t r = 0; r < rings; ++r)
        for (uint32_t s = 0; s < segments; ++s) {
            const uint32_t a = r * stride + s, b = a + stride;
            m.indices.insert(m.indices.end(), {a, b, a + 1, a + 1, b, b + 1});
        }
    return m;
}

MeshData makeCapsule(float radius, float height, uint32_t segments, uint32_t rings) {
    // 球从赤道切开，上半球抬高、下半球放低，中间用一圈重复的赤道顶点连成圆柱。
    // 底部在 y = 0（与角色控制器「位置 = 脚底」的约定一致）
    MeshData m;
    rings += rings & 1u; // 环数取偶数，赤道正好落在一环上
    const float halfCylinder = std::max(height * 0.5f - radius, 0.0f);
    const float center = height * 0.5f;
    for (uint32_t r = 0; r <= rings + 1; ++r) {
        // 第 rings/2 环和第 rings/2+1 环都是赤道：前者属于上半球，后者属于下半球
        const uint32_t ring = r <= rings / 2 ? r : r - 1;
        const float phi = float(ring) / float(rings) * kPi;
        const float yOffset = r <= rings / 2 ? halfCylinder : -halfCylinder;
        for (uint32_t s = 0; s <= segments; ++s) {
            const float u = float(s) / float(segments);
            const float theta = u * 2.0f * kPi;
            const vec3 n{std::sin(phi) * std::cos(theta), std::cos(phi), -std::sin(phi) * std::sin(theta)};
            Vertex vert;
            vert.position = n * radius + vec3(0.0f, center + yOffset, 0.0f);
            vert.normal = n;
            vert.uv = {u, vert.position.y / std::max(height, 1e-3f)};
            vert.tangent = vec4(-std::sin(theta), 0.0f, -std::cos(theta), 1.0f);
            m.vertices.push_back(vert);
        }
    }
    const uint32_t stride = segments + 1;
    for (uint32_t r = 0; r <= rings; ++r)
        for (uint32_t s = 0; s < segments; ++s) {
            const uint32_t a = r * stride + s, b = a + stride;
            m.indices.insert(m.indices.end(), {a, b, a + 1, a + 1, b, b + 1});
        }
    return m;
}

MeshData makePlane(float halfSize, float uvScale) {
    MeshData m;
    const vec2 corners[4] = {{-1, 1}, {1, 1}, {1, -1}, {-1, -1}};
    for (const vec2& c : corners) {
        Vertex v;
        v.position = vec3(c.x, 0.0f, c.y) * halfSize;
        v.normal = {0, 1, 0};
        v.uv = (vec2(c.x, c.y) * 0.5f + 0.5f) * uvScale;
        v.tangent = {1, 0, 0, 1};
        m.vertices.push_back(v);
    }
    m.indices = {0, 1, 2, 0, 2, 3};
    return m;
}

void computeTangents(MeshData& mesh) {
    // 对每个三角形，由 UV 的偏导求出切线/副切线，累加到顶点后正交化（Gram-Schmidt）
    std::vector<vec3> tan(mesh.vertices.size(), vec3(0)), bitan(mesh.vertices.size(), vec3(0));
    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const uint32_t i0 = mesh.indices[i], i1 = mesh.indices[i + 1], i2 = mesh.indices[i + 2];
        const Vertex &v0 = mesh.vertices[i0], &v1 = mesh.vertices[i1], &v2 = mesh.vertices[i2];
        const vec3 e1 = v1.position - v0.position, e2 = v2.position - v0.position;
        const vec2 d1 = v1.uv - v0.uv, d2 = v2.uv - v0.uv;
        const float det = d1.x * d2.y - d2.x * d1.y;
        if (std::abs(det) < 1e-12f) continue;
        const float r = 1.0f / det;
        const vec3 t = (e1 * d2.y - e2 * d1.y) * r;
        const vec3 b = (e2 * d1.x - e1 * d2.x) * r;
        for (uint32_t idx : {i0, i1, i2}) {
            tan[idx] += t;
            bitan[idx] += b;
        }
    }
    for (size_t i = 0; i < mesh.vertices.size(); ++i) {
        Vertex& v = mesh.vertices[i];
        vec3 t = tan[i] - v.normal * glm::dot(v.normal, tan[i]);
        if (glm::dot(t, t) < 1e-12f) {
            // 退化（UV 缺失）：随便取一个与法线垂直的方向
            t = std::abs(v.normal.x) < 0.9f ? glm::cross(v.normal, vec3(1, 0, 0)) : glm::cross(v.normal, vec3(0, 1, 0));
        }
        t = glm::normalize(t);
        // glTF 约定：B = cross(N, T) * w 指向贴图的「上方」（v 减小的方向，因为 UV 原点在左上角），
        // 而 bitan 累加的是 v 增大的方向，所以两者反向时 w = +1。
        const float w = glm::dot(glm::cross(v.normal, t), bitan[i]) < 0.0f ? 1.0f : -1.0f;
        v.tangent = vec4(t, w);
    }
}

} // namespace eng
