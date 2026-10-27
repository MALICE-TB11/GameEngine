#include "App.h"

#include "core/FileSystem.h"
#include "core/Log.h"
#include "debug/RenderPanels.h"

#include <GLFW/glfw3.h>
#include <imgui.h>

namespace eng {

// 参数结构体 ↔ JSON。必须写在结构体所在的命名空间里（ADL 才能找到）。
// WITH_DEFAULT：JSON 缺字段时用结构体里的默认值。
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(WindowSettings, title, width, height)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(RendererSettings, vsync, clearColor, debugView)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(LightingSettings, sunElevationDeg, sunAzimuthDeg, sunIntensity, sunColor,
                                                sunAngularRadiusDeg, sunHalo, skyZenith, skyHorizon, groundColor, skyIntensity,
                                                ambientIntensity, exposure, tonemapper)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(FixedStepSettings, fixedHz, maxStepsPerFrame, maxFrameTime)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(CameraSettings, fovDeg, nearPlane, moveSpeed, sprintMultiplier, lookSensitivity,
                                                startPosition, startYawDeg, startPitchDeg)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(DebugSettings, renderdoc)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(SceneSettings, model, modelPosition, modelScale, spinSpeedDeg)

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
        // 渲染插值：在上一固定步与当前固定步之间按 alpha 混合
        m_scene.buildDrawList(m_drawList, float(m_loop.alpha()));
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
    m_showUI = !options.hideUI;
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
    m_renderer.setLighting(m_lighting);
    if (!m_imgui.init(m_device, m_window, m_renderer.swapchain().format(), m_renderer.swapchain().imageCount())) return false;
    m_renderer.setOverlay([this](VkCommandBuffer cmd) { m_imgui.render(cmd); });
    createScene();
    return true;
}

void App::shutdown() {
    if (m_device.device()) vkDeviceWaitIdle(m_device.device());
    m_scene.destroy(m_device);
    m_imgui.shutdown();
    m_renderer.shutdown();
    m_device.shutdown();
    m_window.destroy();
}

void App::loadSettings() {
    m_windowSettings = m_config.get<WindowSettings>("window");
    m_rendererSettings = m_config.get<RendererSettings>("renderer");
    m_lighting = m_config.get<LightingSettings>("lighting");
    m_loopSettings = m_config.get<FixedStepSettings>("loop");
    m_cameraSettings = m_config.get<CameraSettings>("camera");
    m_debugSettings = m_config.get<DebugSettings>("debug");
    m_sceneSettings = m_config.get<SceneSettings>("scene");
    m_loop.setSettings(m_loopSettings);
    m_renderer.setSettings(m_rendererSettings);
    m_renderer.setLighting(m_lighting);
    m_camera.setSettings(m_cameraSettings);
}

void App::saveSettings() {
    m_cameraSettings.moveSpeed = m_camera.speed();
    m_config.set("window", m_windowSettings);
    m_config.set("renderer", m_rendererSettings);
    m_config.set("lighting", m_lighting);
    m_config.set("loop", m_loopSettings);
    m_config.set("camera", m_cameraSettings);
    m_config.set("debug", m_debugSettings);
    m_config.set("scene", m_sceneSettings);
    m_config.save();
}

