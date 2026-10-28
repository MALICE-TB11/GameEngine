#pragma once
// 海浪频谱（纯 CPU，可单元测试）。
//
// 线性波浪理论把海面看成无数个正弦波的叠加，每个波满足色散关系 ω² = g·k·tanh(k·h)
// （深水时 ω = √(gk)：波长越长跑得越快）。「海面长什么样」由每个波的能量分布 —— 频谱 —— 决定：
//   JONSWAP 频谱 S(ω)：海洋观测拟合出的风浪能量随频率的分布，由风速和风区长度决定
//   TMA 修正：有限水深下长波能量被压低
//   方向分布 D(θ)：能量集中在风向附近，cos^2s(θ/2)
// 两种实现都从这同一份频谱出发：
//   FFT（优化）：在 256² 的波数网格上给每个波随机振幅和相位（h0），GPU 每帧按色散关系推进相位再逆 FFT
//   Gerstner（参考）：从频谱里按能量挑几十个代表性的波，直接求和 —— 简单、可在 CPU 精确查询高度

#include "ocean/OceanSettings.h"

#include <vector>

namespace eng::ocean {

inline constexpr float kGravity = 9.81f;

// 色散关系：ω(k) = √(g k tanh(k h))
float dispersion(float k, float depth);
// 单位频率区间的能量 S(ω)（JONSWAP × TMA × 波高倍数²），单位 m²·s
float frequencySpectrum(float omega, const OceanSettings& s);
// 方向分布 D(θ)，θ 相对风向；在 [-π, π] 上积分为 1
float directionalSpread(float theta, const OceanSettings& s);
// 峰值角频率 ωp
float peakFrequency(const OceanSettings& s);
// 有效波高 Hs = 4·√(∫S(ω)dω)：最高三分之一波的平均波高，航海上说的「浪高」
float significantWaveHeight(const OceanSettings& s);

// 某个级联负责的波数区间 [kLow, kHigh)：相邻级联互不重叠，避免同一个波被算两次
void cascadeRange(const OceanSettings& s, uint32_t cascade, float& kLow, float& kHigh);

// 生成一个级联的初始频谱 h0：N×N 个 vec4 = (h0(k).re, h0(k).im, conj(h0(-k)).re, conj(h0(-k)).im)。
// 下标 (n, m) 对应波数 k = 2π/L · (n - N/2, m - N/2)。seed 相同则结果相同（固定随机相位）。
std::vector<vec4> generateCascadeSpectrum(const OceanSettings& s, uint32_t cascade, uint32_t n, uint32_t seed);
// 级联频谱中所有波的能量之和 Σ|h0|²·2 ≈ 该波数区间的 ∫S（测试用：检查归一化）
double cascadeEnergy(const std::vector<vec4>& h0);

// ---- Gerstner 参考实现 ----
struct GerstnerWave {
    vec2 direction;  // 传播方向（单位向量）
    float k;         // 波数 2π/λ
    float omega;     // 角频率
    float amplitude; // 振幅（米）
    float phase;     // 初相位
    float steepness; // Q：水平位移系数（0 = 正弦波，越大波峰越尖）
};
std::vector<GerstnerWave> generateGerstnerWaves(const OceanSettings& s, uint32_t count, uint32_t seed);

// 位置 (x, z)（未位移的「拉格朗日」坐标）在时刻 t 的位移 (dx, dy, dz)
vec3 gerstnerDisplacement(const std::vector<GerstnerWave>& waves, float x, float z, float t);
// 世界坐标 (x, z) 处的水面高度：Gerstner 会水平移动水面点，所以要反解「哪个点移到了这里」（不动点迭代）。
// M6 的浮力、Medium::Query 用它。
float gerstnerHeightAt(const std::vector<GerstnerWave>& waves, float x, float z, float t, int iterations = 4);

} // namespace eng::ocean
