#pragma once
// 海洋：作为 RenderFeature 挂在「半透明」阶段（天空之后、色调映射之前）。
//
// 每帧的 pass（FFT 模式）：
//   OceanSpectrum   compute  h0 → 时刻 t 的频谱（位移 + 导数，三个级联）        写 Spectrum
//   OceanFFTRows    compute  横向逆 FFT                                       读 Spectrum 写 FFTTemp
//   OceanFFTColumns compute  纵向逆 FFT                                       读 FFTTemp  写 Spatial
//   OceanAssemble   compute  → 位移/导数贴图，泡沫累积（读上一帧的泡沫历史）    读 Spatial  写 Displacement
//   OceanPrepare    graphics 拷贝场景颜色、深度线性化（折射、水深）
//   Ocean           graphics 画海面
// Gerstner 模式只有后两个 pass。
// 容错：compute 链上任何一个被关掉，后面的环节因「必需输入没有内容」被 Render Graph 跳过，
// 海面采样到默认贴图（零位移）—— 退化成平静水面；OceanPrepare 被关掉时海面按深水着色。
//
// 初始频谱 h0 在 CPU 生成（OceanSpectrum.h，可单元测试）。参数变了在后台线程重新生成：
// 拖动滑条时连续的修改会合并（同一时间最多一个生成任务），生成完在主线程上传、替换。

#include "ocean/OceanSettings.h"
#include "ocean/OceanSpectrum.h"
#include "render/PipelineCache.h"
#include "render/RenderFeature.h"
#include "rhi/Resources.h"

#include <array>
#include <memory>
#include <optional>
#include <string>

namespace eng {
class JobSystem;
struct JobCounter;
} // namespace eng

namespace eng::ocean {

struct OceanStats {
    float significantWaveHeight = 0.0f; // 有效波高 Hs（米）
    float peakWavelength = 0.0f;        // 峰值波长（米）
    float peakPeriod = 0.0f;            // 峰值周期（秒）
    float spectrumMs = 0.0f;            // 最近一次后台生成频谱的耗时（不占主线程）
    float uploadMs = 0.0f;              // 最近一次上传频谱贴图时主线程等待的时间
    uint32_t rebuilds = 0;              // 启动以来生成了几次频谱
    uint32_t vertices = 0;
};

// 一份完整的频谱：CPU 端数据 + 生成它的参数。GPU 上的 h0 / ω 贴图就是它的拷贝。
// 用 shared_ptr 共享：回读校验、M6 的 CPU 查询都要确认「用的是哪一份」，后台生成新的时旧的仍然有效。
struct OceanSpectrumData {
    OceanSettings settings;
    uint32_t seed = 0;
    std::vector<vec4> h0;     // N×N×级联数，层优先
    std::vector<float> omega; // 同上，每个格子的 ω(k)
    std::vector<GerstnerWave> waves;
    float significantWaveHeight = 0.0f;
    float generateMs = 0.0f;
};

// GPU 结果回读（校验用，见 verify）
struct OceanReadback {
    std::shared_ptr<const OceanSpectrumData> spectrum; // 那一帧 GPU 用的频谱
    float phaseTime = 0.0f;                            // 那一帧的相位时间
    float lambda = 0.0f;                               // 尖锐度
    float foamBias = 0.0f, foamIntensity = 0.0f;
    bool hadHistory = false;                           // 组装时有没有上一帧的泡沫
    std::vector<vec4> displacement, derivatives;       // N×N×级联数，层优先（已从 half 转成 float）
};

struct OceanCheck {
    bool ok = false;
    float maxError = 0.0f;  // 位移/导数的最大误差，除以该分量的均方根（相对误差）
    float foamError = 0.0f; // 泡沫的最大误差（绝对值）
    uint32_t nonFinite = 0; // NaN / Inf 的个数
    std::string summary;
};

class OceanRenderer final : public RenderFeature {
public:
    bool init(rhi::Device& device, PipelineCache& pipelines, JobSystem* jobs, VkFormat colorFormat, VkFormat depthFormat);
    void shutdown();

    // 频谱相关的参数变了会在后台重新生成 h0；着色参数立即生效
    void setSettings(const OceanSettings& s);
    const OceanSettings& settings() const { return m_settings; }
    void setFoamTexture(const rhi::Image* image) { m_foamTex = image; } // 每帧由 App 从 AssetManager 取（可能为空）
    void reseed() { ++m_seed; m_spectrumDirty = true; }                 // 换一组随机相位

