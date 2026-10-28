// 局部交互（波纹）· 浅水方程 第一步：每个格子算自己右边、上边两条边的新体积流量（m³/s）。
// 与 engine/ocean/OceanRipples.cpp 的 shallowStep「第一步」一行对一行，公式与理由见 OceanRipples.h。
// 顺便把本格的床面高度写出来：第二步直接用，校验时 CPU 也拿它当输入。
//
// 为什么分两个 pass：更新水量要用到四条边（含左、下邻居拥有的两条）的「新」流量，
// 必须等所有格子的流量都算完 —— 一次 dispatch 里线程之间没法全局同步，所以拆成两次，中间由 Render Graph 插 barrier。
#include "ocean/ocean_ripple_common.hlsli"

[[vk::binding(0, 1)]] Texture2D<float4> gPrevState; // (η, η⁻, 泡沫, δ)；flags.y = 0 时是默认贴图，不能读
[[vk::binding(1, 1)]] Texture2D<float4> gPrevFlux;  // (Fx, Fz, 0, 0)；同上
[[vk::binding(2, 1)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> gFlux;
[[vk::binding(3, 1)]] [[vk::image_format("r32f")]] RWTexture2D<float> gBed;
[[vk::binding(4, 1)]] Texture2D<float> gSeabed; // 地形高度图；flags.z = 0 时是默认贴图，不能读
[[vk::binding(5, 1)]] cbuffer RippleSteps
{
    RippleStepData gSteps[RIPPLE_MAX_STEPS];
};

// 静水深：海面以下的床面上有水，高于海面的地方（岸）没有
float RestDepth(float b)
{
    return max(gRipple.water.x - b, 0.0);
}
// 水面高度 = 它 + δ。海里 = 海面 + δ（精确：不会因为 床面 + 水深 的舍入在静水里凭空流动）
float SurfaceBase(float b)
{
    return max(b, gRipple.water.x);
}

// 格子 c 与邻居 c + o 之间那条边的新流量（正 = 从 c 流向邻居）
float EdgeFlux(int2 c, int2 sc, int2 o, float d, float h, float old, float gamma, float dt, float2 origin)
{
    const int2 nc = c + o;
    if (!InGrid(nc))
        return 0.0; // 区域边缘封闭：靠吸收层把波吃掉
    const float nb = BedAt(gSeabed, nc, origin);
    const float nDelta = LoadPrev(gPrevState, sc + o).w;
    const float nd = RestDepth(nb) + nDelta;
    const float nh = SurfaceBase(nb) + nDelta;
    // 边上的有效水深取上游，并截断到 h_cap（波速上限）：干的一侧流不出水，水能爬上干的沙滩
    const float up = h >= nh ? d : nd;
    float f = old * (1.0 - gamma * dt) + dt * gRipple.water.y * min(up, gRipple.water.w) * (h - nh);
    // 一条边一步最多流走上游格子 1/4 的水：四条边都往外流也抽不干，水深不会为负
    const float cell = gRipple.grid.x;
    const float capScale = cell * cell / (4.0 * dt);
    f = f > 0.0 ? min(f, d * capScale) : max(f, -nd * capScale);
    return f;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const int2 c = int2(id.xy);
    if (!InGrid(c))
        return;
    const RippleStepData s = gSteps[(uint)gRipple.flags.w];
    const float dt = s.step.x;
    const float2 origin = s.origin.xy;
    const int2 sc = c + int2(s.step.yz); // 区域移动了 shift 格：上一步的网格里对应 c + shift

    const float b = BedAt(gSeabed, c, origin);
    gBed[c] = b;
    const float delta = LoadPrev(gPrevState, sc).w;
    const float d = RestDepth(b) + delta;
    const float h = SurfaceBase(b) + delta;
    const float gamma = SpongeDamping(c);
    const float4 old = LoadPrev(gPrevFlux, sc);
    gFlux[c] = float4(EdgeFlux(c, sc, int2(1, 0), d, h, old.x, gamma, dt, origin), EdgeFlux(c, sc, int2(0, 1), d, h, old.y, gamma, dt, origin),
                      0.0, 0.0);
}
