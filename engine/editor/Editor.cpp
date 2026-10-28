// 编辑器外壳：菜单栏、工具栏、状态栏、停靠布局、视口、快捷键。
// 各内容面板在 EditorPanels.cpp。
#include "editor/Editor.h"

#include "core/FileSystem.h"
#include "core/Log.h"
#include "debug/ImGuiLayer.h"
#include "debug/RenderDocCapture.h"
#include "debug/RenderPanels.h"
#include "editor/EditorInternal.h"
#include "editor/EditorTheme.h"
#include "math/Geometry.h"
#include "rhi/Device.h"

#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <imgui_internal.h> // DockBuilder*、BeginViewportSideBar

#include <chrono>
#include <cmath>
#include <ctime>

namespace eng {

using namespace editor;

namespace {

ImTextureID toTextureId(void* p) {
    return static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(p));
}

// 工具栏里的竖分隔线
void toolbarSeparator() {
    ImGui::SameLine(0.0f, 10.0f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetFrameHeight();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y + 3), ImVec2(p.x, p.y + h - 3), ImGui::GetColorU32(ImGuiCol_Separator), 1.0f);
    ImGui::Dummy(ImVec2(1, h));
    ImGui::SameLine(0.0f, 10.0f);
}

double nowSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

} // namespace

// =====================================================================================
// 生命周期
// =====================================================================================

void Editor::init(const EditorContext& ctx, bool startInGameMode) {
    m_ctx = ctx;
    m_gameMode = startInGameMode;
    m_gameModeEnteredAt = nowSeconds(); // 以游戏模式启动时也显示「如何返回编辑器」的提示
    theme::apply();
    // 没有布局文件（第一次启动或删掉了）→ 生成默认的 UE 风格布局
    m_needLayout = !std::filesystem::exists(ImGuiLayer::layoutIniPath());
}

void Editor::shutdown() {
    if (!m_ctx.device) return;
    // 视口贴图的 ImGui 描述符可能还在延迟销毁队列里：先等 GPU 空闲并清空队列，
    // 必须在 ImGui Vulkan 后端关闭之前完成
    vkDeviceWaitIdle(m_ctx.device->device());
    m_ctx.device->flushDeferred();
    if (m_viewportTexture) ImGui_ImplVulkan_RemoveTexture(static_cast<VkDescriptorSet>(m_viewportTexture));
    m_viewportTexture = nullptr;
    m_ctx.device = nullptr;
}

void Editor::updateViewportTexture(VkImageView view) {
    if (view == m_viewportView && m_viewportTexture) return;
    if (m_viewportTexture) {
        // 旧描述符可能还被在途的帧引用：延迟到 GPU 用完再释放
        VkDescriptorSet old = static_cast<VkDescriptorSet>(m_viewportTexture);
        m_ctx.device->deferDestroy([old] { ImGui_ImplVulkan_RemoveTexture(old); });
    }
    m_viewportTexture = ImGui_ImplVulkan_AddTexture(m_ctx.renderer->defaults().linearClamp(), view,
                                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    m_viewportView = view;
}

bool Editor::cameraMouseAllowed() const {
    if (m_gameMode) return !ImGui::GetIO().WantCaptureMouse || m_cameraLooking;
    return m_viewportHovered || m_cameraLooking;
}

bool Editor::cameraKeyboardAllowed() const {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) return false; // 正在输入框里打字
    if (m_gameMode) return true;
    return m_viewportHovered || m_cameraLooking;
}

float Editor::highlightStrength(double timeSeconds) const {
    // 轻微「呼吸」：比静态高亮更容易在复杂场景里一眼找到
    return 0.35f + 0.15f * float(std::sin(timeSeconds * 4.0));
}

// =====================================================================================
// 每帧入口
// =====================================================================================

void Editor::draw(float dtSeconds) {
    m_stats.addFrame(dtSeconds);
    ImGuiIO& io = ImGui::GetIO();
    // 右键转视角时鼠标被锁定：让 ImGui 暂时忽略鼠标，避免光标「穿过」各面板产生悬停效果
    if (m_cameraLooking)
        io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
    else
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;

    handleShortcuts();
    processPendingPlacements();

    if (m_gameMode) {
        m_ctx.renderer->useBackbufferOutput();
        m_viewportHovered = false;
        drawGameModeOverlay();
        drawShaderErrorBanner(m_ctx.renderer->pipelines());
        drawHelpPopup();
        return;
    }

    // 视口尺寸取上一帧视口面板的大小（本帧的布局还没算出来），差一帧肉眼不可见
    const ImVec2 fbScale = io.DisplayFramebufferScale;
    const VkImageView view = m_ctx.renderer->useViewportOutput(uint32_t(m_viewportSize.x * fbScale.x),
                                                               uint32_t(m_viewportSize.y * fbScale.y));
    updateViewportTexture(view);

    // 顺序要求：菜单栏 → 工具栏 → 状态栏（它们占用视口边缘）→ 停靠区（占用剩余区域）
    drawMenuBar();
    drawToolbar();
    drawStatusBar();
    drawDockSpace();

    // 停靠面板不显示标签上的关闭按钮（满屏的 × 很乱）；开关统一走「视图」菜单
    drawViewport();
    if (m_show.outliner) drawOutliner();
    if (m_show.assets) drawAssetBrowser();
    if (m_show.inspector) drawInspector();
    if (m_show.environment) drawEnvironmentPanel();
    if (m_show.renderSettings) drawRenderSettingsPanel();
    if (m_show.project) drawProjectSettingsPanel();
    if (m_show.world) drawWorldPanel();
    if (m_show.ocean) drawOceanPanel();
    if (m_show.console) drawConsolePanel(nullptr);
    if (m_show.profiler) {
        const LoopStats loop{m_ctx.loop->settings().fixedHz, m_ctx.loop->lastSteps(), m_ctx.loop->alpha(), m_ctx.sim->paused,
                             m_ctx.sim->timeScale};
        drawProfilerPanel(m_stats, *m_ctx.device, *m_ctx.renderer, loop, nullptr);
    }
    if (m_show.renderGraph) drawRenderGraphPanel(*m_ctx.renderer, m_ctx.renderdoc, nullptr);
    if (m_show.shader) drawShaderPanel(m_ctx.renderer->pipelines(), nullptr);

    // 刚生成默认布局：每组标签页默认选中最常用的那个（日志、环境光照），最后把焦点交给视口
    if (m_layoutJustBuilt) {
        ImGui::SetWindowFocus(kConsoleWindow);
        ImGui::SetWindowFocus(kEnvironmentWindow);
        ImGui::SetWindowFocus(kViewportWindow);
        m_layoutJustBuilt = false;
    }
    drawHelpPopup();
    drawSaveSceneAsPopup();
}

