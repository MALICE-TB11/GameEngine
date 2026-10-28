#include "ocean/OceanRipples.h"

#include "ocean/OceanSpectrum.h"

#include <algorithm>
#include <cmath>

namespace eng::ocean {

float rippleSpeedLimit(float cellSize, float dt) {
    return kRippleCfl * cellSize / std::max(dt, 1e-6f);
}

RippleParams makeRippleParams(const OceanSettings& s, float dt) {
    RippleParams p;
    p.n = uint32_t(std::max(s.rippleResolution, 8));
    p.cellSize = std::max(s.rippleSize, 1.0f) / float(p.n);
    p.maxSpeed = std::min(s.rippleMaxSpeed, rippleSpeedLimit(p.cellSize, dt));
    p.damping = s.rippleDamping;
    p.spongeCells = std::max(s.rippleSpongeWidth * float(p.n), 1.0f);
    p.spongeStrength = s.rippleSpongeStrength;
    p.foamSlope = s.rippleFoamSlope;
    p.foamGain = s.rippleFoamGain;
    p.foamDecay = std::max(s.rippleFoamDecay, 0.01f);
    return p;
}

ivec2 rippleOriginCell(vec2 center, const RippleParams& p) {
    return ivec2(glm::floor(center / p.cellSize)) - ivec2(int(p.n / 2));
}

void rippleStep(const RippleGrid& in, bool hasHistory, const std::vector<float>& depth, const RippleParams& p,
                const RippleStepInput& step, RippleGrid& out) {
    const int n = int(p.n);
    out.assign(size_t(n) * n, vec4(0.0f));
    const float dt = step.dt;
    const float invDx2 = 1.0f / (p.cellSize * p.cellSize);
    const float foamKeep = std::exp(-dt / p.foamDecay);
    // 上一步的状态，按「上一步的网格坐标」取；超出上一步的网格 = 平静水面
    auto load = [&](int i, int j) {
        if (!hasHistory || i < 0 || j < 0 || i >= n || j >= n) return vec4(0.0f);
        return in[size_t(j) * n + i];
    };
    auto wet = [&](int i, int j) { return depth[size_t(j) * n + i] > kRippleMinDepth; };

    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            const float h = depth[size_t(j) * n + i];
            if (!(h > kRippleMinDepth)) {
                out[size_t(j) * n + i] = vec4(0.0f, 0.0f, 0.0f, h); // 岸：没有水
                continue;
            }
            // 区域移动了 shift 格：本步的格子 (i, j) 在上一步的网格里是 (i, j) + shift
            const int si = i + step.shift.x, sj = j + step.shift.y;
            const vec4 cur = load(si, sj);
            const float u = cur.x, up = cur.y;
            // 邻居：区域外 = 平静水面（开边界，靠吸收层避免反射）；岸 = 和自己一样高（零梯度，反射）
            auto neighbor = [&](int di, int dj) {
                const int ni = i + di, nj = j + dj;
                if (ni < 0 || nj < 0 || ni >= n || nj >= n) return 0.0f;
                if (!wet(ni, nj)) return u;
                return load(si + di, sj + dj).x;
            };
            const float uL = neighbor(-1, 0), uR = neighbor(1, 0), uD = neighbor(0, -1), uU = neighbor(0, 1);
            const float lap = (uL + uR + uD + uU - 4.0f * u) * invDx2;
            const float c2 = std::min(kGravity * h, p.maxSpeed * p.maxSpeed);

            // 吸收层：离最近的区域边缘 edge 格，ramp 从 0（内侧）到 1（最外圈），阻尼按 ramp² 增大
            const float edge = float(std::min(std::min(i, j), std::min(n - 1 - i, n - 1 - j)));
            const float ramp = std::clamp(1.0f - edge / p.spongeCells, 0.0f, 1.0f);
            const float sponge = p.spongeStrength * ramp * ramp;
            const float gamma = p.damping + sponge;

            // 扰动源：格子中心处各个高斯的加速度之和
            const vec2 x = step.origin + (vec2(float(i), float(j)) + 0.5f) * p.cellSize;
            float f = 0.0f;
            for (uint32_t k = 0; k < step.sourceCount && k < kMaxRippleSources; ++k) {
                const RippleSource& s = step.sources[k];
                const vec2 d = x - s.position;
                const float r = std::max(s.radius, 0.01f);
                f += std::exp(-glm::dot(d, d) / (r * r)) * (s.strength + glm::dot(d, s.push) / r);
            }

            // 吸收层只加大「速度阻尼」γ。曾经再把高度乘 e^{-σΔt} 往 0 拉：蛙跳格式里这会凭空产生速度、
            // 把一部分能量困在吸收层里来回振（单元测试测到 4 秒后还剩 40%）；只阻尼速度时剩 0.2%
            const float next = u + (u - up) * (1.0f - gamma * dt) + c2 * dt * dt * lap + dt * dt * f;
            // 尾迹泡沫：坡度超过阈值的地方起泡，之后按时间常数消散（在世界坐标里原地消散，不随水流走）
            const vec2 grad = vec2(uR - uL, uU - uD) / (2.0f * p.cellSize);
            const float fresh = std::clamp((glm::length(grad) - p.foamSlope) * p.foamGain, 0.0f, 1.0f);
            out[size_t(j) * n + i] = vec4(next, u, std::max(cur.z * foamKeep, fresh), h);
        }
}

double rippleEnergy(const RippleGrid& g, const std::vector<float>& depth, const RippleParams& p, float dt) {
    const int n = int(p.n);
    double e = 0.0;
    for (int j = 0; j + 1 < n; ++j)
        for (int i = 0; i + 1 < n; ++i) {
            const size_t c = size_t(j) * n + i;
            if (!(depth[c] > kRippleMinDepth)) continue;
            const double v = (double(g[c].x) - g[c].y) / dt;
            const double gx = depth[c + 1] > kRippleMinDepth ? (double(g[c + 1].x) - g[c].x) / p.cellSize : 0.0;
            const double gz = depth[c + n] > kRippleMinDepth ? (double(g[c + n].x) - g[c].x) / p.cellSize : 0.0;
            const double c2 = std::min(double(kGravity) * depth[c], double(p.maxSpeed) * p.maxSpeed);
            e += (v * v + c2 * (gx * gx + gz * gz)) * p.cellSize * p.cellSize;
        }
    return e;
}

} // namespace eng::ocean
