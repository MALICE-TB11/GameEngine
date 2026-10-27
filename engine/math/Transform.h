#pragma once
// 位置 + 旋转 + 缩放。固定步长主循环里，渲染在「上一步」和「当前步」的 Transform 之间插值，
// 所以这里提供 lerp：位置/缩放线性插值，旋转用 slerp（四元数球面插值，保证匀速且不变形）。

#include "math/MathTypes.h"

namespace eng {

struct Transform {
    vec3 position{0.0f};
    quat rotation{1.0f, 0.0f, 0.0f, 0.0f}; // glm::quat 构造顺序是 (w, x, y, z)
    vec3 scale{1.0f};

    mat4 matrix() const {
        // 先缩放、再旋转、最后平移：M = T * R * S
        mat4 m = glm::translate(mat4(1.0f), position);
        m *= glm::mat4_cast(rotation);
        return glm::scale(m, scale);
    }

    static Transform lerp(const Transform& a, const Transform& b, float t) {
        return {glm::mix(a.position, b.position, t), glm::slerp(a.rotation, b.rotation, t), glm::mix(a.scale, b.scale, t)};
    }
};

} // namespace eng
