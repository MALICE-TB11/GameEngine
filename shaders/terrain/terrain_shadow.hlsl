// 地形投射阴影：与主 pass 生成完全相同的顶点，只换成光源的视图投影矩阵。
#include "terrain/terrain_common.hlsli"

float4 VSMain(uint vid : SV_VertexID) : SV_Position
{
    TerrainVertex v = TerrainVertexFromId(vid);
    return mul(gFrame.shadowViewProj[(uint)gTerrain.misc.z], float4(v.worldPos, 1.0));
}

void PSMain()
{
}
