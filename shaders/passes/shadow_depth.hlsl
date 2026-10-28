// 阴影深度：用光源的视图投影矩阵变换顶点，只写深度。级联序号放在 push constant 的材质参数空位里。
#include "common/mesh.hlsli"

// 只声明位置属性：与 C++ 端（ShadowPass::init）裁剪后的顶点输入一致
float4 VSMain([[vk::location(0)]] float3 position : POSITION) : SV_Position
{
    uint cascade = (uint)gDraw.material.flags.w;
    float4 world = mul(gDraw.model, float4(position, 1.0));
    return mul(gFrame.shadowViewProj[cascade], world);
}

// 没有颜色输出的片元着色器：深度由固定功能写入
void PSMain()
{
}
