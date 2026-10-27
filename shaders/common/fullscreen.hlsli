// 全屏三角形：用一个覆盖整个屏幕的大三角形代替两个三角形组成的矩形。
// 好处：没有对角线接缝处的重复着色（GPU 按 2x2 像素块着色，接缝处会浪费），也不需要顶点缓冲。
//
//   id=0 → uv(0,0)  id=1 → uv(2,0)  id=2 → uv(0,2)
//   超出 [0,1] 的部分被视口裁掉，屏幕内的 uv 正好是 [0,1]。
#pragma once

struct FullscreenVSOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0; // (0,0) = 屏幕左上角，与贴图坐标一致
};

FullscreenVSOut FullscreenTriangle(uint id)
{
    FullscreenVSOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    // 引擎使用负高度视口（NDC y=+1 在屏幕顶部），所以 uv.y=0 对应 ndc.y=+1
    o.position = float4(o.uv.x * 2.0 - 1.0, 1.0 - o.uv.y * 2.0, 0.0, 1.0);
    return o;
}
