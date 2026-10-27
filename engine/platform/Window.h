#pragma once
// GLFW 窗口。负责：创建窗口、事件泵、framebuffer 尺寸变化与最小化检测。
//
// 窗口尺寸（屏幕坐标）≠ framebuffer 尺寸（像素）：高 DPI 缩放时两者不同。
// Vulkan 交换链只关心 framebuffer 尺寸，所以这里对外只暴露 framebufferSize()。

#include "math/MathTypes.h"

#include <string>

struct GLFWwindow;

namespace eng {

struct WindowSettings {
    std::string title = "Ocean Engine";
    int width = 1600;
    int height = 900;
};

class Window {
public:
    bool create(const WindowSettings& settings);
    void destroy();

    void pollEvents();
    // 最小化时 framebuffer 尺寸为 0，交换链无法创建：阻塞等待直到窗口恢复，期间不渲染
    void waitWhileMinimized();

    bool shouldClose() const;
    void requestClose();
    void setSize(int width, int height); // 冒烟测试用来触发交换链重建
    // 隐藏并锁定鼠标（相机转视角时），鼠标移动量不受屏幕边缘限制
    void setCursorCaptured(bool captured);
    ivec2 framebufferSize() const;
    bool isMinimized() const;
    // framebuffer 尺寸是否变过（读取后清零）。驱动不一定总会返回 OUT_OF_DATE，所以自己也记一份
    bool consumeResized();

    GLFWwindow* handle() const { return m_window; }
    // 滚轮在本帧的累计量（GLFW 只提供回调，所以存在窗口这里，由 Input 读取）
    float consumeScroll();

private:
    GLFWwindow* m_window = nullptr;
    bool m_resized = false;
    float m_scroll = 0.0f;

    static void onFramebufferSize(GLFWwindow* w, int width, int height);
    static void onScroll(GLFWwindow* w, double dx, double dy);
};

} // namespace eng
