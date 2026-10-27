#pragma once
// 数学类型与常用函数。GLM 的全局配置宏在 engine/CMakeLists.txt 的 engine_math target 上定义。
//
// 坐标系约定（全引擎统一，改之前想清楚）：
//   - 右手系，+Y 向上，相机看向 -Z（与 glTF 一致，导入模型不需要转换）。
//   - 海平面 y = 0：水面高度、水深都以此为基准。
//   - 长度单位：米。

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>

namespace eng {

using glm::ivec2;
using glm::mat3;
using glm::mat4;
using glm::quat;
using glm::uvec2;
using glm::vec2;
using glm::vec3;
using glm::vec4;

inline constexpr vec3 kWorldUp{0.0f, 1.0f, 0.0f};
inline constexpr float kPi = 3.14159265358979323846f;

inline float saturate(float x) {
    return glm::clamp(x, 0.0f, 1.0f);
}

// 把 x 从 [a0,a1] 线性映射到 [b0,b1]
inline float remap(float x, float a0, float a1, float b0, float b1) {
    return b0 + (x - a0) * (b1 - b0) / (a1 - a0);
}

} // namespace eng
