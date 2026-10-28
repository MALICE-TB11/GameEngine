// 海浪频谱与 Gerstner 参考实现（纯 CPU）：检查物理关系和归一化，而不只是「不崩」。
#include "ocean/OceanRipples.h"
#include "ocean/OceanSpectrum.h"

#include <doctest/doctest.h>

#include <cmath>

using namespace eng;
using namespace eng::ocean;

TEST_CASE("海洋：色散关系在深水和浅水两端退化成已知公式") {
    // 深水（kh ≫ 1）：ω = √(gk)
    CHECK(dispersion(0.5f, 1000.0f) == doctest::Approx(std::sqrt(kGravity * 0.5f)).epsilon(1e-4));
    // 浅水（kh ≪ 1）：ω ≈ k√(gh)，波速与波长无关
    CHECK(dispersion(0.01f, 2.0f) == doctest::Approx(0.01f * std::sqrt(kGravity * 2.0f)).epsilon(0.01));
}

TEST_CASE("海洋：风越大、风区越长，有效波高越高；波高倍数线性缩放") {
    OceanSettings s;
    s.depth = 1000.0f; // 排除浅水修正
    const float base = significantWaveHeight(s);
    OceanSettings windy = s;
    windy.windSpeed = 18.0f;
    OceanSettings longFetch = s;
    longFetch.fetchKm = 600.0f;
    CHECK(significantWaveHeight(windy) > base * 1.5f);
    CHECK(significantWaveHeight(longFetch) > base);
    OceanSettings doubled = s;
    doubled.heightScale = 2.0f;
    CHECK(significantWaveHeight(doubled) == doctest::Approx(base * 2.0f).epsilon(1e-3));
    // 量级检查：9 m/s、150 km 风区的风浪有效波高约在 1~2 米（Beaufort 5 级附近）
    CHECK(base > 0.8f);
    CHECK(base < 2.5f);
}

TEST_CASE("海洋：方向分布在 [-π, π] 上积分为 1") {
    for (float spread : {1.0f, 6.0f, 20.0f}) {
        OceanSettings s;
        s.spread = spread;
        double sum = 0.0;
        const int steps = 2000;
        for (int i = 0; i < steps; ++i) sum += directionalSpread(-kPi + (i + 0.5f) * 2.0f * kPi / steps, s) * (2.0 * kPi / steps);
        CHECK(sum == doctest::Approx(1.0).epsilon(0.01));
    }
}

TEST_CASE("海洋：级联的波数区间首尾相接、互不重叠") {
    OceanSettings s;
    float lo0, hi0, lo1, hi1, lo2, hi2;
    cascadeRange(s, 0, lo0, hi0);
    cascadeRange(s, 1, lo1, hi1);
    cascadeRange(s, 2, lo2, hi2);
    CHECK(lo0 == 0.0f);
    CHECK(hi0 == lo1);
    CHECK(hi1 == lo2);
    CHECK(std::isinf(hi2));
    CHECK(lo1 < hi1);
}

TEST_CASE("海洋：三个级联的总能量 ≈ 频谱积分 m0 = (Hs/4)²（归一化正确、没有重复计算）") {
    OceanSettings s;
    const double m0 = std::pow(significantWaveHeight(s) / 4.0, 2.0);
    // 随机振幅有统计涨落（峰值附近只有几十个格子），多取几个随机种子求平均
    double sum = 0.0;
    const int seeds = 8;
    for (int seed = 1; seed <= seeds; ++seed)
        for (uint32_t c = 0; c < kOceanCascades; ++c) sum += cascadeEnergy(generateCascadeSpectrum(s, c, 64, uint32_t(seed)));
    CHECK(sum / seeds == doctest::Approx(m0).epsilon(0.3));
}

