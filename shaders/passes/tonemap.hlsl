// 色调映射：HDR 场景颜色 → 交换链。
#include "common/frame.hlsli"
#include "common/fullscreen.hlsli"
#include "common/color.hlsli"

[[vk::binding(0, 1)]] Texture2D<float4> gSceneColor;

FullscreenVSOut VSMain(uint id : SV_VertexID)
{
    return FullscreenTriangle(id);
}

float4 PSMain(FullscreenVSOut i) : SV_Target
{
    float3 hdr = gSceneColor.SampleLevel(gLinearClamp, i.uv, 0).rgb * gFrame.exposure.x;
    // EXTEND: M3 在这里加入 ACES / AgX 曲线
    return float4(LinearToSRGB(hdr), 1.0);
}