void App::createScene() {
    // ---- 地面：沙地 ----
    Material sand;
    sand.name = "Sand";
    sand.params.baseColor = {0.50f, 0.42f, 0.28f, 1.0f}; // 线性值（约等于 sRGB 的 #BAAA8F）
    sand.params.pbr = {0.0f, 0.92f, 1.0f, 0.5f};
    Entity ground;
    ground.name = "Ground";
    ground.mesh = m_scene.addMesh(m_device, makePlane(40.0f, 20.0f), "Ground");
    ground.material = m_scene.addMaterial(sand);
    m_scene.add(ground);

    // ---- 材质球阵列：横向粗糙度 0→1；前排电介质（红漆），后排金属（金） ----
    const GpuMesh* sphere = m_scene.addMesh(m_device, makeUvSphere(0.45f, 48, 24), "Sphere");
    const int kColumns = 7;
    for (int row = 0; row < 2; ++row) {
        for (int i = 0; i < kColumns; ++i) {
            Material m;
            const float rough = float(i) / float(kColumns - 1);
            m.name = (row == 0 ? "Dielectric_r" : "Gold_r") + std::to_string(i);
            m.params.baseColor = row == 0 ? vec4(0.55f, 0.06f, 0.05f, 1.0f) : vec4(1.0f, 0.77f, 0.34f, 1.0f);
            m.params.pbr = {row == 0 ? 0.0f : 1.0f, rough, 1.0f, 0.5f};
            Entity e;
            e.name = m.name;
            e.mesh = sphere;
            e.material = m_scene.addMaterial(m);
            e.current.position = {-4.5f + 1.5f * float(i), 0.45f, 2.0f + 1.4f * float(row)};
            m_scene.add(e);
        }
    }

    // ---- 旋转立方体：固定步长更新 + 渲染插值的演示 ----
    Material painted;
    painted.name = "PaintedCube";
    painted.params.baseColor = {0.05f, 0.25f, 0.6f, 1.0f};
    painted.params.pbr = {0.0f, 0.35f, 1.0f, 0.5f};
    Entity cube;
    cube.name = "Spinner";
    cube.mesh = m_scene.addMesh(m_device, makeCube(0.5f), "Cube");
    cube.material = m_scene.addMaterial(painted);
    cube.current.position = {-4.0f, 1.2f, -1.0f};
    m_spinner = m_scene.add(cube);

    // ---- glTF 模型（遗迹） ----
    const std::string modelPath = m_options.model.empty() ? m_sceneSettings.model : m_options.model;
    Transform root;
    root.position = m_sceneSettings.modelPosition;
    root.scale = vec3(m_sceneSettings.modelScale);
    if (!m_scene.addModel(m_device, fs::resolve(modelPath), root))
        LOG_WARN("模型 {} 加载失败；运行 python tools/make_test_assets.py 生成测试资源", modelPath);
}

void App::fixedUpdate(double dt) {
    // 模拟状态只在这里以固定步长改变。先保存上一步，渲染时在两步之间插值。
    // M4 起：物理世界也在这里推进。
    m_simTime += dt;
    m_scene.beginFixedStep();
    Entity& spinner = m_scene.entity(m_spinner);
    const quat step = glm::angleAxis(glm::radians(m_sceneSettings.spinSpeedDeg) * float(dt), glm::normalize(vec3(0.3f, 1.0f, 0.2f)));
    spinner.current.rotation = glm::normalize(step * spinner.current.rotation);
}

