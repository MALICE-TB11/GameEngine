// 逆 FFT 结果 → 渲染用的位移与导数贴图，并累积泡沫。
//
// 符号校正：频谱下标 n 对应波数 (n - N/2)，平移 N/2 等于在空间域乘 e^{-iπx} = (-1)^x，二维就是 (-1)^(x+z)。
//
// 泡沫：水平位移把水面点往浪峰挤，挤到一定程度水面「折叠」—— 浪头破碎。
// 折叠程度用位移映射的雅可比行列式衡量：J = (1+λDxx)(1+λDzz) - (λDxz)²
//   J = 1 平静水面；J 越小挤得越厉害；J < 0 水面自相交。
// 新泡沫 = saturate((阈值 - J) × 强度)；旧泡沫按 exp(-dt/τ) 衰减；取两者较大者。
// 贴图坐标是「未位移」的拉格朗日坐标，所以泡沫自动跟着水面点一起移动 —— 不需要额外的平流计算。
// 泡沫历史单独存一张持久贴图（A/B 交替）：位移贴图只表示「这一帧」的结果，组装被跳过时海面读到默认贴图
// （平静水面），而不是两帧前残留的旧位移。
#include "ocean/ocean_common.hlsli"

[[vk::binding(0, 1)]] [[vk::image_format("rgba32f")]] RWTexture2DArray<float4> gSpace0; // (Dx + iDz, Dy + iDxz) 已逆变换
[[vk::binding(1, 1)]] [[vk::image_format("rgba32f")]] RWTexture2DArray<float4> gSpace1; // (Dyx + iDyz, Dxx + iDzz)
[[vk::binding(2, 1)]] Texture2DArray<float> gPrevFoam;                                    // 上一帧的泡沫（持久贴图）
[[vk::binding(3, 1)]] [[vk::image_format("rgba16f")]] RWTexture2DArray<float4> gDisplacement; // xyz 位移，w 泡沫
[[vk::binding(4, 1)]] [[vk::image_format("rgba16f")]] RWTexture2DArray<float4> gDerivatives;  // ∂y/∂x, ∂y/∂z, λ∂Dx/∂x, λ∂Dz/∂z
[[vk::binding(5, 1)]] [[vk::image_format("r32f")]] RWTexture2DArray<float> gFoam;             // 本帧泡沫 → 下一帧的历史

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const float sign = ((id.x + id.y) & 1) ? -1.0 : 1.0;
    const float4 a = gSpace0[id] * sign;
    const float4 b = gSpace1[id] * sign;
    const float lambda = gSim.time.w;

    const float dx = a.x, dz = a.y, dy = a.z, dxz = a.w;
    const float dyx = b.x, dyz = b.y, dxx = b.z, dzz = b.w;

    const float jxx = 1.0 + lambda * dxx;
    const float jzz = 1.0 + lambda * dzz;
    const float jxz = lambda * dxz;
    const float jacobian = jxx * jzz - jxz * jxz;

    // 第一帧（或泡沫历史刚被丢弃）没有上一帧：这时绑定的是 1×1 的默认贴图，用整数坐标读会越界
    // （设备没开 robustImageAccess，越界读的结果未定义），所以由 CPU 告诉我们有没有历史，而不是指望读到 0
    float previous = 0.0;
    if (gSim.foam.w > 0.5)
        previous = gPrevFoam.Load(int4(id, 0));
    const float fresh = saturate((gSim.foam.x - jacobian) * gSim.foam.y);
    const float foam = max(previous * gSim.foam.z, fresh);

    gDisplacement[id] = float4(lambda * dx, dy, lambda * dz, foam);
    gDerivatives[id] = float4(dyx, dyz, lambda * dxx, lambda * dzz);
    gFoam[id] = foam;
}
