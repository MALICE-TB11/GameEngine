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
    p.solver = s.rippleSolver == int(RippleSolver::Shallow) ? RippleSolver::Shallow : RippleSolver::Wave;
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

float rippleSourceRate(const RippleStepInput& step, vec2 x) {
    float q = 0.0f;
    for (uint32_t k = 0; k < step.sourceCount && k < kMaxRippleSources; ++k) {
        const RippleSource& s = step.sources[k];
        const vec2 d = x - s.position;
        const float r = std::max(s.radius, 0.01f);
        q += std::exp(-glm::dot(d, d) / (r * r)) * (s.strength + glm::dot(d, s.push) / r);
    }
    return q;
}

namespace {

// 吸收层：离最近的区域边缘 edge 格，ramp 从 0（内侧）到 1（最外圈），速度阻尼按 ramp² 增大
float spongeDamping(int i, int j, const RippleParams& p) {
    const int n = int(p.n);
    const float edge = float(std::min(std::min(i, j), std::min(n - 1 - i, n - 1 - j)));
    const float ramp = std::clamp(1.0f - edge / p.spongeCells, 0.0f, 1.0f);
    return p.damping + p.spongeStrength * ramp * ramp;
}

vec2 cellCenter(const RippleStepInput& step, const RippleParams& p, int i, int j) {
    return step.origin + (vec2(float(i), float(j)) + 0.5f) * p.cellSize;
}

} // namespace

// ================================================================ 波动方程

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
            const float gamma = spongeDamping(i, j, p);
            // 吸收层只加大「速度阻尼」γ。曾经再把高度乘 e^{-σΔt} 往 0 拉：蛙跳格式里这会凭空产生速度、
            // 把一部分能量困在吸收层里来回振（单元测试测到 4 秒后还剩 40%）；只阻尼速度时剩 0.2%
            const float next = u + (u - up) * (1.0f - gamma * dt) + c2 * dt * dt * lap;
            // 扰动源是体积流量：这一步直接加 Δt·q 的水。「上一步」也加同样的量 —— 这样下一步算速度 (u − u⁻) 时
            // 不会把这次加水当成速度：加的是水，不是冲量
            const float add = dt * rippleSourceRate(step, cellCenter(step, p, i, j));
            // 尾迹泡沫：坡度超过阈值的地方起泡，之后按时间常数消散（在世界坐标里原地消散，不随水流走）
            const vec2 grad = vec2(uR - uL, uU - uD) / (2.0f * p.cellSize);
            const float fresh = std::clamp((glm::length(grad) - p.foamSlope) * p.foamGain, 0.0f, 1.0f);
            out[size_t(j) * n + i] = vec4(next + add, u + add, std::max(cur.z * foamKeep, fresh), h);
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

// ================================================================ 浅水方程

