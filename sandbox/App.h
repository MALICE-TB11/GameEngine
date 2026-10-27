#pragma once
// 演示程序：把各模块串起来的「胶水」。
// 引擎模块不知道 App 的存在；App 负责创建模块、驱动主循环、组织调试 UI。

#include "core/Config.h"
#include "core/Time.h"
#include "debug/ImGuiLayer.h"
#include "debug/StatsPanel.h"
#include "platform/Input.h"
#include "platform/Window.h"
#include "render/Renderer.h"
#include "rhi/Device.h"

#include <filesystem>
#include <optional>

namespace eng {

struct LaunchOptions {
    int frames = -1;                    // --frames N：跑 N 帧后退出（冒烟测试）
    std::filesystem::path capturePath;  // --capture file.png：退出前截最后一帧
    std::optional<bool> validation;     // --validation / --no-validation，覆盖默认值
};

class App {
public:
    // 返回进程退出码：0 成功；1 初始化失败；2 运行中出现 error 日志（含 validation 报错）
    int run(const LaunchOptions& options);

private:
    bool init(const LaunchOptions& options);
    void shutdown();
    void loadSettings();
    void fixedUpdate(double dt);
    void update(double dt);
    void drawUI();

    LaunchOptions m_options;
    ConfigFile m_config;
    WindowSettings m_windowSettings;
    RendererSettings m_rendererSettings;
    FixedStepSettings m_loopSettings;

    Window m_window;
    Input m_input;
    rhi::Device m_device;
    Renderer m_renderer;
    ImGuiLayer m_imgui;
    StatsPanel m_stats;
    FixedStepLoop m_loop;
    Clock m_clock;
    double m_simTime = 0.0;
};

} // namespace eng
