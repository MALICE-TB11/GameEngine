#include "platform/Input.h"

#include "platform/Window.h"

#include <GLFW/glfw3.h>

namespace eng {

void Input::update(Window& window) {
    GLFWwindow* w = window.handle();
    m_prevKeys = m_keys;
    // GLFW 的有效键码从 GLFW_KEY_SPACE(32) 开始，更小的值查询会报错
    for (int k = GLFW_KEY_SPACE; k < kMaxKeys; ++k) m_keys[k] = glfwGetKey(w, k) == GLFW_PRESS;
    for (int b = 0; b < kMaxButtons; ++b) m_buttons[b] = glfwGetMouseButton(w, b) == GLFW_PRESS;

    double x = 0, y = 0;
    glfwGetCursorPos(w, &x, &y);
    const vec2 pos{static_cast<float>(x), static_cast<float>(y)};
    // 第一帧没有「上一帧位置」，delta 记为 0，否则相机会猛地转一下
    m_mouseDelta = m_firstUpdate ? vec2(0.0f) : pos - m_mousePos;
    m_mousePos = pos;
    m_firstUpdate = false;
    m_scroll = window.consumeScroll();
}

} // namespace eng
