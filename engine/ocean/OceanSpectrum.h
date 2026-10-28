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

// 海面的「循环周期」T：所有角频率都取 2π/T 的整数倍，整片海每 T 秒精确重复一次。
// 为什么：GPU 里时间是 float，运行几小时后 t 的精度只剩毫秒级，ω·t 的误差让小波开始抖动。
// 频率量化后只需要传 t mod T（永远不超过 T，精度约 0.06 ms）。量化带来的频率误差 ≤ π/T ≈ 0.003 rad/s，
// 对最慢的波（ω ≈ 0.4）也不到 1%，肉眼看不出；T = 1000 秒的重复也不会被注意到。
inline constexpr double kOceanRepeatPeriod = 1000.0; // 必须与 shaders/ocean/ocean_common.hlsli 的 OCEAN_REPEAT_PERIOD 一致
// 把 ω 取到最近的 2π/T 整数倍
float quantizeFrequency(float omega);
// 模拟时间 → 传给 GPU / 波形计算的相位时间 t mod T
float oceanPhaseTime(double simTime);

// 色散关系：ω(k) = √(g k tanh(k h))
float dispersion(float k, float depth);
// 色散关系的反函数：由 ω 求 k。g·k·tanh(kh) 随 k 单调递增，所以解唯一；
// 用「带区间保护的牛顿法」求：牛顿步跳出当前区间就改用二分，按残差判断收敛（浅水也收敛）
float waveNumber(float omega, float depth);
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
// 每个格子的角频率 ω(k)（已量化）。由 CPU 算好和 h0 一起上传：GPU 演化与 CPU 参考用的是同一组 ω，
// 否则 float 舍入可能让个别频率落到不同的量化格上，t 接近周期时相位差能到整整一圈
std::vector<float> cascadeFrequencies(const OceanSettings& s, uint32_t cascade, uint32_t n);

// ---- FFT 的 CPU 参考：直接对所有波求和（离散傅里叶变换），和 GPU 的 演化 → 逆 FFT → 组装 算的是同一个量 ----
// 很慢（每个点 N² 次复数乘法），只用于测试和 GPU 结果校验。约定与 ocean_evolve.hlsl 相同：
//   ĥ(k,t) = h0(k)·e^{-iωt} + conj(h0(-k))·e^{+iωt}   →  单个波 cos(k·x - ωt)，沿 +k 方向传播（与 Gerstner 一致）
struct CascadeSample {
    vec4 displacement; // 与 GPU 位移贴图相同：(λDx, y, λDz, 雅可比 J)
    vec4 derivatives;  // 与 GPU 导数贴图相同：(∂y/∂x, ∂y/∂z, λ∂Dx/∂x, λ∂Dz/∂z)
};
// h0 / omega：generateCascadeSpectrum / cascadeFrequencies 的结果；position：未位移坐标（米）；phaseTime：oceanPhaseTime 的结果
CascadeSample evaluateCascade(const std::vector<vec4>& h0, const std::vector<float>& omega, uint32_t n, float size, float lambda,
                              vec2 position, float phaseTime);

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
// t 是相位时间（oceanPhaseTime）。M6 的浮力、Medium::Query 用它。
float gerstnerHeightAt(const std::vector<GerstnerWave>& waves, float x, float z, float t, int iterations = 4);

} // namespace eng::ocean
