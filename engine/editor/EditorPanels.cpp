// 编辑器内容面板：场景大纲、资源、检视器、环境光照、渲染设置、项目设置。
#include "editor/Editor.h"

#include "editor/EditorInternal.h"
#include "editor/EditorTheme.h"
#include "render/Mesh.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace eng {

using namespace editor;

namespace {

// 线性颜色 → 近似 sRGB，用于在 UI 色块里正确显示材质颜色（ImGui 色块按 sRGB 值直接显示）
ImVec4 linearToDisplay(const vec4& c) {
    auto f = [](float x) { return std::pow(std::clamp(x, 0.0f, 1.0f), 1.0f / 2.2f); };
    return ImVec4(f(c.r), f(c.g), f(c.b), 1.0f);
}

// 「模型名/节点名」→ 分组名与显示名
void splitName(const std::string& full, std::string& group, std::string& leaf) {
    const auto slash = full.find('/');
    if (slash == std::string::npos) {
        group = "场景";
        leaf = full;
    } else {
        group = full.substr(0, slash);
        leaf = full.substr(slash + 1);
    }
}

// 材质参数编辑（实体覆盖和资产材质共用），返回是否有改动
bool drawMaterialParams(MaterialParams& p) {
    bool c = false;
    c |= ImGui::ColorEdit4("基础色", &p.baseColor.x, ImGuiColorEditFlags_Float);
    c |= ImGui::SliderFloat("金属度", &p.pbr.x, 0.0f, 1.0f);
    c |= ImGui::SliderFloat("粗糙度", &p.pbr.y, 0.0f, 1.0f);
    theme::helpMarker("感知粗糙度。BRDF 内部使用 α = 粗糙度²，让滑条在视觉上更线性。");
    c |= ImGui::SliderFloat("AO 强度", &p.pbr.z, 0.0f, 1.0f);
    c |= ImGui::SliderFloat("法线强度", &p.emissive.w, 0.0f, 3.0f);
    c |= ImGui::ColorEdit3("自发光", &p.emissive.x, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
    c |= ImGui::SliderFloat("自发光倍数", &p.flags.y, 0.0f, 20.0f);
    return c;
}

// 贴图槽位：有贴图 ✔，没有则显示用的是哪张默认贴图
void drawTextureSlots(const Material& m) {
    if (ImGui::BeginTable("##tex", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        auto row = [](const char* slot, bool has, const char* fallback) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", slot);
            ImGui::TableNextColumn();
            if (has)
                ImGui::TextColored(theme::kOk, "✔ 贴图");
            else
                ImGui::TextDisabled("— 默认（%s）", fallback);
        };
        row("基础色", m.baseColorTex, "白");
        row("法线", m.normalTex, "平坦");
        row("金属/粗糙", m.metalRoughTex, "白 = 用参数");
        row("AO", m.occlusionTex, "白 = 无遮蔽");
        row("自发光", m.emissiveTex, "白 × 参数");
        ImGui::EndTable();
    }
}

// 资产材质编辑器（资源浏览器里选中的材质）：修改的是内存中的资产，影响所有使用它的实体
void drawMaterialEditor(Material& m) {
    ImGui::PushID(&m);
    ImGui::TextColored(theme::kAccentBright, "%s", m.name.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled(m.shadingModel() == ShadingModel::Toon ? "· Toon" : "· PBR");
    drawMaterialParams(m.params);
    ImGui::Checkbox("双面", &m.doubleSided);
    theme::helpMarker("关闭背面剔除，背面法线翻转。用于树叶、布料、薄片。");
    drawTextureSlots(m);
    ImGui::TextColored(theme::kWarn, "运行时修改，影响所有使用该材质的实体，不写回资产文件");
    ImGui::PopID();
}

// 太阳方位盘：俯视的天空半球。圆心 = 天顶，圆周 = 地平线；点的角度 = 方位角，离圆心越远仰角越低。
// 直接拖动太阳点即可同时调整仰角和方位角 —— 比两个滑条更直观。
bool sunDial(LightingSettings& L, float radius) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float pad = 26.0f; // 留出 "-X" 等轴标签的位置
    const ImVec2 c(p0.x + radius + pad, p0.y + radius + pad);
    ImGui::InvisibleButton("##sundial", ImVec2((radius + pad) * 2, (radius + pad) * 2));
    bool changed = false;
    if (ImGui::IsItemActive()) {
        const ImVec2 m = ImGui::GetMousePos();
        const float dx = (m.x - c.x) / radius, dy = (m.y - c.y) / radius;
        const float r = std::min(std::sqrt(dx * dx + dy * dy), 1.11f); // 允许略低于地平线（-10°）
        L.sunElevationDeg = std::clamp(90.0f * (1.0f - r), -10.0f, 90.0f);
        float az = glm::degrees(std::atan2(dy, dx));
        if (az < 0) az += 360.0f;
        L.sunAzimuthDeg = az;
        changed = true;
    }
    // 背景：地平线、30°/60° 仰角圈、十字
    dl->AddCircleFilled(c, radius, IM_COL32(16, 40, 70, 255), 48);
    dl->AddCircle(c, radius, IM_COL32(80, 130, 170, 255), 48, 1.5f);
    dl->AddCircle(c, radius * (1.0f - 30.0f / 90.0f), IM_COL32(60, 100, 140, 140), 48);
    dl->AddCircle(c, radius * (1.0f - 60.0f / 90.0f), IM_COL32(60, 100, 140, 140), 48);
    dl->AddLine(ImVec2(c.x - radius, c.y), ImVec2(c.x + radius, c.y), IM_COL32(60, 100, 140, 120));
    dl->AddLine(ImVec2(c.x, c.y - radius), ImVec2(c.x, c.y + radius), IM_COL32(60, 100, 140, 120));
    // 世界轴标签：方位角 0° = +X，90° = +Z（与 LightingSettings::sunDirection 一致）
    const ImU32 lab = IM_COL32(150, 180, 200, 220);
    dl->AddText(ImVec2(c.x + radius + 3, c.y - 8), lab, "+X");
    dl->AddText(ImVec2(c.x - radius - 22, c.y - 8), lab, "-X");
    dl->AddText(ImVec2(c.x - 8, c.y + radius + 1), lab, "+Z");
    dl->AddText(ImVec2(c.x - 8, c.y - radius - 16), lab, "-Z");
    // 太阳
    const float r = radius * (1.0f - L.sunElevationDeg / 90.0f);
    const float az = glm::radians(L.sunAzimuthDeg);
    const ImVec2 s(c.x + std::cos(az) * r, c.y + std::sin(az) * r);
    const vec3 sc = glm::clamp(L.sunColor, vec3(0.0f), vec3(1.0f));
    const ImU32 sunCol = IM_COL32(int(255 * sc.r), int(255 * sc.g), int(255 * sc.b), 255);
    dl->AddLine(c, s, IM_COL32(255, 210, 120, 90), 1.0f);
    dl->AddCircleFilled(s, 9.0f, IM_COL32(255, 200, 80, 60));
    dl->AddCircleFilled(s, 6.0f, sunCol);
    if (ImGui::IsItemHovered() && !ImGui::IsItemActive()) ImGui::SetTooltip("拖动太阳：角度 = 方位角，到圆心距离 = 仰角");
    return changed;
}

struct LightingPreset {
    const char* name;
    float elevation, azimuth, intensity;
    vec3 sunColor, zenith, horizon, ground;
    float skyIntensity, exposure;
};
// 「光即叙事」：几个时间段的参考预设，一键切换后再微调。
// EXTEND: 预设也可以放进 config/ 里做成数据（例如 config/lighting_presets.json）。
const LightingPreset kPresets[] = {
    {"清晨", 8.0f, 120.0f, 2.5f, {1.0f, 0.62f, 0.38f}, {0.10f, 0.16f, 0.38f}, {0.85f, 0.55f, 0.42f}, {0.10f, 0.08f, 0.07f}, 0.9f, 1.4f},
    {"正午", 70.0f, 210.0f, 6.0f, {1.0f, 0.97f, 0.92f}, {0.05f, 0.20f, 0.65f}, {0.40f, 0.60f, 0.88f}, {0.13f, 0.11f, 0.09f}, 1.1f, 1.0f},
    {"黄昏", 4.0f, 300.0f, 2.0f, {1.0f, 0.45f, 0.20f}, {0.12f, 0.10f, 0.30f}, {0.95f, 0.45f, 0.25f}, {0.09f, 0.06f, 0.05f}, 0.8f, 1.6f},
    {"月夜", 35.0f, 60.0f, 0.25f, {0.55f, 0.65f, 1.0f}, {0.005f, 0.01f, 0.03f}, {0.02f, 0.035f, 0.07f}, {0.01f, 0.01f, 0.012f}, 1.0f, 5.0f},
};

} // namespace

// =====================================================================================
// 场景大纲
// =====================================================================================

void Editor::drawOutliner() {
    if (!ImGui::Begin(kOutlinerWindow)) {
        ImGui::End();
        return;
    }
    Scene& scene = *m_ctx.scene;
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##outliner_filter", "搜索实体…", m_outlinerFilter, sizeof(m_outlinerFilter));
    ImGui::TextDisabled("%zu 个实体 · 左键选择 · 双击聚焦", scene.entities().size());
    ImGui::Separator();

    // 按「模型名/」前缀分组，保持出现顺序
    std::vector<std::string> groupOrder;
    std::map<std::string, std::vector<EntityId>> groups;
    for (EntityId id = 0; id < scene.entities().size(); ++id) {
        std::string g, leaf;
        splitName(scene.entities()[id].name, g, leaf);
        if (m_outlinerFilter[0] && scene.entities()[id].name.find(m_outlinerFilter) == std::string::npos) continue;
        if (!groups.count(g)) groupOrder.push_back(g);
        groups[g].push_back(id);
    }

    ImGui::BeginChild("##outliner_list");
    for (const std::string& g : groupOrder) {
        ImGui::SetNextItemOpen(true, ImGuiCond_Once);
        // 选中的实体在这个分组里：确保分组展开，才能滚动到它
        const auto& ids = groups[g];
        if (m_scrollOutlinerToSelection && m_selection && std::find(ids.begin(), ids.end(), *m_selection) != ids.end())
            ImGui::SetNextItemOpen(true);
        const bool open = ImGui::TreeNodeEx(g.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding, "▤ %s  (%zu)",
                                            g.c_str(), groups[g].size());
        if (!open) continue;
        for (EntityId id : groups[g]) {
            Entity& e = scene.entity(id);
            std::string group, leaf;
            splitName(e.name, group, leaf);
            ImGui::PushID(int(id));
            const bool selected = m_selection && *m_selection == id;
            if (selected) ImGui::PushStyleColor(ImGuiCol_Text, theme::kSelection);
            else if (!e.visible) ImGui::PushStyleColor(ImGuiCol_Text, theme::kTextDim);
            // 图标表示资产状态：◆ 就绪  ◌ 加载中  ✖ 失败（显示兜底立方体）
            const asset::AssetStatus st = m_ctx.assets->status(e.model);
            const char* icon = st == asset::AssetStatus::Ready ? "◆ " : st == asset::AssetStatus::Loading ? "◌ " : "✖ ";
            const std::string label = icon + leaf;
            if (ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_AllowOverlap | ImGuiSelectableFlags_AllowDoubleClick)) {
                m_selection = id;
                m_selectedMaterial = nullptr;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) focusSelection();
            }
            if (selected || !e.visible) ImGui::PopStyleColor();
            bool deleteThis = false;
            if (ImGui::BeginPopupContextItem("##ctx")) {
                m_selection = id;
                if (ImGui::MenuItem("聚焦", "F")) focusSelection();
                if (ImGui::MenuItem(e.visible ? "隐藏" : "显示")) e.visible = !e.visible;
                if (ImGui::MenuItem("删除", "Delete")) deleteThis = true;
                ImGui::EndPopup();
            }
            if (selected && m_scrollOutlinerToSelection) {
                ImGui::SetScrollHereY(0.5f);
                m_scrollOutlinerToSelection = false;
            }
            // 右侧：可见性开关
            ImGui::SameLine(ImGui::GetContentRegionMax().x - 22.0f);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            if (ImGui::SmallButton(e.visible ? "◉" : "○")) e.visible = !e.visible;
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip(e.visible ? "隐藏" : "显示");
            ImGui::PopID();
            if (deleteThis) {
                // 删除会改变后面实体的下标：本帧剩下的列表已经失效，直接结束绘制
                deleteSelection();
                ImGui::TreePop();
                ImGui::EndChild();
                ImGui::End();
                return;
            }
        }
        ImGui::TreePop();
    }
    ImGui::EndChild();
    ImGui::End();
}