void Editor::drawSaveSceneAsPopup() {
    if (m_openSaveAs) {
        ImGui::OpenPopup("场景另存为");
        m_openSaveAs = false;
    }
    if (ImGui::BeginPopupModal("场景另存为", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextDisabled("保存到 scenes/<名字>.scene.json");
        ImGui::SetNextItemWidth(260);
        ImGui::InputText("名字", m_saveAsName, sizeof(m_saveAsName));
        if (ImGui::Button("保存", ImVec2(120, 0)) && m_saveAsName[0] && m_ctx.scenePath && m_ctx.saveScene) {
            *m_ctx.scenePath = std::filesystem::path("scenes") / (std::string(m_saveAsName) + ".scene.json");
            m_ctx.saveScene();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("取消", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

Material* Editor::selectedMaterial() {
    if (!m_selectedMaterial) return nullptr;
    const MaterialRef& ref = *m_selectedMaterial;
    Model* model = m_ctx.assets->mutableModel(ref.model);
    // 模型被热重载过（版本变了）、正在重载、或材质变少了：选中的那份材质已经不存在，取消选择
    if (!model || m_ctx.assets->version(ref.model) != ref.version || ref.index >= model->materials.size()) {
        m_selectedMaterial.reset();
        return nullptr;
    }
    return &model->materials[ref.index];
}

void Editor::onSceneReplaced() {
    m_selection.reset();
    m_selectedMaterial.reset();
}

void Editor::deleteSelection() {
    if (!m_selection) return;
    m_ctx.scene->remove(*m_selection);
    // 删除会让后面实体的 id 前移；最简单可靠的做法是清掉选择
    m_selection.reset();
    m_selectedMaterial.reset();
}

void Editor::placeModelInFront(ModelHandle model, const std::string& name) {
    // 放在相机前方 6 米处的地面上（y 不低于 0：不要埋进海平面以下）
    Transform t;
    const vec3 p = m_ctx.camera->position() + m_ctx.camera->forward() * 6.0f;
    t.position = vec3(p.x, std::max(0.0f, p.y - 1.5f), p.z);
    // 模型还在后台烘焙/加载时，不知道它有几个实例：先记下，等加载完成再实例化（见 processPendingPlacements）
    m_pendingPlacements.push_back({model, t, name});
}

void Editor::processPendingPlacements() {
    for (size_t i = 0; i < m_pendingPlacements.size();) {
        const PendingPlacement& p = m_pendingPlacements[i];
        if (m_ctx.assets->status(p.model) == asset::AssetStatus::Loading) {
            ++i;
            continue;
        }
        // 就绪或失败都放进场景：失败时显示兜底立方体，至少看得到「这里有个东西没加载出来」
        m_selection = m_ctx.scene->instantiate(p.model, p.transform, p.name);
        m_selectedMaterial.reset();
        m_scrollOutlinerToSelection = true;
        m_pendingPlacements.erase(m_pendingPlacements.begin() + long(i));
    }
}

void Editor::onFilesDropped(const std::vector<std::filesystem::path>& files) {
    for (const auto& file : files) {
        auto& db = m_ctx.assets->database();
        const auto guid = db.importExternal(file);
        if (!guid) continue;
        const auto rec = db.find(*guid);
        if (rec && rec->type == asset::AssetType::Model) {
            placeModelInFront(m_ctx.assets->loadModel(*guid), file.stem().string());
            LOG_INFO("已放入场景: {}（首次导入会在后台烘焙，完成前显示品红占位立方体）", file.filename().string());
        }
    }
}

void Editor::selectByName(const std::string& name) {
    for (EntityId id = 0; id < m_ctx.scene->entities().size(); ++id) {
        if (m_ctx.scene->entities()[id].name == name) {
            m_selection = id;
            m_selectedMaterial.reset();
            m_scrollOutlinerToSelection = true;
            return;
        }
    }
    LOG_WARN("没有名为 {} 的实体", name);
}

// =====================================================================================
// 快捷键
// =====================================================================================

void Editor::handleShortcuts() {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) return; // 打字时不触发任何快捷键
    auto pressed = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, false); };
    SimulationControl& sim = *m_ctx.sim;

    if (pressed(ImGuiKey_F1)) {
        m_gameMode = !m_gameMode;
        m_gameModeEnteredAt = nowSeconds();
    }
    if (pressed(ImGuiKey_Escape)) {
        if (m_gameMode)
            m_gameMode = false; // 游戏模式下 Esc 回到编辑器
        else {
            m_selection.reset();
            m_selectedMaterial.reset();
        }
    }
    if (pressed(ImGuiKey_F5)) sim.paused = !sim.paused;
    if (pressed(ImGuiKey_F6)) {
        sim.paused = true; // 单步总是在暂停状态下进行
        ++sim.pendingSteps;
    }
    if (pressed(ImGuiKey_F9)) takeScreenshot();
    if (io.KeyCtrl && pressed(ImGuiKey_S)) {
        if (m_ctx.saveScene) m_ctx.saveScene();
        if (m_ctx.saveConfig) m_ctx.saveConfig();
    }
    if (pressed(ImGuiKey_Delete) && !m_gameMode) deleteSelection();
    if (io.KeyCtrl && pressed(ImGuiKey_R)) m_ctx.renderer->pipelines().reloadAll();
    if (!io.KeyCtrl && pressed(ImGuiKey_F) && !m_gameMode) focusSelection();
}

// =====================================================================================
// 菜单栏
// =====================================================================================

void Editor::drawMenuBar() {
    if (!ImGui::BeginMainMenuBar()) return;
    SimulationControl& sim = *m_ctx.sim;
    RendererSettings& rs = *m_ctx.rendererSettings;

    if (ImGui::BeginMenu("文件")) {
        if (ImGui::MenuItem("新建场景") && m_ctx.newScene) m_ctx.newScene();
        if (ImGui::BeginMenu("打开场景")) {
            // 列出 scenes/ 目录下的场景文件
            bool any = false;
            std::error_code ec;
            for (const auto& entry : std::filesystem::directory_iterator(fs::resolve("scenes"), ec)) {
                const std::string fname = entry.path().filename().string();
                if (fname.size() < 11 || fname.substr(fname.size() - 11) != ".scene.json") continue;
                any = true;
                const bool current = m_ctx.scenePath && entry.path().filename() == m_ctx.scenePath->filename();
                if (ImGui::MenuItem(fname.c_str(), nullptr, current) && m_ctx.openScene)
                    m_ctx.openScene(std::filesystem::path("scenes") / entry.path().filename());
            }
            if (!any) ImGui::TextDisabled("scenes/ 下还没有场景文件");
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("保存场景与配置", "Ctrl+S")) {
            if (m_ctx.saveScene) m_ctx.saveScene();
            if (m_ctx.saveConfig) m_ctx.saveConfig();
        }
        if (ImGui::MenuItem("场景另存为…")) m_openSaveAs = true;
        ImGui::Separator();
        if (ImGui::MenuItem("从文件重新加载配置") && m_ctx.reloadConfig) m_ctx.reloadConfig();
        ImGui::Separator();
        if (ImGui::MenuItem("截图", "F9")) takeScreenshot();
        ImGui::Separator();
        if (ImGui::MenuItem("退出") && m_ctx.requestQuit) m_ctx.requestQuit();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("编辑")) {
        if (ImGui::MenuItem("聚焦选中物体", "F", false, m_selection.has_value())) focusSelection();
        if (ImGui::MenuItem("取消选择", "Esc", false, m_selection.has_value() || m_selectedMaterial.has_value())) {
            m_selection.reset();
            m_selectedMaterial.reset();
        }
        if (ImGui::MenuItem("删除选中实体", "Delete", false, m_selection.has_value())) deleteSelection();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("视图")) {
        ImGui::MenuItem("场景大纲", nullptr, &m_show.outliner);
        ImGui::MenuItem("资源", nullptr, &m_show.assets);
        ImGui::MenuItem("检视器", nullptr, &m_show.inspector);
        ImGui::MenuItem("环境光照", nullptr, &m_show.environment);
        ImGui::MenuItem("渲染设置", nullptr, &m_show.renderSettings);
        ImGui::MenuItem("项目设置", nullptr, &m_show.project);
        ImGui::MenuItem("地形与物理", nullptr, &m_show.world);
        ImGui::MenuItem("海洋", nullptr, &m_show.ocean);
        ImGui::Separator();
        ImGui::MenuItem("日志", nullptr, &m_show.console);
        ImGui::MenuItem("性能", nullptr, &m_show.profiler);
        ImGui::MenuItem("Render Graph", nullptr, &m_show.renderGraph);
        ImGui::MenuItem("Shader", nullptr, &m_show.shader);
        ImGui::Separator();
        if (ImGui::MenuItem("重置布局")) {
            m_show = Panels{};
            m_needLayout = true;
        }
        if (ImGui::MenuItem("游戏模式（全屏场景）", "F1")) {
            m_gameMode = true;
            m_gameModeEnteredAt = nowSeconds();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("渲染")) {
        if (ImGui::BeginMenu("调试视图")) {
            for (int i = 0; i < IM_ARRAYSIZE(kDebugViewNames); ++i)
                if (ImGui::MenuItem(kDebugViewNames[i], nullptr, rs.debugView == i)) {
                    rs.debugView = i;
                    applyRendererSettings();
                }
            ImGui::EndMenu();
        }
        ImGui::MenuItem("线框", nullptr, &m_ctx.renderer->forwardPass().wireframe);
        if (ImGui::MenuItem("垂直同步", nullptr, &rs.vsync)) applyRendererSettings();
        if (ImGui::BeginMenu("色调映射")) {
            for (int i = 0; i < IM_ARRAYSIZE(kTonemapperNames); ++i)
                if (ImGui::MenuItem(kTonemapperNames[i], nullptr, m_ctx.lighting->tonemapper == i)) {
                    m_ctx.lighting->tonemapper = i;
                    m_ctx.renderer->setLighting(*m_ctx.lighting);
                }
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("模拟")) {
        if (ImGui::MenuItem(sim.paused ? "▶ 继续" : "‖ 暂停", "F5")) sim.paused = !sim.paused;
        if (ImGui::MenuItem("▶| 单步", "F6")) {
            sim.paused = true;
            ++sim.pendingSteps;
        }
        if (ImGui::BeginMenu("时间缩放")) {
            for (float s : kTimeScales) {
                char label[16];
                snprintf(label, sizeof(label), "×%.2g", s);
                if (ImGui::MenuItem(label, nullptr, sim.timeScale == s)) sim.timeScale = s;
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("工具")) {
        if (ImGui::MenuItem("重新编译全部 Shader", "Ctrl+R")) m_ctx.renderer->pipelines().reloadAll();
        const bool rd = m_ctx.renderdoc && m_ctx.renderdoc->available();
        if (ImGui::MenuItem("RenderDoc 捕获下一帧", "F12", false, rd)) m_ctx.renderdoc->triggerCapture();
        if (ImGui::MenuItem("打开 RenderDoc", nullptr, false, rd)) m_ctx.renderdoc->launchReplayUI();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("帮助")) {
        if (ImGui::MenuItem("快捷键与操作")) m_showHelp = true;
        ImGui::EndMenu();
    }

    // 右侧：品牌标识
    const char* brand = "◆ OCEAN ENGINE";
    const float w = ImGui::CalcTextSize(brand).x;
    ImGui::SameLine(ImGui::GetWindowWidth() - w - 16.0f);
    ImGui::TextColored(theme::kAccentBright, "%s", brand);
    ImGui::EndMainMenuBar();
}

// =====================================================================================
// 工具栏
// =====================================================================================

void Editor::drawToolbar() {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float height = ImGui::GetFrameHeight() + 2.0f * 6.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 6.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.060f, 0.075f, 0.095f, 1.0f));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse;
    if (ImGui::BeginViewportSideBar("##Toolbar", ImGui::GetMainViewport(), ImGuiDir_Up, height, flags)) {
        SimulationControl& sim = *m_ctx.sim;
        RendererSettings& rs = *m_ctx.rendererSettings;

        // ---- 模拟 ----
        if (theme::toolButton(sim.paused ? "▶ 继续" : "‖ 暂停", !sim.paused, "运行 / 暂停固定步长模拟 (F5)\n暂停时相机和渲染照常工作"))
            sim.paused = !sim.paused;
        ImGui::SameLine();
        ImGui::BeginDisabled(!sim.paused);
        if (theme::toolButton("▶| 单步", false, "暂停时推进一个固定步长 (F6)")) ++sim.pendingSteps;
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(78.0f);
        char scaleLabel[16];
        snprintf(scaleLabel, sizeof(scaleLabel), "×%.2g", sim.timeScale);
        if (ImGui::BeginCombo("##timescale", scaleLabel)) {
            for (float s : kTimeScales) {
                char l[16];
                snprintf(l, sizeof(l), "×%.2g", s);
                if (ImGui::Selectable(l, sim.timeScale == s)) sim.timeScale = s;
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("时间缩放：慢放观察插值与物理");

        toolbarSeparator();
        // ---- 视图 ----
        ImGui::SetNextItemWidth(118.0f);
        if (ImGui::Combo("##debugview", &rs.debugView, kDebugViewNames, IM_ARRAYSIZE(kDebugViewNames))) applyRendererSettings();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("视图模式：检查法线、粗糙度等材质通道");
        ImGui::SameLine();
        bool& wire = m_ctx.renderer->forwardPass().wireframe;
        if (theme::toolButton("▦ 线框", wire, "线框模式")) wire = !wire;
        ImGui::SameLine();
        bool& sky = m_ctx.renderer->graph().passEnabled("Sky");
        if (theme::toolButton("☀ 天空", sky, "开关 Sky pass（关闭后显示背景色）")) sky = !sky;

        toolbarSeparator();
        // ---- 工具 ----
        if (theme::toolButton("◉ 截图", false, "保存当前画面到 captures/ (F9)")) takeScreenshot();
        ImGui::SameLine();
        const bool rd = m_ctx.renderdoc && m_ctx.renderdoc->available();
        ImGui::BeginDisabled(!rd);
        if (theme::toolButton("● 捕获帧", false, rd ? "RenderDoc 捕获下一帧 (F12)" : "RenderDoc 未加载（项目设置中开启后重启）"))
            m_ctx.renderdoc->triggerCapture();
        ImGui::EndDisabled();
        ImGui::SameLine();
        const uint32_t shaderErrors = m_ctx.renderer->pipelines().errorCount();
        if (shaderErrors > 0) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.45f, 0.10f, 0.10f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.60f, 0.15f, 0.15f, 1.0f));
        }
        char shaderLabel[48];
        snprintf(shaderLabel, sizeof(shaderLabel), shaderErrors ? "↻ Shader ⚠%u" : "↻ Shader", shaderErrors);
        if (theme::toolButton(shaderLabel, false, "重新编译全部 shader (Ctrl+R)\n保存 .hlsl 时会自动热重载")) {
            m_ctx.renderer->pipelines().reloadAll();
        }
        if (shaderErrors > 0) ImGui::PopStyleColor(2);
        ImGui::SameLine();
        if (theme::toolButton("✔ 保存", false, "保存全部参数到 config/engine.json (Ctrl+S)") && m_ctx.saveConfig) m_ctx.saveConfig();

        // ---- 右侧：编辑器 / 游戏 模式切换（分段按钮） ----
        const float segW = ImGui::CalcTextSize("编辑器").x + ImGui::CalcTextSize("▶ 游戏").x + style.FramePadding.x * 4 + style.ItemSpacing.x;
        ImGui::SameLine(ImGui::GetWindowWidth() - segW - 12.0f);
        theme::toolButton("编辑器", true, "当前：编辑器模式");
        ImGui::SameLine(0, 2);
        if (theme::toolButton("▶ 游戏", false, "游戏模式：场景全屏、隐藏界面 (F1)，Esc 返回")) {
            m_gameMode = true;
            m_gameModeEnteredAt = nowSeconds();
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

// =====================================================================================
// 状态栏：背景色反映整体状态（正常 青 / 暂停 橙 / 有错误 红），余光就能注意到
// =====================================================================================

void Editor::drawStatusBar() {
    const uint32_t shaderErrors = m_ctx.renderer->pipelines().errorCount();
    const uint32_t logErrors = log::errorCount();
    const bool paused = m_ctx.sim->paused;
    ImVec4 bg(0.035f, 0.20f, 0.235f, 1.0f);
    if (paused) bg = ImVec4(0.30f, 0.17f, 0.03f, 1.0f);
    if (shaderErrors > 0) bg = ImVec4(0.36f, 0.06f, 0.06f, 1.0f);

    const float height = ImGui::GetFrameHeight();
    ImGui::PushStyleColor(ImGuiCol_WindowBg, bg);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 2.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse;
    if (ImGui::BeginViewportSideBar("##StatusBar", ImGui::GetMainViewport(), ImGuiDir_Down, height, flags)) {
        ImGui::AlignTextToFramePadding();
        auto sep = [] {
            // 手绘竖线：不依赖字体里有没有制表符字形
            ImGui::SameLine(0, 12);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float h = ImGui::GetFrameHeight();
            ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y + 4), ImVec2(p.x, p.y + h - 4), IM_COL32(255, 255, 255, 50));
            ImGui::Dummy(ImVec2(1, h - 4));
            ImGui::SameLine(0, 12);
        };
        // 状态
        if (shaderErrors > 0)
            ImGui::TextColored(theme::kError, "⚠ Shader 错误 %u", shaderErrors);
        else if (paused)
            ImGui::TextColored(theme::kWarn, "‖ 模拟已暂停");
        else
            ImGui::TextColored(theme::kOk, "● 就绪");
        sep();
        ImGui::Text("FPS %.0f", m_stats.fps());
        ImGui::SameLine(0, 12);
        ImGui::Text("CPU %.2f ms", m_stats.smoothedMs());
        ImGui::SameLine(0, 12);
        ImGui::Text("GPU %.2f ms", m_ctx.renderer->profiler().totalMs());
        sep();
        float used = 0, budget = 0;
        queryVramMB(*m_ctx.device, used, budget);
        char vram[48];
        snprintf(vram, sizeof(vram), "显存 %.0f MB", used);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, used > 2048.0f ? theme::kError : theme::kAccent);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0.3f));
        ImGui::ProgressBar(std::min(1.0f, used / 2048.0f), ImVec2(120.0f, height - 6.0f), vram);
        ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("显存使用 / 项目预算 2048 MB（驱动可用 %.0f MB）", budget);
        sep();
        ImGui::TextDisabled("△ %.1fk", double(m_ctx.scene->lastTriangleCount()) / 1000.0);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("本帧绘制的三角形数（已按 LOD 选择）");
        sep();
        const std::string sceneName = m_ctx.scenePath ? m_ctx.scenePath->filename().string() : std::string("未命名");
        ImGui::TextDisabled("场景 %s", sceneName.c_str());
        if (const uint32_t pending = m_ctx.assets->pendingCount()) {
            sep();
            ImGui::TextColored(theme::kWarn, "↻ 资源加载中 %u", pending);
        }

        // ---- 右对齐部分 ----
        char selText[128] = "未选中";
        if (m_selection) snprintf(selText, sizeof(selText), "选中: %s", m_ctx.scene->entity(*m_selection).name.c_str());
        const vec3 cp = m_ctx.camera->position();
        char camText[64];
        snprintf(camText, sizeof(camText), "相机 (%.1f, %.1f, %.1f)", cp.x, cp.y, cp.z);
        const char* valText = m_ctx.device->validationEnabled() ? "Validation ✔" : "Validation 关";
        char errText[32];
        snprintf(errText, sizeof(errText), "错误 %u", logErrors);
        const float spacing = 30.0f;
        const float rightW = ImGui::CalcTextSize(selText).x + ImGui::CalcTextSize(camText).x + ImGui::CalcTextSize(valText).x +
                             ImGui::CalcTextSize(errText).x + spacing * 3 + 20.0f;
        ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 20.0f, ImGui::GetWindowWidth() - rightW));
        ImGui::TextColored(m_selection ? theme::kSelection : theme::kTextDim, "%s", selText);
        ImGui::SameLine(0, spacing);
        ImGui::TextDisabled("%s", camText);
        ImGui::SameLine(0, spacing);
        ImGui::TextColored(m_ctx.device->validationEnabled() ? theme::kOk : theme::kTextDim, "%s", valText);
        ImGui::SameLine(0, spacing);
        // 点击错误计数 → 打开并聚焦日志面板
        ImGui::PushStyleColor(ImGuiCol_Text, logErrors ? theme::kError : theme::kTextDim);
        if (ImGui::Selectable(errText, false, ImGuiSelectableFlags_None, ImGui::CalcTextSize(errText))) {
            m_show.console = true;
            ImGui::SetWindowFocus(kConsoleWindow);
        }
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("点击打开日志");
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

// =====================================================================================
// 停靠布局
// =====================================================================================

void Editor::drawDockSpace() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImGuiID dockId = ImGui::GetID("EditorDockSpace");
    // 布局文件里没有这个停靠节点也要重建：例如第一次启动就是 --no-ui 游戏模式，
    // 退出时 ImGui 照样写了 ini（但里面没有停靠布局），只看「文件是否存在」会误判
    if (m_needLayout || !ImGui::DockBuilderGetNode(dockId)) {
        buildDefaultLayout(dockId, vp->WorkSize);
        m_needLayout = false;
        m_layoutJustBuilt = true;
    }
    ImGui::DockSpaceOverViewport(dockId, vp, ImGuiDockNodeFlags_None);
}

