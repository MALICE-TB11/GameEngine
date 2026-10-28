#pragma once
// 计时与固定步长主循环。
//
// 为什么物理要固定步长：物理积分（尤其碰撞和约束求解）的结果依赖 dt。
// dt 随帧率抖动会导致：高帧率和低帧率下跳跃高度不同、低帧率时穿模、回放不可复现。
// 所以模拟永远以固定的 fixedDt 推进，渲染帧率则自由变化。
//
// 两者如何对齐：累加器 accumulator 收集真实流逝的时间，每攒够一个 fixedDt 就跑一步模拟。
// 剩下不足一步的时间 alpha = accumulator / fixedDt ∈ [0,1)，
// 渲染时在「上一步状态」和「当前步状态」之间按 alpha 插值，画面才不会一顿一顿。
// 代价是渲染画面比模拟落后最多一个 fixedDt（60Hz 时约 16ms），对动作游戏可以接受。

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace eng {

class Clock {
public:
    Clock() : m_start(std::chrono::steady_clock::now()) {}
    // 自构造以来的秒数
    double seconds() const {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - m_start).count();
    }

private:
    std::chrono::steady_clock::time_point m_start;
};

struct FixedStepSettings {
    double fixedHz = 60.0;      // 模拟频率
    int maxStepsPerFrame = 5;   // 单帧最多补几步：防止「越卡越补、越补越卡」的死亡螺旋
    double maxFrameTime = 0.25; // 单帧最长计入时间：断点调试回来后不会一次补几百步
};

class FixedStepLoop {
public:
    explicit FixedStepLoop(const FixedStepSettings& s = {}) { setSettings(s); }

    void setSettings(const FixedStepSettings& s) {
        m_settings = s;
        // 自己保证不变量，不依赖调用方：std::clamp 要求 lo ≤ hi，负的 maxFrameTime 会是未定义行为
        m_settings.fixedHz = std::clamp(s.fixedHz, 1.0, 1000.0);
        m_settings.maxStepsPerFrame = std::max(1, s.maxStepsPerFrame);
        m_settings.maxFrameTime = std::max(s.maxFrameTime, 1.0 / m_settings.fixedHz);
    }
    const FixedStepSettings& settings() const { return m_settings; }

    double fixedDt() const { return 1.0 / m_settings.fixedHz; }

    // 输入本帧真实耗时，返回本帧应执行的模拟步数
    int advance(double frameDt) {
        m_accumulator += std::clamp(frameDt, 0.0, m_settings.maxFrameTime);
        int steps = 0;
        const double dt = fixedDt();
        while (m_accumulator >= dt && steps < m_settings.maxStepsPerFrame) {
            m_accumulator -= dt;
            ++steps;
        }
        // 补不完的时间直接丢掉（相当于游戏世界短暂变慢），而不是留到下一帧继续欠债
        if (steps == m_settings.maxStepsPerFrame && m_accumulator >= dt) {
            m_droppedSeconds += m_accumulator;
            ++m_droppedFrames;
            m_accumulator = 0.0;
        }
        m_lastSteps = steps;
        return steps;
    }

    // 渲染插值因子：0 = 上一步状态，1 = 当前步状态
    double alpha() const { return m_accumulator / fixedDt(); }
    int lastSteps() const { return m_lastSteps; }
    // 因为补步上限而丢弃模拟时间的帧数与总时长：比平均帧率更能说明「卡顿时模拟慢了多少」
    uint64_t droppedFrames() const { return m_droppedFrames; }
    double droppedSeconds() const { return m_droppedSeconds; }

private:
    FixedStepSettings m_settings;
    double m_accumulator = 0.0;
    int m_lastSteps = 0;
    uint64_t m_droppedFrames = 0;
    double m_droppedSeconds = 0.0;
};

} // namespace eng