    const char* name() const override { return "Ocean"; }
    void addTransparentPasses(rg::RenderGraph& graph, const SceneTargets& targets) override;

    // CPU 端水面高度，simTime 与 RenderView::simTime 同一时间轴（物理步里传当前步的模拟时间）。
    // Gerstner 模式：与画面是同一组波，逐点一致。
    // FFT 模式：仍是 Gerstner —— 统计上（波高、波长、方向）相同，但逐点不是同一片海面！
    // EXTEND: M6 浮力前要选一种和 FFT 画面逐点一致的查询（GPU 回读 / CPU 小尺寸 FFT），见 docs/06-海洋.md
    float heightAt(float x, float z, double simTime) const;

    // ---- 校验（自测用，平时不调用） ----
    // 请求回读：之后第一个「组装真正执行」的帧，把位移/导数贴图拷到 CPU 可读的缓冲
    void requestReadback() { m_readbackRequested = true; }
    // 取回读结果（没有则返回空）。会 vkDeviceWaitIdle 等那一帧执行完 —— 只在测试里用
    std::optional<OceanReadback> takeReadback();
    // 用 CPU 参考（直接求和的 DFT）逐点比较 GPU 结果：每个级联随机抽 samples 个点
    static OceanCheck verify(const OceanReadback& readback, uint32_t samples);

    bool wireframe = false;
    const OceanStats& stats() const { return m_stats; }
    // 当前 GPU 在用的频谱（启动后第一帧之前为空）
    std::shared_ptr<const OceanSpectrumData> spectrum() const { return m_spectrum; }

private:
    struct SimPush;
    struct SurfacePush;
    struct PendingSpectrum;
    void updateSpectrum();
    void launchSpectrumJob();
    void uploadSpectrum(std::shared_ptr<const OceanSpectrumData> data);
    void addReadbackPass(rg::RenderGraph& graph, rg::TextureHandle disp, rg::TextureHandle deriv,
                         std::shared_ptr<const OceanSpectrumData> spectrum, float phaseTime);
    void rebuildGrid();

    rhi::Device* m_device = nullptr;
    PipelineCache* m_pipelines = nullptr;
    JobSystem* m_jobs = nullptr;
    VkFormat m_colorFormat = VK_FORMAT_UNDEFINED;
    OceanSettings m_settings;
    OceanSettings m_requestedFrom; // 最近一次发起生成时的参数（与它比较决定要不要再生成）
    bool m_spectrumDirty = true;
    uint32_t m_seed = 1;
    uint64_t m_frame = 0;

    VkDescriptorSetLayout m_evolveLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_fftLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_assembleLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_prepareLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_surfaceLayout = VK_NULL_HANDLE;
    PipelineHandle m_evolve = kInvalidPipeline, m_fft = kInvalidPipeline, m_assemble = kInvalidPipeline;
    PipelineHandle m_prepare = kInvalidPipeline, m_surface = kInvalidPipeline, m_surfaceWire = kInvalidPipeline;

    std::shared_ptr<const OceanSpectrumData> m_spectrum; // 当前 GPU 在用的
    std::shared_ptr<PendingSpectrum> m_pending;          // 后台正在生成的（最多一个）
    rhi::Image m_h0;    // 初始频谱（RGBA32F，3 层）
    rhi::Image m_omega; // 每个格子的 ω（R32F，3 层）
    std::array<rhi::Buffer, rhi::kFramesInFlight> m_waveBuffers{}; // 每个在途帧一份，CPU 写时 GPU 不会正在读
    rhi::Buffer m_indexBuffer;
    uint32_t m_indexCount = 0;
    int m_gridRings = 0, m_gridSegments = 0;
    const rhi::Image* m_foamTex = nullptr;
    OceanStats m_stats;

    bool m_readbackRequested = false;
    bool m_lastAssembleHadHistory = false; // 组装 pass 执行时写，回读 pass（同一帧、在它之后）读
    rhi::Buffer m_readbackBuffer;
    std::optional<OceanReadback> m_readbackRecorded; // 本帧录制了拷贝（数据要等 GPU 执行完才有效）
};

} // namespace eng::ocean
