// 频谱随时间演化：每个波按自己的角频率 ω(k) 转动相位。
//   ĥ(k,t) = h0(k)·e^{-iωt} + conj(h0(-k))·e^{+iωt}      （保证逆变换结果是实数）
// 符号决定传播方向：h0(k)·e^{-iωt}·e^{ik·x} = e^{i(k·x - ωt)}，波峰随时间沿 +k 移动 ——
// 与 Gerstner 的 cos(k·x - ωt) 相同，频谱按风向分配的能量才真的「顺风」走。
// （Tessendorf 原文写的是 e^{+iωt}，波会逆着 k 跑；两种实现切换时主浪方向会反过来。）
// 再由 ĥ 推出位移和导数的频谱（乘 ik 就是对空间求导）：
//   水平位移 D⃗ = Σ i(k⃗/k) ĥ      高度斜率 ∂y/∂x = Σ i kx ĥ      ∂Dx/∂x = Σ -(kx²/k) ĥ ……
// 两个「实数场」打包成一个复数场（A + iB）只需一次逆 FFT：结果的实部是 A、虚部是 B。
#include "ocean/ocean_common.hlsli"

[[vk::binding(0, 1)]] Texture2DArray<float4> gH0;                                    // (h0(k), conj(h0(-k)))
[[vk::binding(1, 1)]] [[vk::image_format("rgba32f")]] RWTexture2DArray<float4> gSpec0; // (Dx + iDz, Dy + iDxz)
[[vk::binding(2, 1)]] [[vk::image_format("rgba32f")]] RWTexture2DArray<float4> gSpec1; // (Dyx + iDyz, Dxx + iDzz)
[[vk::binding(3, 1)]] Texture2DArray<float> gOmega;                                  // ω(k)，CPU 算好（已量化）

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const uint c = id.z;
    const float L = gSim.sizes[c];
    const float2 k = (float2(id.xy) - OCEAN_N / 2) * (2.0 * OCEAN_PI / L);
    const float kLen = length(k);
    const float invK = kLen > 1e-6 ? 1.0 / kLen : 0.0;

    // ω 由 CPU 按色散关系 ω = √(g k tanh(k h)) 算好并量化（OceanSpectrum.h 的 cascadeFrequencies）：
    // 和 CPU 参考实现用同一组数，GPU 结果才能逐点校验
    const float omega = gOmega.Load(int4(id.xy, c, 0));
    const float phase = omega * gSim.time.x;
    const float2 e = float2(cos(phase), sin(phase)); // e^{iωt}
    const float4 h0 = gH0.Load(int4(id.xy, c, 0));
    const float2 h = CMul(h0.xy, Conj(e)) + CMul(h0.zw, e);

    const float2 ih = MulI(h);
    const float2 dx = ih * (k.x * invK);
    const float2 dz = ih * (k.y * invK);
    const float2 dyx = ih * k.x;
    const float2 dyz = ih * k.y;
    const float2 dxx = -h * (k.x * k.x * invK);
    const float2 dzz = -h * (k.y * k.y * invK);
    const float2 dxz = -h * (k.x * k.y * invK);

    gSpec0[id] = float4(dx + MulI(dz), h + MulI(dxz));
    gSpec1[id] = float4(dyx + MulI(dyz), dxx + MulI(dzz));
}