void Editor::buildDefaultLayout(ImGuiID dockId, const ImVec2& size) {
    ImGui::DockBuilderRemoveNode(dockId);
    ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockId, size);

    // 左 17%：场景大纲（上）+ 资源（下）
    // 右 24%：检视器（上）+ 环境/渲染/项目设置（下，标签页）
    // 中间：视口（上）+ 日志/性能/Render Graph/Shader（下 30%，标签页）
    ImGuiID left = 0, rest = 0, right = 0, center = 0, bottom = 0, viewport = 0;
    ImGui::DockBuilderSplitNode(dockId, ImGuiDir_Left, 0.17f, &left, &rest);
    ImGui::DockBuilderSplitNode(rest, ImGuiDir_Right, 0.29f, &right, &center);
    ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.30f, &bottom, &viewport);
    ImGuiID leftTop = 0, leftBottom = 0, rightTop = 0, rightBottom = 0;
    ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.40f, &leftBottom, &leftTop);
    ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.55f, &rightBottom, &rightTop);

    ImGui::DockBuilderDockWindow(kViewportWindow, viewport);
    ImGui::DockBuilderDockWindow(kOutlinerWindow, leftTop);
    ImGui::DockBuilderDockWindow(kAssetsWindow, leftBottom);
    ImGui::DockBuilderDockWindow(kInspectorWindow, rightTop);
    ImGui::DockBuilderDockWindow(kEnvironmentWindow, rightBottom);
    ImGui::DockBuilderDockWindow(kRenderSettingsWindow, rightBottom);
    ImGui::DockBuilderDockWindow(kProjectWindow, rightBottom);
    ImGui::DockBuilderDockWindow(kWorldWindow, rightBottom);
    ImGui::DockBuilderDockWindow(kOceanWindow, rightBottom);
    ImGui::DockBuilderDockWindow(kConsoleWindow, bottom);
    ImGui::DockBuilderDockWindow(kProfilerWindow, bottom);
    ImGui::DockBuilderDockWindow(kRenderGraphWindow, bottom);
    ImGui::DockBuilderDockWindow(kShaderWindow, bottom);
    // 视口只有一个窗口时隐藏标签栏，让画面尽量大
    if (ImGuiDockNode* node = ImGui::DockBuilderGetNode(viewport)) node->LocalFlags |= ImGuiDockNodeFlags_AutoHideTabBar;
    ImGui::DockBuilderFinish(dockId);
}

