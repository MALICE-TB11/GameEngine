// 地形分层材质：沙滩 / 岩石 / 礁石 / 苔藓。
//
// 每个像素按「高度 + 坡度」算出四层的基础权重，再用各层贴图 alpha 里的「高度」做高度混合
// （石头会从沙子里「冒出来」，而不是两张贴图半透明叠在一起）。岩石层用三平面投影，陡坡上不会拉伸。
// 和所有材质一样，这里只负责填 SurfaceData，光照、阴影、介质（水下）全部走公共函数。
#include "terrain/terrain_common.hlsli"
#include "common/surface.hlsli"
#include "common/shading.hlsli"
#include "common/medium.hlsli"
#include "common/shadow.hlsli"

[[vk::binding(1, 1)]] Texture2D<float4> gNormalMap;     // 宏观法线（由高度场计算，RGBA8，xyz*0.5+0.5）
[[vk::binding(2, 1)]] Texture2D<float4> gLayerAlbedo0;  // 沙：rgb = 颜色（sRGB），a = 高度
[[vk::binding(3, 1)]] Texture2D<float4> gLayerAlbedo1;  // 岩
[[vk::binding(4, 1)]] Texture2D<float4> gLayerAlbedo2;  // 礁
[[vk::binding(5, 1)]] Texture2D<float4> gLayerAlbedo3;  // 苔
[[vk::binding(6, 1)]] Texture2D<float4> gLayerNormal0;  // 切线空间法线（BC5，只用 xy）
[[vk::binding(7, 1)]] Texture2D<float4> gLayerNormal1;
[[vk::binding(8, 1)]] Texture2D<float4> gLayerNormal2;
[[vk::binding(9, 1)]] Texture2D<float4> gLayerNormal3;
[[vk::binding(10, 1)]] Texture2D<float> gShadowAtlas;

struct TerrainVSOut
{
    float4 position : SV_Position;
    float3 worldPos : WORLDPOS;
    float2 heightUV : TEXCOORD0;
};

TerrainVSOut VSMain(uint vid : SV_VertexID)
{
    TerrainVertex v = TerrainVertexFromId(vid);
    TerrainVSOut o;
    o.worldPos = v.worldPos;
    o.heightUV = v.heightUV;
    o.position = mul(gFrame.viewProj, float4(v.worldPos, 1.0));
    return o;
}

float3 UnpackXY(float4 t)
{
    float2 xy = t.xy * 2.0 - 1.0;
    return float3(xy, sqrt(saturate(1.0 - dot(xy, xy))));
}

// 水平投影（uv = 世界 XZ）的细节法线 → 世界空间。「whiteout」混合：细节法线的 xy 叠加到宏观法线上
float3 PlanarNormal(float3 tn, float3 N)
{
    float3 t = float3(tn.xy + N.xz, abs(tn.z) * N.y);
    return normalize(t.xzy);
}

struct Layer
{
    float4 albedo; // rgb 颜色，a 高度
    float3 normal; // 世界空间
};

// 远处平铺的重复感：同一张贴图再按约 5 倍尺寸采样一次，随距离逐渐混进来。
// 两个尺度的图案叠在一起就看不出周期了（5.3 不是整数倍，两套网格不会对齐）。
static const float kFarTilingScale = 5.3;

float FarBlend(float3 p)
{
    return saturate((length(gFrame.cameraPos.xyz - p) - 25.0) / 120.0) * 0.6;
}

Layer SamplePlanar(Texture2D<float4> albedoTex, Texture2D<float4> normalTex, float3 p, float tiling, float3 N)
{
    float2 uv = p.xz / tiling;
    float far = FarBlend(p);
    float2 uvFar = uv / kFarTilingScale;
    Layer l;
    l.albedo = lerp(albedoTex.Sample(gLinearWrap, uv), albedoTex.Sample(gLinearWrap, uvFar), far);
    float3 tn = lerp(UnpackXY(normalTex.Sample(gLinearWrap, uv)), UnpackXY(normalTex.Sample(gLinearWrap, uvFar)), far);
    l.normal = PlanarNormal(tn, N);
    return l;
}

// 三平面投影：分别沿 X、Y、Z 轴投影采样三次，按法线朝向加权。陡坡和悬崖上不会出现拉伸
Layer SampleTriplanar(Texture2D<float4> albedoTex, Texture2D<float4> normalTex, float3 p, float tiling, float3 N)
{
    float3 blend = pow(abs(N), 4.0);
    blend /= dot(blend, 1.0);
    float3 axisSign = 1.0 - 2.0 * (float3)(N < 0.0); // HLSL 2021 的 ?: 只接受标量条件
    // 背面投影时把 u 镜像，否则贴图（和法线的 x 分量）是反的
    float2 uvX = float2(p.z * axisSign.x, p.y) / tiling;
    float2 uvY = float2(p.x * axisSign.y, p.z) / tiling;
    float2 uvZ = float2(-p.x * axisSign.z, p.y) / tiling;

    float4 ax = albedoTex.Sample(gLinearWrap, uvX);
    float4 ay = albedoTex.Sample(gLinearWrap, uvY);
    float4 az = albedoTex.Sample(gLinearWrap, uvZ);
    float3 nx = UnpackXY(normalTex.Sample(gLinearWrap, uvX));
    float3 ny = UnpackXY(normalTex.Sample(gLinearWrap, uvY));
    float3 nz = UnpackXY(normalTex.Sample(gLinearWrap, uvZ));
    nx.x *= axisSign.x;
    ny.x *= axisSign.y;
    nz.x *= -axisSign.z;
    // whiteout 混合后按轴交换回世界空间（Ben Golus《Normal Mapping for a Triplanar Shader》）
    nx = float3(nx.xy + N.zy, abs(nx.z) * N.x);
    ny = float3(ny.xy + N.xz, abs(ny.z) * N.y);
    nz = float3(nz.xy + N.xy, abs(nz.z) * N.z);

    Layer l;
    l.albedo = ax * blend.x + ay * blend.y + az * blend.z;
    l.normal = normalize(nx.zyx * blend.x + ny.xzy * blend.y + nz.xyz * blend.z);
    return l;
}

