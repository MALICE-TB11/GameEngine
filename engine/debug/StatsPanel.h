#pragma once
// 性能统计面板 + 日志面板。

#include <array>
#include <cstdint>

namespace eng {

class Renderer;
namespace rhi { class Device; }

struct LoopStats {
    double fixedHz = 60.0;
    int stepsThisFrame = 0;
    double alpha = 0.0;
};

class StatsPanel {
public:
    // 每帧调用一次，记录帧时间（秒）
    void addFrame(float dtSeconds);
    void draw(const rhi::Device& device, const Renderer& renderer, const LoopStats& loop);

private:
    static constexpr int kHistory = 240;
    std::array<float, kHistory> m_frameMs{};
    int m_head = 0;
    float m_smoothedMs = 16.6f;
};

// 显示最近的日志；error 红色、warning 黄色
void drawLogPanel();

} // namespace eng
