#include "terrain/Heightfield.h"

#include "core/JobSystem.h"

#include <algorithm>
#include <cmath>

namespace eng::terrain {
namespace {

// ---- 梯度噪声（Perlin 风格）：比值噪声更平滑、没有网格感 ----
uint32_t hash2(int x, int y, uint32_t seed) {
    uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

float gradNoise(float x, float y, uint32_t seed) {
    const int xi = int(std::floor(x)), yi = int(std::floor(y));
    const float fx = x - float(xi), fy = y - float(yi);
    auto grad = [&](int gx, int gy, float dx, float dy) {
        const float a = float(hash2(gx, gy, seed) & 0xFFFF) / 65535.0f * 6.2831853f; // 随机方向的梯度
        return std::cos(a) * dx + std::sin(a) * dy;
    };
    // 五次平滑插值 6t⁵-15t⁴+10t³：二阶导连续，地形法线不会出现网格状折痕
    auto fade = [](float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); };
    const float u = fade(fx), v = fade(fy);
    const float n00 = grad(xi, yi, fx, fy), n10 = grad(xi + 1, yi, fx - 1, fy);
    const float n01 = grad(xi, yi + 1, fx, fy - 1), n11 = grad(xi + 1, yi + 1, fx - 1, fy - 1);
    const float nx0 = n00 + (n10 - n00) * u, nx1 = n01 + (n11 - n01) * u;
    return (nx0 + (nx1 - nx0) * v) * 1.4142f; // 大致归一化到 [-1, 1]
}

// 分形叠加（fBm）：多个频率的噪声相加，频率翻倍、振幅减半 → 大起伏上叠小细节
float fbm(float x, float y, uint32_t seed, int octaves) {
    float sum = 0.0f, amp = 0.5f, freq = 1.0f, norm = 0.0f;
    for (int i = 0; i < octaves; ++i) {
        sum += gradNoise(x * freq, y * freq, seed + uint32_t(i) * 101u) * amp;
        norm += amp;
        amp *= 0.5f;
        freq *= 2.0f;
    }
    return sum / norm;
}

// 山脊噪声：|噪声| 取反，形成尖锐的山脊线（比 fBm 更像侵蚀过的山）
float ridged(float x, float y, uint32_t seed, int octaves) {
    float sum = 0.0f, amp = 0.5f, freq = 1.0f, norm = 0.0f;
    for (int i = 0; i < octaves; ++i) {
        const float n = 1.0f - std::abs(gradNoise(x * freq, y * freq, seed + 977u + uint32_t(i) * 131u));
        sum += n * n * amp;
        norm += amp;
        amp *= 0.5f;
        freq *= 2.0f;
    }
    return sum / norm;
}

float smoothstep(float a, float b, float x) {
    // 退化区间（例如编辑器里把平台半径拖到 0）：按阶跃处理。否则 0/0 = NaN 会写进高度场，
    // 渲染出黑洞、Jolt 里变成没有碰撞的窟窿
    if (b <= a) return x < a ? 0.0f : 1.0f;
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// 世界 XZ 位置 p 处的岛屿高度（米）
float islandHeight(const TerrainSettings& s, vec2 p) {
    const float r = glm::length(p);                    // 到原点（平台中心）的距离
    const float rIsland = glm::length(p - s.islandCenter); // 到岛心的距离

    // 1. 海岸线：到岛心的距离被低频噪声扰动 → 不规则的海岸
    const float coast = fbm(p.x / 300.0f, p.y / 300.0f, s.seed, 3);
    const float d = rIsland / s.islandRadius + s.coastNoise * coast;
    const float land = 1.0f - smoothstep(0.75f, 1.05f, d); // 1 = 陆地，0 = 海

    // 2. 陆地：越往岛心越高的基础坡 + 山脊 + 细节起伏
    const float inland = std::clamp(1.0f - d, 0.0f, 1.0f);
    const float hills = ridged(p.x / s.hillScale, p.y / s.hillScale, s.seed, 5);
    const float detail = fbm(p.x / 35.0f, p.y / 35.0f, s.seed + 17u, 4);
    const float landHeight = 1.0f + inland * inland * s.maxHeight * (0.35f + 0.65f * hills) + detail * 1.5f;

    // 3. 海床：离岸越远越深，带一些沙脊起伏
    const float sea = smoothstep(0.9f, 2.2f, d);
    const float seabed = -2.0f - sea * s.seabedDepth + fbm(p.x / 60.0f, p.y / 60.0f, s.seed + 31u, 3) * 3.0f;

    float h = seabed + (landHeight - seabed) * land;

    // 4. 原点平台：整平一块地方放遗迹和角色出生点
    const float plateau = 1.0f - smoothstep(s.plateauRadius, s.plateauRadius * 2.2f, r);
    return h + (s.plateauHeight - h) * plateau;
}

} // namespace

void Heightfield::generate(const TerrainSettings& s, JobSystem* jobs) {
    m_resolution = std::max(3u, s.resolution);
    m_worldSize = s.worldSize;
    m_heights.assign(size_t(m_resolution) * m_resolution, 0.0f);
    const float step = spacing();
    const vec2 org = origin();

    // 每个采样只依赖自己的坐标（纯函数）→ 各行之间没有数据竞争，可以直接并行
    auto rows = [&](uint32_t zBegin, uint32_t zEnd) {
        for (uint32_t z = zBegin; z < zEnd; ++z)
            for (uint32_t x = 0; x < m_resolution; ++x)
                m_heights[size_t(z) * m_resolution + x] = islandHeight(s, org + vec2(float(x), float(z)) * step);
    };
    if (jobs) jobs->parallelFor(m_resolution, 16, rows);
    else rows(0, m_resolution);

    const auto [mn, mx] = std::minmax_element(m_heights.begin(), m_heights.end());
    m_min = *mn;
    m_max = *mx;
}

float Heightfield::heightAt(float x, float z) const {
    if (m_heights.empty()) return 0.0f;
    const vec2 o = origin();
    const float fx = std::clamp((x - o.x) / spacing(), 0.0f, float(m_resolution - 1));
    const float fz = std::clamp((z - o.y) / spacing(), 0.0f, float(m_resolution - 1));
    const uint32_t x0 = std::min(uint32_t(fx), m_resolution - 2), z0 = std::min(uint32_t(fz), m_resolution - 2);
    const float tx = fx - float(x0), tz = fz - float(z0);
    const float h00 = sample(x0, z0), h10 = sample(x0 + 1, z0), h01 = sample(x0, z0 + 1), h11 = sample(x0 + 1, z0 + 1);
    // 不用双线性插值：渲染网格和 Jolt 碰撞体都是「三角形」，每格沿 (x,z)→(x+1,z+1) 对角线切开。
    // 查询必须落在同一个三角形平面上，三者才是同一个表面。
    if (tx >= tz) return h00 + tx * (h10 - h00) + tz * (h11 - h10); // 三角形 (0,0)-(1,0)-(1,1)
    return h00 + tz * (h01 - h00) + tx * (h11 - h01);               // 三角形 (0,0)-(0,1)-(1,1)
}

vec3 Heightfield::normalAt(float x, float z) const {
    // 中心差分：法线 = normalize(-∂h/∂x, 1, -∂h/∂z)
    const float e = spacing();
    const float dx = heightAt(x + e, z) - heightAt(x - e, z);
    const float dz = heightAt(x, z + e) - heightAt(x, z - e);
    return glm::normalize(vec3(-dx, 2.0f * e, -dz));
}

void Heightfield::rangeMinMax(uint32_t x0, uint32_t z0, uint32_t x1, uint32_t z1, float& outMin, float& outMax) const {
    outMin = 1e30f;
    outMax = -1e30f;
    x1 = std::min(x1, m_resolution - 1);
    z1 = std::min(z1, m_resolution - 1);
    for (uint32_t z = z0; z <= z1; ++z)
        for (uint32_t x = x0; x <= x1; ++x) {
            const float h = sample(x, z);
            outMin = std::min(outMin, h);
            outMax = std::max(outMax, h);
        }
}

} // namespace eng::terrain
