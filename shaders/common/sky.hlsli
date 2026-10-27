// 解析天空：天顶 → 地平线渐变 + 地面 + 太阳光晕 + 太阳盘。
// 用途：Sky pass 画背景；PBR 环境光用它近似「从某方向射来的天光」。
// EXTEND: 换成物理大气散射（Bruneton / Hillaire LUT），接口 SkyRadiance(dir, L, withSun) 不变。
#pragma once
#include "common/lighting.hlsli"

float3 SkyRadiance(float3 dir, LightingInputs L, bool withSun)
{
    float y = dir.y;
    // 天顶附近颜色变化慢、地平线附近变化快：用 sqrt 形状的混合
    float3 sky = lerp(L.skyHorizon, L.skyZenith, sqrt(saturate(y)));
    // 地平线以下很快过渡到地面颜色
    float3 below = lerp(L.skyHorizon, L.ground, saturate(-y * 6.0));
    float3 c = (y >= 0.0 ? sky : below) * L.skyIntensity;

    // 太阳光晕：大气对阳光的前向散射，越靠近太阳越亮（两个指数叠加：宽的柔光 + 窄的亮环）
    float cosT = dot(dir, L.sunDir);
    float halo = pow(saturate(cosT), 8.0) * 0.15 + pow(saturate(cosT), 200.0);
    c += L.sunIrradiance * L.sunHalo * halo * 0.05;

    if (withSun)
    {
        // 太阳盘：照度 E 均匀分布在立体角 π r² 上，所以盘面辐亮度 = E / (π r²)。
        // 真实值约 10^5 量级，会超出 RGBA16F 的上限（65504）变成 inf，因此钳制。
        float r = max(L.sunAngularRadius, 1e-4);
        float edge = smoothstep(cos(r * 1.15), cos(r), cosT);
        float3 disk = min(L.sunIrradiance / (PI * r * r), 20000.0);
        c += disk * edge;
    }
    return c;
}

// 半球环境光：法线方向上「余弦加权的平均天空辐亮度」的粗略近似。
// Lambert 漫反射在此光照下的出射辐亮度 = albedo × AmbientRadiance(n)。
float3 AmbientRadiance(float3 n, LightingInputs L)
{
    float up = n.y * 0.5 + 0.5;
    float3 skyAvg = lerp(L.skyHorizon, L.skyZenith, 0.5);
    return lerp(L.ground, skyAvg, up) * L.skyIntensity;
}
