#pragma once
// 编辑器界面：仿 UE / Unity 的布局。
//
//   ┌ 菜单栏 ───────────────────────────────────────────────────────────┐
//   ├ 工具栏：模拟 ▶‖ 单步 · 视图模式 · 线框 · 截图 · 捕获 · Shader · 保存 · 编辑器/游戏 ┤
//   ├──────────┬──────────────────────────────────┬──────────────────┤
//   │ 场景大纲  │            视 口                  │ 检视器            │
//   │          │   （场景渲染到离屏贴图再显示）       │                  │
//   ├──────────┤                                  ├──────────────────┤
//   │ 资源浏览  ├──────────────────────────────────┤ 环境光照/渲染/项目 │
//   │          │ 日志 · 性能 · Render Graph · Shader│                  │
//   ├──────────┴──────────────────────────────────┴──────────────────┤
//   └ 状态栏：状态 · FPS · GPU · 显存 · 选中 · 相机 · Validation · 错误 ───┘
//
// 编辑器只通过 EditorContext 里的指针读写引擎状态，引擎模块不知道编辑器的存在。
// F1 在「编辑器模式」和「游戏模式」（场景全屏、界面隐藏）之间切换。

#include "core/Config.h"
#include "core/Time.h"
#include "debug/StatsPanel.h"
#include "math/MathTypes.h"
#include "render/Camera.h"
#include "render/Lighting.h"
#include "render/Renderer.h"
#include "world/Scene.h"

#include <functional>
#include <optional>
#include <string>

struct ImVec2;

namespace eng {

class RenderDocCapture;
class ScenePhysics;
namespace rhi { class Device; }
namespace physics { class PhysicsWorld; }
namespace terrain {
struct TerrainSettings;
class TerrainRenderer;
} // namespace terrain
namespace ocean {
struct OceanSettings;
class OceanRenderer;
} // namespace ocean

// 模拟控制：暂停 / 单步 / 时间缩放。App 在推进固定步长时读取它。
struct SimulationControl {
    bool paused = false;
    float timeScale = 1.0f;
    int pendingSteps = 0; // 暂停时「单步」请求的步数
};

struct EditorContext {
    rhi::Device* device = nullptr;
    Renderer* renderer = nullptr;
    Scene* scene = nullptr;
    asset::AssetManager* assets = nullptr;
    std::filesystem::path* scenePath = nullptr; // 当前场景文件（工程相对路径）
    FlyCamera* camera = nullptr;
    FixedStepLoop* loop = nullptr;
    RenderDocCapture* renderdoc = nullptr;
    ConfigFile* config = nullptr;
    RendererSettings* rendererSettings = nullptr;
    LightingSettings* lighting = nullptr;
    FixedStepSettings* loopSettings = nullptr;
    CameraSettings* cameraSettings = nullptr;
    bool* renderdocAutoLoad = nullptr;
    SimulationControl* sim = nullptr;
    std::function<void()> saveConfig;
    std::function<void()> reloadConfig;
    std::function<void()> requestQuit;
    std::function<void()> saveScene;                                   // 保存到 *scenePath
    std::function<void(const std::filesystem::path&)> openScene;       // 打开并设为当前场景
    std::function<void()> newScene;
    std::function<void()> projectSettingsExtra; // 应用自己的参数（显示在「项目设置」末尾）
    // ---- M4：阴影、地形、物理 ----
    ShadowSettings* shadowSettings = nullptr;
    terrain::TerrainSettings* terrainSettings = nullptr;
    terrain::TerrainRenderer* terrain = nullptr;
    physics::PhysicsWorld* physics = nullptr;
    ScenePhysics* scenePhysics = nullptr;
    std::function<void()> regenerateTerrain; // 用 *terrainSettings 重新生成高度场（渲染 + 碰撞一起更新）
    std::function<void()> worldPanelExtra;   // 应用自己的信息（玩家状态等），显示在「地形与物理」末尾
    // ---- M5：海洋（面板直接改 *oceanSettings，再交给 ocean->setSettings） ----
    ocean::OceanSettings* oceanSettings = nullptr;
    ocean::OceanRenderer* ocean = nullptr;
};

class Editor {
public:
    void init(const EditorContext& ctx, bool startInGameMode);
    void shutdown();