float3 LodColor(float level)
{
    static const float3 kColors[8] = {float3(1, 0.2, 0.2), float3(1, 0.6, 0.1), float3(1, 1, 0.2), float3(0.3, 1, 0.3),
                                      float3(0.2, 0.9, 1), float3(0.3, 0.4, 1), float3(0.8, 0.3, 1), float3(1, 1, 1)};
    return kColors[min((uint)level, 7u)];
}

SurfaceData BuildSurface(TerrainVSOut i)
{
    float3 p = i.worldPos;
    float3 N = normalize(gNormalMap.Sample(gLinearClamp, i.heightUV).xyz * 2.0 - 1.0);
    float slope = 1.0 - N.y;
    // 分层用的高度加一点大尺度扰动：否则层与层的分界就是一条笔直的等高线。
    // 噪声源直接借用苔藓贴图的高度通道，按 ~70 m 一个周期采样（两个不相关的尺度叠加，看不出重复）
    float macro = gLayerAlbedo3.SampleLevel(gLinearWrap, p.xz / 71.0, 4.0).a + gLayerAlbedo1.SampleLevel(gLinearWrap, p.xz / 23.0, 3.0).a;
    float h = p.y + (macro - 1.0) * 2.5;

    // ---- 1. 基础权重：高度决定沙/礁/苔，坡度决定岩石 ----
    float sandMax = gTerrain.layers.x, reefMax = gTerrain.layers.y, rockSlope = gTerrain.layers.z, mossMin = gTerrain.layers.w;
    float reef = 1.0 - smoothstep(reefMax - 1.0, reefMax + 1.0, h);
    float sand = (1.0 - reef) * (1.0 - smoothstep(sandMax - 1.0, sandMax + 1.0, h));
    float moss = smoothstep(sandMax - 1.0, mossMin, h);
    float rock = smoothstep(rockSlope - 0.08, rockSlope + 0.08, slope);
    float4 w = float4(sand, rock, reef, moss) * float4(1.0 - rock, 1.0, 1.0 - rock, 1.0 - rock);

    Layer l0 = SamplePlanar(gLayerAlbedo0, gLayerNormal0, p, gTerrain.tiling.x, N);
    Layer l1 = SampleTriplanar(gLayerAlbedo1, gLayerNormal1, p, gTerrain.tiling.y, N);
    Layer l2 = SamplePlanar(gLayerAlbedo2, gLayerNormal2, p, gTerrain.tiling.z, N);
    Layer l3 = SamplePlanar(gLayerAlbedo3, gLayerNormal3, p, gTerrain.tiling.w, N);

    // ---- 2. 高度混合：权重 + 层高度，只保留最高的那一档（宽度 = blendSharpness）----
    float4 heights = float4(l0.albedo.a, l1.albedo.a, l2.albedo.a, l3.albedo.a);
    float4 v = w + heights;
    float top = max(max(v.x, v.y), max(v.z, v.w)) - max(gTerrain.misc.x, 1e-3);
    float4 b = max(v - top, 0.0) * step(1e-3, w);
    b /= max(dot(b, 1.0), 1e-5);

    SurfaceData s;
    s.position = p;
    s.viewDir = normalize(gFrame.cameraPos.xyz - p);
    s.geometricNormal = N;
    s.normal = normalize(l0.normal * b.x + l1.normal * b.y + l2.normal * b.z + l3.normal * b.w);
    s.albedo = l0.albedo.rgb * b.x + l1.albedo.rgb * b.y + l2.albedo.rgb * b.z + l3.albedo.rgb * b.w;
    s.roughness = dot(gTerrain.roughness, b);
    s.alpha = 1.0;
    s.metallic = 0.0;
    // 用层高度当作廉价的 AO：缝隙（高度低处）稍暗
    s.ao = lerp(0.6, 1.0, saturate(dot(heights, b) * 1.5));
    s.emissive = 0.0;
    s.shadingModel = SHADING_MODEL_PBR;

    // ---- 3. 潮湿：海平面附近被海水打湿 → 颜色变深、表面变光滑（用真实高度，水线要平） ----
    float wet = 1.0 - smoothstep(0.0, 0.8, p.y);
    s.albedo *= lerp(1.0, 0.55, wet);
    s.roughness = lerp(s.roughness, 0.3, wet * 0.8);

    if (gTerrain.misc.w > 0.5)
        s.albedo = lerp(s.albedo, LodColor(gTerrain.node.w), 0.6);
    return s;
}

float4 PSMain(TerrainVSOut i) : SV_Target
{
    SurfaceData s = BuildSurface(i);
    MediumSample medium = SampleMedium(s.position);
    ApplyMediumToSurface(s, medium);

    LightingInputs L = GetLightingInputs();
    L.sunShadow = SampleSunShadow(gShadowAtlas, s.position, s.geometricNormal);
    float3 color = ShadeSurface(s, L);
    color = ApplyMediumToRadiance(color, s, medium, L);

    uint view = (uint)gFrame.debug.x;
    if (view == 1) color = s.normal * 0.5 + 0.5;
    else if (view == 2) color = s.albedo;
    else if (view == 3) color = s.roughness.xxx;
    else if (view == 4) color = s.metallic.xxx;
    else if (view == 5) color = s.ao.xxx;
    return float4(color, 1.0);
}
