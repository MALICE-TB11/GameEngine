#pragma once
// 级联阴影（CSM）的 CPU 端计算：把视锥按距离切成 4 段，每段用一张正交阴影图覆盖。
//
// 为什么要分级联：一张阴影图覆盖 150 米，每个阴影纹素约 4 厘米 —— 近处锯齿严重；
// 只覆盖 10 米又照不到远处。分段后近处用小范围（高精度），远处用大范围（低精度但离得远看不出）。
//
// 四个级联放在同一张图集的四个象限里（4096² 图集 = 4 × 2048²），
// 这样 Render Graph 只需要管理一张普通 2D 深度图，不需要数组贴图。

#include "math/MathTypes.h"

#include <array>

namespace eng {

inline constexpr uint32_t kShadowCascades = 4;

struct ShadowSettings {
    bool enabled = true;
    float maxDistance = 150.0f; // 超过这个距离不计算阴影（逐渐淡出）
    float splitLambda = 0.75f;  // 0 = 均匀切分，1 = 对数切分；对数让近处级联更小、精度更高
    uint32_t atlasSize = 4096;  // 图集边长（每个级联占一半边长）
    float depthBias = 0.0005f;  // 深度偏移：消除「阴影痤疮」（表面自己挡住自己的条纹）
    float normalBias = 1.5f;    // 沿法线偏移多少个阴影纹素：比深度偏移更不容易产生「彼得潘」漏光
    float pcfRadius = 1.0f;     // PCF 采样半径（纹素）：越大阴影边缘越软
    float casterExtent = 250.0f; // 光源方向上向后延伸多远：级联范围外（例如身后的高山）也能投下阴影
};

struct ShadowCascades {
    std::array<mat4, kShadowCascades> viewProj{};
    vec4 splits{0.0f};       // 每个级联覆盖到的视空间距离（米）
    vec4 texelWorldSize{0.0f}; // 每个级联一个阴影纹素对应的世界尺寸（米）
};

// 反向 Z 的正交投影（近 = 1，远 = 0），与引擎的透视投影约定一致
mat4 orthoReverseZ(float left, float right, float bottom, float top, float zNear, float zFar);

// 根据相机与太阳方向计算 4 个级联的矩阵
ShadowCascades computeCascades(const mat4& cameraView, float fovYRadians, float aspect, float cameraNear, const vec3& sunDir,
                               const ShadowSettings& settings);

} // namespace eng
