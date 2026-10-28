// 级联阴影采样。所有材质（PBR、地形、以后的 toon）共用 —— 角色与环境拿到同一份阴影。
//
// 阴影图集：4 个级联放在 4 个象限（0 左上，1 右上，2 左下，3 右下）。
// 反向 Z：阴影图里存的是离光源最近的遮挡物深度（值越大越近）。
// 片元被照亮 ⇔ 片元深度 ≥ 遮挡物深度（片元在遮挡物前面或就是它自己）。
#pragma once
#include "common/frame.hlsli"

// 返回 1 = 完全照亮，0 = 完全在阴影里。shadowMap 由调用者从自己的 set 1 传入
// （被关掉的阴影 pass 会被 Render Graph 换成「深度 0」的默认图 → 处处照亮）。
float SampleSunShadow(Texture2D<float> shadowMap, float3 worldPos, float3 normal)
{
    if (gFrame.shadowParams.w < 0.5)
        return 1.0;
    float viewDepth = -mul(gFrame.view, float4(worldPos, 1.0)).z;

    // 选择级联：第一个覆盖到这个距离的
    uint cascade = 4;
    [unroll] for (uint i = 0; i < 4; ++i)
    {
        if (viewDepth < gFrame.cascadeSplits[i]) { cascade = i; break; }
    }
    if (cascade >= 4)
        return 1.0;

    // 法线偏移：沿法线把采样点挪出表面一点（单位：该级联的纹素大小），掠射角下的阴影痤疮主要靠它消除
    float texel = gFrame.cascadeTexel[cascade];
    float3 p = worldPos + normal * gFrame.shadowParams.y * texel;
    float4 clip = mul(gFrame.shadowViewProj[cascade], float4(p, 1.0));
    // 渲染阴影时用的也是负高度视口，所以 y 要翻转：NDC y = +1 对应图像顶部（v = 0）
    float2 uv = float2(clip.x * 0.5 + 0.5, 0.5 - clip.y * 0.5);
    if (any(uv < 0.0) || any(uv > 1.0))
        return 1.0;

    float2 atlasSize;
    shadowMap.GetDimensions(atlasSize.x, atlasSize.y);
    float2 halfTexel = 0.5 / (atlasSize * 0.5);
    uv = clamp(uv, halfTexel, 1.0 - halfTexel); // 不让 PCF 采到相邻级联
    float2 atlasUV = (uv + float2(cascade & 1, cascade >> 1)) * 0.5;
    float ref = clip.z + gFrame.shadowParams.x;

    // PCF：在周围多点做深度比较再平均（每次比较本身又是硬件 2×2 双线性），得到柔和的阴影边缘
    float radius = gFrame.shadowParams.z / atlasSize.x;
    float sum = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x)
            sum += shadowMap.SampleCmpLevelZero(gShadowSampler, atlasUV + float2(x, y) * radius, ref);
    float lit = sum / 9.0;

    // 最后一个级联的末尾淡出，避免阴影在远处突然消失
    float fade = saturate((gFrame.cascadeSplits[3] - viewDepth) / (gFrame.cascadeSplits[3] * 0.1));
    return lerp(1.0, lit, cascade == 3 ? fade : 1.0);
}
