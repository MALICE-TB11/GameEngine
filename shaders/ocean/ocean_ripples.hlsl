// 局部交互（波纹）：二维波动方程的一个固定步。
// 与 engine/ocean/OceanRipples.cpp 的 rippleStep 一行对一行 —— 公式、边界条件、吸收层的解释都在 OceanRipples.h。
// 改这里要同步改那边，并运行 sandbox --selftest ocean（会逐格比较 GPU 与 CPU 的结果）。
//
// 每个线程一个格子。状态贴图每格 = (u 本步, u 上一步, 尾迹泡沫, 水深)，A/B 两张交替（持久贴图）。
// 水深 = 平均海面 - 地形高度：每步按格子的世界坐标从地形高度图上双线性取（区域跟着玩家走，格子对应的地点会变）。
#include "common/frame.hlsli"

// 必须与 OceanRenderer.cpp 的 RipplePush 一致
struct RipplePush
{
    float4 grid;   // x = 格子边长（米），y = 波速上限，z = 阻尼 γ，w = 每边格子数 n
    float4 sponge; // x = 吸收层宽度（格），y = 吸收层强度，z = 泡沫坡度阈值，w = 泡沫强度
    float4 flags;  // x = 泡沫保留系数 exp(-Δt/τ)，y = 1 有上一步状态，z = 1 有海床高度图，w = 本步在常量缓冲里的序号
    float4 seabed; // xy = 高度图纹素 (0,0) 的世界 XZ，z = 采样间距（米），w = 高度图分辨率
    float4 water;  // x = 平均海面高度，y = 重力加速度 g，z = 最小水深（更浅视为岸）
};
[[vk::push_constant]] RipplePush gRipple;

// 每一步的输入（扰动源、区域移动）。必须与 OceanRenderer.cpp 的 RippleStepGpu 一致
#define RIPPLE_MAX_SOURCES 7 // = kMaxRippleSources
#define RIPPLE_MAX_STEPS 4   // = kMaxRippleStepsPerFrame
struct RippleStepData
{
    float4 step;   // x = Δt，yz = 区域原点移动的格数，w = 扰动源个数
    float4 origin; // xy = 区域原点（格子 (0,0) 左下角的世界 XZ）
    // 每个源两个 float4：[2k] = (位置 xy, 半径, 竖直加速度)，[2k+1] = (水平偶极子 xy, -, -)
    float4 sources[RIPPLE_MAX_SOURCES * 2];
};

[[vk::binding(0, 1)]] Texture2D<float4> gPrev; // 上一步的状态；flags.y = 0 时是 1×1 默认贴图，不能读
[[vk::binding(1, 1)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> gNext;
[[vk::binding(2, 1)]] Texture2D<float> gSeabed; // 地形高度图；flags.z = 0 时是默认贴图，不能读
[[vk::binding(3, 1)]] cbuffer RippleSteps
{
    RippleStepData gSteps[RIPPLE_MAX_STEPS];
};

int GridSize()
{
    return (int)gRipple.grid.w;
}

// 上一步的状态，按「上一步的网格坐标」取；超出网格或没有历史 = 平静水面
float4 LoadPrev(int2 c)
{
    const int n = GridSize();
    if (gRipple.flags.y < 0.5 || c.x < 0 || c.y < 0 || c.x >= n || c.y >= n)
        return 0.0;
    return gPrev.Load(int3(c, 0));
}

// 地形高度（双线性）：用 Load + 手动插值而不是 Sample —— R32F 不保证支持线性过滤
float SeabedHeight(float2 xz)
{
    const int res = (int)gRipple.seabed.w;
    const float2 f = (xz - gRipple.seabed.xy) / gRipple.seabed.z;
    const int2 i0 = clamp(int2(floor(f)), 0, res - 2);
    const float2 t = saturate(f - float2(i0));
    const float h00 = gSeabed.Load(int3(i0, 0));
    const float h10 = gSeabed.Load(int3(i0 + int2(1, 0), 0));
    const float h01 = gSeabed.Load(int3(i0 + int2(0, 1), 0));
    const float h11 = gSeabed.Load(int3(i0 + int2(1, 1), 0));
    return lerp(lerp(h00, h10, t.x), lerp(h01, h11, t.x), t.y);
}

// 格子 c（本步的网格）的水深；没有海床数据时处处是深水
float DepthAt(int2 c, float2 origin)
{
    if (gRipple.flags.z < 0.5)
        return 1000.0;
    const float2 xz = origin + (float2(c) + 0.5) * gRipple.grid.x;
    return gRipple.water.x - SeabedHeight(xz);
}

bool Wet(float h)
{
    return h > gRipple.water.z;
}

// 邻居的高度：区域外 = 平静水面（开边界，靠吸收层避免反射）；岸 = 和自己一样高（零梯度，反射）
float Neighbor(int2 c, int2 sc, int2 o, float u, float2 origin)
{
    const int n = GridSize();
    const int2 nc = c + o;
    if (nc.x < 0 || nc.y < 0 || nc.x >= n || nc.y >= n)
        return 0.0;
    if (!Wet(DepthAt(nc, origin)))
        return u;
    return LoadPrev(sc + o).x;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const int n = GridSize();
    const int2 c = int2(id.xy);
    if (c.x >= n || c.y >= n)
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
    const float4 cur = LoadPrev(sc);
    const float u = cur.x, up = cur.y;
    const float uL = Neighbor(c, sc, int2(-1, 0), u, origin);
    const float uR = Neighbor(c, sc, int2(1, 0), u, origin);
    const float uD = Neighbor(c, sc, int2(0, -1), u, origin);
    const float uU = Neighbor(c, sc, int2(0, 1), u, origin);
    const float cell = gRipple.grid.x;
    const float lap = (uL + uR + uD + uU - 4.0 * u) * (1.0 / (cell * cell));
    const float c2 = min(gRipple.water.y * h, gRipple.grid.y * gRipple.grid.y);

    // 吸收层：离最近的区域边缘 edge 格，ramp 从 0（内侧）到 1（最外圈），速度阻尼按 ramp² 增大
    const float edge = (float)min(min(c.x, c.y), min(n - 1 - c.x, n - 1 - c.y));
    const float ramp = saturate(1.0 - edge / gRipple.sponge.x);
    const float gamma = gRipple.grid.z + gRipple.sponge.y * ramp * ramp;

    // 扰动源：格子中心处各个高斯的加速度之和
    const float2 x = origin + (float2(c) + 0.5) * cell;
    float f = 0.0;
    const uint count = min((uint)s.step.w, (uint)RIPPLE_MAX_SOURCES);
    for (uint k = 0; k < count; ++k)
    {
        const float4 src = s.sources[2 * k];
        const float2 push = s.sources[2 * k + 1].xy;
        const float2 d = x - src.xy;
        const float r = max(src.z, 0.01);
        f += exp(-dot(d, d) / (r * r)) * (src.w + dot(d, push) / r);
    }

    const float next = u + (u - up) * (1.0 - gamma * dt) + c2 * dt * dt * lap + dt * dt * f;
    // 尾迹泡沫：坡度超过阈值的地方起泡，之后按时间常数消散
    const float2 grad = float2(uR - uL, uU - uD) / (2.0 * cell);
    const float fresh = saturate((length(grad) - gRipple.sponge.z) * gRipple.sponge.w);
    gNext[c] = float4(next, u, max(cur.z * gRipple.flags.x, fresh), h);
}
