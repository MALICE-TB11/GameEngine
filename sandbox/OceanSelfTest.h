#pragma once
// 海洋 GPU 自测（sandbox --selftest ocean）。
//
// 单元测试只能测 CPU 部分；GPU 上的 演化 → FFT → 组装 是否算对、pass 被关掉时下游是否真的退化，
// 只有在真实的渲染循环里才能验证。这里按脚本一步步操作，每步检查「输出对不对」，而不只是「有没有崩」：
//   1. 首帧：回读 GPU 位移/导数，与 CPU 直接求和逐点比较；此时没有泡沫历史，泡沫应正好等于新泡沫
//   2. 稳定运行几帧后再比一次（有泡沫历史）
//   3. 关掉 OceanFFTColumns：组装、回读应因缺输入被跳过，海面读到默认贴图；重新打开后结果再次正确
//   4. 关掉 OceanSpectrum：后面三个 compute pass 都应跳过
//   5. 关掉 OceanPrepare：海面照常运行（按深水着色）
//   6. 暂停模拟：两次回读的相位时间与位移完全相同（海浪走模拟时间）
//   7. 连续拖动风速 120 帧：统计帧时间 P95/P99、频谱重建次数，最后结果仍与 CPU 一致
//   8. 切到 Gerstner：FFT 的 pass 都不再出现
// 任何一步不符合预期都用 LOG_ERROR 记录 —— 进程返回码随之变成 2，与冒烟测试同一套判定。

#include "render/RenderGraph.h"

#include <chrono>
#include <string>
#include <vector>

namespace eng {
struct SimulationControl;
namespace ocean {
class OceanRenderer;
struct OceanSettings;
} // namespace ocean

class OceanSelfTest {
public:
    // 初始化之后、第一帧之前调用：切到 FFT 模式并请求第一次回读
    void begin(ocean::OceanRenderer& ocean, ocean::OceanSettings& settings);
    // 每帧 drawFrame 成功之后调用。可能修改 settings（App 随后要把它交给 OceanRenderer）。返回 true = 测试结束
    bool afterFrame(ocean::OceanRenderer& ocean, rg::RenderGraph& graph, ocean::OceanSettings& settings, SimulationControl& sim);
    int failures() const { return m_failures; }

private:
    void enter(int step);
    void fail(const std::string& what);
    // 有回读结果就校验并返回 true；还没有返回 false
    bool checkReadback(ocean::OceanRenderer& ocean, const char* label, bool expectHistory);
    bool expectStatus(const rg::RenderGraph& graph, const char* pass, rg::PassReport::Status want);

    int m_step = -1;
    int m_frame = 0;       // 当前步骤已经过的帧数
    int m_totalFrames = 0;
    int m_failures = 0;
    bool m_waiting = false;  // 已请求回读，等结果
    float m_windSpeed = 0.0f; // 原来的风速（拖动结束后恢复）
    // 暂停检查用
    float m_pausedTime = 0.0f;
    std::vector<float> m_pausedHeights;
    // 拖动风速时的帧时间
    std::chrono::steady_clock::time_point m_last;
    std::vector<float> m_frameMs;
    unsigned m_rebuildsBefore = 0;
};

} // namespace eng