// =====================================================================================
// 视口
// =====================================================================================

void Editor::drawViewport() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    const bool visible = ImGui::Begin(kViewportWindow, nullptr,
                                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse);
    ImGui::PopStyleVar();
    if (!visible) {
        m_viewportHovered = false;
        ImGui::End();
        return;
    }
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 size = ImGui::GetContentRegionAvail();
    size.x = std::max(size.x, 16.0f);
    size.y = std::max(size.y, 16.0f);
    m_viewportSize = {size.x, size.y};

    // 场景图像（上一帧尺寸 = 本帧尺寸，除非刚拖动过面板边缘）
    if (m_viewportTexture)
        ImGui::GetWindowDrawList()->AddImage(toTextureId(m_viewportTexture), origin, ImVec2(origin.x + size.x, origin.y + size.y));

    // 一个覆盖整个视口的隐形按钮负责接收鼠标：悬停判断、左键点选、阻止拖动窗口
    ImGui::InvisibleButton("##viewport_input", size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    m_viewportHovered = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        const ImVec2 m = ImGui::GetMousePos();
        pickAt(ImVec2((m.x - origin.x) / size.x, (m.y - origin.y) / size.y));
    }
    if (m_viewportHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) focusSelection();

    drawViewportOverlay(origin, size);
    ImGui::End();
}

