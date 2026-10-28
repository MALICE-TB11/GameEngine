// 海面：几何位移 + 着色。
//
// 网格：以相机为中心的极坐标网格（没有顶点缓冲，SV_VertexID → 第几环、第几段）。
//   环半径按几何级数增长：近处密、远处疏，一张网格从脚下一直铺到地平线。
//   EXTEND: 换成 geometry clipmap（嵌套方形网格 + 网格对齐），远处顶点不会随相机移动而「游动」。
// 位移：FFT 模式采样三个级联的位移贴图；Gerstner 模式直接对几十个波求和（参考实现）。
//   顶点间距大于波长的级联不参与几何位移（采样不足会抖动），只留给像素着色器做法线。
// 着色（和其他材质共用 LightingInputs、BRDF、阴影；水面是介质边界，所以自己处理反射 + 折射）：
//   反射 = 天空 × 菲涅尔（F0 = 0.02）+ 太阳 GGX 高光
//   折射 = 拷贝的场景颜色 × e^{-σ·厚度}（RGB 各自吸收：红光最先消失）+ 水体散射
//   浪尖透光（SSS）、泡沫（雅可比累积 + 岸边水深）
#include "common/lighting.hlsli"
#include "common/sky.hlsli"
#include "common/brdf.hlsli"
#include "common/shadow.hlsli"

// 必须与 OceanRenderer.cpp 的 SurfacePush 一致（128 字节）
struct OceanSurfacePush
{
    float4 sizes;      // xyz = 级联尺寸（米），w = 标志位（OCEAN_FLAG_*）
    float4 grid;       // xy = 网格中心（相机 XZ），z = 第一环半径，w = 最外环半径
    float4 gridDims;   // x = 环数，y = 每环分段数，z = Gerstner 波数，w = 有效波高 Hs（米）
    float4 water;      // x = 折射强度，y = 粗糙度，z = 浪尖透光强度，w = 岸边泡沫宽度（米）
    float4 absorption; // rgb = 吸收系数（1/米），w = 天空反射强度
    float4 scatter;    // rgb = 水体散射颜色，w = 泡沫细节贴图尺寸（米）
    float4 sss;        // rgb = 浪尖透光颜色，w = 相位时间（秒，已对循环周期取模）
    float4 foam;       // x = 起泡阈值，y = 泡沫强度，z = 尖锐度 λ，w = 岸边涌浪的相位（弧度）
};
[[vk::push_constant]] OceanSurfacePush gOcean;

static const uint OCEAN_FLAG_FFT = 1;   // 1 = FFT 模式，0 = Gerstner
static const uint OCEAN_FLAG_SCENE = 2; // 有场景颜色/深度的拷贝（OceanPrepare 执行了）
bool HasFlag(uint flag)
{
    return ((uint)gOcean.sizes.w & flag) != 0;
}

[[vk::binding(0, 1)]] Texture2DArray<float4> gDisplacement; // FFT：xyz 位移，w 泡沫
[[vk::binding(1, 1)]] Texture2DArray<float4> gDerivatives;  // FFT：∂y/∂x, ∂y/∂z, λ∂Dx/∂x, λ∂Dz/∂z
[[vk::binding(2, 1)]] Texture2D<float4> gSceneColorCopy;
[[vk::binding(3, 1)]] Texture2D<float> gSceneDepthLinear;   // 视空间距离（米），天空 = 1e6；没有 OCEAN_FLAG_SCENE 时是默认贴图
[[vk::binding(4, 1)]] Texture2D<float> gShadowAtlas;
[[vk::binding(5, 1)]] Texture2D<float4> gFoamTex;
// Gerstner 波：A = (方向 xy, 波数 k, 角频率 ω)，B = (振幅, 初相位, Q, 0)
[[vk::binding(6, 1)]] cbuffer GerstnerWaves
{
    float4 gWaveA[64];
    float4 gWaveB[64];
};

static const float F0_WATER = 0.02; // 水的折射率 1.33 → ((1.33-1)/(1.33+1))² ≈ 0.02

// ---------------------------------------------------------------- 位移

