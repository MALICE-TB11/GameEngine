// 波纹（局部交互）两种求解器共用的输入与工具函数。CPU 参考：engine/ocean/OceanRipples.cpp。
#pragma once
#include "common/frame.hlsli"

// 必须与 OceanRenderer.cpp 的 RipplePush 一致
struct RipplePush
{
    float4 grid;   // x = 格子边长（米），y = 波速上限，z = 阻尼 γ，w = 每边格子数 n
    float4 sponge; // x = 吸收层宽度（格），y = 吸收层强度，z = 泡沫坡度阈值，w = 泡沫强度
    float4 flags;  // x = 泡沫保留系数 exp(-Δt/τ)，y = 1 有上一步状态，z = 1 有海床高度图，w = 本步在常量缓冲里的序号
    float4 seabed; // xy = 高度图纹素 (0,0) 的世界 XZ，z = 采样间距（米），w = 高度图分辨率
    float4 water;  // x = 平均海面高度，y = 重力加速度 g，z = 最小水深（波动方程：岸；浅水方程：画成干的），w = 截断水深 h_cap
};
[[vk::push_constant]] RipplePush gRipple;

// 每一步的输入（扰动源、区域移动）。必须与 OceanRenderer.cpp 的 RippleStepGpu 一致
#define RIPPLE_MAX_SOURCES 7 // = kMaxRippleSources
#define RIPPLE_MAX_STEPS 4   // = kMaxRippleStepsPerFrame
struct RippleStepData
{
    float4 step;   // x = Δt，yz = 区域原点移动的格数，w = 扰动源个数
    float4 origin; // xy = 区域原点（格子 (0,0) 左下角的世界 XZ）
    // 每个源两个 float4：[2k] = (位置 xy, 半径, 竖直体积流量)，[2k+1] = (水平偶极子 xy, -, -)
    float4 sources[RIPPLE_MAX_SOURCES * 2];
};

int GridSize()
{
    return (int)gRipple.grid.w;
}

bool InGrid(int2 c)
{
    const int n = GridSize();
    return c.x >= 0 && c.y >= 0 && c.x < n && c.y < n;
}

// 上一步的状态，按「上一步的网格坐标」取；超出网格或没有历史 = 平静水面（全 0）。
// flags.y = 0 时绑定的是 1×1 默认贴图，不能用整数坐标读
float4 LoadPrev(Texture2D<float4> tex, int2 c)
{
    if (gRipple.flags.y < 0.5 || !InGrid(c))
        return 0.0;
    return tex.Load(int3(c, 0));
}

// 地形高度（双线性）：用 Load + 手动插值而不是 Sample —— R32F 不保证支持线性过滤
float SeabedHeight(Texture2D<float> seabed, float2 xz)
{
    const int res = (int)gRipple.seabed.w;
    const float2 f = (xz - gRipple.seabed.xy) / gRipple.seabed.z;
    const int2 i0 = clamp(int2(floor(f)), 0, res - 2);
    const float2 t = saturate(f - float2(i0));
    const float h00 = seabed.Load(int3(i0, 0));
    const float h10 = seabed.Load(int3(i0 + int2(1, 0), 0));
    const float h01 = seabed.Load(int3(i0 + int2(0, 1), 0));
    const float h11 = seabed.Load(int3(i0 + int2(1, 1), 0));
    return lerp(lerp(h00, h10, t.x), lerp(h01, h11, t.x), t.y);
}

// 格子中心的世界 XZ
float2 CellCenter(int2 c, float2 origin)
{
    return origin + (float2(c) + 0.5) * gRipple.grid.x;
}

// 格子的床面高度；没有海床数据（flags.z = 0，默认贴图不能读）时当作处处是 1000 米深的海
float BedAt(Texture2D<float> seabed, int2 c, float2 origin)
{
    if (gRipple.flags.z < 0.5)
        return gRipple.water.x - 1000.0;
    return SeabedHeight(seabed, CellCenter(c, origin));
}

// 吸收层：离最近的区域边缘 edge 格，ramp 从 0（内侧）到 1（最外圈），速度阻尼按 ramp² 增大
float SpongeDamping(int2 c)
{
    const int n = GridSize();
    const float edge = (float)min(min(c.x, c.y), min(n - 1 - c.x, n - 1 - c.y));
    const float ramp = saturate(1.0 - edge / gRipple.sponge.x);
    return gRipple.grid.z + gRipple.sponge.y * ramp * ramp;
}

// 扰动源在 x 处的体积流量之和（m/s）：q = G·(竖直 + 偶极子·d/r)
float SourceRate(RippleStepData s, float2 x)
{
    float q = 0.0;
    const uint count = min((uint)s.step.w, (uint)RIPPLE_MAX_SOURCES);
    for (uint k = 0; k < count; ++k)
    {
        const float4 src = s.sources[2 * k];
        const float2 push = s.sources[2 * k + 1].xy;
        const float2 d = x - src.xy;
        const float r = max(src.z, 0.01);
        q += exp(-dot(d, d) / (r * r)) * (src.w + dot(d, push) / r);
    }
    return q;
}

// 尾迹泡沫：坡度超过阈值的地方起泡，旧泡沫按时间常数消散
float NextFoam(float oldFoam, float2 grad)
{
    const float fresh = saturate((length(grad) - gRipple.sponge.z) * gRipple.sponge.w);
    return max(oldFoam * gRipple.flags.x, fresh);
}