void Editor::drawViewportOverlay(const ImVec2& origin, const ImVec2& size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 end(origin.x + size.x, origin.y + size.y);

    // ---- 左上：浮动工具条（子窗口：悬停在它上面时不会触发视口点选） ----
    ImGui::SetCursorScreenPos(ImVec2(origin.x + 8, origin.y + 8));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.04f, 0.05f, 0.07f, 0.72f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 4));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 5.0f);
    if (ImGui::BeginChild("##vp_tools", ImVec2(0, 0),
                          ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_AutoResizeY,
                          ImGuiWindowFlags_NoScrollbar)) {
        RendererSettings& rs = *m_ctx.rendererSettings;
        ImGui::SetNextItemWidth(96.0f);
        if (ImGui::Combo("##vp_view", &rs.debugView, kDebugViewNames, IM_ARRAYSIZE(kDebugViewNames))) applyRendererSettings();
        ImGui::SameLine();
        bool& wire = m_ctx.renderer->forwardPass().wireframe;
        if (theme::toolButton("▦", wire, "线框")) wire = !wire;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        ImGui::SliderFloat("##vp_speed", &m_ctx.camera->speed(), 0.1f, 200.0f, "速度 %.1f", ImGuiSliderFlags_Logarithmic);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("相机移动速度（米/秒），也可以用滚轮调节");
        ImGui::SameLine();
        ImGui::BeginDisabled(!m_selection);
        if (theme::toolButton("◎ 聚焦", false, "把相机移到选中物体前 (F / 双击)")) focusSelection();
        ImGui::EndDisabled();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    // ---- 右上：分辨率与帧率（只显示，不交互） ----
    const VkExtent2D se = m_ctx.renderer->sceneExtent();
    char info[96];
    snprintf(info, sizeof(info), "%u×%u · %.0f FPS · GPU %.2f ms", se.width, se.height, m_stats.fps(),
             m_ctx.renderer->profiler().totalMs());
    const ImVec2 infoSize = ImGui::CalcTextSize(info);
    const ImVec2 infoPos(end.x - infoSize.x - 14, origin.y + 12);
    dl->AddRectFilled(ImVec2(infoPos.x - 6, infoPos.y - 3), ImVec2(infoPos.x + infoSize.x + 6, infoPos.y + infoSize.y + 3),
                      IM_COL32(8, 12, 18, 170), 4.0f);
    dl->AddText(infoPos, IM_COL32(200, 225, 235, 255), info);

    // ---- 顶部中央：暂停 / shader 错误提示 ----
    float bannerY = origin.y + 46;
    auto banner = [&](const char* text, ImU32 bg) {
        const ImVec2 ts = ImGui::CalcTextSize(text);
        const ImVec2 p(origin.x + (size.x - ts.x) * 0.5f, bannerY);
        dl->AddRectFilled(ImVec2(p.x - 10, p.y - 4), ImVec2(p.x + ts.x + 10, p.y + ts.y + 4), bg, 4.0f);
        dl->AddText(p, IM_COL32(255, 255, 255, 255), text);
        bannerY += ts.y + 14;
    };
    if (const uint32_t n = m_ctx.renderer->pipelines().errorCount()) {
        char t[96];
        snprintf(t, sizeof(t), "⚠ Shader 编译错误 %u 个 —— 见下方「Shader」面板", n);
        banner(t, IM_COL32(150, 20, 20, 230));
    }
    if (m_ctx.sim->paused) banner("‖ 模拟已暂停（F5 继续，F6 单步）", IM_COL32(150, 85, 10, 220));

    // ---- 右上（信息条下方）：坐标轴指示器（随相机旋转），帮助判断朝向 ----
    const mat4 view = m_ctx.camera->view();
    const float axisLen = 30.0f;
    const float axisRadius = axisLen + 10.0f;
    const ImVec2 axisCenter(end.x - axisRadius - 12.0f, infoPos.y + infoSize.y + 12.0f + axisRadius);
    struct Axis {
        vec3 dir;
        ImU32 color;
        const char* label;
    };
    const Axis axes[3] = {{{1, 0, 0}, IM_COL32(235, 80, 80, 255), "X"},
                          {{0, 1, 0}, IM_COL32(110, 220, 100, 255), "Y"},
                          {{0, 0, 1}, IM_COL32(80, 150, 255, 255), "Z"}};
    dl->AddCircleFilled(axisCenter, axisRadius, IM_COL32(8, 12, 18, 120));
    for (const Axis& a : axes) {
        const vec3 v = vec3(view * vec4(a.dir, 0.0f)); // 只取旋转：世界轴在相机空间里的方向
        const ImVec2 tip(axisCenter.x + v.x * axisLen, axisCenter.y - v.y * axisLen);
        dl->AddLine(axisCenter, tip, a.color, 2.5f);
        dl->AddText(ImVec2(tip.x - 3, tip.y - 8), a.color, a.label);
    }

    // ---- 右下：操作提示 ----
    const char* hint = "右键拖动 + WASD/QE 飞行 · 滚轮调速 · 左键选择 · F 聚焦 · F1 游戏模式";
    const ImVec2 hs = ImGui::CalcTextSize(hint);
    dl->AddText(ImVec2(end.x - hs.x - 12, end.y - hs.y - 10), IM_COL32(190, 205, 215, 150), hint);
}