// 级联 c 对「采样间距为 spacing 米」的位置的权重：间距超过波长的 1/8 开始淡出，1/4 时完全不参与
float CascadeWeight(uint c, float spacing)
{
    return saturate(2.0 - spacing * 8.0 / gOcean.sizes[c]);
}

float3 FftDisplacement(float2 xz, float spacing)
{
    float3 d = 0.0;
    [unroll] for (uint c = 0; c < 3; ++c)
    {
        const float w = CascadeWeight(c, spacing);
        if (w > 0.0)
            d += gDisplacement.SampleLevel(gLinearWrap, float3(xz / gOcean.sizes[c], c), 0).xyz * w;
    }
    return d;
}

// Gerstner：y = Σ a cos θ，水平 = -Σ Q a d sin θ，θ = k(d·x) - ωt + φ（与 C++ 的 gerstnerDisplacement 相同）
float3 GerstnerDisplacement(float2 xz, float spacing)
{
    float3 d = 0.0;
    const uint count = (uint)gOcean.gridDims.z;
    for (uint i = 0; i < count; ++i)
    {
        const float4 A = gWaveA[i], B = gWaveB[i];
        const float wavelength = 2.0 * PI / A.z;
        const float w = saturate(2.0 - spacing * 8.0 / wavelength);
        const float theta = A.z * dot(A.xy, xz) - A.w * gOcean.sss.w + B.y;
        const float s = sin(theta), c = cos(theta);
        d += float3(-B.z * B.x * A.x * s, B.x * c, -B.z * B.x * A.y * s) * w;
    }
    return d;
}

// ---------------------------------------------------------------- 顶点

struct OceanVSOut
{
    float4 position : SV_Position;
    float3 worldPos : WORLDPOS;
    float2 lagrangian : TEXCOORD0; // 未位移的 XZ：所有贴图都按它采样
    float4 clip : TEXCOORD1;
};

OceanVSOut VSMain(uint vid : SV_VertexID)
{
    const uint rings = (uint)gOcean.gridDims.x;
    const uint segments = (uint)gOcean.gridDims.y;
    const uint ring = vid / (segments + 1);
    const uint seg = vid % (segments + 1);

    // 第 0 环是圆心；之后的环半径按几何级数从 inner 增长到 outer
    const float t = float(ring - 1) / float(max(rings - 2, 1u));
    const float radius = ring == 0 ? 0.0 : gOcean.grid.z * pow(gOcean.grid.w / gOcean.grid.z, t);
    const float angle = float(seg) / float(segments) * 2.0 * PI;
    const float2 xz = gOcean.grid.xy + radius * float2(cos(angle), sin(angle));
    // 这个顶点附近的网格间距（取径向和切向的较大者）
    const float growth = pow(gOcean.grid.w / gOcean.grid.z, 1.0 / float(max(rings - 2, 1u)));
    const float spacing = max(radius * (growth - 1.0), radius * 2.0 * PI / float(segments)) + 0.02;

    const float3 d = HasFlag(OCEAN_FLAG_FFT) ? FftDisplacement(xz, spacing) : GerstnerDisplacement(xz, spacing);
    OceanVSOut o;
    o.worldPos = float3(xz.x + d.x, d.y, xz.y + d.z);
    o.lagrangian = xz;
    o.position = mul(gFrame.viewProj, float4(o.worldPos, 1.0));
    o.clip = o.position;
    return o;
}

// ---------------------------------------------------------------- 像素

struct WaveSample
{
    float3 normal;
    float foam;
};

// FFT：三个级联的斜率相加；远处按像素覆盖的尺寸淡出小级联（否则闪烁），损失的粗糙度在高光里补回
WaveSample SampleFft(float2 xz, float footprint)
{
    float2 slope = 0.0;
    float2 dDiag = 0.0;
    float foam = 0.0;
    [unroll] for (uint c = 0; c < 3; ++c)
    {
        const float w = saturate(2.0 - footprint * 64.0 / gOcean.sizes[c]);
        if (w <= 0.0)
            continue;
        const float3 uvw = float3(xz / gOcean.sizes[c], c);
        const float4 der = gDerivatives.Sample(gLinearWrap, uvw);
        slope += der.xy * w;
        dDiag += der.zw * w;
        foam += gDisplacement.Sample(gLinearWrap, uvw).w * w;
    }
    WaveSample s;
    // 位移后的表面斜率 = ∂y/∂x0 ÷ ∂x/∂x0（链式法则）
    s.normal = normalize(float3(-slope.x / (1.0 + dDiag.x), 1.0, -slope.y / (1.0 + dDiag.y)));
    s.foam = foam;
    return s;
}