TEST_CASE("海洋：h0 满足共轭对称，逆变换结果是实数") {
    OceanSettings s;
    const uint32_t n = 32;
    const auto h0 = generateCascadeSpectrum(s, 0, n, 7);
    for (uint32_t m = 1; m < n; ++m)
        for (uint32_t i = 1; i < n; ++i) {
            const vec4 a = h0[m * n + i];
            const vec4 b = h0[(n - m) * n + (n - i)];
            // 第 (i,m) 格存的 conj(h0(-k)) 就是 -k 那一格的 h0(k) 取共轭
            CHECK(a.z == doctest::Approx(b.x));
            CHECK(a.w == doctest::Approx(-b.y));
        }
}

TEST_CASE("海洋：Gerstner 高度查询能反解水平位移") {
    OceanSettings s;
    s.choppiness = 1.0f;
    const auto waves = generateGerstnerWaves(s, 32, 3);
    REQUIRE(waves.size() == 32);
    for (int i = 0; i < 20; ++i) {
        const float x0 = float(i) * 3.7f - 30.0f, z0 = float(i % 7) * 5.1f - 15.0f, t = 1.5f * float(i);
        const vec3 d = gerstnerDisplacement(waves, x0, z0, t);
        // 水面点 x0 被移到了 x0 + d.xz；在那里查询高度，应得到 d.y
        CHECK(gerstnerHeightAt(waves, x0 + d.x, z0 + d.z, t, 8) == doctest::Approx(d.y).epsilon(0.02));
    }
}

TEST_CASE("海洋：Gerstner 波的能量与频谱一致、陡度不会自相交") {
    OceanSettings s;
    const auto waves = generateGerstnerWaves(s, 48, 11);
    double energy = 0.0, steep = 0.0;
    for (const GerstnerWave& w : waves) {
        energy += 0.5 * double(w.amplitude) * w.amplitude; // 正弦波方差 = a²/2
        steep += double(w.steepness) * w.k * w.amplitude;
    }
    const double hs = 4.0 * std::sqrt(energy);
    // 只取了峰值附近 0.7~8 ωp 的频段，允许比完整频谱略低
    CHECK(hs == doctest::Approx(significantWaveHeight(s)).epsilon(0.35));
    CHECK(steep < 1.0); // Σ Q k A < 1：水面不会翻卷成环
}

TEST_CASE("海洋：由 ω 反解波数在浅水也收敛（旧的不动点迭代在这里来回振荡）") {
    // 审查给出的例子：水深 2 m、ω = 0.5 rad/s，正确解 k ≈ 0.11385
    CHECK(waveNumber(0.5f, 2.0f) == doctest::Approx(0.11385).epsilon(1e-3));
    // 全范围：代回色散关系误差 < 0.01%
    for (float depth : {0.3f, 1.0f, 2.0f, 10.0f, 40.0f, 500.0f, 4000.0f})
        for (float omega : {0.05f, 0.2f, 0.5f, 1.0f, 3.0f, 10.0f, 30.0f}) {
            CAPTURE(depth);
            CAPTURE(omega);
            const float k = waveNumber(omega, depth);
            CHECK(std::isfinite(k));
            CHECK(dispersion(k, depth) == doctest::Approx(omega).epsilon(1e-4));
        }
    CHECK(waveNumber(0.0f, 10.0f) == 0.0f);
}

TEST_CASE("海洋：频率量化后整片海按周期 T 精确重复，相位时间不会无限增长") {
    const double w0 = 2.0 * kPi / kOceanRepeatPeriod;
    for (float omega : {0.37f, 1.0f, 12.345f}) {
        const float q = quantizeFrequency(omega);
        CHECK(std::abs(double(q) - omega) <= w0 * 0.5 + 1e-6);
        CHECK(std::abs(q / w0 - std::round(q / w0)) < 1e-3); // 是 2π/T 的整数倍
    }
    CHECK(oceanPhaseTime(12.5) == doctest::Approx(12.5));
    CHECK(oceanPhaseTime(kOceanRepeatPeriod * 7 + 3.25) == doctest::Approx(3.25).epsilon(1e-5));
    CHECK(oceanPhaseTime(-1.0) == doctest::Approx(kOceanRepeatPeriod - 1.0));
    // 真实频谱：t = 0 与 t = T 的海面相同
    OceanSettings s;
    const uint32_t n = 16;
    const std::vector<vec4> h0 = generateCascadeSpectrum(s, 0, n, 7);
    const std::vector<float> omega = cascadeFrequencies(s, 0, n);
    const float L = s.cascadeSizes[0];
    const CascadeSample a = evaluateCascade(h0, omega, n, L, 1.0f, vec2(13.0f, -4.0f), 0.0f);
    const CascadeSample b = evaluateCascade(h0, omega, n, L, 1.0f, vec2(13.0f, -4.0f), float(kOceanRepeatPeriod));
    for (int c = 0; c < 4; ++c) {
        CHECK(b.displacement[c] == doctest::Approx(a.displacement[c]).epsilon(1e-3));
        CHECK(b.derivatives[c] == doctest::Approx(a.derivatives[c]).epsilon(1e-3));
    }
}

