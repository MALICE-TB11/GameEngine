#pragma once
// 键盘/鼠标输入。每帧调用 update() 轮询一次 GLFW 状态，然后可以问：
//   isDown(key)    —— 当前按住
//   wasPressed(key)—— 本帧刚按下（上一帧没按）
// 为什么用轮询而不是回调：回调会和 ImGui 的回调抢，轮询简单且天然「每帧一致」。
// 键码直接用 GLFW_KEY_*（为了不再造一套枚举）。
// EXTEND: 做手柄支持、按键重映射时，在这里加一层「动作 → 按键」的映射表。

#include "math/MathTypes.h"

#include <array>

namespace eng {

class Window;

class Input {
public:
    static constexpr int kMaxKeys = 349;   // GLFW_KEY_LAST + 1
    static constexpr int kMaxButtons = 8;  // GLFW_MOUSE_BUTTON_LAST + 1

    void update(Window& window);

    bool isDown(int key) const { return valid(key) && m_keys[key]; }
    bool wasPressed(int key) const { return valid(key) && m_keys[key] && !m_prevKeys[key]; }
    bool isMouseDown(int button) const { return button >= 0 && button < kMaxButtons && m_buttons[button]; }
    vec2 mouseDelta() const { return m_mouseDelta; }
    float scroll() const { return m_scroll; }

private:
    static bool valid(int key) { return key >= 0 && key < kMaxKeys; }

    std::array<bool, kMaxKeys> m_keys{};
    std::array<bool, kMaxKeys> m_prevKeys{};
    std::array<bool, kMaxButtons> m_buttons{};
    vec2 m_mousePos{0.0f};
    vec2 m_mouseDelta{0.0f};
    float m_scroll = 0.0f;
    bool m_firstUpdate = true;
};

} // namespace eng
