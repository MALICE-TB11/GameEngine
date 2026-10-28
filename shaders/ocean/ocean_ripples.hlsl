// 局部交互（波纹）· 波动方程（参考实现）的一个固定步。
// 与 engine/ocean/OceanRipples.cpp 的 rippleStep 一行对一行 —— 公式、边界条件、吸收层的解释都在 OceanRipples.h。
// 改这里要同步改那边，并运行 sandbox --selftest ocean（会逐格比较 GPU 与 CPU 的结果）。
//
// 每个线程一个格子。状态贴图每格 = (u 本步, u 上一步, 尾迹泡沫, 静水深)，A/B 两张交替（持久贴图）。
// 静水深 = 平均海面 - 地形高度：每步按格子的世界坐标从地形高度图上取（区域跟着玩家走，格子对应的地点会变）。
#include "ocean/ocean_ripple_common.hlsli"

[[vk::binding(0, 1)]] Texture2D<float4> gPrev; // 上一步的状态；flags.y = 0 时是 1×1 默认贴图，不能读
[[vk::binding(1, 1)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> gNext;
[[vk::binding(2, 1)]] Texture2D<float> gSeabed; // 地形高度图；flags.z = 0 时是默认贴图，不能读
[[vk::binding(3, 1)]] cbuffer RippleSteps
{
    RippleStepData gSteps[RIPPLE_MAX_STEPS];
};

float DepthAt(int2 c, float2 origin)
{
    return gRipple.water.x - BedAt(gSeabed, c, origin);
}

bool Wet(float h)
{
    return h > gRipple.water.z;
}

// 邻居的高度：区域外 = 平静水面（开边界，靠吸收层避免反射）；岸 = 和自己一样高（零梯度，反射）
float Neighbor(int2 c, int2 sc, int2 o, float u, float2 origin)
{
    const int2 nc = c + o;
    if (!InGrid(nc))
        return 0.0;
    if (!Wet(DepthAt(nc, origin)))
        return u;
    return LoadPrev(gPrev, sc + o).x;
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

    const float h = DepthAt(c, origin);
    if (!Wet(h))
    {
        gNext[c] = float4(0.0, 0.0, 0.0, h); // 岸：没有水
        return;
    }
    // 区域移动了 shift 格：本步的格子 c 在上一步的网格里是 c + shift
    const int2 sc = c + int2(s.step.yz);
    const float4 cur = LoadPrev(gPrev, sc);
    const float u = cur.x, up = cur.y;
    const float uL = Neighbor(c, sc, int2(-1, 0), u, origin);
    const float uR = Neighbor(c, sc, int2(1, 0), u, origin);
    const float uD = Neighbor(c, sc, int2(0, -1), u, origin);
    const float uU = Neighbor(c, sc, int2(0, 1), u, origin);
    const float cell = gRipple.grid.x;
    const float lap = (uL + uR + uD + uU - 4.0 * u) * (1.0 / (cell * cell));
    const float c2 = min(gRipple.water.y * h, gRipple.grid.y * gRipple.grid.y);
    const float gamma = SpongeDamping(c);

    // 吸收层只阻尼速度（u − u⁻），不把高度往 0 拉（原因见 OceanRipples.h）
    const float next = u + (u - up) * (1.0 - gamma * dt) + c2 * dt * dt * lap;
    // 扰动源是体积流量：这一步加 Δt·q 的水，「上一步」也加同样的量，下一步不会把它当成速度
    const float add = dt * SourceRate(s, CellCenter(c, origin));
    const float2 grad = float2(uR - uL, uU - uD) / (2.0 * cell);
    gNext[c] = float4(next + add, u + add, NextFoam(cur.z, grad), h);
}