TEST_CASE("海洋：FFT 约定与 Gerstner 一致 —— 单个波的高度、水平位移、斜率、雅可比逐点相同，且沿 +k 传播") {
    // 在频谱里只放一个波 h0(k) = a（k 沿 +x）。按 evolve 的约定 ĥ(k,t) = h0 e^{-iωt} + conj(h0(-k)) e^{iωt}，
    // 逆变换后应正好是 2a·cos(kx - ωt) —— 与振幅 2a 的 Gerstner 波相同。
    const uint32_t n = 8;
    const float L = 16.0f, a = 0.3f, lambda = 0.8f;
    const float k = 2.0f * kPi / L; // 下标 n/2 + 1
    const float w = 1.7f;
    std::vector<vec4> h0(n * n, vec4(0.0f));
    std::vector<float> omega(n * n, w);
    const uint32_t row = n / 2;
    h0[row * n + n / 2 + 1] = vec4(a, 0.0f, 0.0f, 0.0f); // (h0(k), conj(h0(-k)) = 0)
    h0[row * n + n / 2 - 1] = vec4(0.0f, 0.0f, a, 0.0f); // -k 处：(h0(-k) = 0, conj(h0(k)) = a)

    GerstnerWave g;
    g.direction = vec2(1.0f, 0.0f);
    g.k = k;
    g.omega = w;
    g.amplitude = 2.0f * a;
    g.phase = 0.0f;
    g.steepness = lambda;
    const std::vector<GerstnerWave> waves = {g};

    for (float t : {0.0f, 0.4f, 1.3f})
        for (float x : {0.0f, 1.0f, 2.5f, 7.0f}) {
            CAPTURE(t);
            CAPTURE(x);
            const CascadeSample f = evaluateCascade(h0, omega, n, L, lambda, vec2(x, 3.0f), t);
            const vec3 gd = gerstnerDisplacement(waves, x, 3.0f, t);
            const float theta = k * x - w * t;
            CHECK(f.displacement.y == doctest::Approx(gd.y).epsilon(1e-4).scale(1.0));
            CHECK(f.displacement.x == doctest::Approx(gd.x).epsilon(1e-4).scale(1.0)); // 水平位移指向浪峰
            CHECK(f.displacement.z == doctest::Approx(0.0f).scale(1.0));
            CHECK(f.derivatives.x == doctest::Approx(-2.0f * a * k * std::sin(theta)).epsilon(1e-4).scale(1.0)); // ∂y/∂x
            CHECK(f.displacement.w == doctest::Approx(1.0f - 2.0f * lambda * a * k * std::cos(theta)).epsilon(1e-4)); // 浪峰处 J < 1
        }
    // 传播方向：t 时刻的浪峰在 x = ωt/k（向 +x 移动）
    const float t = 0.5f;
    CHECK(evaluateCascade(h0, omega, n, L, lambda, vec2(w * t / k, 0.0f), t).displacement.y == doctest::Approx(2.0f * a).epsilon(1e-4));
}

