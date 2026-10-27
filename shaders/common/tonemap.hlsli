// 色调映射：把 [0, ∞) 的 HDR 辐亮度压到显示器的 [0, 1]。
// 两种曲线可在 ImGui 中切换对比：
//   ACES（Hill 拟合）：对比度高、高光偏暖、饱和色在高亮处会「跑色」（蓝天变青）
//   AgX（Troy Sobotka，Wrensch 的多项式近似）：高亮处自然去饱和，色相更稳定，适合强光叙事
#pragma once

// ---- ACES：Stephen Hill 的 RRT + ODT 拟合（输入/输出都是线性 sRGB） ----
static const float3x3 ACES_INPUT = {
    0.59719, 0.35458, 0.04823,
    0.07600, 0.90834, 0.01566,
    0.02840, 0.13383, 0.83777 };
static const float3x3 ACES_OUTPUT = {
    1.60475, -0.53108, -0.07367,
    -0.10208, 1.10813, -0.00605,
    -0.00327, -0.07276, 1.07602 };

float3 RRTAndODTFit(float3 v)
{
    float3 a = v * (v + 0.0245786) - 0.000090537;
    float3 b = v * (0.983729 * v + 0.4329510) + 0.238081;
    return a / b;
}

float3 TonemapACES(float3 c)
{
    c = mul(ACES_INPUT, c);
    c = RRTAndODTFit(c);
    c = mul(ACES_OUTPUT, c);
    return saturate(c);
}

// ---- AgX：Benjamin Wrensch 的最小实现（原始矩阵按 GLSL 列主序给出，所以这里用 mul(v, M)） ----
static const float3x3 AGX_INSET = {
    0.842479062253094, 0.0423282422610123, 0.0423756549057051,
    0.0784335999999992, 0.878468636469772, 0.0784336,
    0.0792237451477643, 0.0791661274605434, 0.879142973793104 };
static const float3x3 AGX_OUTSET = {
    1.19687900512017, -0.0528968517574562, -0.0529716355144438,
    -0.0980208811401368, 1.15190312990417, -0.0980434501171241,
    -0.0990297440797205, -0.0989611768448433, 1.15107367264116 };

// S 形对比度曲线的 6 次多项式拟合（均方误差约 3.7e-6）
float3 AgxContrast(float3 x)
{
    float3 x2 = x * x;
    float3 x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}

float3 TonemapAgX(float3 c)
{
    const float minEv = -12.47393;
    const float maxEv = 4.026069;
    c = mul(max(c, 1e-10), AGX_INSET);
    c = clamp(log2(c), minEv, maxEv);  // 对数编码：曝光每 +1 档，输入只 +1
    c = (c - minEv) / (maxEv - minEv);
    c = AgxContrast(c);
    c = mul(c, AGX_OUTSET);
    // 曲线输出是显示编码值；转回线性（2.2 次方），之后统一做 sRGB 编码
    return pow(saturate(c), 2.2);
}
