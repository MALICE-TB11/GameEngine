#pragma once
// 性能与日志面板（可停靠窗口）。
//   FrameStats    —— 记录每帧耗时的环形缓冲，状态栏和性能面板共用
//   drawProfilerPanel —— 帧时间曲线、每个 pass 的 GPU 耗时条形图、显存
//   drawConsolePanel  —— 日志：按级别过滤、文字搜索、清空、复制

#include <array>
#include <cstdint>

namespace eng {

class Renderer;
namespace rhi { class Device; }

struct LoopStats {
    double fixedHz = 60.0;
    int stepsThisFrame = 0;
    double alpha = 0.0;
    bool paused = false;
    float timeScale = 1.0f;
};

class FrameStats {
public:
    static constexpr int kHistory = 240;
    void addFrame(float dtSeconds);
    float smoothedMs() const { return m_smoothedMs; }
    float fps() const { return 1000.0f / (m_smoothedMs > 0.001f ? m_smoothedMs : 0.001f); }
    const float* history() const { return m_frameMs.data(); }
    int head() const { return m_head; }
    float maxMs() const;

private:
    std::array<float, kHistory> m_frameMs{};
    int m_head = 0;
    float m_smoothedMs = 16.6f;
};

// 窗口 ID 固定（### 之后的部分），标题可以带图标和中文而不影响停靠布局的保存
inline constexpr const char* kProfilerWindow = "◔ 性能###Profiler";
inline constexpr const char* kConsoleWindow = "☰ 日志###Console";

void drawProfilerPanel(const FrameStats& stats, const rhi::Device& device, const Renderer& renderer, const LoopStats& loop,
                       bool* open);
void drawConsolePanel(bool* open);

// 显存使用（MB）：状态栏与性能面板共用
void queryVramMB(const rhi::Device& device, float& usedMB, float& budgetMB);

} // namespace eng