// Gerstner：解析法线与雅可比（参考实现的泡沫没有累积，只看当前时刻的挤压程度）
WaveSample SampleGerstner(float2 xz, float footprint)
{
    float sxx = 0.0, szz = 0.0, sxz = 0.0, gx = 0.0, gz = 0.0;
    const uint count = (uint)gOcean.gridDims.z;
    for (uint i = 0; i < count; ++i)
    {
        const float4 A = gWaveA[i], B = gWaveB[i];
        const float wavelength = 2.0 * PI / A.z;
        const float w = saturate(2.0 - footprint * 64.0 / wavelength);
        const float theta = A.z * dot(A.xy, xz) - A.w * gOcean.sss.w + B.y;
        const float s = sin(theta), c = cos(theta);
        const float ka = A.z * B.x * w;
        gx += ka * A.x * s;
        gz += ka * A.y * s;
        sxx += B.z * ka * A.x * A.x * c;
        szz += B.z * ka * A.y * A.y * c;
        sxz += B.z * ka * A.x * A.y * c;
    }
    const float3 tx = float3(1.0 - sxx, -gx, -sxz);
    const float3 tz = float3(-sxz, -gz, 1.0 - szz);
    WaveSample s;
    s.normal = normalize(cross(tz, tx));
    const float jacobian = (1.0 - sxx) * (1.0 - szz) - sxz * sxz;
    s.foam = saturate((gOcean.foam.x - jacobian) * gOcean.foam.y);
    return s;
}

