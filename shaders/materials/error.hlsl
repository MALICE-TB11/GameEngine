// 错误材质：品红/黑 斜条纹，不受光照影响，一眼就能看出「这里的材质坏了」。
// 刻意不依赖任何贴图、材质参数或其它 include（除了网格输入），保证它几乎总能编译成功。
#include "common/mesh.hlsli"

MeshVSOut VSMain(VSIn i)
{
    return MeshVertex(i);
}

float4 PSMain(MeshVSOut i) : SV_Target
{
    float stripe = frac((i.worldPos.x + i.worldPos.y + i.worldPos.z) * 2.0);
    float3 magenta = float3(1.0, 0.0, 1.0) * 4.0; // HDR 值：色调映射后依旧醒目
    return float4(stripe < 0.5 ? magenta : float3(0, 0, 0), 1.0);
}