void Editor::drawGameModeOverlay() {
    // 游戏模式：只在角落显示帧率；进入后的前 4 秒显示如何返回
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    char fps[32];
    snprintf(fps, sizeof(fps), "%.0f FPS", m_stats.fps());
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    const ImVec2 fs = ImGui::CalcTextSize(fps);
    dl->AddText(ImVec2(ds.x - fs.x - 12, 10), IM_COL32(220, 235, 240, 180), fps);
    const double t = nowSeconds() - m_gameModeEnteredAt;
    if (t < 4.0) {
        const float alpha = float(std::min(1.0, (4.0 - t) / 1.0));
        const char* msg = "游戏模式 · 按 F1 或 Esc 返回编辑器";
        const ImVec2 ms = ImGui::CalcTextSize(msg);
        const ImVec2 p((ds.x - ms.x) * 0.5f, ds.y - ms.y - 40);
        dl->AddRectFilled(ImVec2(p.x - 14, p.y - 8), ImVec2(p.x + ms.x + 14, p.y + ms.y + 8), IM_COL32(8, 12, 18, int(190 * alpha)), 6.0f);
        dl->AddText(p, IM_COL32(220, 240, 245, int(255 * alpha)), msg);
    }
}

void Editor::drawHelpPopup() {
    if (m_showHelp) {
        ImGui::OpenPopup("快捷键与操作");
        m_showHelp = false;
    }
    ImGui::SetNextWindowSize(ImVec2(460, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("快捷键与操作", nullptr, ImGuiWindowFlags_NoResize)) {
        struct Row {
            const char* key;
            const char* what;
        };
        const Row rows[] = {
            {"右键拖动", "转动视角"}, {"W A S D / Q E", "前后左右 / 下降上升（需鼠标在视口内）"},
            {"Shift", "加速"}, {"滚轮", "调整移动速度"}, {"左键", "选择物体"}, {"F / 双击", "聚焦选中物体"},
            {"Esc", "取消选择 / 退出游戏模式"}, {"F1", "编辑器 ↔ 游戏模式"}, {"F5 / F6", "暂停继续 / 单步模拟"},
            {"F9", "截图到 captures/"}, {"F12", "RenderDoc 捕获（需加载）"}, {"Ctrl+S", "保存配置"},
            {"Ctrl+R", "重新编译全部 shader"},
        };
        if (ImGui::BeginTable("keys", 2, ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("键", ImGuiTableColumnFlags_WidthFixed, 140);
            for (const Row& r : rows) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextColored(theme::kAccentBright, "%s", r.key);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(r.what);
            }
            ImGui::EndTable();
        }
        ImGui::TextDisabled("面板可拖动标签重新停靠；「视图 → 重置布局」恢复默认。");
        if (ImGui::Button("关闭", ImVec2(-1, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// =====================================================================================
// 操作
// =====================================================================================

void Editor::pickAt(const ImVec2& uv) {
    const VkExtent2D se = m_ctx.renderer->sceneExtent();
    const float aspect = float(se.width) / float(std::max(se.height, 1u));
    const mat4 viewProj = m_ctx.camera->projection(aspect) * m_ctx.camera->view();
    const Ray ray = screenRay(glm::inverse(viewProj), m_ctx.camera->position(), vec2(uv.x, uv.y));

    // 射线与每个实体的世界包围盒求交，取最近的。
    // EXTEND: 包围盒点选在物体镂空处（拱门中间）会误选；需要像素级精确时改用 GPU ID 缓冲。
    std::optional<EntityId> best;
    float bestT = std::numeric_limits<float>::max();
    Scene& scene = *m_ctx.scene;
    for (EntityId id = 0; id < scene.entities().size(); ++id) {
        const Entity& e = scene.entities()[id];
        if (!e.visible) continue;
        vec3 mn, mx;
        if (!scene.worldBounds(id, mn, mx)) continue;
        // 地面这样的薄平面包围盒厚度为 0：稍微加厚，避免数值误差导致点不中
        mn -= vec3(1e-3f);
        mx += vec3(1e-3f);
        if (auto t = intersectRayAabb(ray, mn, mx); t && *t < bestT) {
            bestT = *t;
            best = id;
        }
    }
    m_selection = best;
    m_selectedMaterial.reset();
    m_scrollOutlinerToSelection = best.has_value();
}

void Editor::focusSelection() {
    if (!m_selection) return;
    vec3 mn, mx;
    if (!m_ctx.scene->worldBounds(*m_selection, mn, mx)) return;
    m_ctx.camera->focus((mn + mx) * 0.5f, glm::length(mx - mn) * 0.5f);
}

void Editor::takeScreenshot() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char name[64];
    std::strftime(name, sizeof(name), "screenshot_%Y%m%d_%H%M%S.png", &tm);
    m_ctx.renderer->requestCapture(fs::projectRoot() / "captures" / name);
}

void Editor::applyRendererSettings() {
    m_ctx.renderer->setSettings(*m_ctx.rendererSettings);
}

} // namespace eng
