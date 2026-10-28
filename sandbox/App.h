#pragma once
// 演示程序：把各模块串起来的「胶水」。
// 引擎模块不知道 App 的存在；App 负责创建模块、驱动主循环，界面交给 Editor。

#include "asset/AssetDatabase.h"
#include "asset/AssetManager.h"
#include "core/Config.h"
#include "core/JobSystem.h"
#include "core/Time.h"
#include "debug/ImGuiLayer.h"
#include "debug/RenderDocCapture.h"
#include "editor/Editor.h"
#include "Player.h"
#include "math/Transform.h"
#include "physics/PhysicsWorld.h"
#include "platform/Input.h"
#include "platform/Window.h"
#include "render/Camera.h"
#include "render/Lighting.h"
#include "render/Renderer.h"
#include "rhi/Device.h"
#include "terrain/Heightfield.h"
#include "terrain/TerrainRenderer.h"
#include "world/Scene.h"
#include "world/ScenePhysics.h"

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace eng {

struct LaunchOptions {
    int frames = -1;                    // --frames N：跑 N 帧后退出（冒烟测试）
    std::filesystem::path capturePath;  // --capture file.png：退出前截最后一帧
    std::optional<bool> validation;     // --validation / --no-validation，覆盖默认值
    std::optional<bool> renderdoc;      // --renderdoc / --no-renderdoc，覆盖 config
    std::string model;                  // --model path.glb：额外放入一个模型
    std::string scene;                  // --scene scenes/x.scene.json：打开指定场景
    bool hideUI = false;                // --no-ui：以游戏模式启动（场景全屏，F1 切回编辑器）
    std::string select;                 // --select 名字：启动时选中该实体（截图/演示用）
    std::optional<std::array<float, 5>> camera; // --camera x,y,z,yaw,pitch：编辑器相机初始位置（截图用）
    bool terrainLod = false;            // --terrain-lod：地形按四叉树级别着色
    std::vector<std::string> disabledPasses; // --disable-pass A,B：启动时关闭这些 pass（故障演练）
};

struct DebugSettings {
    bool renderdoc = false; // 启动时加载 RenderDoc（F12 捕获）
};

struct SceneSettings {
    std::string file = "scenes/island.scene.json"; // 启动时打开的场景
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
    void sanitizeSettings(); // 配置数值范围检查：非法值修正为最近的合法值并警告
    void saveSettings();
    bool openScene(const std::filesystem::path& projectRelative);
    void saveScene();
    void createDemoScene();
    void waitForModel(ModelHandle h);
    void fixedUpdate(double dt);
    void updateCamera(double dt);
    // 按 m_terrainSettings 生成高度场 → 上传渲染 → 重建地形碰撞体
    void rebuildTerrain();
    terrain::TerrainLayerTextures loadTerrainLayers();
    void applyPlayerSettings(); // 出生点高度按地形修正后交给 Player
    void addPlayerDraw(float alpha);
    void drawPlayerPanel();

    LaunchOptions m_options;
    ConfigFile m_config;
    WindowSettings m_windowSettings;
    RendererSettings m_rendererSettings;
    ShadowSettings m_shadowSettings;
    LightingSettings m_lighting;
    FixedStepSettings m_loopSettings;
    CameraSettings m_cameraSettings;
    DebugSettings m_debugSettings;
    SceneSettings m_sceneSettings;
    terrain::TerrainSettings m_terrainSettings;
    physics::PhysicsSettings m_physicsSettings;
    PlayerSettings m_playerSettings;

    Window m_window;
    Input m_input;
    JobSystem m_jobs;
    rhi::Device m_device;
    Renderer m_renderer;
    asset::AssetDatabase m_assetDb;
    asset::AssetManager m_assets;
    ImGuiLayer m_imgui;
    RenderDocCapture m_renderdoc;
    Editor m_editor;
    SimulationControl m_sim;
    FixedStepLoop m_loop;
    Clock m_clock;
    FlyCamera m_camera;
    double m_simTime = 0.0;

    Scene m_scene;
    std::filesystem::path m_scenePath;
    DrawList m_drawList;
    RenderView m_view;

    // ---- M4：地形 + 物理 + 玩家 ----
    terrain::Heightfield m_heightfield;
    terrain::TerrainRenderer m_terrain;
    physics::PhysicsWorld m_physics;
    ScenePhysics m_scenePhysics;
    physics::ShapeId m_terrainShape;
    physics::BodyId m_terrainBody;
    Player m_player;
    MaterialParams m_playerMaterial;
};

} // namespace eng
