// 全局资源（set 0），所有 shader 共享。
// FrameConstants 必须与 engine/render/FrameConstants.h 逐字段一致。
#pragma once

struct FrameConstants
{
    float4x4 view;
    float4x4 proj;
    float4x4 viewProj;
    float4x4 invViewProj;
    float4 cameraPos;    // xyz = 相机世界坐标
    float4 screen;       // xy = 分辨率，zw = 1/分辨率
    float4 time;         // x = 秒，y = 帧间隔，z = 帧序号
    // ---- 光照输入：所有材质共用（见 common/lighting.hlsli） ----
    float4 sunDirection; // xyz = 指向太阳，w = 照度
    float4 sunColor;     // rgb = 颜色，w = 角半径（弧度）
    float4 skyZenith;    // rgb = 天顶颜色，w = 天空亮度倍数
    float4 skyHorizon;   // rgb = 地平线颜色，w = 太阳光晕强度
    float4 groundColor;  // rgb = 地面反弹颜色
    float4 exposure;     // x = 曝光，y = 色调映射模式，z = 环境光强度
    float4 debug;        // x = 调试视图
    // ---- 级联阴影（见 common/shadow.hlsli） ----
    float4x4 shadowViewProj[4];
    float4 cascadeSplits;
    float4 cascadeTexel;
    float4 shadowParams; // x = 深度偏移，y = 法线偏移（纹素），z = PCF 半径（纹素），w = 是否启用
};

[[vk::binding(0, 0)]] ConstantBuffer<FrameConstants> gFrame;
// 不可变采样器（在 C++ 端创建布局时烘焙进去）
[[vk::binding(1, 0)]] SamplerState gLinearWrap;  // 材质贴图（16x 各向异性）
[[vk::binding(2, 0)]] SamplerState gLinearClamp; // 全屏后处理
[[vk::binding(3, 0)]] SamplerState gPointClamp;  // 精确取像素
[[vk::binding(4, 0)]] SamplerComparisonState gShadowSampler; // 阴影比较采样（硬件 2×2 PCF）
