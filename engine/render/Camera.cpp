#include "render/Camera.h"

#include "math/Projection.h"
#include "platform/Input.h"

#include <GLFW/glfw3.h>

#include <algorithm>

namespace eng {

void FlyCamera::reset(const CameraSettings& s) {
    m_settings = s;
    m_position = s.startPosition;
    m_yaw = s.startYawDeg;
    m_pitch = s.startPitchDeg;
}

vec3 FlyCamera::forward() const {
    // 球坐标 → 方向：yaw 绕 Y 轴，pitch 抬头为正
    const float y = glm::radians(m_yaw), p = glm::radians(m_pitch);
    return glm::normalize(vec3(std::cos(y) * std::cos(p), std::sin(p), std::sin(y) * std::cos(p)));
}

bool FlyCamera::update(const Input& input, float dt, bool allowMouse, bool allowKeyboard) {
    const bool looking = allowMouse && input.isMouseDown(GLFW_MOUSE_BUTTON_RIGHT);
    if (looking) {
        const vec2 d = input.mouseDelta() * m_settings.lookSensitivity;
        m_yaw += d.x;
        // 限制俯仰角，避免越过 ±90° 时 lookAt 的 up 向量与视线平行导致翻转
        m_pitch = std::clamp(m_pitch - d.y, -89.0f, 89.0f);
    }
    if (allowMouse && input.scroll() != 0.0f) {
        // 滚轮按比例调速：远距离飞行和贴近观察都好用
        m_settings.moveSpeed = std::clamp(m_settings.moveSpeed * std::pow(1.2f, input.scroll()), 0.1f, 500.0f);
    }
    if (!allowKeyboard) return looking;

    const vec3 f = forward();
    const vec3 r = glm::normalize(glm::cross(f, kWorldUp));
    vec3 move(0.0f);
    if (input.isDown(GLFW_KEY_W)) move += f;
    if (input.isDown(GLFW_KEY_S)) move -= f;
    if (input.isDown(GLFW_KEY_D)) move += r;
    if (input.isDown(GLFW_KEY_A)) move -= r;
    if (input.isDown(GLFW_KEY_E)) move += kWorldUp;
    if (input.isDown(GLFW_KEY_Q)) move -= kWorldUp;
    if (glm::dot(move, move) > 0.0f) {
        float speed = m_settings.moveSpeed;
        if (input.isDown(GLFW_KEY_LEFT_SHIFT)) speed *= m_settings.sprintMultiplier;
        m_position += glm::normalize(move) * speed * dt;
    }
    return looking;
}

void FlyCamera::focus(const vec3& center, float radius) {
    // 球恰好填满视野时的距离：d = r / sin(fov/2)；再留 20% 余量
    const float halfFov = glm::radians(m_settings.fovDeg) * 0.5f;
    const float dist = std::max(radius, 0.1f) / std::sin(halfFov) * 1.2f;
    m_position = center - forward() * dist; // 保持当前朝向，只平移
}

mat4 FlyCamera::view() const {
    return glm::lookAt(m_position, m_position + forward(), kWorldUp);
}

mat4 FlyCamera::projection(float aspect) const {
    return perspectiveReverseZ(glm::radians(m_settings.fovDeg), aspect, m_settings.nearPlane);
}

} // namespace eng
