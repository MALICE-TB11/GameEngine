// 网格的顶点输入、push constant 与通用顶点着色器。
// VSIn 必须与 engine/render/Mesh.h 的 Vertex 及 setMeshVertexInput 的 location 一致；
// DrawPush / MaterialParams 必须与 engine/render/FrameConstants.h、Material.h 一致。
#pragma once
#include "common/frame.hlsli"

struct MaterialParams
{
    float4 baseColor; // 线性 RGBA
    float4 emissive;  // rgb = 自发光，w = 法线贴图强度
    float4 pbr;       // x = 金属度，y = 粗糙度，z = AO 强度，w = alpha 裁剪阈值
    float4 flags;     // x = 着色模型，y = 自发光倍数，z = 1 表示 alpha 裁剪（glTF MASK）
};

struct DrawPush
{
    float4x4 model;
    MaterialParams material;
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

// 法线变换：需要模型矩阵的「逆转置」（非均匀缩放时直接乘模型矩阵会让法线不再垂直于表面）。
// 技巧：逆转置 = 余子式矩阵 / 行列式，而 3x3 余子式矩阵的三列正好是原矩阵列向量两两的叉积。
// 法线最后要归一化，所以除以行列式可以省掉，只保留它的符号（镜像变换时行列式为负）。
float3 TransformNormal(float4x4 m, float3 n)
{
    float3 c0 = float3(m[0][0], m[1][0], m[2][0]);
    float3 c1 = float3(m[0][1], m[1][1], m[2][1]);
    float3 c2 = float3(m[0][2], m[1][2], m[2][2]);
    float det = dot(c0, cross(c1, c2));
    float3 r = cross(c1, c2) * n.x + cross(c2, c0) * n.y + cross(c0, c1) * n.z;
    return det < 0.0 ? -r : r;
}

MeshVSOut MeshVertex(VSIn i)
{
    MeshVSOut o;
    float4 world = mul(gDraw.model, float4(i.position, 1.0));
    o.worldPos = world.xyz;
    o.position = mul(gFrame.viewProj, world);
    o.normal = TransformNormal(gDraw.model, i.normal);
    // 切线在表面内，直接跟随模型矩阵变换
    o.tangent = float4(mul((float3x3)gDraw.model, i.tangent.xyz), i.tangent.w);
    o.uv = i.uv;
    return o;
}
