// 海浪频谱与 Gerstner 参考实现（纯 CPU）：检查物理关系和归一化，而不只是「不崩」。
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