TEST_CASE("海洋：Nyquist 行/列为零 —— 否则导数场不是实数，打包进一次 FFT 时会串到别的场（GPU 校验发现的问题）") {
    OceanSettings s;
    const uint32_t n = 32;
    for (uint32_t c = 0; c < kOceanCascades; ++c) {
        const auto h0 = generateCascadeSpectrum(s, c, n, 3);
        for (uint32_t j = 0; j < n; ++j) {
            CHECK(h0[j] == vec4(0.0f));     // 第 0 行（kz = -N/2·Δk）
            CHECK(h0[j * n] == vec4(0.0f)); // 第 0 列（kx = -N/2·Δk）
        }
    }
}

// ---------------------------------------------------------------- 局部交互（波纹）

namespace {

// 测试用的小网格：n×n，格子 0.25 m，水深处处为 depth
struct RippleFixture {
    RippleParams p;
    std::vector<float> depth;
    RippleGrid grid;
    RippleStepInput step;
    explicit RippleFixture(uint32_t n = 96, float h = 10.0f, float speed = 3.0f) {
        p.n = n;
        p.cellSize = 0.25f;
        p.maxSpeed = speed;
        p.damping = 0.0f;
        p.spongeStrength = 0.0f;
        p.spongeCells = 8.0f;
        depth.assign(size_t(n) * n, h);
        grid.assign(size_t(n) * n, vec4(0.0f));
        step.dt = 1.0f / 60.0f;
    }
    // 静止的高斯隆起（u = u⁻：初速度为零）
    void bump(float ci, float cj, float amplitude, float radiusCells) {
        for (uint32_t j = 0; j < p.n; ++j)
            for (uint32_t i = 0; i < p.n; ++i) {
                const float d2 = (float(i) - ci) * (float(i) - ci) + (float(j) - cj) * (float(j) - cj);
                const float u = amplitude * std::exp(-d2 / (radiusCells * radiusCells));
                grid[j * p.n + i] = vec4(u, u, 0.0f, depth[j * p.n + i]);
            }
    }
    void run(int steps) {
        RippleGrid next;
        for (int k = 0; k < steps; ++k) {
            rippleStep(grid, true, depth, p, step, next);
            grid.swap(next);
        }
    }
    float at(uint32_t i, uint32_t j) const { return grid[j * p.n + i].x; }
};

} // namespace

TEST_CASE("波纹：稳定性条件会截断过大的波速") {
    OceanSettings s;
    s.rippleSize = 64.0f;
    s.rippleResolution = 256;
    s.rippleMaxSpeed = 100.0f;
    const RippleParams p = makeRippleParams(s, 1.0f / 60.0f);
    CHECK(p.cellSize == doctest::Approx(0.25f));
    CHECK(p.maxSpeed == doctest::Approx(rippleSpeedLimit(0.25f, 1.0f / 60.0f)));
    CHECK(p.maxSpeed * (1.0f / 60.0f) / p.cellSize <= kRippleCfl + 1e-5f);
}

TEST_CASE("波纹：没有阻尼时能量守恒，波以 c 的速度向外传播") {
    RippleFixture f;
    const float c = f.p.maxSpeed; // 水深 10 m：√(g·h) ≈ 9.9 > 3，按上限
    f.bump(48.0f, 48.0f, 0.1f, 3.0f);
    f.run(1);
    const double e0 = rippleEnergy(f.grid, f.depth, f.p, f.step.dt);
    f.run(59); // 共 1 秒：波前走了 3 m = 12 格，还没碰到边界
    const double e1 = rippleEnergy(f.grid, f.depth, f.p, f.step.dt);
    CHECK(e1 == doctest::Approx(e0).epsilon(0.05));
    // 沿 +X 找最高的波峰：应在 c·t = 3 m 附近（脉冲本身有宽度，允许 ±0.5 m）
    uint32_t peak = 48;
    for (uint32_t i = 49; i < 96; ++i)
        if (f.at(i, 48) > f.at(peak, 48)) peak = i;
    CHECK(float(peak - 48) * f.p.cellSize == doctest::Approx(c * 1.0f).epsilon(0.17));
}

