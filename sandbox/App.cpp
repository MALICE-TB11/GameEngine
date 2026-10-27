#include "App.h"

#include "core/Log.h"
#include "debug/RenderPanels.h"

#include <GLFW/glfw3.h>
#include <imgui.h>

namespace eng {

// 参数结构体 ↔ JSON。必须写在结构体所在的命名空间里（ADL 才能找到）。
// WITH_DEFAULT：JSON 缺字段时用结构体里的默认值。
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(WindowSettings, title, width, height)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(RendererSettings, vsync, clearColor, sunElevationDeg, sunAzimuthDeg, exposure)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(FixedStepSettings, fixedHz, maxStepsPerFrame, maxFrameTime)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(CameraSettings, fovDeg, nearPlane, moveSpeed, sprintMultiplier, lookSensitivity,
                                                startPosition, startYawDeg, startPitchDeg)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(DebugSettings, renderdoc)

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
        buildDrawList(float(m_loop.alpha()));
        if (m_renderer.drawFrame(m_view, m_drawList)) ++frame;
        m_stats.addFrame(float(dt));

        if (m_options.frames > 0 && frame >= m_options.frames) break;
    }

    const uint32_t errors = log::errorCount();
    shutdown();
    if (errors > 0) {
        LOG_ERROR("运行期间出现 {} 条错误日志，返回码 2", errors);
        return 2;
    }
    return 0;
}

bool App::init(const LaunchOptions& options) {
    m_options = options;
    m_config = ConfigFile("config/engine.json");
    loadSettings();
    m_camera.reset(m_cameraSettings);

    // RenderDoc 必须在创建 Vulkan 实例之前加载
    const bool wantRenderDoc = options.renderdoc.value_or(m_debugSettings.renderdoc && options.frames <= 0);
    m_renderdoc.init(wantRenderDoc);

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
    createScene();
    return true;
}

void App::shutdown() {
    if (m_device.device()) vkDeviceWaitIdle(m_device.device());
    destroyMesh(m_device, m_cube);
    m_imgui.shutdown();
    m_renderer.shutdown();
    m_device.shutdown();
    m_window.destroy();
}

void App::loadSettings() {
    m_windowSettings = m_config.get<WindowSettings>("window");
    m_rendererSettings = m_config.get<RendererSettings>("renderer");
    m_loopSettings = m_config.get<FixedStepSettings>("loop");
    m_cameraSettings = m_config.get<CameraSettings>("camera");
    m_debugSettings = m_config.get<DebugSettings>("debug");
    m_loop.setSettings(m_loopSettings);
    m_renderer.setSettings(m_rendererSettings);
    m_camera.setSettings(m_cameraSettings);
}

void App::createScene() {
    m_cube = uploadMesh(m_device, makeCube(0.5f), "Cube");
    m_spinCurr.position = {0.0f, 1.0f, 0.0f};
    m_spinPrev = m_spinCurr;
}

void App::fixedUpdate(double dt) {
    // 模拟状态只在这里以固定步长改变。先保存上一步，渲染时在两步之间插值。
    // M4 起：物理世界也在这里推进。
    m_simTime += dt;
    m_spinPrev = m_spinCurr;
    const quat step = glm::angleAxis(glm::radians(m_spinSpeedDeg) * float(dt), glm::normalize(vec3(0.3f, 1.0f, 0.2f)));
    m_spinCurr.rotation = glm::normalize(step * m_spinCurr.rotation);
}

void App::update(double dt) {
    if (m_input.wasPressed(GLFW_KEY_ESCAPE) && !m_imgui.wantsKeyboard()) m_window.requestClose();
    // 转视角时 ImGui 不应抢鼠标；ImGui 正在被操作时相机不响应
    const bool looking = m_camera.update(m_input, float(dt), !m_imgui.wantsMouse(), !m_imgui.wantsKeyboard());
    m_window.setCursorCaptured(looking);

    const ivec2 fb = m_window.framebufferSize();
    const float aspect = fb.y > 0 ? float(fb.x) / float(fb.y) : 1.0f;
    m_view.view = m_camera.view();
    m_view.proj = m_camera.projection(aspect);
    m_view.position = m_camera.position();
    m_view.time = m_clock.seconds();
    m_view.dt = float(dt);
}

