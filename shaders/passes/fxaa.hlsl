// FXAA（Lottes 2009 的精简版）：在 LDR（已 sRGB 编码）图像上检测亮度边缘，沿边缘方向混合。
//
// 思路：
//   1. 取中心与上下左右的亮度，对比度太低（平坦区域）就直接输出 —— 大部分像素在这一步就结束
//   2. 用 3×3 邻域估计边缘方向（水平还是竖直、法线朝哪边）
//   3. 沿垂直于边缘的方向，在两侧取样混合，混合比例由局部对比度决定
#include "common/frame.hlsli"
#include "common/fullscreen.hlsli"

[[vk::binding(0, 1)]] Texture2D<float4> gLdr;

FullscreenVSOut VSMain(uint id : SV_VertexID)
{
    return FullscreenTriangle(id);
}

float Luma(float3 c)
{
    return dot(c, float3(0.299, 0.587, 0.114)); // sRGB 编码值上的感知亮度
}

float4 PSMain(FullscreenVSOut i) : SV_Target
{
    float2 texel = gFrame.screen.zw;
    float3 cM = gLdr.SampleLevel(gLinearClamp, i.uv, 0).rgb;
    float lM = Luma(cM);
    float lN = Luma(gLdr.SampleLevel(gLinearClamp, i.uv + float2(0, -texel.y), 0).rgb);
    float lS = Luma(gLdr.SampleLevel(gLinearClamp, i.uv + float2(0, texel.y), 0).rgb);
    float lW = Luma(gLdr.SampleLevel(gLinearClamp, i.uv + float2(-texel.x, 0), 0).rgb);
    float lE = Luma(gLdr.SampleLevel(gLinearClamp, i.uv + float2(texel.x, 0), 0).rgb);

    float lMin = min(lM, min(min(lN, lS), min(lW, lE)));
    float lMax = max(lM, max(max(lN, lS), max(lW, lE)));
    float contrast = lMax - lMin;
    // 阈值：绝对 0.0312，相对 12.5%（Lottes 推荐的「质量」档）
    if (contrast < max(0.0312, lMax * 0.125))
        return float4(cM, 1.0);

    float lNW = Luma(gLdr.SampleLevel(gLinearClamp, i.uv + float2(-texel.x, -texel.y), 0).rgb);
    float lNE = Luma(gLdr.SampleLevel(gLinearClamp, i.uv + float2(texel.x, -texel.y), 0).rgb);
    float lSW = Luma(gLdr.SampleLevel(gLinearClamp, i.uv + float2(-texel.x, texel.y), 0).rgb);
    float lSE = Luma(gLdr.SampleLevel(gLinearClamp, i.uv + float2(texel.x, texel.y), 0).rgb);

    // 边缘方向：水平方向的亮度二阶差分大 → 竖直边缘，反之亦然
    float horizontal = abs(lN + lS - 2.0 * lM) * 2.0 + abs(lNE + lSE - 2.0 * lE) + abs(lNW + lSW - 2.0 * lW);
    float vertical = abs(lE + lW - 2.0 * lM) * 2.0 + abs(lNE + lNW - 2.0 * lN) + abs(lSE + lSW - 2.0 * lS);
    bool isHorizontal = horizontal >= vertical;

    // 边缘法线朝亮度变化更大的一侧
    float lPos = isHorizontal ? lS : lE;
    float lNeg = isHorizontal ? lN : lW;
    float step = isHorizontal ? texel.y : texel.x;
    if (abs(lNeg - lM) > abs(lPos - lM))
        step = -step;

    // 混合比例：局部平均亮度与中心的差，归一化后平滑（亚像素混叠的强度）
    float avg = (2.0 * (lN + lS + lW + lE) + lNW + lNE + lSW + lSE) / 12.0;
    float blend = saturate(abs(avg - lM) / contrast);
    blend = smoothstep(0.0, 1.0, blend);
    blend = blend * blend * 0.75;

    // 朝边缘另一侧偏移 blend 个像素做一次双线性采样：硬件插值自动完成「按比例混合两侧颜色」
    float2 offset = isHorizontal ? float2(0, step) : float2(step, 0);
    return float4(gLdr.SampleLevel(gLinearClamp, i.uv + offset * blend, 0).rgb, 1.0);
}