TEST_CASE("波纹：浅水里波速按 √(g·h) 变慢") {
    RippleFixture f(96, 0.4f, 4.0f); // √(9.81 × 0.4) ≈ 1.98 m/s < 上限 4
    f.bump(48.0f, 48.0f, 0.05f, 3.0f);
    f.run(60);
    uint32_t peak = 48;
    for (uint32_t i = 49; i < 96; ++i)
        if (f.at(i, 48) > f.at(peak, 48)) peak = i;
    CHECK(float(peak - 48) * f.p.cellSize == doctest::Approx(std::sqrt(kGravity * 0.4f)).epsilon(0.25));
}

TEST_CASE("波纹：撞到岸会反射 —— 岸上没有水，能量留在水里") {
    RippleFixture f;
    for (uint32_t j = 0; j < f.p.n; ++j)
        for (uint32_t i = 60; i < f.p.n; ++i) f.depth[j * f.p.n + i] = -1.0f; // x ≥ 60 格是陆地
    f.bump(48.0f, 48.0f, 0.1f, 3.0f);
    f.run(1);
    const double e0 = rippleEnergy(f.grid, f.depth, f.p, f.step.dt);
    f.run(80); // 波前 4 m = 16 格：已经撞到 12 格外的岸并反射
    for (uint32_t j = 0; j < f.p.n; ++j)
        for (uint32_t i = 60; i < f.p.n; ++i) REQUIRE(f.grid[j * f.p.n + i].x == 0.0f);
    CHECK(rippleEnergy(f.grid, f.depth, f.p, f.step.dt) == doctest::Approx(e0).epsilon(0.06));
}

TEST_CASE("波纹：边缘吸收层把波吃掉，不会反弹回来") {
    RippleFixture f(64);
    f.p.spongeCells = 16.0f;
    f.p.spongeStrength = 12.0f;
    f.bump(32.0f, 32.0f, 0.1f, 3.0f);
    f.run(1);
    const double e0 = rippleEnergy(f.grid, f.depth, f.p, f.step.dt);
    f.run(240); // 4 秒：波早就走到边缘了
    CHECK(rippleEnergy(f.grid, f.depth, f.p, f.step.dt) < e0 * 0.05);
}

TEST_CASE("波纹：区域跟着玩家移动时，读上一步状态按格子偏移（图案在世界里不动）") {
    RippleFixture a(64), b(64);
    a.bump(30.0f, 32.0f, 0.1f, 3.0f);
    b.bump(30.0f, 32.0f, 0.1f, 3.0f);
    a.run(10);
    b.run(10);
    // b 的区域原点向 +X 移动 3 格：b 的格子 i 对应 a 的格子 i + 3
    RippleGrid shifted, plain;
    b.step.shift = ivec2(3, 0);
    rippleStep(b.grid, true, b.depth, b.p, b.step, shifted);
    rippleStep(a.grid, true, a.depth, a.p, a.step, plain);
    for (uint32_t j = 4; j < 60; ++j)
        for (uint32_t i = 4; i < 56; ++i) REQUIRE(shifted[j * 64 + i].x == plain[j * 64 + i + 3].x);
}

TEST_CASE("波纹：扰动源（体积流量）把水面往下压，泡沫只出现在陡的地方") {
    RippleFixture f(64);
    f.step.sourceCount = 1;
    f.step.origin = vec2(0.0f);
    f.step.sources[0] = {vec2(8.0f, 8.0f), 0.5f, -2.0f}; // 区域中心（32 格 × 0.25 m），每秒抽走 2 m 的水
    f.p.foamSlope = 0.05f;
    f.p.foamGain = 10.0f;
    f.run(5);
    CHECK(f.at(31, 31) < -1e-3f);           // 中心被压下去
    CHECK(f.grid[31 * 64 + 31].w == 10.0f); // 第 4 个分量记录静水深
    CHECK(f.grid[2 * 64 + 2].z == 0.0f);    // 远处平静：没有泡沫
    float maxFoam = 0.0f;
    for (const vec4& v : f.grid) maxFoam = std::max(maxFoam, v.z);
    CHECK(maxFoam > 0.0f);
}

