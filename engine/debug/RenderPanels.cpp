#include "debug/RenderPanels.h"

#include "debug/RenderDocCapture.h"
#include "render/PipelineCache.h"
#include "render/Renderer.h"

#include <imgui.h>

#include <cstdio>

namespace eng {

void drawRenderGraphPanel(Renderer& renderer, RenderDocCapture* renderdoc, bool* open) {
    if (!ImGui::Begin(kRenderGraphWindow, open)) {
        ImGui::End();
        return;
    }
    rg::RenderGraph& graph = renderer.graph();
    ImGui::Text("GPU 总耗时 %.3f ms", renderer.profiler().totalMs());
    ImGui::SameLine();
    ImGui::TextDisabled("（两帧前的测量值；关掉 pass 后下游自动读默认贴图）");

    if (ImGui::BeginTable("passes", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("开", ImGuiTableColumnFlags_WidthFixed, 26);
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 20);
        ImGui::TableSetupColumn("Pass", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("GPU ms", ImGuiTableColumnFlags_WidthFixed, 64);
        ImGui::TableSetupColumn("状态", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableHeadersRow();
        int index = 0;
        for (const rg::PassReport& p : graph.lastReport()) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(p.name.c_str());
            ImGui::Checkbox("##on", &graph.passEnabled(p.name));
            ImGui::PopID();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%d", index++);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(p.name.c_str());
            ImGui::TableNextColumn();
            if (p.status == rg::PassReport::Status::Ran) ImGui::Text("%.3f", p.gpuMs);
            ImGui::TableNextColumn();
            switch (p.status) {
            case rg::PassReport::Status::Ran: ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.6f, 1), "● 运行"); break;
            case rg::PassReport::Status::Disabled: ImGui::TextDisabled("○ 已关闭"); break;
            case rg::PassReport::Status::NotReady: ImGui::TextColored(ImVec4(1, 0.6f, 0.2f, 1), "⚠ 未就绪（管线错误）"); break;
            }
            for (const std::string& s : p.substituted) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1, 0.85f, 0.3f, 1), "[%s→默认]", s.c_str());
            }
        }
        ImGui::EndTable();
    }

    if (ImGui::CollapsingHeader("资源")) {
        if (ImGui::BeginTable("res", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            for (const auto& r : graph.resourceInfos()) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(r.name.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%u × %u", r.extent.width, r.extent.height);
                ImGui::TableNextColumn();
                ImGui::TextDisabled("format %d%s", int(r.format), r.imported ? " · 外部" : "");
            }
            ImGui::EndTable();
        }
    }

    if (ImGui::CollapsingHeader("RenderDoc", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (renderdoc && renderdoc->available()) {
            if (ImGui::Button("● 捕获下一帧 (F12)")) renderdoc->triggerCapture();
            ImGui::SameLine();
            if (ImGui::Button("打开 RenderDoc")) renderdoc->launchReplayUI();
            ImGui::SameLine();
            ImGui::Text("已捕获 %u 帧", renderdoc->numCaptures());
            if (renderdoc->numCaptures() > 0) ImGui::TextWrapped("%s", renderdoc->lastCapturePath().c_str());
        } else {
            ImGui::TextDisabled("未加载：在「项目设置」中开启「启动时加载 RenderDoc」后重启");
        }
    }
    ImGui::End();
}

void drawShaderPanel(PipelineCache& pipelines, bool* open) {
    if (!ImGui::Begin(kShaderWindow, open)) {
        ImGui::End();
        return;
    }
    ImGui::Checkbox("自动热重载", &pipelines.autoReload);
    ImGui::SameLine();
    if (ImGui::Button("↻ 全部重新编译 (Ctrl+R)")) pipelines.reloadAll();
    ImGui::SameLine();
    ImGui::TextDisabled("已热重载 %u 次 · 保存 .hlsl 即生效", pipelines.reloadCount());
    ImGui::Separator();
    for (const auto& s : pipelines.status()) {
        const bool err = !s.error.empty();
        const ImVec4 col = err ? ImVec4(1, 0.45f, 0.45f, 1) : s.ready ? ImVec4(0.45f, 0.9f, 0.6f, 1) : ImVec4(1, 0.6f, 0.2f, 1);
        ImGui::TextColored(col, err ? "✖" : "✔");
        ImGui::SameLine();
        ImGui::TextUnformatted(s.name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s.hlsl", s.shader.c_str());
        if (err) {
            ImGui::Indent();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.6f, 0.6f, 1));
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
    snprintf(text, sizeof(text), "⚠ Shader 编译错误 %u 个 —— 详见「Shader」面板", n);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 size = ImGui::CalcTextSize(text);
    const ImVec2 pos((ImGui::GetIO().DisplaySize.x - size.x) * 0.5f, 8.0f);
    dl->AddRectFilled(ImVec2(pos.x - 10, pos.y - 4), ImVec2(pos.x + size.x + 10, pos.y + size.y + 4), IM_COL32(150, 20, 20, 230), 4.0f);
    dl->AddText(pos, IM_COL32(255, 255, 255, 255), text);
}

} // namespace eng
