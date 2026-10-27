// M2 的调试材质：法线着色 + 简单的漫反射，用来确认几何、深度与相机都正确。
// M3 起被 PBR 材质取代，但保留它作为「最简单的网格 shader」参考。
#include "common/mesh.hlsli"

MeshVSOut VSMain(VSIn i)
{
    return MeshVertex(i);
}

float4 PSMain(MeshVSOut i) : SV_Target
{
    float3 n = normalize(i.normal);
    float3 albedo = n * 0.5 + 0.5; // 法线可视化：+X 红、+Y 绿、+Z 蓝
    float ndl = saturate(dot(n, gFrame.sunDirection.xyz));
    float3 color = albedo * (0.15 + 0.85 * ndl);
    return float4(color, 1.0);
}
