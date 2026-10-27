// Cook-Torrance 微表面 BRDF 的各项。公式推导见 docs/03-网格与PBR.md。
//
//   f = D·G·F / (4·NoL·NoV) + (1-F)(1-metallic)·albedo/π
//   这里用 V = G / (4·NoL·NoV)（「可见性项」）把分母合并进去，数值更稳定。
#pragma once
#include "common/lighting.hlsli"

// GGX / Trowbridge-Reitz 法线分布：有多少微表面的法线朝向半程向量 h
float D_GGX(float NoH, float a)
{
    float a2 = a * a;
    float d = NoH * NoH * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

// 高度相关的 Smith-GGX 可见性项（Heitz 2014），已包含 1/(4·NoL·NoV)
float V_SmithGGXCorrelated(float NoV, float NoL, float a)
{
    float a2 = a * a;
    float gv = NoL * sqrt(NoV * NoV * (1.0 - a2) + a2);
    float gl = NoV * sqrt(NoL * NoL * (1.0 - a2) + a2);
    return 0.5 / max(gv + gl, 1e-5);
}

// Schlick 菲涅尔近似：F = F0 + (1-F0)(1-cosθ)^5
float3 F_Schlick(float VoH, float3 F0)
{
    float f = pow(1.0 - VoH, 5.0);
    return F0 + (1.0 - F0) * f;
}

// 环境镜面反射的 BRDF 积分（split-sum 的第二项）的解析近似（Karis 2014，移动端版本）。
// 返回值 = 在均匀环境光下，镜面反射的总反照率。
float3 EnvBRDFApprox(float3 F0, float roughness, float NoV)
{
    const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
    const float4 c1 = float4(1.0, 0.0425, 1.04, -0.04);
    float4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
    float2 AB = float2(-1.04, 1.04) * a004 + r.zw;
    return F0 * AB.x + AB.y;
}
