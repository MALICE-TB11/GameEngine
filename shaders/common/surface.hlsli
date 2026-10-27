// SurfaceData：材质与光照之间的固定接口。
//
//   材质 shader（materials/*.hlsl）：读贴图和参数 → 填 SurfaceData
//   着色模型（common/shading.hlsli）：SurfaceData + LightingInputs → 颜色
//
// 新增材质只需写「怎么填 SurfaceData」；修改光照只需改 shading.hlsli，所有材质同时生效。
#pragma once

static const uint SHADING_MODEL_PBR = 0;
static const uint SHADING_MODEL_TOON = 1; // EXTEND: M7

struct SurfaceData
{
    float3 position;        // 世界坐标
    float3 normal;          // 世界空间，已应用法线贴图，单位向量
    float3 geometricNormal; // 世界空间，插值后的顶点法线（不含法线贴图）
    float3 viewDir;         // 从表面指向相机，单位向量
    float3 albedo;          // 线性 RGB（金属的 albedo 即其反射颜色 F0）
    float alpha;
    float metallic;         // 0 = 电介质，1 = 金属
    float roughness;        // 感知粗糙度（美术调节用的线性值），BRDF 内部使用 α = roughness²
    float ao;               // 环境光遮蔽：1 = 无遮蔽
    float3 emissive;        // 自发光辐亮度（线性，可 > 1）
    uint shadingModel;
};

// 用切线空间法线贴图扰动法线。
// TBN：T = 切线，B = cross(N, T) * w（glTF 约定，w 处理镜像 UV），N = 法线
float3 ApplyNormalMap(float3 n, float4 tangent, float3 tangentSpaceNormal, float strength)
{
    float3 t = tangent.xyz;
    // Gram-Schmidt：插值后的 T 与 N 不再严格垂直，先正交化
    t = normalize(t - n * dot(n, t));
    float3 b = cross(n, t) * (tangent.w < 0.0 ? -1.0 : 1.0);
    float3 ts = float3(tangentSpaceNormal.xy * strength, tangentSpaceNormal.z);
    return normalize(t * ts.x + b * ts.y + n * ts.z);
}
