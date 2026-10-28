// 频谱随时间演化：每个波按自己的角频率 ω(k) 转动相位。
//   ĥ(k,t) = h0(k)·e^{iωt} + conj(h0(-k))·e^{-iωt}      （保证逆变换结果是实数）
// 再由 ĥ 推出位移和导数的频谱（乘 ik 就是对空间求导）：
//   水平位移 D⃗ = Σ i(k⃗/k) ĥ      高度斜率 ∂y/∂x = Σ i kx ĥ      ∂Dx/∂x = Σ -(kx²/k) ĥ ……
// 两个「实数场」打包成一个复数场（A + iB）只需一次逆 FFT：结果的实部是 A、虚部是 B。
#include "ocean/ocean_common.hlsli"

[[vk::binding(0, 1)]] Texture2DArray<float4> gH0;                                    // (h0(k), conj(h0(-k)))
[[vk::binding(1, 1)]] [[vk::image_format("rgba32f")]] RWTexture2DArray<float4> gSpec0; // (Dx + iDz, Dy + iDxz)
[[vk::binding(2, 1)]] [[vk::image_format("rgba32f")]] RWTexture2DArray<float4> gSpec1; // (Dyx + iDyz, Dxx + iDzz)

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const uint c = id.z;
    const float L = gSim.sizes[c];
    const float2 k = (float2(id.xy) - OCEAN_N / 2) * (2.0 * OCEAN_PI / L);
    const float kLen = length(k);
    const float invK = kLen > 1e-6 ? 1.0 / kLen : 0.0;

    // 色散关系（有限水深）：ω = √(g k tanh(k h))
    const float omega = sqrt(OCEAN_G * kLen * tanh(min(kLen * gSim.time.z, 20.0)));
    const float phase = omega * gSim.time.x;
    const float2 e = float2(cos(phase), sin(phase));
    const float4 h0 = gH0.Load(int4(id.xy, c, 0));
    const float2 h = CMul(h0.xy, e) + CMul(h0.zw, Conj(e));

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
