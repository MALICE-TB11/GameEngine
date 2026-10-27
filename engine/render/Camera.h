#pragma once
// 自由飞行相机：按住鼠标右键转视角，WASD 移动，Q/E 下降/上升，Shift 加速，滚轮调速度。

#include "math/MathTypes.h"

namespace eng {

class Input;

struct CameraSettings {
    float fovDeg = 60.0f;
    float nearPlane = 0.05f; // 反向 Z + 无限远平面：near 可以放心取小
    float moveSpeed = 5.0f;  // 米/秒
    float sprintMultiplier = 4.0f;
    float lookSensitivity = 0.15f; // 度/像素
    vec3 startPosition{0.0f, 2.0f, 6.0f};
    float startYawDeg = -90.0f; // -90° = 看向 -Z
    float startPitchDeg = -10.0f;
};

class FlyCamera {
public:
    void reset(const CameraSettings& s);
    void setSettings(const CameraSettings& s) { m_settings = s; }
    const CameraSettings& settings() const { return m_settings; }

    // allowMouse / allowKeyboard：ImGui 占用输入时传 false
    // 返回值：是否正在用右键转视角（App 据此隐藏/锁定鼠标）
    bool update(const Input& input, float dt, bool allowMouse, bool allowKeyboard);

    mat4 view() const;
    mat4 projection(float aspect) const;
    vec3 position() const { return m_position; }
    vec3 forward() const;
    float yawDeg() const { return m_yaw; }
    float pitchDeg() const { return m_pitch; }
    float& speed() { return m_settings.moveSpeed; }

private:
    CameraSettings m_settings;
    vec3 m_position{0.0f};
    float m_yaw = -90.0f;
    float m_pitch = 0.0f;
};

} // namespace eng
