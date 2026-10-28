#include "OceanSelfTest.h"

#include "core/Log.h"
#include "editor/Editor.h"
#include "ocean/OceanRenderer.h"
#include "render/RenderGraph.h"

#include <algorithm>
#include <cmath>

namespace eng {
namespace {

constexpr int kMaxFrames = 3000;    // 超过就算失败（例如回读一直等不到）
constexpr uint32_t kSamples = 48;   // 每个级联抽样比较的点数
constexpr int kTuningFrames = 120;  // 拖动风速的帧数

const rg::PassReport* findPass(const rg::RenderGraph& graph, const char* name) {
    for (const rg::PassReport& p : graph.lastReport())
        if (p.name == name) return &p;
    return nullptr;
}

const char* statusName(rg::PassReport::Status s) {
    switch (s) {
    case rg::PassReport::Status::Ran: return "运行";
    case rg::PassReport::Status::Disabled: return "已关闭";
    case rg::PassReport::Status::NotReady: return "未就绪";
    case rg::PassReport::Status::MissingInput: return "缺输入跳过";
    }
    return "?";
}

} // namespace

void OceanSelfTest::fail(const std::string& what) {
    ++m_failures;
    LOG_ERROR("海洋自测失败（第 {} 步）：{}", m_step, what);
}

void OceanSelfTest::enter(int step) {
    m_step = step;
    m_frame = 0;
    m_waiting = false;
}

void OceanSelfTest::begin(ocean::OceanRenderer& ocean, ocean::OceanSettings& settings) {
    // 在第一帧之前请求回读：第一次真正执行的组装没有泡沫历史，正好检查「首帧」分支
    settings.enabled = true;
    settings.mode = int(ocean::OceanMode::Fft);
    m_windSpeed = settings.windSpeed;
    ocean.requestReadback();
    enter(0);
    LOG_INFO("海洋自测开始");
}

bool OceanSelfTest::checkReadback(ocean::OceanRenderer& ocean, const char* label, bool expectHistory) {
    std::optional<ocean::OceanReadback> rb = ocean.takeReadback();
    if (!rb) return false;
    const ocean::OceanCheck c = ocean::OceanRenderer::verify(*rb, kSamples);
    if (!c.ok) fail(std::string(label) + "：GPU 结果与 CPU 参考不一致 —— " + c.summary);
    else if (rb->hadHistory != expectHistory) fail(std::string(label) + "：泡沫历史状态不对 —— " + c.summary);
    else LOG_INFO("海洋自测 [{}] 通过：t = {:.3f} s，{}", label, rb->phaseTime, c.summary);
    return true;
}

bool OceanSelfTest::expectStatus(const rg::RenderGraph& graph, const char* pass, rg::PassReport::Status want) {
    const rg::PassReport* p = findPass(graph, pass);
    if (!p) {
        fail(std::string(pass) + " 不在本帧的 pass 列表里");
        return false;
    }
    if (p->status != want) {
        fail(std::string(pass) + " 的状态是「" + statusName(p->status) + "」，应为「" + statusName(want) + "」");
        return false;
    }
    return true;
}

bool OceanSelfTest::afterFrame(ocean::OceanRenderer& ocean, rg::RenderGraph& graph, ocean::OceanSettings& settings,
                               SimulationControl& sim) {
    if (++m_totalFrames > kMaxFrames) {
        fail("超过 " + std::to_string(kMaxFrames) + " 帧仍未完成");
        return true;
    }
    ++m_frame;
    switch (m_step) {
    case 0: // 首帧：没有泡沫历史，泡沫 = 新泡沫
        if (checkReadback(ocean, "首帧", false)) enter(1);
        break;

    case 1: // 稳定运行：有泡沫历史
        if (m_frame == 10) ocean.requestReadback();
        if (m_frame > 10 && checkReadback(ocean, "稳定运行", true)) {
            graph.passEnabled("OceanFFTColumns") = false;
            ocean.requestReadback(); // 应该一直等不到：拷贝源（位移）没有内容
            enter(2);
        }
        break;

    case 2: // 关掉列 FFT：组装、回读缺输入跳过，海面读默认贴图
        if (m_frame == 3) {
            expectStatus(graph, "OceanFFTColumns", rg::PassReport::Status::Disabled);
            expectStatus(graph, "OceanAssemble", rg::PassReport::Status::MissingInput);
            expectStatus(graph, "OceanReadback", rg::PassReport::Status::MissingInput);
            if (expectStatus(graph, "Ocean", rg::PassReport::Status::Ran)) {
                const auto& subs = findPass(graph, "Ocean")->substituted;
                if (std::find(subs.begin(), subs.end(), "OceanDisplacement") == subs.end())
                    fail("关掉列 FFT 后海面没有改读默认位移贴图");
            }
            if (ocean.takeReadback()) fail("关掉列 FFT 后仍然回读到了数据");
            LOG_INFO("海洋自测 [关闭 OceanFFTColumns] 检查完成");
            graph.passEnabled("OceanFFTColumns") = true; // 回读请求还在，重新打开后应该立刻得到正确结果
            enter(3);
        }
        break;

    case 3: // 重新打开：结果再次正确（泡沫历史在关闭期间保留着）
        if (checkReadback(ocean, "重新打开 OceanFFTColumns", true)) {
            graph.passEnabled("OceanSpectrum") = false;
            enter(4);
        }
        break;

    case 4: // 关掉源头：后面三个 compute pass 都缺输入
        if (m_frame == 3) {
            expectStatus(graph, "OceanSpectrum", rg::PassReport::Status::Disabled);
            expectStatus(graph, "OceanFFTRows", rg::PassReport::Status::MissingInput);
            expectStatus(graph, "OceanFFTColumns", rg::PassReport::Status::MissingInput);
            expectStatus(graph, "OceanAssemble", rg::PassReport::Status::MissingInput);
            expectStatus(graph, "Ocean", rg::PassReport::Status::Ran);
            LOG_INFO("海洋自测 [关闭 OceanSpectrum] 检查完成");
            graph.passEnabled("OceanSpectrum") = true;
            graph.passEnabled("OceanPrepare") = false;
            enter(5);
        }
        break;

    case 5: // 关掉场景拷贝：海面照常（按深水着色）
        if (m_frame == 3) {
            if (expectStatus(graph, "Ocean", rg::PassReport::Status::Ran)) {
                const auto& subs = findPass(graph, "Ocean")->substituted;
                if (std::find(subs.begin(), subs.end(), "OceanSceneDepth") == subs.end())
                    fail("关掉 OceanPrepare 后海面没有改读默认深度");
            }
            LOG_INFO("海洋自测 [关闭 OceanPrepare] 检查完成");
            graph.passEnabled("OceanPrepare") = true;
            sim.paused = true;
            enter(6);
        }
        break;

    case 6: { // 暂停：海浪走模拟时间，暂停时应完全静止
        if (m_frame == 3 || (m_frame == 10 && !m_pausedHeights.empty())) ocean.requestReadback();
        std::optional<ocean::OceanReadback> rb = m_frame > 3 ? ocean.takeReadback() : std::nullopt;
        if (!rb) break;
        std::vector<float> heights;
        for (size_t i = 0; i < rb->displacement.size(); i += 97) heights.push_back(rb->displacement[i].y);
        if (m_pausedHeights.empty()) {
            m_pausedTime = rb->phaseTime;
            m_pausedHeights = std::move(heights);
        } else {
            if (rb->phaseTime != m_pausedTime || heights != m_pausedHeights)
                fail("暂停期间海面仍在变化（t " + std::to_string(m_pausedTime) + " → " + std::to_string(rb->phaseTime) + "）");
            else LOG_INFO("海洋自测 [暂停] 通过：暂停 7 帧，相位时间与位移完全不变");
            sim.paused = false;
            m_rebuildsBefore = ocean.stats().rebuilds;
            m_frameMs.clear();
            m_last = std::chrono::steady_clock::now();
            enter(7);
        }
        break;
    }

    case 7: { // 连续拖动风速：频谱在后台重建并合并，主线程不应卡顿；结束后结果仍然正确
        const auto now = std::chrono::steady_clock::now();
        if (m_frame > 1) m_frameMs.push_back(std::chrono::duration<float, std::milli>(now - m_last).count());
        m_last = now;
        if (m_frame <= kTuningFrames) {
            settings.windSpeed = m_windSpeed + 5.0f * std::sin(float(m_frame) * 0.15f);
            break;
        }
        if (m_frame == kTuningFrames + 1) {
            settings.windSpeed = m_windSpeed;
            std::vector<float> s = m_frameMs;
            std::sort(s.begin(), s.end());
            auto pct = [&](float p) { return s.empty() ? 0.0f : s[std::min(s.size() - 1, size_t(p * float(s.size() - 1) + 0.5f))]; };
            LOG_INFO("海洋自测 [拖动风速 {} 帧]：帧时间 P50 {:.2f} · P95 {:.2f} · P99 {:.2f} · 最长 {:.2f} ms；频谱重建 {} 次，"
                     "最近一次后台生成 {:.1f} ms、主线程上传 {:.2f} ms",
                     kTuningFrames, pct(0.5f), pct(0.95f), pct(0.99f), s.empty() ? 0.0f : s.back(),
                     ocean.stats().rebuilds - m_rebuildsBefore, ocean.stats().spectrumMs, ocean.stats().uploadMs);
            if (ocean.stats().rebuilds - m_rebuildsBefore >= unsigned(kTuningFrames))
                fail("拖动期间每帧都重建了频谱，修改没有被合并");
            break;
        }
        // 等最后一组参数的频谱生效，再比一次
        if (!m_waiting && ocean.spectrum() && ocean.spectrum()->settings.windSpeed == settings.windSpeed) {
            ocean.requestReadback();
            m_waiting = true;
        } else if (m_waiting && checkReadback(ocean, "拖动风速之后", true)) {
            settings.mode = int(ocean::OceanMode::Gerstner);
            enter(8);
        }
        break;
    }

    case 8: // Gerstner：FFT 的 pass 都不再出现，海面照常
        if (m_frame == 3) {
            if (findPass(graph, "OceanSpectrum") || findPass(graph, "OceanAssemble")) fail("Gerstner 模式下仍然有 FFT 的 pass");
            expectStatus(graph, "Ocean", rg::PassReport::Status::Ran);
            LOG_INFO("海洋自测 [Gerstner] 检查完成");
            LOG_INFO("海洋自测结束：{} 项失败", m_failures);
            return true;
        }
        break;

    default: return true;
    }
    return false;
}

} // namespace eng