    // 每帧在 ImGui::NewFrame 之后调用：选择渲染输出目标、处理快捷键、构建全部界面
    void draw(float dtSeconds);

    bool gameMode() const { return m_gameMode; }
    // 相机输入路由：编辑器模式下只有鼠标在视口里（或正在右键转视角）时才响应
    bool cameraMouseAllowed() const;
    bool cameraKeyboardAllowed() const;
    void setCameraLooking(bool looking) { m_cameraLooking = looking; }

    // 选中高亮：渲染列表里对选中实体设置 DrawItem::highlight
    std::optional<EntityId> selection() const { return m_selection; }
    float highlightStrength(double timeSeconds) const;
    // 按实体名选中（--select 命令行参数用，便于截图/演示）
    void selectByName(const std::string& name);
    // 拖进窗口的文件：导入资产数据库，模型直接放进场景
    void onFilesDropped(const std::vector<std::filesystem::path>& files);
    // 场景被整体替换（打开/新建）后调用：清掉指向旧实体的选择
    void onSceneReplaced();

private:
    // ---- 外壳 ----
    void handleShortcuts();
    void drawMenuBar();
    void drawToolbar();
    void drawStatusBar();
    void drawDockSpace();
    void buildDefaultLayout(unsigned int dockspaceId, const ImVec2& size);
    void drawViewport();
    void drawViewportOverlay(const ImVec2& origin, const ImVec2& size);
    void drawGameModeOverlay();
    void drawHelpPopup();
    void updateViewportTexture(VkImageView view);
    // ---- 面板（EditorPanels.cpp） ----
    void drawOutliner();
    void drawAssetBrowser();
    void drawInspector();
    void drawEntityInspector(Entity& e, EntityId id);
    void drawEnvironmentPanel();
    void drawRenderSettingsPanel();
    void drawProjectSettingsPanel();
    void drawWorldPanel(); // EditorWorldPanel.cpp：地形生成/LOD/分层材质 + 物理统计
    void drawOceanPanel(); // EditorOceanPanel.cpp：风浪、泡沫、水体着色
    void drawSaveSceneAsPopup();
    // ---- 操作 ----
    void pickAt(const ImVec2& uv);
    void focusSelection();
    void takeScreenshot();
    void applyRendererSettings();
    void deleteSelection();
    // 在相机前方放置一个模型的全部实例，并选中第一个（模型加载完成后才真正放入）
    void placeModelInFront(ModelHandle model, const std::string& name);
    void processPendingPlacements();
    struct PendingPlacement {
        ModelHandle model;
        Transform transform;
        std::string name;
    };
    std::vector<PendingPlacement> m_pendingPlacements;

    EditorContext m_ctx;
    FrameStats m_stats;
    bool m_gameMode = false;
    bool m_needLayout = false;
    bool m_layoutJustBuilt = false;
    bool m_showHelp = false;
    double m_gameModeEnteredAt = 0.0;

    struct Panels {
        bool outliner = true, assets = true, inspector = true, environment = true, renderSettings = true, project = true, world = true,
             ocean = true;
        bool console = true, profiler = true, renderGraph = true, shader = true;
    } m_show;

    // 视口
    VkImageView m_viewportView = VK_NULL_HANDLE;
    void* m_viewportTexture = nullptr; // ImGui 纹理 ID（VkDescriptorSet）
    vec2 m_viewportSize{1280.0f, 720.0f};
    bool m_viewportHovered = false;
    bool m_cameraLooking = false;

    // 选择
    std::optional<EntityId> m_selection;
    // 资源浏览器里选中的材质（检视器显示它）。不存 Material*：模型热重载会整个换掉材质数组，
    // 旧指针就悬空了。存「模型句柄 + 材质下标 + 模型版本」，每帧用 selectedMaterial() 重新解析，
    // 版本对不上（被热重载过）就自动取消选择。
    struct MaterialRef {
        ModelHandle model;
        uint32_t index = 0;
        uint32_t version = 0;
    };
    std::optional<MaterialRef> m_selectedMaterial;
    Material* selectedMaterial();
    bool m_scrollOutlinerToSelection = false; // 在视口里点选后，大纲滚动到该实体
    char m_outlinerFilter[64] = {};
    bool m_openSaveAs = false;
    char m_saveAsName[64] = "my_scene";
};

} // namespace eng