TEST_CASE("波纹：偶极子源（走动）前面加水、后面减水；水花扩散成一圈，中心不会越陷越深") {
    SUBCASE("偶极子") {
        RippleFixture f(64);
        f.step.sourceCount = 1;
        f.step.sources[0] = {vec2(8.0f, 8.0f), 0.6f, 0.0f, vec2(4.0f, 0.0f)}; // 向 +X 走
        f.run(3);
        CHECK(f.at(34, 32) > 0.0f); // 前方（+X）隆起
        CHECK(f.at(29, 32) < 0.0f); // 后方凹陷
    }
    SUBCASE("水花") {
        // 一步抽走 0.3 m（q = -深度/Δt）：留下一个静止的凹坑，之后像石子入水一样向外扩散
        const float depthSplash = 0.3f;
        RippleFixture f(96);
        f.step.sourceCount = 1;
        f.step.sources[0] = {vec2(12.0f, 12.0f), 1.0f, -depthSplash / f.step.dt};
        f.run(1);
        CHECK(f.at(47, 47) == doctest::Approx(-depthSplash).epsilon(0.05)); // 第一步正好挖下去约 0.3 m
        f.step.sourceCount = 0;
        f.run(60);
        CHECK(std::abs(f.at(47, 47)) < 0.1f * depthSplash); // 凹坑扩散成波纹走了
    }
}

// ---------------------------------------------------------------- 浅水方程

namespace {

// 浅水方程的小网格：床面高度 bed（海面在 y = 0），状态 (η, η⁻, 泡沫, δ)，流量 (Fx, Fz, 0, 0)
struct ShallowFixture {
    RippleParams p;
    std::vector<float> bed;
    RippleGrid state, flux;
    RippleStepInput step;
    explicit ShallowFixture(uint32_t n = 96, float depth = 10.0f, float speed = 3.0f) {
        p.solver = RippleSolver::Shallow;
        p.n = n;
        p.cellSize = 0.25f;
        p.maxSpeed = speed;
        p.damping = 0.0f;
        p.spongeStrength = 0.0f;
        p.spongeCells = 8.0f;
        bed.assign(size_t(n) * n, -depth);
        state.assign(size_t(n) * n, vec4(0.0f));
        flux.assign(size_t(n) * n, vec4(0.0f));
        step.dt = 1.0f / 60.0f;
    }
    void bump(float ci, float cj, float amplitude, float radiusCells) {
        for (uint32_t j = 0; j < p.n; ++j)
            for (uint32_t i = 0; i < p.n; ++i) {
                const float d2 = (float(i) - ci) * (float(i) - ci) + (float(j) - cj) * (float(j) - cj);
                const float u = amplitude * std::exp(-d2 / (radiusCells * radiusCells));
                if (bed[j * p.n + i] < 0.0f) state[j * p.n + i] = vec4(u, u, 0.0f, u);
            }
    }
    void run(int steps) {
        RippleGrid ns, nf;
        for (int k = 0; k < steps; ++k) {
            shallowStep(state, flux, true, bed, 0.0f, p, step, ns, nf);
            state.swap(ns);
            flux.swap(nf);
        }
    }
    double volume() const {
        double v = 0.0;
        for (const vec4& s : state) v += s.w;
        return v * p.cellSize * p.cellSize;
    }
    float depthAt(uint32_t i, uint32_t j) const { return std::max(-bed[j * p.n + i], 0.0f) + state[j * p.n + i].w; }
};

} // namespace

TEST_CASE("浅水方程：线性、水深超过截断深度时与波动方程逐格相同（同一套离散）") {
    RippleFixture wave(64);
    ShallowFixture shallow(64);
    wave.bump(32.0f, 32.0f, 0.02f, 3.0f);
    shallow.bump(32.0f, 32.0f, 0.02f, 3.0f);
    wave.run(40);
    shallow.run(40);
    float maxU = 0.0f, maxDiff = 0.0f;
    for (size_t c = 0; c < wave.grid.size(); ++c) {
        maxU = std::max(maxU, std::abs(wave.grid[c].x));
        maxDiff = std::max(maxDiff, std::abs(wave.grid[c].x - shallow.state[c].x));
    }
    CHECK(maxDiff < maxU * 1e-3f);
}

