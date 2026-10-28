#pragma once
// 地形四叉树 LOD：每帧决定「用哪些尺寸的网格块覆盖整个地形」。
//
// 每个节点都画成同一个 32×32 格的网格块（patch），只是覆盖的世界范围不同：
//   根节点（级别 0）覆盖整个 1024 m 地形 → 一格 32 m；叶子（级别 5）覆盖 32 m → 一格 1 m（= 高度场原始精度）。
// 选择规则：离相机近的节点继续细分，远的直接画 —— 屏幕上每个格子的大小因此大致恒定。
//   细分条件：到节点包围盒的距离 < 节点边长 × lodDistanceFactor
// 每个节点的最小/最大高度预先算好，用于视锥剔除和距离计算。
//
// 纯 CPU 逻辑，不碰 GPU：可以单独做单元测试（覆盖完整、无重叠、远处更粗）。
// EXTEND: 相邻节点级别差 > 1 时裂缝会变大；需要时加「限制相邻级别差」或 CDLOD 的顶点渐变（geomorphing）。

#include "math/Geometry.h"
#include "terrain/Heightfield.h"

#include <vector>

namespace eng::terrain {

struct TerrainNode {
    vec2 origin{0.0f}; // 最小角的世界 XZ
    float size = 0.0f; // 边长（米）
    uint32_t level = 0;
    float minY = 0.0f, maxY = 0.0f;
};

class TerrainQuadtree {
public:
    // patchQuads：每个网格块每边的格数。高度场 (resolution-1) 必须能被它整除，且商是 2 的幂
    bool build(const Heightfield& hf, uint32_t patchQuads);

    // frustum 为空 = 不做视锥剔除（阴影要画相机背后的投影物）。
    // maxDistance > 0 时，包围盒离相机更远的节点直接丢弃
    void select(const vec3& cameraPos, float lodDistanceFactor, const Frustum* frustum, float maxDistance,
                std::vector<TerrainNode>& out) const;

    uint32_t maxLevel() const { return m_maxLevel; }
    bool valid() const { return !m_levels.empty(); }

private:
    struct Range {
        float minY, maxY;
    };
    void selectNode(uint32_t level, uint32_t x, uint32_t z, const vec3& cam, float factor, const Frustum* frustum,
                    float maxDistance, std::vector<TerrainNode>& out) const;
    TerrainNode node(uint32_t level, uint32_t x, uint32_t z) const;

    vec2 m_origin{0.0f};
    float m_worldSize = 0.0f;
    uint32_t m_maxLevel = 0;
    std::vector<std::vector<Range>> m_levels; // m_levels[level][z * (1<<level) + x]
};

} // namespace eng::terrain
