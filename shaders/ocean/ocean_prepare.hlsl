// 画水面之前，把当前的场景颜色和深度各拷一份：
//   颜色 → 折射（透过水面看到的海床/礁石）
//   深度 → 线性视空间距离：算水的厚度（RGB 吸收）、岸边泡沫
// 为什么要拷：水面 pass 要往场景颜色/深度里写，同一个 pass 不能既把它当附件写、又把它当贴图采样。
#include "common/fullscreen.hlsli"

[[vk::binding(0, 1)]] Texture2D<float4> gSceneColor;
[[vk::binding(1, 1)]] Texture2D<float> gSceneDepth;

struct PreparePush
{
    float4 camera; // x = 近平面
};
[[vk::push_constant]] PreparePush gPrep;

FullscreenVSOut VSMain(uint id : SV_VertexID)
{
    return FullscreenTriangle(id);
}

struct PrepareOut
{
    float4 color : SV_Target0;
    float linearDepth : SV_Target1;
};

PrepareOut PSMain(FullscreenVSOut i)
{
    const int3 p = int3(i.position.xy, 0);
    PrepareOut o;
    o.color = gSceneColor.Load(p);
    // 反向 Z 无限远投影：深度 = 近平面 / 视空间距离；深度 0 = 无穷远（天空）
    const float d = gSceneDepth.Load(p);
    o.linearDepth = d > 0.0 ? gPrep.camera.x / d : 1.0e6;
    return o;
}
