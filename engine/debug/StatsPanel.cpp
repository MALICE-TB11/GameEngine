#include "debug/StatsPanel.h"

#include "core/Log.h"
#include "render/Renderer.h"
#include "rhi/Device.h"

#include <imgui.h>

#include <algorithm>
#include <string>

namespace eng {

namespace {
// 项目约束：显存预算 2GB（见 CLAUDE.md）
constexpr float kVramBudgetMB = 2048.0f;
} // namespace

void FrameStats::addFrame(float dtSeconds) {
    const float ms = dtSeconds * 1000.0f;
    m_frameMs[m_head] = ms;
    m_head = (m_head + 1) % kHistory;
    // 指数平滑：直接显示每帧数字会跳得看不清
    m_smoothedMs += (ms - m_smoothedMs) * 0.05f;
}

float FrameStats::maxMs() const {
    return *std::max_element(m_frameMs.begin(), m_frameMs.end());
}

void queryVramMB(const rhi::Device& device, float& usedMB, float& budgetMB) {
    usedMB = budgetMB = 0.0f;
    if (!device.memoryBudgetSupported()) return;
    uint64_t usage = 0, budget = 0;
    device.queryMemory(usage, budget);
    usedMB = float(usage) / (1024.0f * 1024.0f);
    budgetMB = float(budget) / (1024.0f * 1024.0f);
}

void drawProfilerPanel(const FrameStats& stats, const rhi::Device& device, const Renderer& renderer, const LoopStats& loop,
                       bool* open) {
    if (!ImGui::Begin(kProfilerWindow, open)) {
        ImGui::End();
        return;
    }
    const float avail = ImGui::GetContentRegionAvail().x;

    // ---- 左列：帧时间 ----
    if (ImGui::BeginTable("##prof", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("帧", ImGuiTableColumnFlags_WidthStretch, 0.5f);
        ImGui::TableSetupColumn("GPU", ImGuiTableColumnFlags_WidthStretch, 0.5f);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("CPU 帧时间 %.2f ms  (%.0f FPS)", stats.smoothedMs(), stats.fps());
        const float maxMs = std::max(33.3f, stats.maxMs());
        // 60 FPS 预算线（16.7ms）：曲线超过它说明掉帧
        const ImVec2 plotPos = ImGui::GetCursorScreenPos();
        const ImVec2 plotSize(ImGui::GetContentRegionAvail().x, 90.0f);
        ImGui::PlotLines("##frametime", stats.history(), FrameStats::kHistory, stats.head(), nullptr, 0.0f, maxMs, plotSize);
        const float budgetY = plotPos.y + plotSize.y * (1.0f - 16.7f / maxMs);
        ImGui::GetWindowDrawList()->AddLine(ImVec2(plotPos.x, budgetY), ImVec2(plotPos.x + plotSize.x, budgetY),
                                            IM_COL32(255, 170, 60, 160), 1.0f);
        ImGui::TextDisabled("橙线 = 16.7ms（60 FPS 预算）");
        ImGui::Text("固定步长 %.0f Hz · 本帧 %d 步 · alpha %.2f%s", loop.fixedHz, loop.stepsThisFrame, loop.alpha,
                    loop.paused ? " · 已暂停" : "");
        if (loop.timeScale != 1.0f) ImGui::Text("时间缩放 ×%.2f", loop.timeScale);

        float used = 0, budget = 0;
        queryVramMB(device, used, budget);
        if (budget > 0.0f) {
            char overlay[64];
            snprintf(overlay, sizeof(overlay), "显存 %.0f / %.0f MB", used, kVramBudgetMB);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, used > kVramBudgetMB ? ImVec4(0.9f, 0.3f, 0.3f, 1) : ImVec4(0.1f, 0.7f, 0.75f, 1));
            ImGui::ProgressBar(std::min(1.0f, used / kVramBudgetMB), ImVec2(-1, 0), overlay);
            ImGui::PopStyleColor();
        }

        // ---- 右列：每个 pass 的 GPU 耗时（条形图） ----
        ImGui::TableNextColumn();
        const auto& results = renderer.profiler().results();
        ImGui::Text("GPU 总耗时 %.3f ms", renderer.profiler().totalMs());
        float maxPass = 0.001f;
        for (const auto& r : results) maxPass = std::max(maxPass, r.ms);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float barWidth = ImGui::GetContentRegionAvail().x;
        for (const auto& r : results) {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float h = ImGui::GetTextLineHeight() + 4.0f;
            const float w = barWidth * (r.ms / maxPass);
            dl->AddRectFilled(p, ImVec2(p.x + barWidth, p.y + h), IM_COL32(255, 255, 255, 12), 2.0f);
            dl->AddRectFilled(p, ImVec2(p.x + std::max(w, 2.0f), p.y + h), IM_COL32(20, 160, 175, 200), 2.0f);
            char text[96];
            snprintf(text, sizeof(text), "%s  %.3f ms", r.name.c_str(), r.ms);
            dl->AddText(ImVec2(p.x + 6, p.y + 2), IM_COL32(230, 240, 245, 255), text);
            ImGui::Dummy(ImVec2(barWidth, h + 2.0f));
        }
        if (results.empty()) ImGui::TextDisabled("（等待 GPU 计时结果…）");
        ImGui::EndTable();
    }
    (void)avail;
    ImGui::End();
}

void drawConsolePanel(bool* open) {
    if (!ImGui::Begin(kConsoleWindow, open)) {
        ImGui::End();
        return;
    }
    static bool showInfo = true, showWarn = true, showError = true, autoScroll = true;
    static ImGuiTextFilter filter;

    const auto entries = log::recent();
    int nInfo = 0, nWarn = 0, nError = 0;
    for (const auto& e : entries) {
        if (e.level >= spdlog::level::err) ++nError;
        else if (e.level == spdlog::level::warn) ++nWarn;
        else ++nInfo;
    }

    // ---- 工具条 ----
    if (ImGui::Button("清空")) log::clearRecent();
    ImGui::SameLine();
    if (ImGui::Button("复制")) {
        std::string all;
        for (const auto& e : entries) all += e.text + "\n";
        ImGui::SetClipboardText(all.c_str());
    }
    ImGui::SameLine();
    ImGui::Checkbox("自动滚动", &autoScroll);
    ImGui::SameLine();
    // 级别开关：按钮上直接显示数量，红/黄色一眼看出问题
    auto levelToggle = [](const char* label, int count, bool& on, ImVec4 color) {
        char buf[48];
        snprintf(buf, sizeof(buf), "%s %d", label, count);
        ImGui::PushStyleColor(ImGuiCol_Button, on ? ImVec4(color.x * 0.45f, color.y * 0.45f, color.z * 0.45f, 1) : ImVec4(0.12f, 0.14f, 0.17f, 1));
        ImGui::PushStyleColor(ImGuiCol_Text, on ? color : ImVec4(0.5f, 0.55f, 0.6f, 1));
        if (ImGui::Button(buf)) on = !on;
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
    };
    levelToggle("信息", nInfo, showInfo, ImVec4(0.75f, 0.82f, 0.9f, 1));
    levelToggle("警告", nWarn, showWarn, ImVec4(1.0f, 0.8f, 0.3f, 1));
    levelToggle("错误", nError, showError, ImVec4(1.0f, 0.45f, 0.45f, 1));
    ImGui::SetNextItemWidth(std::max(120.0f, ImGui::GetContentRegionAvail().x));
    filter.Draw("##filter");
    if (!filter.IsActive()) {
        // 占位提示
        const ImVec2 r = ImGui::GetItemRectMin();
        ImGui::GetWindowDrawList()->AddText(ImVec2(r.x + 6, r.y + 3), IM_COL32(120, 130, 140, 255), "搜索日志…（支持 -排除词）");
    }
    ImGui::Separator();

    ImGui::BeginChild("##log", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    for (const auto& e : entries) {
        const bool isErr = e.level >= spdlog::level::err, isWarn = e.level == spdlog::level::warn;
        if ((isErr && !showError) || (isWarn && !showWarn) || (!isErr && !isWarn && !showInfo)) continue;
        if (!filter.PassFilter(e.text.c_str())) continue;
        ImVec4 col(0.80f, 0.85f, 0.9f, 1.0f);
        if (isErr) col = ImVec4(1.0f, 0.45f, 0.45f, 1.0f);
        else if (isWarn) col = ImVec4(1.0f, 0.82f, 0.35f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::TextUnformatted(e.text.c_str());
        ImGui::PopStyleColor();
    }
    if (autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    ImGui::End();
}

} // namespace eng
