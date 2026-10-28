// 局部交互（波纹）· 浅水方程 第二步：流入减流出更新水量，再加扰动源，得到新的波纹高度。
// 与 engine/ocean/OceanRipples.cpp 的 shallowStep「第二步」一行对一行，公式与理由见 OceanRipples.h。
#include "ocean/ocean_ripple_common.hlsli"

[[vk::binding(0, 1)]] Texture2D<float4> gPrevState; // (η, η⁻, 泡沫, δ)；flags.y = 0 时是默认贴图，不能读
[[vk::binding(1, 1)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> gFlux; // 第一步刚算的流量（只读）
[[vk::binding(2, 1)]] [[vk::image_format("r32f")]] RWTexture2D<float> gBed;      // 第一步写的床面高度（只读）
[[vk::binding(3, 1)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> gNext;
[[vk::binding(4, 1)]] cbuffer RippleSteps
{
    RippleStepData gSteps[RIPPLE_MAX_STEPS];
};

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const int2 c = int2(id.xy);
    if (!InGrid(c))
        return;
    const RippleStepData s = gSteps[(uint)gRipple.flags.w];
    const float dt = s.step.x;
    const float2 origin = s.origin.xy;
    const int2 sc = c + int2(s.step.yz);
    const float level = gRipple.water.x;
    const float cell = gRipple.grid.x;

    const float b = gBed[c];
    const float rest = max(level - b, 0.0);
    const float4 prev = LoadPrev(gPrevState, sc);
    const float4 f = gFlux[c];
    // 左边、下边那两条边归邻居所有；区域边缘没有邻居，流量为 0（用 if 而不是三目运算：越界的存储图像读取是未定义行为）
    float fxLeft = 0.0, fzDown = 0.0;
    if (c.x > 0)
        fxLeft = gFlux[c - int2(1, 0)].x;
    if (c.y > 0)
        fzDown = gFlux[c - int2(0, 1)].y;

    // 直接更新 δ（而不是总水深）：保持毫米级变化的精度。水深不能为负：δ ≥ -静水深
    float delta = prev.w + dt * (fxLeft - f.x + fzDown - f.y) / (cell * cell) + dt * SourceRate(s, CellCenter(c, origin));
    delta = max(delta, -rest);
    // 渲染用的波纹高度：有水的格子 = 水面 - 海面；干的格子取 0（海面画在地形下面，被深度测试挡住）
    const float eta = rest + delta > gRipple.water.z ? max(b, level) - level + delta : 0.0;
    // 尾迹泡沫：按上一步的波纹坡度（这一步邻居的新高度还没算出来）
    const float2 grad = float2(LoadPrev(gPrevState, sc + int2(1, 0)).x - LoadPrev(gPrevState, sc - int2(1, 0)).x,
                               LoadPrev(gPrevState, sc + int2(0, 1)).x - LoadPrev(gPrevState, sc - int2(0, 1)).x) /
                        (2.0 * cell);
    // EXTEND: 在这里算格子中心的流速（两侧边上流量的平均 ÷ (格子宽 × 水深)，CPU 版是 shallowVelocity），
    //         写进单独一张贴图回读给 M6 的 Medium::Query
    gNext[c] = float4(eta, prev.x, NextFoam(prev.z, grad), delta);
}
