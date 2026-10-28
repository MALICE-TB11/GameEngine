// 标准 PBR 材质（glTF 金属-粗糙度模型）。
// 本文件只负责「读贴图和参数 → 填 SurfaceData」；光照在 common/shading.hlsli 里统一计算。
#include "common/mesh.hlsli"
#include "common/surface.hlsli"
#include "common/shading.hlsli"
#include "common/medium.hlsli"
#include "common/shadow.hlsli"

// set 1：材质贴图（缺失的由 C++ 端绑定默认贴图：白 / 平坦法线）
[[vk::binding(0, 1)]] Texture2D<float4> gBaseColorTex;  // sRGB，硬件自动转线性
[[vk::binding(1, 1)]] Texture2D<float4> gNormalTex;     // 切线空间法线，[0,1] 编码
[[vk::binding(2, 1)]] Texture2D<float4> gMetalRoughTex; // G = 粗糙度，B = 金属度
[[vk::binding(3, 1)]] Texture2D<float4> gOcclusionTex;  // R = AO
[[vk::binding(4, 1)]] Texture2D<float4> gEmissiveTex;   // sRGB
[[vk::binding(5, 1)]] Texture2D<float> gShadowAtlas;    // 级联阴影图集（Render Graph 管理，关掉阴影时为「处处照亮」）

MeshVSOut VSMain(VSIn i)
{
    return MeshVertex(i);
}

SurfaceData BuildSurface(MeshVSOut i, bool frontFace)
{
    MaterialParams p = gDraw.material;
    SurfaceData s;
    s.position = i.worldPos;
    s.viewDir = normalize(gFrame.cameraPos.xyz - i.worldPos);

    float3 n = normalize(i.normal);
    if (!frontFace)
        n = -n; // 双面材质的背面：法线朝向观察者
    s.geometricNormal = n;
    // 法线贴图只用 XY，Z 由单位长度重建：z = sqrt(1 - x² - y²)。
    // 这样烘焙时可以用只存两个通道的 BC5（精度比把三个通道塞进 BC7 高，体积也相同），
    // 未压缩的 RGBA8 法线贴图和默认平坦法线（0.5, 0.5, 1）同样适用。
    float2 nxy = gNormalTex.Sample(gLinearWrap, i.uv).xy * 2.0 - 1.0;
    float3 tn = float3(nxy, sqrt(saturate(1.0 - dot(nxy, nxy))));
    s.normal = ApplyNormalMap(n, i.tangent, tn, p.emissive.w);

    float4 base = gBaseColorTex.Sample(gLinearWrap, i.uv) * p.baseColor;
    s.albedo = base.rgb;
    s.alpha = base.a;
    float4 mr = gMetalRoughTex.Sample(gLinearWrap, i.uv);
    s.roughness = saturate(mr.g * p.pbr.y);
    s.metallic = saturate(mr.b * p.pbr.x);
    float ao = gOcclusionTex.Sample(gLinearWrap, i.uv).r;
    s.ao = lerp(1.0, ao, p.pbr.z);
    s.emissive = gEmissiveTex.Sample(gLinearWrap, i.uv).rgb * p.emissive.rgb * p.flags.y;
    s.shadingModel = (uint)p.flags.x;
    return s;
}

float4 PSMain(MeshVSOut i, bool frontFace : SV_IsFrontFace) : SV_Target
{
    SurfaceData s = BuildSurface(i, frontFace);
    // glTF alphaMode = MASK：低于阈值的像素直接丢弃（树叶、铁丝网）
    if (gDraw.material.flags.z > 0.5 && s.alpha < gDraw.material.pbr.w)
        discard;

    MediumSample medium = SampleMedium(s.position);
    ApplyMediumToSurface(s, medium);

    LightingInputs L = GetLightingInputs();
    L.sunShadow = SampleSunShadow(gShadowAtlas, s.position, s.geometricNormal);
    float3 color = ShadeSurface(s, L);
    color = ApplyMediumToRadiance(color, s, medium, L);

    // 调试视图：直接输出某个属性（tonemap pass 会跳过色调映射）
    uint view = (uint)gFrame.debug.x;
    if (view == 1) color = s.normal * 0.5 + 0.5;
    else if (view == 2) color = s.albedo;
    else if (view == 3) color = s.roughness.xxx;
    else if (view == 4) color = s.metallic.xxx;
    else if (view == 5) color = s.ao.xxx;
    return float4(color, 1.0);
}
