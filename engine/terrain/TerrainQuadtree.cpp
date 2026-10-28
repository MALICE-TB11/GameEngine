#include "terrain/TerrainQuadtree.h"

#include "core/Log.h"

#include <bit>

namespace eng::terrain {

bool TerrainQuadtree::build(const Heightfield& hf, uint32_t patchQuads) {
    m_levels.clear();
    m_maxLevel = 0;
    const uint32_t cells = hf.resolution() - 1;
    if (patchQuads == 0 || cells % patchQuads != 0 || !std::has_single_bit(cells / patchQuads)) {
        LOG_ERROR("地形分辨率 {} 不合法：(分辨率 - 1) 必须是 {} × 2 的幂", hf.resolution(), patchQuads);
        return false;
    }
    m_origin = hf.origin();
    m_worldSize = hf.worldSize();
    m_maxLevel = uint32_t(std::countr_zero(cells / patchQuads)); // 叶子数 = 2^maxLevel（每边）
    m_levels.resize(m_maxLevel + 1);

    // 自底向上：叶子直接扫高度采样，父节点 = 四个子节点的并集（每个采样只扫一遍）
    const uint32_t leaves = 1u << m_maxLevel;
    m_levels[m_maxLevel].resize(size_t(leaves) * leaves);
    for (uint32_t z = 0; z < leaves; ++z)
        for (uint32_t x = 0; x < leaves; ++x) {
            Range& r = m_levels[m_maxLevel][size_t(z) * leaves + x];
            hf.rangeMinMax(x * patchQuads, z * patchQuads, (x + 1) * patchQuads, (z + 1) * patchQuads, r.minY, r.maxY);
        }
    for (int level = int(m_maxLevel) - 1; level >= 0; --level) {
        const uint32_t n = 1u << level;
        const auto& child = m_levels[level + 1];
        auto& cur = m_levels[level];
        cur.resize(size_t(n) * n);
        for (uint32_t z = 0; z < n; ++z)
            for (uint32_t x = 0; x < n; ++x) {
                Range r{1e30f, -1e30f};
                for (uint32_t c = 0; c < 4; ++c) {
                    const Range& cr = child[size_t(z * 2 + (c >> 1)) * (n * 2) + (x * 2 + (c & 1))];
                    r.minY = std::min(r.minY, cr.minY);
                    r.maxY = std::max(r.maxY, cr.maxY);
                }
                cur[size_t(z) * n + x] = r;
            }
    }
    return true;
}

TerrainNode TerrainQuadtree::node(uint32_t level, uint32_t x, uint32_t z) const {
    TerrainNode n;
    n.level = level;
    n.size = m_worldSize / float(1u << level);
    n.origin = m_origin + vec2(float(x), float(z)) * n.size;
    const Range& r = m_levels[level][size_t(z) * (size_t(1) << level) + x];
    n.minY = r.minY;
    n.maxY = r.maxY;
    return n;
}

void TerrainQuadtree::select(const vec3& cameraPos, float lodDistanceFactor, const Frustum* frustum, float maxDistance,
                             std::vector<TerrainNode>& out) const {
    out.clear();
    if (m_levels.empty()) return;
    selectNode(0, 0, 0, cameraPos, std::max(lodDistanceFactor, 0.1f), frustum, maxDistance, out);
}

void TerrainQuadtree::selectNode(uint32_t level, uint32_t x, uint32_t z, const vec3& cam, float factor, const Frustum* frustum,
                                 float maxDistance, std::vector<TerrainNode>& out) const {
    const TerrainNode n = node(level, x, z);
    // 包围盒向下多留一点：裙边会把边缘顶点往下拉
    const vec3 bmin{n.origin.x, n.minY - n.size * 0.1f, n.origin.y};
    const vec3 bmax{n.origin.x + n.size, n.maxY, n.origin.y + n.size};
    if (frustum && !frustum->intersectsAabb(bmin, bmax)) return;

    const vec3 closest = glm::clamp(cam, bmin, bmax);
    const float dist = glm::length(cam - closest);
    if (maxDistance > 0.0f && dist > maxDistance) return;

    if (level < m_maxLevel && dist < n.size * factor) {
        for (uint32_t c = 0; c < 4; ++c)
            selectNode(level + 1, x * 2 + (c & 1), z * 2 + (c >> 1), cam, factor, frustum, maxDistance, out);
        return;
    }
    out.push_back(n);
}

} // namespace eng::terrain