TEST_CASE("浅水方程：水量守恒（区域边缘封闭，吸收层只阻尼流量）") {
    ShallowFixture f(64);
    f.p.spongeStrength = 12.0f;
    f.p.spongeCells = 16.0f;
    f.bump(32.0f, 32.0f, 0.1f, 3.0f);
    const double v0 = f.volume();
    f.run(240);
    CHECK(f.volume() == doctest::Approx(v0).epsilon(1e-3));
}

TEST_CASE("浅水方程：浪能冲上干沙滩，水深永远不为负") {
    // 床面沿 +X 从 -2 m 升到 +1 m；在水里放一道 0.4 m 高的长浪
    ShallowFixture f(128, 2.0f, 4.0f);
    for (uint32_t j = 0; j < f.p.n; ++j)
        for (uint32_t i = 0; i < f.p.n; ++i) f.bed[j * f.p.n + i] = -2.0f + 3.0f * float(i) / float(f.p.n - 1);
    for (uint32_t j = 0; j < f.p.n; ++j)
        for (uint32_t i = 0; i < f.p.n; ++i)
            if (f.bed[j * f.p.n + i] < 0.0f) {
                const float di = float(i) - 40.0f;
                f.state[j * f.p.n + i].w = 0.4f * std::exp(-di * di / 30.0f);
            }
    auto shoreline = [&] {
        uint32_t last = 0;
        for (uint32_t i = 0; i < f.p.n; ++i)
            if (f.depthAt(i, 64) > kShallowWetDepth) last = i;
        return last;
    };
    const uint32_t rest = shoreline();
    uint32_t furthest = rest;
    float minDepth = 1e9f;
    for (int k = 0; k < 900; ++k) {
        f.run(1);
        furthest = std::max(furthest, shoreline());
        for (uint32_t i = 0; i < f.p.n; ++i) minDepth = std::min(minDepth, f.depthAt(i, 64));
    }
    CHECK(float(furthest - rest) * f.p.cellSize > 2.0f); // 冲上去两米以上
    CHECK(minDepth >= 0.0f);
}

TEST_CASE("浅水方程：流速指向水往低处流的方向") {
    ShallowFixture f(64, 1.0f, 3.0f);
    for (uint32_t j = 0; j < 64; ++j)
        for (uint32_t i = 0; i < 32; ++i) f.state[j * 64 + i].w = 0.1f; // 左半边水多 10 cm
    f.run(5);
    const vec2 v = shallowVelocity(f.state, f.flux, f.bed, 0.0f, f.p, 32, 32);
    CHECK(v.x > 0.05f);
    CHECK(std::abs(v.y) < 1e-4f);
}

TEST_CASE("浅水方程：水花一步抽走的水量 = 深度 × 高斯面积 πr²") {
    ShallowFixture f(64);
    const float depthSplash = 0.3f, r = 1.0f;
    f.step.sourceCount = 1;
    f.step.sources[0] = {vec2(8.0f, 8.0f), r, -depthSplash / f.step.dt};
    f.run(1);
    CHECK(f.volume() == doctest::Approx(-depthSplash * kPi * r * r).epsilon(0.02));
}

TEST_CASE("浅水方程：区域移动时读上一步状态按格子偏移") {
    ShallowFixture a(64), b(64);
    a.bump(30.0f, 32.0f, 0.1f, 3.0f);
    b.bump(30.0f, 32.0f, 0.1f, 3.0f);
    a.run(10);
    b.run(10);
    RippleGrid sa, fa, sb, fb;
    b.step.shift = ivec2(3, 0);
    shallowStep(b.state, b.flux, true, b.bed, 0.0f, b.p, b.step, sb, fb);
    shallowStep(a.state, a.flux, true, a.bed, 0.0f, a.p, a.step, sa, fa);
    for (uint32_t j = 4; j < 60; ++j)
        for (uint32_t i = 4; i < 56; ++i) REQUIRE(sb[j * 64 + i].w == sa[j * 64 + i + 3].w);
}
