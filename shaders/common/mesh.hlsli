// 网格的顶点输入与通用顶点着色器。
// VSIn 必须与 engine/render/Mesh.h 的 Vertex 及 setMeshVertexInput 的 location 一致。
#pragma once
#include "common/frame.hlsli"

struct DrawPush
{
    float4x4 model;
    float4x4 normalMatrix; // 模型矩阵的逆转置
};
[[vk::push_constant]] DrawPush gDraw;

struct VSIn
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
    [[vk::location(3)]] float4 tangent : TANGENT;
};

struct MeshVSOut
{
    float4 position : SV_Position;
    float3 worldPos : WORLDPOS;
    float3 normal : NORMAL;
    float4 tangent : TANGENT;
    float2 uv : TEXCOORD0;
};

MeshVSOut MeshVertex(VSIn i)
{
    MeshVSOut o;
    float4 world = mul(gDraw.model, float4(i.position, 1.0));
    o.worldPos = world.xyz;
    o.position = mul(gFrame.viewProj, world);
    // 法线用逆转置矩阵变换：非均匀缩放时，直接用模型矩阵会让法线不再垂直于表面
    o.normal = mul((float3x3)gDraw.normalMatrix, i.normal);
    // 切线在表面内，跟随模型矩阵变换
    o.tangent = float4(mul((float3x3)gDraw.model, i.tangent.xyz), i.tangent.w);
    o.uv = i.uv;
    return o;
}
