// 海洋 compute pass 共用的常量、push constant 与复数运算。
// 数据流（每帧，三个级联放在数组贴图的三层里，一次 dispatch 全部处理）：
//   ocean_evolve   ：h0(k) 按色散关系推进到时刻 t，算出位移和导数的频谱（打包成 2 张 RGBA32F）
//   ocean_fft ×2   ：先横向再纵向的逆 FFT（每行/列一个线程组，groupshared 内完成全部 log2(N) 级蝶形）
//   ocean_assemble ：符号校正 → 位移贴图（xyz + 泡沫）、导数贴图（法线 + 雅可比）
#pragma once
#include "common/frame.hlsli"

#define OCEAN_N 256      // 必须与 engine/ocean/OceanSettings.h 的 kOceanFftSize 一致
#define OCEAN_LOG_N 8
static const float OCEAN_PI = 3.14159265359;
static const float OCEAN_G = 9.81;

// 必须与 OceanRenderer.cpp 的 SimPush 一致
struct OceanSimPush
{
    float4 time;  // x = 时间（秒），y = 帧间隔，z = 水深（色散关系），w = 尖锐度 λ
    float4 sizes; // xyz = 三个级联覆盖的海面尺寸（米）
    float4 foam;  // x = 起泡阈值（雅可比），y = 新泡沫强度，z = 本帧保留系数 exp(-dt/τ)，w = FFT 方向（0 横 / 1 纵）
};
[[vk::push_constant]] OceanSimPush gSim;

float2 CMul(float2 a, float2 b)
{
    return float2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x);
}
float2 MulI(float2 a) // 乘以虚数单位 i
{
    return float2(-a.y, a.x);
}
float2 Conj(float2 a)
{
    return float2(a.x, -a.y);
}
