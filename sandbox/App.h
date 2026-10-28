#pragma once
// 演示程序：把各模块串起来的「胶水」。
// 引擎模块不知道 App 的存在；App 负责创建模块、驱动主循环，界面交给 Editor。

#include "core/Config.h"
#include "core/Time.h"
#include "debug/ImGuiLayer.h"
#include "debug/RenderDocCapture.h"
#include "editor/Editor.h"
#include "math/Transform.h"
#include "platform/Input.h"
#include "platform/Window.h"
#include "render/Camera.h"
#include "render/Lighting.h"
#include "render/Renderer.h"
#include "rhi/Device.h"
#include "world/Scene.h"

#include <filesystem>
#include <optional>
#include <string>

namespace eng {

struct LaunchOptions {
    int frames = -1;                    // --frames N：跑 N 帧后退出（冒烟测试）
    std::filesystem::path capturePath;  // --capture file.png：退出前截最后一帧
    std::optional<bool> validation;     // --validation / --no-validation，覆盖默认值
    std::optional<bool> renderdoc;      // --renderdoc / --no-renderdoc，覆盖 config
    std::string model;                  // --model path.glb：替换 config 里的演示模型
    bool hideUI = false;                // --no-ui：以游戏模式启动（场景全屏，F1 切回编辑器）
    std::string select;                 // --select 名字：启动时选中该实体（截图/演示用）
};

struct DebugSettings {
    bool renderdoc = false; // 启动时加载 RenderDoc（F12 捕获）
};

struct SceneSettings {
    std::string model = "assets/models/ruins.glb"; // 工程相对路径或绝对路径
    vec3 modelPosition{0.0f, 0.0f, -3.0f};
    float modelScale = 1.0f;
    float spinSpeedDeg = 45.0f;
};

class App {
public:
    // 返回进程退出码：0 成功；1 初始化失败；2 运行中出现 error 日志（含 validation 报错）
    int run(const LaunchOptions& options);

private:
    bool init(const LaunchOptions& options);
    void shutdown();
    void loadSettings();
    void saveSettings();
    void createScene();
    void fixedUpdate(double dt);
    void updateCamera(double dt);

    LaunchOptions m_options;
    ConfigFile m_config;
    WindowSettings m_windowSettings;
    RendererSettings m_rendererSettings;
    LightingSettings m_lighting;
    FixedStepSettings m_loopSettings;
    CameraSettings m_cameraSettings;
    DebugSettings m_debugSettings;
    SceneSettings m_sceneSettings;

    Window m_window;
    Input m_input;
    rhi::Device m_device;
    Renderer m_renderer;
    ImGuiLayer m_imgui;
    RenderDocCapture m_renderdoc;
    Editor m_editor;
    SimulationControl m_sim;
    FixedStepLoop m_loop;
    Clock m_clock;
    FlyCamera m_camera;
    double m_simTime = 0.0;

    Scene m_scene;
    EntityId m_spinner = 0; // 按固定步长旋转的立方体（演示渲染插值）
    DrawList m_drawList;
    RenderView m_view;
};

} // namespace eng
