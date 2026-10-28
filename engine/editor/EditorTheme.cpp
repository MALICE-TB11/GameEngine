#include "editor/EditorTheme.h"

namespace eng::theme {

void apply() {
    ImGuiStyle& s = ImGui::GetStyle();
    // ---- 形状：小圆角、紧凑间距（编辑器信息密度高） ----
    s.WindowRounding = 4.0f;
    s.ChildRounding = 4.0f;
    s.FrameRounding = 3.0f;
    s.PopupRounding = 4.0f;
    s.ScrollbarRounding = 6.0f;
    s.GrabRounding = 3.0f;
    s.TabRounding = 3.0f;
    s.WindowBorderSize = 1.0f;
    s.FrameBorderSize = 0.0f;
    s.PopupBorderSize = 1.0f;
    s.WindowPadding = ImVec2(8, 8);
    s.FramePadding = ImVec2(7, 4);
    s.ItemSpacing = ImVec2(8, 5);
    s.ItemInnerSpacing = ImVec2(6, 4);
    s.IndentSpacing = 14.0f;
    s.ScrollbarSize = 12.0f;
    s.GrabMinSize = 10.0f;
    s.WindowMenuButtonPosition = ImGuiDir_None; // 去掉标题栏左侧的折叠三角，标题更干净
    s.SeparatorTextBorderSize = 1.0f;
    s.DockingSeparatorSize = 3.0f;

    // ---- 颜色 ----
    auto c = [](float r, float g, float b, float a = 1.0f) { return ImVec4(r, g, b, a); };
    ImVec4* col = s.Colors;
    col[ImGuiCol_Text] = c(0.86f, 0.90f, 0.94f);
    col[ImGuiCol_TextDisabled] = kTextDim;
    col[ImGuiCol_WindowBg] = c(0.075f, 0.090f, 0.115f);
    col[ImGuiCol_ChildBg] = c(0, 0, 0, 0);
    col[ImGuiCol_PopupBg] = c(0.085f, 0.105f, 0.135f, 0.98f);
    col[ImGuiCol_Border] = c(0.17f, 0.21f, 0.27f, 0.70f);
    col[ImGuiCol_BorderShadow] = c(0, 0, 0, 0);
    col[ImGuiCol_FrameBg] = c(0.12f, 0.145f, 0.185f);
    col[ImGuiCol_FrameBgHovered] = c(0.15f, 0.20f, 0.25f);
    col[ImGuiCol_FrameBgActive] = c(0.17f, 0.25f, 0.31f);
    col[ImGuiCol_TitleBg] = c(0.055f, 0.068f, 0.088f);
    col[ImGuiCol_TitleBgActive] = c(0.065f, 0.090f, 0.120f);
    col[ImGuiCol_TitleBgCollapsed] = c(0.055f, 0.068f, 0.088f);
    col[ImGuiCol_MenuBarBg] = c(0.050f, 0.062f, 0.080f);
    col[ImGuiCol_ScrollbarBg] = c(0, 0, 0, 0.15f);
    col[ImGuiCol_ScrollbarGrab] = c(0.20f, 0.25f, 0.31f);
    col[ImGuiCol_ScrollbarGrabHovered] = c(0.25f, 0.32f, 0.40f);
    col[ImGuiCol_ScrollbarGrabActive] = kAccentDim;
    col[ImGuiCol_CheckMark] = kAccentBright;
    col[ImGuiCol_SliderGrab] = kAccent;
    col[ImGuiCol_SliderGrabActive] = kAccentBright;
    col[ImGuiCol_Button] = c(0.13f, 0.165f, 0.21f);
    col[ImGuiCol_ButtonHovered] = c(0.06f, 0.42f, 0.48f);
    col[ImGuiCol_ButtonActive] = kAccent;
    col[ImGuiCol_Header] = c(0.04f, 0.38f, 0.44f, 0.55f);
    col[ImGuiCol_HeaderHovered] = c(0.05f, 0.50f, 0.57f, 0.65f);
    col[ImGuiCol_HeaderActive] = c(0.05f, 0.60f, 0.68f, 0.80f);
    col[ImGuiCol_Separator] = c(0.17f, 0.21f, 0.27f);
    col[ImGuiCol_SeparatorHovered] = kAccentDim;
    col[ImGuiCol_SeparatorActive] = kAccent;
    col[ImGuiCol_ResizeGrip] = c(0.05f, 0.68f, 0.76f, 0.15f);
    col[ImGuiCol_ResizeGripHovered] = c(0.05f, 0.68f, 0.76f, 0.55f);
    col[ImGuiCol_ResizeGripActive] = kAccent;
    col[ImGuiCol_Tab] = c(0.065f, 0.080f, 0.105f);
    col[ImGuiCol_TabHovered] = c(0.06f, 0.42f, 0.48f);
    col[ImGuiCol_TabSelected] = c(0.10f, 0.13f, 0.17f);
    col[ImGuiCol_TabSelectedOverline] = kAccent; // 选中标签顶部的强调色细线
    col[ImGuiCol_TabDimmed] = c(0.060f, 0.072f, 0.095f);
    col[ImGuiCol_TabDimmedSelected] = c(0.085f, 0.105f, 0.135f);
    col[ImGuiCol_TabDimmedSelectedOverline] = kAccentDim;
    col[ImGuiCol_DockingPreview] = c(0.05f, 0.68f, 0.76f, 0.45f);
    col[ImGuiCol_DockingEmptyBg] = c(0.045f, 0.055f, 0.070f);
    col[ImGuiCol_PlotLines] = kAccentBright;
    col[ImGuiCol_PlotLinesHovered] = kSelection;
    col[ImGuiCol_PlotHistogram] = kAccent;
    col[ImGuiCol_PlotHistogramHovered] = kSelection;
    col[ImGuiCol_TableHeaderBg] = c(0.095f, 0.12f, 0.155f);
    col[ImGuiCol_TableBorderStrong] = c(0.17f, 0.21f, 0.27f);
    col[ImGuiCol_TableBorderLight] = c(0.13f, 0.16f, 0.20f);
    col[ImGuiCol_TableRowBgAlt] = c(1, 1, 1, 0.025f);
    col[ImGuiCol_TextSelectedBg] = c(0.05f, 0.68f, 0.76f, 0.35f);
    col[ImGuiCol_DragDropTarget] = kSelection;
    col[ImGuiCol_ModalWindowDimBg] = c(0, 0, 0, 0.55f);
}

bool toolButton(const char* label, bool active, const char* tooltip) {
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, kAccentDim);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccent);
    }
    const bool pressed = ImGui::Button(label);
    if (active) ImGui::PopStyleColor(2);
    if (tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", tooltip);
    return pressed;
}

void sectionHeader(const char* text) {
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, kAccentBright);
    ImGui::SeparatorText(text);
    ImGui::PopStyleColor();
}

void helpMarker(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

} // namespace eng::theme
