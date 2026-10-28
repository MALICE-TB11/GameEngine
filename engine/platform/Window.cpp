#include "platform/Window.h"

#include "core/Log.h"

#include <GLFW/glfw3.h>

#include <utility>

namespace eng {

bool Window::create(const WindowSettings& settings) {
    glfwSetErrorCallback([](int code, const char* desc) { LOG_ERROR("GLFW 错误 {}: {}", code, desc); });
    if (!glfwInit()) {
        LOG_ERROR("glfwInit 失败");
        return false;
    }
    // 不让 GLFW 创建 OpenGL 上下文：我们用 Vulkan 自己管理交换链
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    m_window = glfwCreateWindow(settings.width, settings.height, settings.title.c_str(), nullptr, nullptr);
    if (!m_window) {
        LOG_ERROR("创建窗口失败");
        return false;
    }
    // 把 this 存进窗口，静态回调里才能找回对象
    glfwSetWindowUserPointer(m_window, this);
    glfwSetFramebufferSizeCallback(m_window, &Window::onFramebufferSize);
    // 注意：必须在 ImGui_ImplGlfw_InitForVulkan 之前设置回调，ImGui 会把我们的回调「串」在它后面
    glfwSetScrollCallback(m_window, &Window::onScroll);
    glfwSetDropCallback(m_window, &Window::onDrop);
    return true;
}

void Window::destroy() {
    if (m_window) glfwDestroyWindow(m_window);
    m_window = nullptr;
    glfwTerminate();
}

void Window::pollEvents() {
    glfwPollEvents();
}

void Window::waitWhileMinimized() {
    while (isMinimized() && !shouldClose()) glfwWaitEvents();
}

bool Window::shouldClose() const {
    return glfwWindowShouldClose(m_window) != 0;
}

void Window::requestClose() {
    glfwSetWindowShouldClose(m_window, GLFW_TRUE);
}

void Window::setSize(int width, int height) {
    glfwSetWindowSize(m_window, width, height);
}

void Window::setCursorCaptured(bool captured) {
    const int mode = captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL;
    if (glfwGetInputMode(m_window, GLFW_CURSOR) != mode) glfwSetInputMode(m_window, GLFW_CURSOR, mode);
}

ivec2 Window::framebufferSize() const {
    int w = 0, h = 0;
    glfwGetFramebufferSize(m_window, &w, &h);
    return {w, h};
}

bool Window::isMinimized() const {
    const ivec2 s = framebufferSize();
    return s.x == 0 || s.y == 0 || glfwGetWindowAttrib(m_window, GLFW_ICONIFIED);
}

bool Window::consumeResized() {
    const bool r = m_resized;
    m_resized = false;
    return r;
}

float Window::consumeScroll() {
    const float s = m_scroll;
    m_scroll = 0.0f;
    return s;
}

std::vector<std::filesystem::path> Window::consumeDroppedFiles() {
    return std::exchange(m_dropped, {});
}

void Window::onDrop(GLFWwindow* w, int count, const char** paths) {
    auto* self = static_cast<Window*>(glfwGetWindowUserPointer(w));
    // GLFW 给的是 UTF-8 路径；用 u8path 语义构造，中文路径才不会乱码
    for (int i = 0; i < count; ++i)
        self->m_dropped.emplace_back(std::u8string(reinterpret_cast<const char8_t*>(paths[i])));
}

void Window::onFramebufferSize(GLFWwindow* w, int, int) {
    static_cast<Window*>(glfwGetWindowUserPointer(w))->m_resized = true;
}

void Window::onScroll(GLFWwindow* w, double, double dy) {
    static_cast<Window*>(glfwGetWindowUserPointer(w))->m_scroll += static_cast<float>(dy);
}

} // namespace eng
