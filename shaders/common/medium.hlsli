// 介质接口（M6 实现）。「材质读取介质」：所有材质通过这里获取环境介质信息。
//
// 现在只返回「空气」，函数都是恒等变换。但 PBR 材质已经在调用它们：
// M6 只需要在这里接入水面高度、水深、吸收系数，所有材质自动获得
// 水线湿润、水下焦散、按深度的附着层、Beer-Lambert 吸收。
#pragma once
#include "common/lighting.hlsli"
#include "common/surface.hlsli"

static const uint MEDIUM_AIR = 0;
static const uint MEDIUM_WATER = 1;
static const uint MEDIUM_SOLID = 2;

struct MediumSample
{
    uint type;
    float waterHeight;  // 该点上方水面的高度（世界 y）
    float waterDepth;   // 水面到该点的深度（在空气中为负）
    float3 absorption;  // 每米吸收系数（RGB 分别）
};

MediumSample SampleMedium(float3 worldPos)
{
    // EXTEND: M6 —— 从海面高度场与地形数据计算
    MediumSample m;
    m.type = MEDIUM_AIR;
    m.waterHeight = -1e6;
    m.waterDepth = -1e6;
    m.absorption = 0.0.xxx;
    return m;
}

// 介质对表面属性的影响（湿润让表面变暗变光滑、附着层……），在光照计算之前调用
void ApplyMediumToSurface(inout SurfaceData s, MediumSample m)
{
    // EXTEND: M6 水线湿润 / 按深度的附着层
}

// 介质对出射光的影响（水下吸收、雾），在光照计算之后调用
float3 ApplyMediumToRadiance(float3 radiance, SurfaceData s, MediumSample m, LightingInputs L)
{
    // EXTEND: M6 Beer-Lambert 吸收
    return radiance;
}
