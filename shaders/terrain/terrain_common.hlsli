// 地形的 push constant 与顶点生成。terrain.hlsl（主 pass）和 terrain_shadow.hlsl（阴影）共用。
//
// 没有顶点缓冲：所有节点共用同一个「(N+1)×(N+1) 网格 + 四条裙边」的索引缓冲，
// 顶点着色器从 SV_VertexID 算出它在网格里的 (x, z)，再按节点的位置/尺寸放到世界里，
// 高度从高度贴图里按整数坐标精确读取（Load，不做插值）—— 与 CPU 端物理用的高度完全一致。
#pragma once
#include "common/frame.hlsli"

// 必须与 engine/terrain/TerrainRenderer.h 的 kPatchQuads 一致
static const uint kPatchQuads = 32;

// 必须与 TerrainRenderer.cpp 的 TerrainPush 一致（96 字节）
struct TerrainPush
{
    float4 node;      // xy = 节点最小角的世界 XZ，z = 节点边长（米），w = 四叉树级别
    float4 terrain;   // xy = 高度场采样 (0,0) 的世界 XZ，z = 采样间距（米），w = 每边采样数
    float4 layers;    // x = 沙滩最高高度，y = 礁石最高高度，z = 岩石坡度阈值，w = 苔藓最低高度
    float4 tiling;    // 沙 / 岩 / 礁 / 苔 每张贴图覆盖多少米
    float4 misc;      // x = 高度混合宽度，y = 裙边深度（× 网格间距），z = 阴影级联序号，w = 1 显示 LOD 颜色
    float4 roughness; // 四层的粗糙度
};
[[vk::push_constant]] TerrainPush gTerrain;

[[vk::binding(0, 1)]] Texture2D<float> gHeightMap; // R32F，每个纹素一个高度采样（米）

struct TerrainVertex
{
    float3 worldPos;
    float2 heightUV; // 高度场的 UV（纹素中心对齐），PS 用它采样法线贴图
};

TerrainVertex TerrainVertexFromId(uint vid)
{
    const uint n1 = kPatchQuads + 1;
    uint gx, gz;
    float skirt = 0.0;
    if (vid < n1 * n1)
    {
        gx = vid % n1;
        gz = vid / n1;
    }
    else
    {
        // 裙边：沿四条边各一排顶点，位置与边上的顶点相同，只是向下拉。
        // 相邻节点 LOD 不同时边上会有 T 形裂缝，裙边像一圈「围裙」把裂缝从下面挡住
        uint k = vid - n1 * n1;
        uint edge = k / n1, i = k % n1;
        gx = edge == 0 ? i : (edge == 1 ? kPatchQuads : (edge == 2 ? i : 0));
        gz = edge == 0 ? 0 : (edge == 1 ? i : (edge == 2 ? kPatchQuads : i));
        skirt = 1.0;
    }

    float2 xz = gTerrain.node.xy + float2(gx, gz) / kPatchQuads * gTerrain.node.z;
    float res = gTerrain.terrain.w;
    float2 sampleF = (xz - gTerrain.terrain.xy) / gTerrain.terrain.z;
    int2 s = clamp(int2(round(sampleF)), 0, int(res) - 1);
    float h = gHeightMap.Load(int3(s, 0));
    float gridSpacing = gTerrain.node.z / kPatchQuads;
    h -= skirt * gTerrain.misc.y * gridSpacing;

    TerrainVertex v;
    v.worldPos = float3(xz.x, h, xz.y);
    v.heightUV = (float2(s) + 0.5) / res;
    return v;
}