// =====================================================================================
// 资源浏览器
// =====================================================================================

void Editor::drawAssetBrowser() {
    if (!ImGui::Begin(kAssetsWindow)) {
        ImGui::End();
        return;
    }
    asset::AssetManager& assets = *m_ctx.assets;
    asset::AssetDatabase& db = assets.database();
    if (ImGui::BeginTabBar("##assets_tabs")) {
        if (ImGui::BeginTabItem("模型")) {
            ImGui::TextDisabled("把 .glb / .gltf / .png 拖进窗口即可导入");
            if (ImGui::BeginTable("##models", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("ops", ImGuiTableColumnFlags_WidthFixed, 64);
                auto row = [&](const std::string& label, const std::string& tooltip, std::optional<Guid> guid, ModelHandle loaded) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImVec4 col = theme::kTextDim;
                    const char* icon = "○ "; // 未加载
                    if (loaded.valid()) {
                        const auto st = assets.status(loaded);
                        icon = st == asset::AssetStatus::Ready ? "◆ " : st == asset::AssetStatus::Loading ? "◌ " : "✖ ";
                        col = st == asset::AssetStatus::Ready ? theme::kOk : st == asset::AssetStatus::Loading ? theme::kWarn : theme::kError;
                    }
                    ImGui::TextColored(col, "%s", icon);
                    ImGui::SameLine(0, 2);
                    ImGui::TextUnformatted(label.c_str());
                    if (ImGui::IsItemHovered() && !tooltip.empty()) ImGui::SetTooltip("%s", tooltip.c_str());
                    ImGui::TableNextColumn();
                    ImGui::PushID(label.c_str());
                    if (ImGui::SmallButton("放入")) {
                        const ModelHandle h = loaded.valid() ? loaded : assets.loadModel(*guid);
                        std::string stem = std::filesystem::path(label).stem().string();
                        placeModelInFront(h, stem);
                    }
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("放到相机前方 6 米处");
                    if (guid && loaded.valid() && !assets.loaded()[loaded.index].builtin) {
                        ImGui::SameLine(0, 2);
                        if (ImGui::SmallButton("↻")) {
                            // 强制重新烘焙：删掉缓存再加载（例如修改了烘焙器代码之后）
                            std::error_code ec;
                            std::filesystem::remove(db.cookedPath(*guid), ec);
                            assets.reload(loaded);
                        }
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("重新导入（删除缓存并重新烘焙）");
                    }
                    ImGui::PopID();
                };
                // 内置几何体
                for (const auto& info : assets.loaded())
                    if (info.builtin) row(info.name, "内置程序化模型", info.guid, info.handle);
                // 资产数据库里的模型
                for (const auto& rec : db.records()) {
                    if (rec.type != asset::AssetType::Model) continue;
                    ModelHandle loaded;
                    for (const auto& info : assets.loaded())
                        if (info.guid == rec.guid) loaded = info.handle;
                    std::string tip = "GUID " + rec.guid.toString();
                    if (loaded.valid() && !assets.error(loaded).empty()) tip += "\n错误: " + assets.error(loaded);
                    if (!loaded.valid()) tip += db.isStale(rec.guid) ? "\n未烘焙（第一次放入时在后台烘焙）" : "\n已烘焙";
                    row(rec.displayName, tip, rec.guid, loaded);
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("材质")) {
            // 色块网格：已加载模型的材质，点击后在检视器中编辑
            std::vector<Material*> materials;
            for (const auto& info : assets.loaded())
                if (Model* model = assets.mutableModel(info.handle); model && info.status == asset::AssetStatus::Ready)
                    for (Material& m : model->materials) materials.push_back(&m);
            const float cell = 64.0f;
            const int columns = std::max(1, int(ImGui::GetContentRegionAvail().x / (cell + 8.0f)));
            int i = 0;
            for (Material* m : materials) {
                ImGui::PushID(m);
                if (i % columns != 0) ImGui::SameLine();
                ImGui::BeginGroup();
                const bool selected = m_selectedMaterial == m;
                if (selected) {
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f);
                    ImGui::PushStyleColor(ImGuiCol_Border, theme::kSelection);
                }
                if (ImGui::ColorButton("##swatch", linearToDisplay(m->params.baseColor), ImGuiColorEditFlags_NoTooltip, ImVec2(cell, cell * 0.7f))) {
                    m_selectedMaterial = m;
                }
                if (selected) {
                    ImGui::PopStyleColor();
                    ImGui::PopStyleVar();
                }
                // 带贴图的材质在色块角上标一个小点
                if (m->baseColorTex || m->normalTex) {
                    const ImVec2 r = ImGui::GetItemRectMax();
                    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(r.x - 7, r.y - 7), 4.0f, IM_COL32(60, 220, 230, 255));
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n金属度 %.2f · 粗糙度 %.2f%s", m->name.c_str(), m->params.pbr.x,
                                                              m->params.pbr.y, m->baseColorTex ? "\n带贴图" : "");
                // 名字太长时保留结尾（区分 _r0/_r1 这类后缀的往往在末尾），开头用省略号
                std::string name = m->name;
                if (ImGui::CalcTextSize(name.c_str()).x > cell) {
                    while (name.size() > 1 && ImGui::CalcTextSize(("…" + name).c_str()).x > cell) name.erase(0, 1);
                    name = "…" + name;
                }
                ImGui::TextDisabled("%s", name.c_str());
                ImGui::EndGroup();
                ImGui::PopID();
                ++i;
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("网格")) {
            // 每个网格的 LOD 链：三角形数逐级减少
            if (ImGui::BeginTable("##meshes", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                for (const auto& info : assets.loaded()) {
                    if (info.status != asset::AssetStatus::Ready) continue;
                    for (const GpuMesh& mesh : assets.model(info.handle).meshes) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(mesh.name.c_str());
                        ImGui::TableNextColumn();
                        std::string lods;
                        for (const MeshLod& l : mesh.lods) lods += (lods.empty() ? "" : " → ") + std::to_string(l.indexCount / 3);
                        ImGui::TextDisabled("△ %s", lods.c_str());
                    }
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("贴图")) {
            ImGui::TextDisabled("独立贴图资产（地形、天气等将使用），导入时压缩为 BC7/BC5");
            for (const auto& rec : db.records()) {
                if (rec.type != asset::AssetType::Texture) continue;
                ImGui::BulletText("%s", rec.displayName.c_str());
                ImGui::SameLine();
                ImGui::TextDisabled("%s · %s", rec.settings.value("usage", std::string("color")).c_str(),
                                    db.isStale(rec.guid) ? "未烘焙" : "已烘焙");
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

// =====================================================================================
// 检视器
// =====================================================================================

void Editor::drawInspector() {
    if (!ImGui::Begin(kInspectorWindow)) {
        ImGui::End();
        return;
    }
    if (m_selectedMaterial) {
        theme::sectionHeader("材质资源");
        drawMaterialEditor(*m_selectedMaterial);
        if (m_selection && ImGui::Button("← 返回选中实体")) m_selectedMaterial = nullptr;
    } else if (m_selection && *m_selection < m_ctx.scene->entities().size()) {
        drawEntityInspector(m_ctx.scene->entity(*m_selection), *m_selection);
    } else {
        // 空状态：告诉用户怎么开始，而不是一片空白
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const char* l1 = "未选中任何物体";
        const char* l2 = "在视口中左键点击，或在「场景大纲」中选择";
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + avail.y * 0.35f);
        ImGui::SetCursorPosX((avail.x - ImGui::CalcTextSize(l1).x) * 0.5f);
        ImGui::TextDisabled("%s", l1);
        ImGui::SetCursorPosX(std::max(0.0f, (avail.x - ImGui::CalcTextSize(l2).x) * 0.5f));
        ImGui::TextDisabled("%s", l2);
    }
    ImGui::End();
}

void Editor::drawEntityInspector(Entity& e, EntityId id) {
    std::string group, leaf;
    splitName(e.name, group, leaf);
    ImGui::TextColored(theme::kSelection, "◆ %s", leaf.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%s · #%u", group.c_str(), id);
    ImGui::Checkbox("可见", &e.visible);
    ImGui::SameLine();
    if (theme::toolButton("◎ 聚焦", false, "把相机移到物体前 (F)")) focusSelection();

    theme::sectionHeader("变换");
    Transform t = e.current;
    bool changed = false;
    changed |= ImGui::DragFloat3("位置", &t.position.x, 0.05f, 0.0f, 0.0f, "%.2f");
    // 欧拉角（度）只用于编辑；内部仍存四元数，避免万向锁
    vec3 euler = glm::degrees(glm::eulerAngles(t.rotation));
    if (ImGui::DragFloat3("旋转", &euler.x, 0.5f, 0.0f, 0.0f, "%.1f°")) {
        t.rotation = glm::quat(glm::radians(euler));
        changed = true;
    }
    changed |= ImGui::DragFloat3("缩放", &t.scale.x, 0.01f, 0.001f, 1000.0f, "%.3f");
    if (ImGui::Button("重置变换")) {
        t = Transform{};
        changed = true;
    }
    theme::helpMarker("编辑的是实体的「当前」状态；固定步长模拟（例如旋转立方体）会继续在此基础上更新。\n"
                      "glTF 子节点的位置 = 实体变换 × 节点在模型内的偏移，所以这里显示的是模型根的变换。");
    vec3 mn, mx;
    if (m_ctx.scene->worldBounds(id, mn, mx)) {
        const vec3 center = (mn + mx) * 0.5f;
        ImGui::TextDisabled("世界中心 (%.2f, %.2f, %.2f)", center.x, center.y, center.z);
    }
    if (changed) {
        // 同时改上一步与当前步：否则渲染插值会在新旧位置之间闪一下
        e.current = t;
        e.previous = t;
    }

    theme::sectionHeader("模型");
    const asset::AssetStatus st = m_ctx.assets->status(e.model);
    ImGui::Text("%s", m_ctx.assets->name(e.model).c_str());
    ImGui::SameLine();
    ImGui::TextColored(st == asset::AssetStatus::Ready ? theme::kOk : st == asset::AssetStatus::Loading ? theme::kWarn : theme::kError,
                       st == asset::AssetStatus::Ready ? "就绪" : st == asset::AssetStatus::Loading ? "加载中" : "失败");
    if (!m_ctx.assets->error(e.model).empty()) ImGui::TextWrapped("%s", m_ctx.assets->error(e.model).c_str());
    ImGui::TextDisabled("实例 #%u · GUID %s", e.instance, m_ctx.assets->guid(e.model).toString().substr(0, 8).c_str());
    if (const GpuMesh* mesh = m_ctx.scene->mesh(id)) {
        const vec3 size = mesh->bounds.max - mesh->bounds.min;
        ImGui::TextDisabled("包围盒 %.2f × %.2f × %.2f 米", size.x, size.y, size.z);
        std::string lods;
        for (const MeshLod& l : mesh->lods) lods += (lods.empty() ? "" : " → ") + std::to_string(l.indexCount / 3);
        ImGui::TextDisabled("LOD 三角形 %s", lods.c_str());
        theme::helpMarker("渲染时把每级 LOD 的几何误差投影到屏幕上，选误差小于 1 像素的最粗一级。\n远离物体时三角形数会自动减少。");
    }

    theme::sectionHeader("材质");
    const Material* mat = m_ctx.scene->material(id);
    if (mat) {
        ImGui::TextColored(theme::kAccentBright, "%s", mat->name.c_str());
        drawTextureSlots(*mat);
    }
    // 参数编辑写入「实体覆盖」：只改这一个实体，资产本身和其他实体不受影响；保存场景时一起保存
    MaterialParams params = e.materialOverride ? *e.materialOverride : (mat ? mat->params : MaterialParams{});
    if (drawMaterialParams(params)) e.materialOverride = params;
    if (e.materialOverride) {
        ImGui::TextColored(theme::kWarn, "● 已覆盖（只影响这个实体）");
        ImGui::SameLine();
        if (ImGui::SmallButton("恢复资产材质")) e.materialOverride.reset();
    } else {
        ImGui::TextDisabled("使用资产材质；修改参数会为这个实体创建覆盖");
    }
}

// =====================================================================================
// 环境光照
// =====================================================================================

void Editor::drawEnvironmentPanel() {
    if (!ImGui::Begin(kEnvironmentWindow)) {
        ImGui::End();
        return;
    }
    LightingSettings& L = *m_ctx.lighting;
    bool c = false;

    theme::sectionHeader("时间预设");
    for (const LightingPreset& p : kPresets) {
        if (ImGui::Button(p.name)) {
            L.sunElevationDeg = p.elevation;
            L.sunAzimuthDeg = p.azimuth;
            L.sunIntensity = p.intensity;
            L.sunColor = p.sunColor;
            L.skyZenith = p.zenith;
            L.skyHorizon = p.horizon;
            L.groundColor = p.ground;
            L.skyIntensity = p.skyIntensity;
            L.exposure = p.exposure;
            c = true;
        }
        ImGui::SameLine();
    }
    ImGui::NewLine();

    theme::sectionHeader("太阳");
    const float dialRadius = 58.0f;
    if (ImGui::BeginTable("##sun", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("dial", ImGuiTableColumnFlags_WidthFixed, (dialRadius + 26.0f) * 2.0f);
        ImGui::TableSetupColumn("sliders", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextColumn();
        c |= sunDial(L, dialRadius);
        ImGui::TableNextColumn();
        ImGui::PushItemWidth(-1);
        ImGui::TextDisabled("仰角");
        c |= ImGui::SliderFloat("##el", &L.sunElevationDeg, -10.0f, 90.0f, "%.1f°");
        ImGui::TextDisabled("方位角");
        c |= ImGui::SliderFloat("##az", &L.sunAzimuthDeg, 0.0f, 360.0f, "%.1f°");
        ImGui::TextDisabled("照度");
        c |= ImGui::SliderFloat("##int", &L.sunIntensity, 0.0f, 30.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
        ImGui::PopItemWidth();
        ImGui::EndTable();
    }
    c |= ImGui::ColorEdit3("太阳颜色", &L.sunColor.x, ImGuiColorEditFlags_Float);
    c |= ImGui::SliderFloat("角半径", &L.sunAngularRadiusDeg, 0.05f, 3.0f, "%.2f°");
    c |= ImGui::SliderFloat("光晕", &L.sunHalo, 0.0f, 5.0f);

    theme::sectionHeader("天空与环境光");
    c |= ImGui::ColorEdit3("天顶", &L.skyZenith.x, ImGuiColorEditFlags_Float);
    c |= ImGui::ColorEdit3("地平线", &L.skyHorizon.x, ImGuiColorEditFlags_Float);
    c |= ImGui::ColorEdit3("地面反弹", &L.groundColor.x, ImGuiColorEditFlags_Float);
    c |= ImGui::SliderFloat("天空亮度", &L.skyIntensity, 0.0f, 5.0f);
    c |= ImGui::SliderFloat("环境光强度", &L.ambientIntensity, 0.0f, 3.0f);

    theme::sectionHeader("相机后处理");
    c |= ImGui::SliderFloat("曝光", &L.exposure, 0.05f, 16.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
    ImGui::SameLine();
    ImGui::TextDisabled("%+.1f EV", std::log2(std::max(L.exposure, 1e-4f)));
    c |= ImGui::Combo("色调映射", &L.tonemapper, kTonemapperNames, IM_ARRAYSIZE(kTonemapperNames));
    theme::helpMarker("ACES：对比度高，高亮饱和色会偏色（蓝天变青）。\nAgX：在对数域压缩，高亮自然去饱和，色相稳定。\n把照度调到 20 对比两者。");

    if (c) m_ctx.renderer->setLighting(L);
    ImGui::Spacing();
    if (ImGui::Button("✔ 保存到配置 (Ctrl+S)", ImVec2(-1, 0)) && m_ctx.saveConfig) m_ctx.saveConfig();
    ImGui::End();
}

// =====================================================================================
// 渲染设置
// =====================================================================================

void Editor::drawRenderSettingsPanel() {
    if (!ImGui::Begin(kRenderSettingsWindow)) {
        ImGui::End();
        return;
    }
    RendererSettings& rs = *m_ctx.rendererSettings;
    bool c = false;

    theme::sectionHeader("输出");
    c |= ImGui::Checkbox("垂直同步", &rs.vsync);
    const VkExtent2D se = m_ctx.renderer->sceneExtent();
    ImGui::TextDisabled("场景分辨率 %u × %u（跟随视口面板大小）", se.width, se.height);
    c |= ImGui::SliderFloat("LOD 误差阈值", &rs.lodThresholdPx, 0.25f, 16.0f, "%.2f 像素", ImGuiSliderFlags_Logarithmic);
    theme::helpMarker("简化误差投影到屏幕上小于这个像素数时，就使用更粗的 LOD。\n调大可以直观看到远处物体切换 LOD（配合线框模式）。");
    ImGui::TextDisabled("本帧绘制 %.1fk 个三角形", double(m_ctx.scene->lastTriangleCount()) / 1000.0);

    theme::sectionHeader("视图模式");
    for (int i = 0; i < IM_ARRAYSIZE(kDebugViewNames); ++i) {
        if (i % 3 != 0) ImGui::SameLine();
        c |= ImGui::RadioButton(kDebugViewNames[i], &rs.debugView, i);
    }
    ImGui::Checkbox("线框", &m_ctx.renderer->forwardPass().wireframe);
    c |= ImGui::ColorEdit3("无天空时的背景", &rs.clearColor.x, ImGuiColorEditFlags_Float);

    theme::sectionHeader("Pass 开关");
    theme::helpMarker("关闭的 pass 不执行；读取它输出的下游 pass 自动改读默认贴图，不会崩溃。\n详细的 barrier、资源见下方「Render Graph」面板。");
    rg::RenderGraph& graph = m_ctx.renderer->graph();
    for (const rg::PassReport& p : graph.lastReport()) {
        ImGui::PushID(p.name.c_str());
        ImGui::Checkbox("##on", &graph.passEnabled(p.name));
        ImGui::SameLine();
        const ImVec4 col = p.status == rg::PassReport::Status::Ran        ? theme::kOk
                         : p.status == rg::PassReport::Status::NotReady ? theme::kWarn
                                                                         : theme::kTextDim;
        ImGui::TextColored(col, "%s", p.name.c_str());
        if (p.status == rg::PassReport::Status::Ran) {
            ImGui::SameLine(ImGui::GetContentRegionMax().x - 70.0f);
            ImGui::TextDisabled("%.3f ms", p.gpuMs);
        }
        ImGui::PopID();
    }

    if (c) applyRendererSettings();
    ImGui::End();
}

// =====================================================================================
// 项目设置
// =====================================================================================

void Editor::drawProjectSettingsPanel() {
    if (!ImGui::Begin(kProjectWindow)) {
        ImGui::End();
        return;
    }
    FixedStepSettings& ls = *m_ctx.loopSettings;
    CameraSettings& cs = *m_ctx.cameraSettings;

    theme::sectionHeader("主循环");
    bool loopChanged = false;
    float hz = float(ls.fixedHz);
    if (ImGui::SliderFloat("固定步长", &hz, 5.0f, 240.0f, "%.0f Hz")) {
        ls.fixedHz = hz;
        loopChanged = true;
    }
    theme::helpMarker("物理/模拟的更新频率。调到 5Hz 观察：渲染插值让旋转依旧平滑。");
    loopChanged |= ImGui::SliderInt("单帧最多步数", &ls.maxStepsPerFrame, 1, 20);
    float maxFrame = float(ls.maxFrameTime);
    if (ImGui::SliderFloat("单帧最长计入", &maxFrame, 0.05f, 1.0f, "%.2f 秒")) {
        ls.maxFrameTime = maxFrame;
        loopChanged = true;
    }
    if (loopChanged) m_ctx.loop->setSettings(ls);

    theme::sectionHeader("相机");
    bool camChanged = false;
    camChanged |= ImGui::SliderFloat("视野", &cs.fovDeg, 20.0f, 120.0f, "%.0f°");
    camChanged |= ImGui::SliderFloat("近平面", &cs.nearPlane, 0.01f, 1.0f, "%.3f 米", ImGuiSliderFlags_Logarithmic);
    camChanged |= ImGui::SliderFloat("灵敏度", &cs.lookSensitivity, 0.02f, 0.5f, "%.2f°/像素");
    camChanged |= ImGui::SliderFloat("加速倍数", &cs.sprintMultiplier, 1.0f, 10.0f);
    ImGui::SliderFloat("移动速度", &m_ctx.camera->speed(), 0.1f, 200.0f, "%.1f 米/秒", ImGuiSliderFlags_Logarithmic);
    if (camChanged) {
        cs.moveSpeed = m_ctx.camera->speed();
        m_ctx.camera->setSettings(cs);
    }

    theme::sectionHeader("调试");
    if (m_ctx.renderdocAutoLoad) {
        ImGui::Checkbox("启动时加载 RenderDoc", m_ctx.renderdocAutoLoad);
        theme::helpMarker("需要重启生效：RenderDoc 必须在创建 Vulkan 实例之前加载。");
    }

    if (m_ctx.projectSettingsExtra) {
        theme::sectionHeader("应用");
        m_ctx.projectSettingsExtra();
    }

    ImGui::Spacing();
    if (ImGui::Button("✔ 保存到配置 (Ctrl+S)", ImVec2(-1, 0)) && m_ctx.saveConfig) m_ctx.saveConfig();
    ImGui::End();
}

} // namespace eng
