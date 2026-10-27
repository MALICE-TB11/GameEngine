// 天空背景：只在深度仍为 0（无穷远）的像素上运行（深度测试见 SkyPass.cpp）。
#include "common/frame.hlsli"
#include "common/fullscreen.hlsli"
#include "common/sky.hlsli"

FullscreenVSOut VSMain(uint id : SV_VertexID)
{
    return FullscreenTriangle(id);
}

float4 PSMain(FullscreenVSOut i) : SV_Target
{
    // 由屏幕坐标反推视线方向：把近平面（反向 Z 下深度 = 1）上的点变回世界空间，
    // 减去相机位置就是方向。不能用深度 0：无穷远平面反投影后 w = 0。
    float2 ndc = float2(i.uv.x * 2.0 - 1.0, 1.0 - i.uv.y * 2.0);
    float4 p = mul(gFrame.invViewProj, float4(ndc, 1.0, 1.0));
    float3 dir = normalize(p.xyz / p.w - gFrame.cameraPos.xyz);
    return float4(SkyRadiance(dir, GetLightingInputs(), true), 1.0);
}