float4 PSMain(OceanVSOut i, bool frontFace : SV_IsFrontFace) : SV_Target
{
    LightingInputs L = GetLightingInputs();
    const float3 toCam = gFrame.cameraPos.xyz - i.worldPos;
    const float dist = length(toCam);
    const float3 V = toCam / max(dist, 1e-4);
    // 一个像素大约覆盖多少米：视距 × 每像素张角（约 1/1000 弧度，足够做淡出判断）
    const float footprint = dist * 0.001;

    WaveSample ws;
    if (HasFlag(OCEAN_FLAG_FFT))
        ws = SampleFft(i.lagrangian, footprint);
    else
        ws = SampleGerstner(i.lagrangian, footprint);
    float3 N = ws.normal;
    if (!frontFace)
        N = -N; // 从水下往上看（M6 再做完整的水下效果）

    // ---- 屏幕坐标与水的厚度 ----
    const float2 ndc = i.clip.xy / i.clip.w;
    const float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    const float waterDepth = i.clip.w; // 水面的视空间距离
    const float rayScale = dist / max(waterDepth, 1e-4); // 视空间距离 → 沿视线的距离
    // 没有场景拷贝（OceanPrepare 被关掉）时，默认贴图里的数不是距离（白色 = 1 米会让整片海「零厚度」）：
    // 当作水底无穷远 —— 全部按深水着色，也没有岸边泡沫
    const bool hasScene = HasFlag(OCEAN_FLAG_SCENE);
    const float sceneDepth = hasScene ? gSceneDepthLinear.SampleLevel(gPointClamp, uv, 0) : 1.0e6;

    // 折射：沿法线扰动采样位置；扰动后采到的东西如果在水面之前（例如岸上的石头），就退回不扰动
    const float thickness0 = max(sceneDepth - waterDepth, 0.0) * rayScale;
    float2 uvR = uv + N.xz * gOcean.water.x * saturate(thickness0 * 0.5);
    float depthR = hasScene ? gSceneDepthLinear.SampleLevel(gPointClamp, uvR, 0) : 1.0e6;
    if (depthR < waterDepth)
    {
        uvR = uv;
        depthR = sceneDepth;
    }
    const float thickness = max(depthR - waterDepth, 0.0) * rayScale;
    const float3 refracted = gSceneColorCopy.SampleLevel(gLinearClamp, uvR, 0).rgb;

    // ---- 透射：RGB 分别按 Beer–Lambert 吸收，吸收掉的部分被水体散射颜色替代 ----
    const float3 T = exp(-gOcean.absorption.rgb * thickness);
    const float3 waterLight = AmbientRadiance(float3(0, 1, 0), L) * L.ambientIntensity + L.sunIrradiance * saturate(L.sunDir.y) * 0.25;
    const float3 body = gOcean.scatter.rgb * waterLight;
    float3 transmitted = refracted * T + body * (1.0 - T);

    // ---- 反射：天空 × 菲涅尔 + 太阳高光（远处小级联被淡出，粗糙度补回一些） ----
    const float NoV = saturate(dot(N, V));
    const float fresnel = F0_WATER + (1.0 - F0_WATER) * pow(1.0 - NoV, 5.0);
    float3 R = reflect(-V, N);
    // 指向水平面以下的反射线实际会打到另一个浪的背面：以前把它钳到地平线，等于反射了天空最亮的一圈，
    // 整片海都发白。现在镜像到上半球取天空，再按「朝下多深」向水体颜色过渡（被别的浪挡住）
    const float below = saturate(-R.y * 4.0);
    R.y = abs(R.y);
    const float3 sky = lerp(SkyRadiance(normalize(R), L, false), body, below) * gOcean.absorption.w;

    const float roughness = lerp(gOcean.water.y, 0.25, saturate(dist / 4000.0));
    const float a = roughness * roughness;
    const float3 H = normalize(V + L.sunDir);
    const float NoL = saturate(dot(N, L.sunDir));
    const float NoH = saturate(dot(N, H));
    const float shadow = SampleSunShadow(gShadowAtlas, i.worldPos, float3(0, 1, 0));
    const float3 specular = D_GGX(NoH, a) * V_SmithGGXCorrelated(max(NoV, 1e-3), NoL, a) *
                            F_Schlick(saturate(dot(V, H)), F0_WATER.xxx) * L.sunIrradiance * NoL * shadow;

    // ---- 浪尖透光：逆光看浪头时，阳光穿过薄薄的浪尖带出青绿色 ----
    const float crest = saturate(i.worldPos.y / max(gOcean.gridDims.w * 0.5, 0.05));
    const float backlit = pow(saturate(dot(V, -L.sunDir) * 0.5 + 0.5), 4.0) * (1.0 - saturate(N.y * 0.8));
    const float3 sss = gOcean.sss.rgb * L.sunIrradiance * backlit * crest * gOcean.water.z * shadow;

    float3 color = transmitted * (1.0 - fresnel) + sky * fresnel + specular + sss;

    // ---- 泡沫：累积的破浪泡沫 + 岸边浅水泡沫，用细节贴图打碎成一团一团 ----
    const float detail = gFoamTex.Sample(gLinearWrap, i.lagrangian / gOcean.scatter.w).r;
    // 覆盖率决定阈值：覆盖率低时只有细节贴图最亮的气泡边露出来，覆盖率高时连成一片
    const float waveFoam = smoothstep(1.0 - ws.foam, 1.25 - ws.foam, detail);
    const float verticalDepth = thickness * abs(V.y);
    const float shore = saturate(1.0 - verticalDepth / max(gOcean.water.w, 0.01));
    const float surf = 0.6 + 0.4 * sin(gOcean.foam.w - verticalDepth * 2.5); // 一波一波往岸上推
    const float shoreCover = shore * surf;
    const float shoreFoam = sceneDepth < 1.0e5 ? smoothstep(1.0 - shoreCover, 1.25 - shoreCover, detail) : 0.0;
    const float foam = saturate(max(waveFoam, shoreFoam));
    const float3 foamColor = 0.85 * (L.sunIrradiance * NoL * shadow / PI + AmbientRadiance(N, L) * L.ambientIntensity);
    color = lerp(color, foamColor, foam);

    // 调试视图：1 = 法线，2 = 泡沫
    const uint view = (uint)gFrame.debug.x;
    if (view == 1)
        color = N * 0.5 + 0.5;
    else if (view == 2)
        color = foam.xxx;
    return float4(color, 1.0);
}
