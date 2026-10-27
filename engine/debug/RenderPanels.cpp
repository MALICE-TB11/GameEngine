#include "debug/RenderPanels.h"

#include "debug/RenderDocCapture.h"
#include "render/PipelineCache.h"
#include "render/Renderer.h"

#include <imgui.h>

namespace eng {

void drawRenderGraphPanel(Renderer& renderer, RenderDocCapture* renderdoc) {
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - 470, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(460, 360), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Render Graph")) {
        ImGui::End();
        return;
    }
    rg::RenderGraph& graph = renderer.graph();
    ImGui::Text("GPU 总耗时 %.2f ms（两帧前的测量值）", renderer.profiler().totalMs());
    ImGui::Checkbox("线框模式", &renderer.forwardPass().wireframe);

    if (ImGui::BeginTable("passes", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("开", ImGuiTableColumnFlags_WidthFixed, 24);
        ImGui::TableSetupColumn("Pass");
        ImGui::TableSetupColumn("GPU ms", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("状态");
        ImGui::TableHeadersRow();
        for (const rg::PassReport& p : graph.lastReport()) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(p.name.c_str());
            ImGui::Checkbox("##on", &graph.passEnabled(p.name));
            ImGui::PopID();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(p.name.c_str());
            ImGui::TableNextColumn();
            if (p.status == rg::PassReport::Status::Ran) ImGui::Text("%.3f", p.gpuMs);
            ImGui::TableNextColumn();
            switch (p.status) {
            case rg::PassReport::Status::Ran: ImGui::TextColored(ImVec4(0.5f, 1, 0.5f, 1), "运行"); break;
            case rg::PassReport::Status::Disabled: ImGui::TextDisabled("已关闭"); break;
            case rg::PassReport::Status::NotReady: ImGui::TextColored(ImVec4(1, 0.6f, 0.2f, 1), "未就绪（管线错误）"); break;
            }
            for (const std::string& s : p.substituted) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1, 0.85f, 0.3f, 1), "[%s→默认]", s.c_str());
            }
        }
        ImGui::EndTable();
    }

    if (ImGui::TreeNode("资源")) {
        for (const auto& r : graph.resourceInfos())
            ImGui::BulletText("%s  %ux%u  format=%d%s", r.name.c_str(), r.extent.width, r.extent.height, int(r.format),
                              r.imported ? "  (外部)" : "");
        ImGui::TreePop();
    }

    ImGui::SeparatorText("RenderDoc");
    if (renderdoc && renderdoc->available()) {
        if (ImGui::Button("捕获下一帧 (F12)")) renderdoc->triggerCapture();
        ImGui::SameLine();
        if (ImGui::Button("打开 RenderDoc")) renderdoc->launchReplayUI();
        ImGui::Text("已捕获 %u 帧", renderdoc->numCaptures());
        if (renderdoc->numCaptures() > 0) ImGui::TextWrapped("%s", renderdoc->lastCapturePath().c_str());
    } else {
        ImGui::TextDisabled("未加载（config/engine.json: debug.renderdoc = true 后重启）");
    }
    ImGui::End();
}

void drawShaderPanel(PipelineCache& pipelines) {
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - 470, 380), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(460, 240), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Shader")) {
        ImGui::End();
        return;
    }
    ImGui::Checkbox("自动热重载", &pipelines.autoReload);
    ImGui::SameLine();
    if (ImGui::Button("全部重新编译")) pipelines.reloadAll();
    ImGui::SameLine();
    ImGui::Text("已热重载 %u 次", pipelines.reloadCount());
    ImGui::Separator();
    for (const auto& s : pipelines.status()) {
        const ImVec4 col = !s.error.empty() ? ImVec4(1, 0.4f, 0.4f, 1) : s.ready ? ImVec4(0.6f, 1, 0.6f, 1) : ImVec4(1, 0.6f, 0.2f, 1);
        ImGui::TextColored(col, "%s", s.name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", s.shader.c_str());
        if (!s.error.empty()) {
            ImGui::Indent();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.55f, 0.55f, 1));
            ImGui::TextWrapped("%s", s.error.c_str());
            ImGui::PopStyleColor();
            ImGui::TextDisabled(s.ready ? "仍在使用上一次编译成功的版本" : "没有可用版本：使用错误材质 / 跳过 pass");
            ImGui::Unindent();
        }
    }
    ImGui::End();
}

void drawShaderErrorBanner(const PipelineCache& pipelines) {
    const uint32_t n = pipelines.errorCount();
    if (n == 0) return;
    char text[128];
    snprintf(text, sizeof(text), "Shader 编译错误 %u 个 —— 详见「Shader」面板", n);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 size = ImGui::CalcTextSize(text);
    const ImVec2 pos((ImGui::GetIO().DisplaySize.x - size.x) * 0.5f, 8.0f);
    dl->AddRectFilled(ImVec2(pos.x - 10, pos.y - 4), ImVec2(pos.x + size.x + 10, pos.y + size.y + 4), IM_COL32(150, 20, 20, 230), 4.0f);
    dl->AddText(pos, IM_COL32(255, 255, 255, 255), text);
}

} // namespace eng
