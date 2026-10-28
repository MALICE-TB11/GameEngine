// 着色模型：SurfaceData + LightingInputs → 出射辐亮度（线性 HDR）。
// 这是全引擎「计算光照」的唯一位置。
#pragma once
#include "common/brdf.hlsli"
#include "common/lighting.hlsli"
#include "common/sky.hlsli"
#include "common/surface.hlsli"

// 电介质（非金属）的垂直入射反射率约 4%（折射率 1.5）。水是 2%（M5 海面单独处理）。
static const float DIELECTRIC_F0 = 0.04;

float3 ShadePBR(SurfaceData s, LightingInputs L)
{
    float3 n = s.normal;
    float3 v = s.viewDir;
    float3 l = L.sunDir;
    float3 h = normalize(v + l);
    float NoV = max(dot(n, v), 1e-4);
    float NoL = saturate(dot(n, l));
    float NoH = saturate(dot(n, h));
    float VoH = saturate(dot(v, h));

    // α = roughness²：让美术调节的粗糙度在视觉上更线性；钳制下限避免极小高光导致的闪烁
    float a = max(s.roughness * s.roughness, 0.002);
    float3 F0 = lerp(DIELECTRIC_F0.xxx, s.albedo, s.metallic);
    float3 diffuseColor = s.albedo * (1.0 - s.metallic); // 金属没有漫反射

    // ---- 直接光：太阳 ----
    float3 F = F_Schlick(VoH, F0);
    float3 specular = D_GGX(NoH, a) * V_SmithGGXCorrelated(NoV, NoL, a) * F;
    float3 diffuse = (1.0 - F) * diffuseColor / PI; // 被镜面反射掉的能量不再参与漫反射
    float3 direct = (diffuse + specular) * L.sunIrradiance * NoL * L.sunShadow;

    // ---- 环境光：解析天空 ----
    float3 ambientDiffuse = diffuseColor * AmbientRadiance(n, L);
    // 镜面：沿反射方向取天空颜色。粗糙表面的反射是大范围平均，用漫反射环境光近似「模糊后的天空」
    float3 r = reflect(-v, n);
    float3 skyR = lerp(SkyRadiance(r, L, false), AmbientRadiance(r, L), saturate(a * 2.0));
    float3 ambientSpecular = skyR * EnvBRDFApprox(F0, s.roughness, NoV);
    float3 ambient = (ambientDiffuse + ambientSpecular) * s.ao * L.ambientIntensity;

    return direct + ambient + s.emissive;
}

// 按着色模型分派。所有材质最终都调用它。
float3 ShadeSurface(SurfaceData s, LightingInputs L)
{
    // EXTEND: M7 —— case SHADING_MODEL_TOON: return ShadeToon(s, L);（同一份 L，只是映射不同）
    return ShadePBR(s, L);
}
