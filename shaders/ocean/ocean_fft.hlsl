// 一维逆 FFT（基 2，按时间抽取）：一个线程组处理一整行（或一整列），N 个线程各管一个元素，
// log2(N) 级蝶形全部在 groupshared 内存里完成，只读写一次显存。二维逆 FFT = 先所有行、再所有列。
//
// 蝶形（第 s 级，把两个长度为 half 的子变换合成长度 2·half）：
//   X[j]        = E[j] + w^j · O[j]
//   X[j + half] = E[j] - w^j · O[j]        w = e^{+2πi / (2·half)}（逆变换取正号）
// 输入按「位反转」顺序放进共享内存，各级的子变换正好是连续的块，输出就是自然顺序。
// 每个元素是两个复数（float4 = 两个打包的实数场），两张输入贴图同时变换。
#include "ocean/ocean_common.hlsli"

[[vk::binding(0, 1)]] [[vk::image_format("rgba32f")]] RWTexture2DArray<float4> gIn0;
[[vk::binding(1, 1)]] [[vk::image_format("rgba32f")]] RWTexture2DArray<float4> gIn1;
[[vk::binding(2, 1)]] [[vk::image_format("rgba32f")]] RWTexture2DArray<float4> gOut0;
[[vk::binding(3, 1)]] [[vk::image_format("rgba32f")]] RWTexture2DArray<float4> gOut1;

groupshared float4 sA[2][OCEAN_N];
groupshared float4 sB[2][OCEAN_N];

float4 Butterfly(float4 e, float4 o, float2 w, bool upper)
{
    const float4 wo = float4(CMul(o.xy, w), CMul(o.zw, w));
    return upper ? e + wo : e - wo;
}

[numthreads(OCEAN_N, 1, 1)]
void CSMain(uint3 group : SV_GroupID, uint t : SV_GroupIndex)
{
    const uint row = group.y;
    const uint cascade = group.z;
    const bool vertical = gSim.foam.w > 0.5;
    const int3 coord = vertical ? int3(row, t, cascade) : int3(t, row, cascade);

    const uint r = reversebits(t) >> (32 - OCEAN_LOG_N);
    sA[0][r] = gIn0[coord];
    sB[0][r] = gIn1[coord];

    uint src = 0;
    [unroll] for (uint s = 0; s < OCEAN_LOG_N; ++s)
    {
        GroupMemoryBarrierWithGroupSync();
        const uint halfSize = 1u << s;
        const uint pos = t & (2 * halfSize - 1); // 在本组蝶形里的位置
        const uint j = pos & (halfSize - 1);
        const uint even = (t - pos) + j;
        const uint odd = even + halfSize;
        const float angle = OCEAN_PI * float(j) / float(halfSize); // 2π j / (2·half)
        const float2 w = float2(cos(angle), sin(angle));
        const bool upper = pos < halfSize;
        sA[1 - src][t] = Butterfly(sA[src][even], sA[src][odd], w, upper);
        sB[1 - src][t] = Butterfly(sB[src][even], sB[src][odd], w, upper);
        src = 1 - src;
    }
    GroupMemoryBarrierWithGroupSync();
    gOut0[coord] = sA[src][t];
    gOut1[coord] = sB[src][t];
}