void App::buildDrawList(float alpha) {
    m_drawList.opaque.clear();
    // 旋转立方体：在上一步和当前步之间插值（固定步长 + 渲染插值）
    m_drawList.opaque.push_back({&m_cube, Transform::lerp(m_spinPrev, m_spinCurr, alpha).matrix(), nullptr});
    // 地面：压扁的立方体
    Transform ground;
    ground.scale = {20.0f, 0.1f, 20.0f};
    ground.position = {0.0f, -0.05f, 0.0f};
    m_drawList.opaque.push_back({&m_cube, ground.matrix(), nullptr});
    // 一排静态立方体：检验深度与透视
    for (int i = 0; i < 5; ++i) {
        Transform t;
        t.position = {-4.0f + 2.0f * float(i), 0.5f, -3.0f - float(i)};
        m_drawList.opaque.push_back({&m_cube, t.matrix(), nullptr});
    }
}

void App::drawUI() {
    ImGui::DockSpaceOverViewport(0, nullptr, ImGuiDockNodeFlags_PassthruCentralNode);
    m_stats.draw(m_device, m_renderer, {m_loop.settings().fixedHz, m_loop.lastSteps(), m_loop.alpha()});
    drawLogPanel();
    drawRenderGraphPanel(m_renderer, &m_renderdoc);
    drawShaderPanel(m_renderer.pipelines());
    drawShaderErrorBanner(m_renderer.pipelines());

    // 第一次启动（没有 imgui.ini）时的默认布局；之后用户拖动的位置会保存在 build/imgui.ini
    ImGui::SetNextWindowPos(ImVec2(10, 330), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(400, 330), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("参数")) {
        bool changed = false;
        ImGui::SeparatorText("渲染");
        changed |= ImGui::Checkbox("垂直同步", &m_rendererSettings.vsync);
        changed |= ImGui::ColorEdit3("背景颜色", &m_rendererSettings.clearColor.x);
        changed |= ImGui::SliderFloat("太阳仰角", &m_rendererSettings.sunElevationDeg, -10.0f, 90.0f, "%.1f°");
        changed |= ImGui::SliderFloat("太阳方位角", &m_rendererSettings.sunAzimuthDeg, 0.0f, 360.0f, "%.1f°");
        changed |= ImGui::SliderFloat("曝光", &m_rendererSettings.exposure, 0.05f, 8.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
        ImGui::SeparatorText("主循环");
        float hz = float(m_loopSettings.fixedHz);
        if (ImGui::SliderFloat("固定步长 Hz", &hz, 5.0f, 240.0f, "%.0f")) {
            m_loopSettings.fixedHz = hz;
            changed = true;
        }
        changed |= ImGui::SliderInt("单帧最多步数", &m_loopSettings.maxStepsPerFrame, 1, 20);
        ImGui::SliderFloat("立方体转速", &m_spinSpeedDeg, 0.0f, 360.0f, "%.0f°/s");
        ImGui::TextDisabled("把固定步长调到 5Hz：插值让旋转依旧平滑");
        ImGui::SeparatorText("相机");
        changed |= ImGui::SliderFloat("视野", &m_cameraSettings.fovDeg, 20.0f, 120.0f, "%.0f°");
        ImGui::SliderFloat("移动速度", &m_camera.speed(), 0.1f, 500.0f, "%.1f m/s", ImGuiSliderFlags_Logarithmic);
        const vec3 p = m_camera.position();
        ImGui::Text("位置 (%.1f, %.1f, %.1f)  yaw %.0f° pitch %.0f°", p.x, p.y, p.z, m_camera.yawDeg(), m_camera.pitchDeg());
        ImGui::TextDisabled("右键拖动转视角，WASD 移动，Q/E 升降，Shift 加速，滚轮调速");
        if (changed) {
            m_cameraSettings.moveSpeed = m_camera.speed();
            m_renderer.setSettings(m_rendererSettings);
            m_loop.setSettings(m_loopSettings);
            m_camera.setSettings(m_cameraSettings);
        }
        ImGui::Separator();
        if (ImGui::Button("保存到 config/engine.json")) {
            m_cameraSettings.moveSpeed = m_camera.speed();
            m_config.set("window", m_windowSettings);
            m_config.set("renderer", m_rendererSettings);
            m_config.set("loop", m_loopSettings);
            m_config.set("camera", m_cameraSettings);
            m_config.set("debug", m_debugSettings);
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
