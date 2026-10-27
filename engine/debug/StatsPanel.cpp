#include "debug/StatsPanel.h"

#include "core/Log.h"
#include "render/Renderer.h"
#include "rhi/Device.h"

#include <imgui.h>

#include <algorithm>

namespace eng {

void StatsPanel::addFrame(float dtSeconds) {
    const float ms = dtSeconds * 1000.0f;
    m_frameMs[m_head] = ms;
    m_head = (m_head + 1) % kHistory;
    // 指数平滑：直接显示每帧数字会跳得看不清
    m_smoothedMs += (ms - m_smoothedMs) * 0.05f;
}

void StatsPanel::draw(const rhi::Device& device, const Renderer& renderer, const LoopStats& loop) {
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(400, 310), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("统计")) {
        ImGui::End();
        return;
    }
    ImGui::Text("FPS %.1f   帧时间 %.2f ms", 1000.0f / std::max(m_smoothedMs, 0.001f), m_smoothedMs);

    // 帧时间曲线：以环形缓冲当前写入点为起点，曲线从左到右按时间顺序
    float maxMs = 0.0f;
    for (float v : m_frameMs) maxMs = std::max(maxMs, v);
    ImGui::PlotLines("##frametime", m_frameMs.data(), kHistory, m_head, "帧时间 (ms)", 0.0f, std::max(33.3f, maxMs),
                     ImVec2(-1, 60));

    ImGui::SeparatorText("主循环");
    ImGui::Text("固定步长 %.0f Hz，本帧 %d 步，插值 alpha %.2f", loop.fixedHz, loop.stepsThisFrame, loop.alpha);

    ImGui::SeparatorText("设备");
    ImGui::TextUnformatted(device.properties().deviceName);
    const auto& sc = renderer.swapchain();
    ImGui::Text("交换链 %u x %u，%u 张，%s", sc.extent().width, sc.extent().height, sc.imageCount(),
                sc.vsync() ? "垂直同步" : "无垂直同步");
    if (device.memoryBudgetSupported()) {
        uint64_t usage = 0, budget = 0;
        device.queryMemory(usage, budget);
        const float usedMB = float(usage) / (1024.0f * 1024.0f);
        // 项目预算 2GB：超了标红
        const ImVec4 col = usedMB > 2048.0f ? ImVec4(1, 0.3f, 0.3f, 1) : ImVec4(0.6f, 1, 0.6f, 1);
        ImGui::TextColored(col, "显存 %.0f MB / 预算 2048 MB（驱动可用 %.0f MB）", usedMB, float(budget) / (1024.0f * 1024.0f));
    }
    ImGui::Text("Validation: %s   错误日志: %u", device.validationEnabled() ? "开" : "关", log::errorCount());
    ImGui::End();
}

void drawLogPanel() {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(420, display.y - 190), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(display.x - 430, 180), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("日志")) {
        ImGui::End();
        return;
    }
    static bool autoScroll = true;
    ImGui::Checkbox("自动滚动", &autoScroll);
    ImGui::Separator();
    ImGui::BeginChild("##log", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    for (const auto& e : log::recent()) {
        ImVec4 col(0.85f, 0.85f, 0.85f, 1.0f);
        if (e.level >= spdlog::level::err)
            col = ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
        else if (e.level == spdlog::level::warn)
            col = ImVec4(1.0f, 0.85f, 0.3f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::TextUnformatted(e.text.c_str());
        ImGui::PopStyleColor();
    }
    if (autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    ImGui::End();
}

} // namespace eng
