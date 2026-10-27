// 色调映射：HDR 场景颜色 → 交换链（sRGB 编码）。
// 顺序：曝光 → 色调曲线（ACES / AgX / 无）→ 线性转 sRGB。
#include "common/frame.hlsli"
#include "common/fullscreen.hlsli"
#include "common/color.hlsli"
#include "common/tonemap.hlsli"

[[vk::binding(0, 1)]] Texture2D<float4> gSceneColor;

FullscreenVSOut VSMain(uint id : SV_VertexID)
{
    return FullscreenTriangle(id);
}

float4 PSMain(FullscreenVSOut i) : SV_Target
{
    float3 hdr = gSceneColor.SampleLevel(gLinearClamp, i.uv, 0).rgb;
    // 调试视图（法线、粗糙度……）要看原始数值，不做曝光和色调映射
    if (gFrame.debug.x > 0.5)
        return float4(LinearToSRGB(hdr), 1.0);

    hdr *= gFrame.exposure.x;
    uint mode = (uint)gFrame.exposure.y;
    float3 ldr = mode == 0 ? TonemapACES(hdr) : (mode == 1 ? TonemapAgX(hdr) : saturate(hdr));
    return float4(LinearToSRGB(ldr), 1.0);
}
