#include "ocean/OceanSpectrum.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>

namespace eng::ocean {
namespace {

// 涌浪：来自远方风暴、已经「走」了很远的浪 —— 更长、更顺着一个方向。
// 这里用两个简单的映射近似：峰值频率降低（浪变长）、方向更集中。是艺术控制，不是严格的双峰频谱。
float swellPeakScale(const OceanSettings& s) { return 1.0f - 0.35f * std::clamp(s.swell, 0.0f, 1.0f); }
float effectiveSpread(const OceanSettings& s) { return std::max(s.spread, 0.0f) * (1.0f + 4.0f * std::clamp(s.swell, 0.0f, 1.0f)); }

// cos^2s(θ/2) 在 [-π, π] 上的积分的倒数（数值积分；s 不同积分值不同，没有简单的闭式）
float spreadNormalization(float spread) {
    constexpr int kSteps = 512;
    double sum = 0.0;
    for (int i = 0; i < kSteps; ++i) {
        const double theta = -kPi + (double(i) + 0.5) * (2.0 * kPi / kSteps);
        sum += std::pow(std::abs(std::cos(theta * 0.5)), 2.0 * spread);
    }
    return float(1.0 / (sum * (2.0 * kPi / kSteps)));
}

float wrapAngle(float a) {
    a = std::fmod(a + kPi, 2.0f * kPi);
    if (a < 0.0f) a += 2.0f * kPi;
    return a - kPi;
}

// 色散关系对 k 的导数 dω/dk（群速度）：把 S(ω) 换算成 S(k) 时需要的雅可比因子
float dispersionDerivative(float k, float depth) {
    const float kh = std::min(k * depth, 20.0f);
    const float t = std::tanh(kh);
    const float omega = std::sqrt(kGravity * k * t);
    return kGravity * (t + kh * (1.0f - t * t)) / (2.0f * std::max(omega, 1e-6f));
}

// 由 ω 反解 k（ω² = g k tanh(kh)）：深水初值 k = ω²/g，再做几次不动点迭代
float waveNumber(float omega, float depth) {
    float k = omega * omega / kGravity;
    for (int i = 0; i < 8; ++i) k = omega * omega / (kGravity * std::tanh(std::max(std::min(k * depth, 20.0f), 1e-4f)));
    return k;
}

} // namespace

float dispersion(float k, float depth) {
    return std::sqrt(kGravity * k * std::tanh(std::min(k * depth, 20.0f)));
}

float peakFrequency(const OceanSettings& s) {
    const float U = std::max(s.windSpeed, 0.1f);
    const float F = std::max(s.fetchKm * 1000.0f, 1.0f);
    // Hasselmann 等（1973）JONSWAP 观测拟合：ωp = 22 (g² / (U F))^(1/3)
    return 22.0f * std::cbrt(kGravity * kGravity / (U * F)) * swellPeakScale(s);
}

float frequencySpectrum(float omega, const OceanSettings& s) {
    if (omega <= 1e-4f) return 0.0f;
    const float U = std::max(s.windSpeed, 0.1f);
    const float F = std::max(s.fetchKm * 1000.0f, 1.0f);
    const float alpha = 0.076f * std::pow(U * U / (F * kGravity), 0.22f); // Phillips 常数随风区变化
    const float wp = peakFrequency(s);
    const float sigma = omega <= wp ? 0.07f : 0.09f;
    const float r = std::exp(-(omega - wp) * (omega - wp) / (2.0f * sigma * sigma * wp * wp));
    // JONSWAP：Pierson–Moskowitz 形状 α g²/ω⁵ · exp(-5/4 (ωp/ω)⁴)，峰值处再乘 γ^r 变尖
    float S = alpha * kGravity * kGravity / std::pow(omega, 5.0f) * std::exp(-1.25f * std::pow(wp / omega, 4.0f)) *
              std::pow(std::max(s.peakEnhancement, 1.0f), r);
    // TMA：有限水深的修正（Kitaigorodskii 近似），ωh = ω √(h/g)
    const float wh = omega * std::sqrt(std::max(s.depth, 0.1f) / kGravity);
    const float phi = wh <= 1.0f ? 0.5f * wh * wh : (wh < 2.0f ? 1.0f - 0.5f * (2.0f - wh) * (2.0f - wh) : 1.0f);
    return S * phi * s.heightScale * s.heightScale;
}

float directionalSpread(float theta, const OceanSettings& s) {
    const float sp = effectiveSpread(s);
    return spreadNormalization(sp) * std::pow(std::abs(std::cos(wrapAngle(theta) * 0.5f)), 2.0f * sp);
}

float significantWaveHeight(const OceanSettings& s) {
    // m0 = ∫S(ω)dω，按对数间隔积分（能量集中在峰值附近，低频高频都要覆盖）
    constexpr int kSteps = 4000;
    const double lo = std::log(0.01), hi = std::log(40.0);
    double m0 = 0.0;
    for (int i = 0; i < kSteps; ++i) {
        const double a = std::exp(lo + (hi - lo) * i / kSteps), b = std::exp(lo + (hi - lo) * (i + 1) / kSteps);
        m0 += 0.5 * (frequencySpectrum(float(a), s) + frequencySpectrum(float(b), s)) * (b - a);
    }
    return float(4.0 * std::sqrt(m0));
}

void cascadeRange(const OceanSettings& s, uint32_t cascade, float& kLow, float& kHigh) {
    // 每个级联的最低 6 个频率格太稀疏，交给更大的上一级联负责：分界 = 2π/L × 6
    auto boundary = [&](uint32_t c) { return 2.0f * kPi / std::max(s.cascadeSizes[int(c)], 0.01f) * 6.0f; };
    kLow = cascade == 0 ? 0.0f : boundary(cascade);
    kHigh = cascade + 1 < kOceanCascades ? boundary(cascade + 1) : std::numeric_limits<float>::infinity();
}

std::vector<vec4> generateCascadeSpectrum(const OceanSettings& s, uint32_t cascade, uint32_t n, uint32_t seed) {
    const float L = std::max(s.cascadeSizes[int(cascade)], 0.01f);
    const float dk = 2.0f * kPi / L;
    float kLow, kHigh;
    cascadeRange(s, cascade, kLow, kHigh);
    const float sp = effectiveSpread(s);
    const float norm = spreadNormalization(sp);
    const float windAngle = glm::radians(s.windDirectionDeg);

    std::mt19937 rng(seed * 2654435761u + cascade * 1000003u);
    std::normal_distribution<float> gauss(0.0f, 1.0f);
    std::vector<vec2> h0(size_t(n) * n, vec2(0.0f));
    for (uint32_t m = 0; m < n; ++m)
        for (uint32_t i = 0; i < n; ++i) {
            // 随机数每格都取：这样改截断范围不会打乱其余格子的相位（调参时画面连续）
            const vec2 xi(gauss(rng), gauss(rng));
            const vec2 kv = vec2(float(int(i) - int(n / 2)), float(int(m) - int(n / 2))) * dk;
            const float k = glm::length(kv);
            if (k <= 1e-6f || k < kLow || k >= kHigh) continue;
            const float omega = dispersion(k, s.depth);
            const float theta = std::atan2(kv.y, kv.x) - windAngle;
            const float D = norm * std::pow(std::abs(std::cos(wrapAngle(theta) * 0.5f)), 2.0f * sp);
            // 二维波数谱 S(k⃗) = S(ω) D(θ) (dω/dk) / k   （dk⃗ = k dk dθ）
            const float Sk = frequencySpectrum(omega, s) * D * dispersionDerivative(k, s.depth) / k;
            // E|h0|² = S Δk² / 2：ĥ(k,t) = h0(k)e^{iωt} + conj(h0(-k))e^{-iωt} 的方差正好是 S Δk²
            const float amp = std::sqrt(std::max(Sk, 0.0f) * dk * dk * 0.5f);
            h0[size_t(m) * n + i] = xi * (amp * 0.70710678f);
        }

    std::vector<vec4> out(size_t(n) * n);
    for (uint32_t m = 0; m < n; ++m)
        for (uint32_t i = 0; i < n; ++i) {
            const vec2 a = h0[size_t(m) * n + i];
            const vec2 b = h0[size_t((n - m) % n) * n + (n - i) % n]; // -k 所在的格子
            out[size_t(m) * n + i] = vec4(a.x, a.y, b.x, -b.y);    // 第二个存共轭
        }
    return out;
}

double cascadeEnergy(const std::vector<vec4>& h0) {
    double e = 0.0;
    for (const vec4& v : h0) e += double(v.x) * v.x + double(v.y) * v.y + double(v.z) * v.z + double(v.w) * v.w;
    return e;
}

std::vector<GerstnerWave> generateGerstnerWaves(const OceanSettings& s, uint32_t count, uint32_t seed) {
    count = std::clamp(count, 1u, kMaxGerstnerWaves);
    std::vector<GerstnerWave> waves;
    waves.reserve(count);
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> uni(0.0f, 1.0f);
    const float wp = peakFrequency(s);
    const float sp = effectiveSpread(s);
    const float windAngle = glm::radians(s.windDirectionDeg);
    // 频率按对数均匀分成 count 段：[0.7 ωp, 8 ωp]，每段一个代表波，振幅 = √(2 S(ω) Δω)
    const float lo = std::log(0.7f * wp), hi = std::log(8.0f * wp);
    const float choppy = std::clamp(s.choppiness, 0.0f, 1.5f) * 0.7f; // 留余量：Σ Q k A > 1 时波形自相交成环
    for (uint32_t i = 0; i < count; ++i) {
        const float w0 = std::exp(lo + (hi - lo) * float(i) / float(count));
        const float w1 = std::exp(lo + (hi - lo) * float(i + 1) / float(count));
        GerstnerWave w;
        w.omega = std::exp(lo + (hi - lo) * (float(i) + uni(rng)) / float(count));
        w.k = waveNumber(w.omega, s.depth);
        w.amplitude = std::sqrt(2.0f * frequencySpectrum(w.omega, s) * (w1 - w0));
        // 方向按 D(θ) 抽样（拒绝采样：D 的最大值在 θ = 0）
        float theta = 0.0f;
        for (int tries = 0; tries < 64; ++tries) {
            theta = (uni(rng) * 2.0f - 1.0f) * kPi;
            if (uni(rng) < std::pow(std::abs(std::cos(theta * 0.5f)), 2.0f * sp)) break;
        }
        const float a = windAngle + theta;
        w.direction = vec2(std::cos(a), std::sin(a));
        w.phase = uni(rng) * 2.0f * kPi;
        w.steepness = w.k * w.amplitude > 1e-6f ? choppy / (w.k * w.amplitude * float(count)) : 0.0f;
        waves.push_back(w);
    }
    return waves;
}

vec3 gerstnerDisplacement(const std::vector<GerstnerWave>& waves, float x, float z, float t) {
    // 与 FFT 版同一约定：y = a cos θ，水平 = -Q a d sin θ（水面点向浪峰聚拢 → 浪峰尖、浪谷平）
    vec3 d(0.0f);
    for (const GerstnerWave& w : waves) {
        const float theta = w.k * (w.direction.x * x + w.direction.y * z) - w.omega * t + w.phase;
        const float s = std::sin(theta), c = std::cos(theta);
        d.x -= w.steepness * w.amplitude * w.direction.x * s;
        d.y += w.amplitude * c;
        d.z -= w.steepness * w.amplitude * w.direction.y * s;
    }
    return d;
}

float gerstnerHeightAt(const std::vector<GerstnerWave>& waves, float x, float z, float t, int iterations) {
    // 找 x0 使 x0 + D_h(x0) = (x, z)：不动点迭代 x0 ← (x, z) - D_h(x0)。水平位移远小于波长时收敛很快
    vec2 p0(x, z);
    for (int i = 0; i < iterations; ++i) {
        const vec3 d = gerstnerDisplacement(waves, p0.x, p0.y, t);
        p0 = vec2(x, z) - vec2(d.x, d.z);
    }
    return gerstnerDisplacement(waves, p0.x, p0.y, t).y;
}

} // namespace eng::ocean