void App::update(double dt) {
    if (m_input.wasPressed(GLFW_KEY_ESCAPE) && !m_imgui.wantsKeyboard()) m_window.requestClose();
    if (m_input.wasPressed(GLFW_KEY_F1)) m_showUI = !m_showUI;
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

void App::drawUI() {
    // shader 错误横幅即使隐藏了界面也要显示：「改坏了却没发现」是最糟的情况
    drawShaderErrorBanner(m_renderer.pipelines());
    if (!m_showUI) return;
    ImGui::DockSpaceOverViewport(0, nullptr, ImGuiDockNodeFlags_PassthruCentralNode);
    m_stats.draw(m_device, m_renderer, {m_loop.settings().fixedHz, m_loop.lastSteps(), m_loop.alpha()});
    drawLogPanel();
    drawRenderGraphPanel(m_renderer, &m_renderdoc);
    drawShaderPanel(m_renderer.pipelines());
    drawLightingPanel();
    drawMaterialPanel();

    // 第一次启动（没有 imgui.ini）时的默认布局；之后用户拖动的位置会保存在 build/imgui.ini
    ImGui::SetNextWindowPos(ImVec2(10, 330), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(400, 330), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("参数")) {
        bool changed = false;
        ImGui::SeparatorText("渲染");
        changed |= ImGui::Checkbox("垂直同步", &m_rendererSettings.vsync);
        const char* views[] = {"正常", "法线", "Albedo", "粗糙度", "金属度", "AO"};
        changed |= ImGui::Combo("调试视图", &m_rendererSettings.debugView, views, IM_ARRAYSIZE(views));
        changed |= ImGui::ColorEdit3("无天空时的背景", &m_rendererSettings.clearColor.x);
        ImGui::SeparatorText("主循环");
        float hz = float(m_loopSettings.fixedHz);
        if (ImGui::SliderFloat("固定步长 Hz", &hz, 5.0f, 240.0f, "%.0f")) {
            m_loopSettings.fixedHz = hz;
            changed = true;
        }
        changed |= ImGui::SliderInt("单帧最多步数", &m_loopSettings.maxStepsPerFrame, 1, 20);
        ImGui::SliderFloat("立方体转速", &m_sceneSettings.spinSpeedDeg, 0.0f, 360.0f, "%.0f°/s");
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
        if (ImGui::Button("保存到 config/engine.json")) saveSettings();
        ImGui::SameLine();
        if (ImGui::Button("从文件重新加载")) {
            m_config.load();
            loadSettings();
        }
        if (!m_config.lastError().empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "配置错误: %s", m_config.lastError().c_str());
    }
    ImGui::End();
}

void App::drawLightingPanel() {
    ImGui::SetNextWindowPos(ImVec2(420, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(380, 420), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowCollapsed(true, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("光照")) {
        ImGui::End();
        return;
    }
    LightingSettings& L = m_lighting;
    bool c = false;
    ImGui::SeparatorText("太阳");
    c |= ImGui::SliderFloat("仰角", &L.sunElevationDeg, -10.0f, 90.0f, "%.1f°");
    c |= ImGui::SliderFloat("方位角", &L.sunAzimuthDeg, 0.0f, 360.0f, "%.1f°");
    c |= ImGui::SliderFloat("照度", &L.sunIntensity, 0.0f, 30.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
    c |= ImGui::ColorEdit3("颜色", &L.sunColor.x, ImGuiColorEditFlags_Float);
    c |= ImGui::SliderFloat("角半径", &L.sunAngularRadiusDeg, 0.05f, 3.0f, "%.2f°");
    c |= ImGui::SliderFloat("光晕", &L.sunHalo, 0.0f, 5.0f);
    ImGui::SeparatorText("天空与环境光");
    c |= ImGui::ColorEdit3("天顶", &L.skyZenith.x, ImGuiColorEditFlags_Float);
    c |= ImGui::ColorEdit3("地平线", &L.skyHorizon.x, ImGuiColorEditFlags_Float);
    c |= ImGui::ColorEdit3("地面反弹", &L.groundColor.x, ImGuiColorEditFlags_Float);
    c |= ImGui::SliderFloat("天空亮度", &L.skyIntensity, 0.0f, 5.0f);
    c |= ImGui::SliderFloat("环境光强度", &L.ambientIntensity, 0.0f, 3.0f);
    ImGui::SeparatorText("相机后处理");
    c |= ImGui::SliderFloat("曝光", &L.exposure, 0.05f, 8.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
    const char* tonemappers[] = {"ACES (Hill 拟合)", "AgX", "无（直接截断）"};
    c |= ImGui::Combo("色调映射", &L.tonemapper, tonemappers, IM_ARRAYSIZE(tonemappers));
    ImGui::TextDisabled("对比：把照度调到 20，看饱和色在高光处的表现");
    if (c) m_renderer.setLighting(L);
    if (ImGui::Button("保存到 config/engine.json")) saveSettings();
    ImGui::End();
}

void App::drawMaterialPanel() {
    ImGui::SetNextWindowPos(ImVec2(810, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(320, 420), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowCollapsed(true, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("材质")) {
        ImGui::End();
        return;
    }
    ImGui::TextDisabled("参数走 push constant：拖动即时生效");
    for (Material* m : m_scene.materials()) {
        ImGui::PushID(m);
        if (ImGui::TreeNode(m->name.c_str())) {
            MaterialParams& p = m->params;
            ImGui::ColorEdit4("基础色", &p.baseColor.x, ImGuiColorEditFlags_Float);
            ImGui::SliderFloat("金属度", &p.pbr.x, 0.0f, 1.0f);
            ImGui::SliderFloat("粗糙度", &p.pbr.y, 0.0f, 1.0f);
            ImGui::SliderFloat("AO 强度", &p.pbr.z, 0.0f, 1.0f);
            ImGui::SliderFloat("法线强度", &p.emissive.w, 0.0f, 3.0f);
            ImGui::ColorEdit3("自发光", &p.emissive.x, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
            ImGui::SliderFloat("自发光倍数", &p.flags.y, 0.0f, 20.0f);
            ImGui::Text("贴图: %s%s%s%s%s", m->baseColorTex ? "[基础色]" : "", m->normalTex ? "[法线]" : "",
                        m->metalRoughTex ? "[金属粗糙]" : "", m->occlusionTex ? "[AO]" : "", m->emissiveTex ? "[自发光]" : "");
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    ImGui::End();
}

} // namespace eng
