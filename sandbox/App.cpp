#include "App.h"

#include "core/FileSystem.h"
#include "core/Log.h"

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

        // ---- 固定步长模拟（受编辑器的暂停 / 单步 / 时间缩放控制） ----
        int steps = m_loop.advance(m_sim.paused ? 0.0 : dt * m_sim.timeScale);
        if (m_sim.paused && m_sim.pendingSteps > 0) {
            steps = 1;
            --m_sim.pendingSteps;
        }
        for (int i = 0; i < steps; ++i) fixedUpdate(m_loop.fixedDt());

        // ---- 相机：输入是否交给相机由编辑器决定（鼠标在视口里才响应） ----
        updateCamera(dt);

        // ---- 界面：编辑器在这里决定渲染输出到视口贴图还是全屏 ----
        m_imgui.beginFrame();
        m_editor.draw(float(dt));
        m_imgui.endFrame();

        // 投影矩阵的宽高比取自本帧实际的场景分辨率（视口面板或整个窗口）
        const VkExtent2D se = m_renderer.sceneExtent();
        m_view.proj = m_camera.projection(float(se.width) / float(std::max(se.height, 1u)));

        // 冒烟测试顺带测窗口缩放：1/3 处缩小、2/3 处恢复，覆盖交换链与视口贴图重建路径
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
        // 渲染插值：在上一固定步与当前固定步之间按 alpha 混合；选中的实体带高亮
        m_scene.buildDrawList(m_drawList, float(m_loop.alpha()), m_editor.selection(), m_editor.highlightStrength(now));
        if (m_renderer.drawFrame(m_view, m_drawList)) ++frame;

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
    m_renderer.setLighting(m_lighting);
    if (!m_imgui.init(m_device, m_window, m_renderer.swapchain().format(), m_renderer.swapchain().imageCount())) return false;
    m_renderer.setOverlay([this](VkCommandBuffer cmd) { m_imgui.render(cmd); });
    createScene();

    EditorContext ctx;
    ctx.device = &m_device;
    ctx.renderer = &m_renderer;
    ctx.scene = &m_scene;
    ctx.camera = &m_camera;
    ctx.loop = &m_loop;
    ctx.renderdoc = &m_renderdoc;
    ctx.config = &m_config;
    ctx.rendererSettings = &m_rendererSettings;
    ctx.lighting = &m_lighting;
    ctx.loopSettings = &m_loopSettings;
    ctx.cameraSettings = &m_cameraSettings;
    ctx.renderdocAutoLoad = &m_debugSettings.renderdoc;
    ctx.sim = &m_sim;
    ctx.saveConfig = [this] { saveSettings(); };
    ctx.reloadConfig = [this] {
        m_config.load();
        loadSettings();
    };
    ctx.requestQuit = [this] { m_window.requestClose(); };
    ctx.projectSettingsExtra = [this] {
        ImGui::SliderFloat("立方体转速", &m_sceneSettings.spinSpeedDeg, 0.0f, 360.0f, "%.0f°/秒");
    };
    m_editor.init(ctx, options.hideUI);
    if (!options.select.empty()) m_editor.selectByName(options.select);
    return true;
}

void App::shutdown() {
    if (m_device.device()) vkDeviceWaitIdle(m_device.device());
    m_editor.shutdown(); // 必须在 ImGui 后端关闭之前：要释放视口贴图的 ImGui 描述符
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
            e.name = std::string("材质球/") + m.name;
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

void App::updateCamera(double dt) {
    const bool looking = m_camera.update(m_input, float(dt), m_editor.cameraMouseAllowed(), m_editor.cameraKeyboardAllowed());
    m_window.setCursorCaptured(looking);
    m_editor.setCameraLooking(looking);
    m_view.view = m_camera.view();
    m_view.position = m_camera.position();
    m_view.time = m_clock.seconds();
    m_view.dt = float(dt);
}

} // namespace eng
