#include "App.h"

#include "core/Log.h"

#include <GLFW/glfw3.h>
#include <imgui.h>

namespace eng {

// 参数结构体 ↔ JSON。必须写在结构体所在的命名空间里（ADL 才能找到）。
// WITH_DEFAULT：JSON 缺字段时用结构体里的默认值。
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(WindowSettings, title, width, height)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(RendererSettings, vsync, clearColor)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(FixedStepSettings, fixedHz, maxStepsPerFrame, maxFrameTime)

int App::run(const LaunchOptions& options) {
    if (!init(options)) {
        shutdown();
        return 1;
    }

    double last = m_clock.seconds();
    int frame = 0;
    bool resizedDown = false, resizedUp = false;
    while (!m_window.shouldClose()) {
        m_window.pollEvents();
        if (m_window.isMinimized()) {
            // 最小化：不渲染、不推进模拟，阻塞等待恢复；恢复后重置计时，避免一次补很多步
            m_window.waitWhileMinimized();
            last = m_clock.seconds();
            continue;
        }

        const double now = m_clock.seconds();
        const double dt = now - last;
        last = now;
        m_input.update(m_window);

        // 外部编辑器修改了 config/engine.json → 热加载
        if (m_config.pollChanged()) loadSettings();

        // ---- 固定步长模拟 ----
        const int steps = m_loop.advance(dt);
        for (int i = 0; i < steps; ++i) fixedUpdate(m_loop.fixedDt());

        // ---- 每帧更新（相机、UI 等与模拟无关的东西） ----
        update(dt);

        m_imgui.beginFrame();
        drawUI();
        m_imgui.endFrame();

        // 冒烟测试顺带测窗口缩放：1/3 处缩小、2/3 处恢复，覆盖交换链重建路径
        if (m_options.frames >= 30) {
            if (frame == m_options.frames / 3 && !resizedDown) {
                m_window.setSize(m_windowSettings.width * 3 / 4, m_windowSettings.height * 3 / 4);
                resizedDown = true;
            } else if (frame == m_options.frames * 2 / 3 && !resizedUp) {
                m_window.setSize(m_windowSettings.width, m_windowSettings.height);
                resizedUp = true;
            }
        }

        const bool lastFrame = m_options.frames > 0 && frame + 1 >= m_options.frames;
        if (lastFrame && !m_options.capturePath.empty()) m_renderer.requestCapture(m_options.capturePath);
        if (m_renderer.drawFrame()) ++frame;
        m_stats.addFrame(float(dt));

        if (m_options.frames > 0 && frame >= m_options.frames) break;
    }

    const uint32_t errors = log::errorCount();
    shutdown();
    if (errors > 0) {
        // 此时 logger 还在（log::shutdown 在 main 里），可以输出
        LOG_ERROR("运行期间出现 {} 条错误日志，返回码 2", errors);
        return 2;
    }
    return 0;
}

bool App::init(const LaunchOptions& options) {
    m_options = options;
    m_config = ConfigFile("config/engine.json");
    loadSettings();

    if (!m_window.create(m_windowSettings)) return false;

    rhi::DeviceSettings ds;
#ifdef ENGINE_DEBUG
    ds.validation = true;
#else
    ds.validation = false;
#endif
    if (options.validation) ds.validation = *options.validation;
    if (!m_device.init(ds, m_window.handle())) return false;
    if (!m_renderer.init(m_device, m_window, m_rendererSettings)) return false;
    if (!m_imgui.init(m_device, m_window, m_renderer.swapchain().format(), m_renderer.swapchain().imageCount())) return false;
    m_renderer.setOverlay([this](VkCommandBuffer cmd) { m_imgui.render(cmd); });
    return true;
}

void App::shutdown() {
    m_imgui.shutdown();
    m_renderer.shutdown();
    m_device.shutdown();
    m_window.destroy();
}

void App::loadSettings() {
    m_windowSettings = m_config.get<WindowSettings>("window");
    m_rendererSettings = m_config.get<RendererSettings>("renderer");
    m_loopSettings = m_config.get<FixedStepSettings>("loop");
    m_loop.setSettings(m_loopSettings);
    m_renderer.setSettings(m_rendererSettings);
}

void App::fixedUpdate(double dt) {
    // M4 起：物理世界在这里以固定步长推进
    m_simTime += dt;
}

void App::update(double) {
    if (m_input.wasPressed(GLFW_KEY_ESCAPE) && !m_imgui.wantsKeyboard()) m_window.requestClose();
}

void App::drawUI() {
    ImGui::DockSpaceOverViewport(0, nullptr, ImGuiDockNodeFlags_PassthruCentralNode);
    m_stats.draw(m_device, m_renderer, {m_loop.settings().fixedHz, m_loop.lastSteps(), m_loop.alpha()});
    drawLogPanel();

    // 第一次启动（没有 imgui.ini）时的默认布局；之后用户拖动的位置会保存在 build/imgui.ini
    ImGui::SetNextWindowPos(ImVec2(10, 330), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(400, 260), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("参数")) {
        bool changed = false;
        ImGui::SeparatorText("渲染");
        changed |= ImGui::Checkbox("垂直同步", &m_rendererSettings.vsync);
        changed |= ImGui::ColorEdit3("清屏颜色", &m_rendererSettings.clearColor.x);
        ImGui::SeparatorText("主循环");
        float hz = float(m_loopSettings.fixedHz);
        if (ImGui::SliderFloat("固定步长 Hz", &hz, 10.0f, 240.0f, "%.0f")) {
            m_loopSettings.fixedHz = hz;
            changed = true;
        }
        changed |= ImGui::SliderInt("单帧最多步数", &m_loopSettings.maxStepsPerFrame, 1, 20);
        if (changed) {
            m_renderer.setSettings(m_rendererSettings);
            m_loop.setSettings(m_loopSettings);
        }
        ImGui::Separator();
        if (ImGui::Button("保存到 config/engine.json")) {
            m_config.set("window", m_windowSettings);
            m_config.set("renderer", m_rendererSettings);
            m_config.set("loop", m_loopSettings);
            m_config.save();
        }
        ImGui::SameLine();
        if (ImGui::Button("从文件重新加载")) {
            m_config.load();
            loadSettings();
        }
        if (!m_config.lastError().empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "配置错误: %s", m_config.lastError().c_str());
    }
    ImGui::End();
}

} // namespace eng