void shallowStep(const RippleGrid& inState, const RippleGrid& inFlux, bool hasHistory, const std::vector<float>& bed, float waterLevel,
                 const RippleParams& p, const RippleStepInput& step, RippleGrid& outState, RippleGrid& outFlux) {
    const int n = int(p.n);
    outState.assign(size_t(n) * n, vec4(0.0f));
    outFlux.assign(size_t(n) * n, vec4(0.0f));
    const float dt = step.dt;
    const float l = p.cellSize;
    const float hCap = p.maxSpeed * p.maxSpeed / kGravity; // c² = g·min(d, h_cap) ≤ c_max²
    const float capScale = l * l / (4.0f * dt);            // 一条边一步最多流走上游格子 1/4 的水
    const float foamKeep = std::exp(-dt / p.foamDecay);
    auto inside = [&](int i, int j) { return i >= 0 && j >= 0 && i < n && j < n; };
    // 上一步的状态 / 流量，按「上一步的网格坐标」取；没有历史或超出上一步的网格 = 静水（δ = 0，流量 0）
    auto prevState = [&](int si, int sj) { return hasHistory && inside(si, sj) ? inState[size_t(sj) * n + si] : vec4(0.0f); };
    auto prevFlux = [&](int si, int sj) { return hasHistory && inside(si, sj) ? inFlux[size_t(sj) * n + si] : vec4(0.0f); };
    // 静水深：海面以下的床面上有水，高于海面的地方（岸）没有
    auto restDepth = [&](float b) { return std::max(waterLevel - b, 0.0f); };
    auto surfaceBase = [&](float b) { return std::max(b, waterLevel); }; // 水面高度 = 它 + δ

    // ---- 第一步（GPU：ocean_shallow_flux）：每个格子算自己右边、上边两条边的新流量 ----
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            const int si = i + step.shift.x, sj = j + step.shift.y;
            const float b = bed[size_t(j) * n + i];
            const float delta = prevState(si, sj).w;
            const float d = restDepth(b) + delta;
            const float h = surfaceBase(b) + delta;
            const float gamma = spongeDamping(i, j, p);
            const vec4 fOld = prevFlux(si, sj);
            auto edgeFlux = [&](int di, int dj, float old) {
                const int ni = i + di, nj = j + dj;
                if (!inside(ni, nj)) return 0.0f; // 区域边缘封闭：靠吸收层把波吃掉
                const float nb = bed[size_t(nj) * n + ni];
                const float nDelta = prevState(si + di, sj + dj).w;
                const float nd = restDepth(nb) + nDelta;
                const float nh = surfaceBase(nb) + nDelta;
                // 边上的有效水深取上游（水往哪边流，就看哪边的水）：干的一侧流不出水，水能爬上干的沙滩
                const float up = h >= nh ? d : nd;
                float f = old * (1.0f - gamma * dt) + dt * kGravity * std::min(up, hCap) * (h - nh);
                f = f > 0.0f ? std::min(f, d * capScale) : std::max(f, -nd * capScale);
                return f;
            };
            outFlux[size_t(j) * n + i] = vec4(edgeFlux(1, 0, fOld.x), edgeFlux(0, 1, fOld.y), 0.0f, 0.0f);
        }

    // ---- 第二步（GPU：ocean_shallow_depth）：流入减流出更新水量，再加扰动源 ----
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            const int si = i + step.shift.x, sj = j + step.shift.y;
            const float b = bed[size_t(j) * n + i];
            const float rest = restDepth(b);
            const vec4 prev = prevState(si, sj);
            const vec4 f = outFlux[size_t(j) * n + i];
            const float fxLeft = i > 0 ? outFlux[size_t(j) * n + i - 1].x : 0.0f;
            const float fzDown = j > 0 ? outFlux[size_t(j - 1) * n + i].y : 0.0f;
            // 直接更新 δ（而不是 d = 静水深 + δ）：保持毫米级变化的精度。水深不能为负：δ ≥ -静水深
            float delta = prev.w + dt * (fxLeft - f.x + fzDown - f.y) / (l * l) + dt * rippleSourceRate(step, cellCenter(step, p, i, j));
            delta = std::max(delta, -rest);
            // 渲染用的波纹高度：有水的格子 = 水面 - 海面；干的格子取 0（海面画在地形下面，被深度测试挡住）
            const float eta = rest + delta > kShallowWetDepth ? surfaceBase(b) - waterLevel + delta : 0.0f;
            // 尾迹泡沫：与波动方程相同，按上一步的波纹坡度（这一步邻居的新高度还没算出来）
            const vec2 grad = vec2(prevState(si + 1, sj).x - prevState(si - 1, sj).x, prevState(si, sj + 1).x - prevState(si, sj - 1).x) /
                              (2.0f * l);
            const float fresh = std::clamp((glm::length(grad) - p.foamSlope) * p.foamGain, 0.0f, 1.0f);
            outState[size_t(j) * n + i] = vec4(eta, prev.x, std::max(prev.z * foamKeep, fresh), delta);
        }
}

vec2 shallowVelocity(const RippleGrid& state, const RippleGrid& flux, const std::vector<float>& bed, float waterLevel,
                     const RippleParams& p, int i, int j) {
    const int n = int(p.n);
    const size_t c = size_t(j) * n + i;
    const float d = std::max(waterLevel - bed[c], 0.0f) + state[c].w;
    if (d <= kShallowWetDepth) return vec2(0.0f);
    // 格子两侧边上流量的平均 ÷ 过水断面（格子宽 × 水深）
    const float fx = 0.5f * (flux[c].x + (i > 0 ? flux[c - 1].x : 0.0f));
    const float fz = 0.5f * (flux[c].y + (j > 0 ? flux[c - n].y : 0.0f));
    return vec2(fx, fz) / (p.cellSize * d);
}

} // namespace eng::ocean
