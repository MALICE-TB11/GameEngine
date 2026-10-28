// LightingInputs：全引擎唯一的光照输入。
// PBR（环境）和 toon（角色）都从这里取光 —— 角色不允许有独立光照。
#pragma once
#include "common/frame.hlsli"

static const float PI = 3.14159265359;

struct LightingInputs
{
    float3 sunDir;          // 指向太阳的单位向量
    float3 sunIrradiance;   // 太阳垂直照射时的照度（颜色 × 强度）
    float sunAngularRadius; // 弧度
    float3 skyZenith;
    float3 skyHorizon;
    float3 ground;
    float skyIntensity;
    float sunHalo;          // 太阳周围的光晕强度（大气前向散射的近似）
    float ambientIntensity;
    float sunShadow;        // 太阳可见度：1 = 照亮，0 = 阴影（材质 shader 用 SampleSunShadow 填）
    // EXTEND: 阴影（shadow map 采样结果）、M6 的介质信息（水下吸收、焦散）会加在这里，
    //         所有着色模型自动获得。
};

LightingInputs GetLightingInputs()
{
    LightingInputs L;
    L.sunDir = normalize(gFrame.sunDirection.xyz);
    L.sunIrradiance = gFrame.sunColor.rgb * gFrame.sunDirection.w;
    L.sunAngularRadius = gFrame.sunColor.w;
    L.skyZenith = gFrame.skyZenith.rgb;
    L.skyHorizon = gFrame.skyHorizon.rgb;
    L.ground = gFrame.groundColor.rgb;
    L.skyIntensity = gFrame.skyZenith.w;
    L.sunHalo = gFrame.skyHorizon.w;
    L.ambientIntensity = gFrame.exposure.z;
    L.sunShadow = 1.0;
    return L;
}
