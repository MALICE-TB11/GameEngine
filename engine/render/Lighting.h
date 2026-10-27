#pragma once
// 光照参数：一个强方向光（太阳/月亮）+ 解析天空 + 半球环境光。
// 「光即叙事」：少量强光源，而不是很多小灯。所有材质（PBR 与 toon）读同一份。
// 这些值写进 FrameConstants，shader 端通过 common/lighting.hlsli 的 GetLightingInputs() 读取。

#include "math/MathTypes.h"

#include <cmath>

namespace eng {

enum class Tonemapper : int { ACES = 0, AgX = 1, None = 2 };

struct LightingSettings {
    // 太阳
    float sunElevationDeg = 28.0f;
    float sunAzimuthDeg = 210.0f;
    float sunIntensity = 4.5f;               // 垂直照射时的照度（相对单位）
    vec3 sunColor{1.0f, 0.93f, 0.82f};       // 线性
    float sunAngularRadiusDeg = 0.27f;       // 真实太阳约 0.27°；调大让天空里的太阳盘更明显
    float sunHalo = 1.0f;                    // 太阳周围光晕（大气前向散射）的强度
    // 天空（解析渐变，M3 的「简单天空」）
    vec3 skyZenith{0.06f, 0.20f, 0.62f};     // 线性
    vec3 skyHorizon{0.38f, 0.56f, 0.85f};
    vec3 groundColor{0.12f, 0.10f, 0.08f};   // 地面反弹
    float skyIntensity = 1.0f;
    float ambientIntensity = 1.0f;
    // 后处理
    float exposure = 1.0f;
    int tonemapper = int(Tonemapper::AgX);

    vec3 sunDirection() const {
        const float el = glm::radians(sunElevationDeg), az = glm::radians(sunAzimuthDeg);
        return {std::cos(el) * std::cos(az), std::sin(el), std::cos(el) * std::sin(az)};
    }
};

} // namespace eng
