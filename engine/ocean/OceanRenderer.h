#pragma once
// 海洋：作为 RenderFeature 挂在「半透明」阶段（天空之后、色调映射之前）。
//
// 每帧的 pass（FFT 模式）：
//   OceanSpectrum   compute  h0 → 时刻 t 的频谱（位移 + 导数，三个级联）        写 Spectrum
//   OceanFFTRows    compute  横向逆 FFT                                       读 Spectrum 写 FFTTemp
//   OceanFFTColumns compute  纵向逆 FFT                                       读 FFTTemp  写 Spatial
//   OceanAssemble   compute  → 位移/导数贴图，泡沫累积（读上一帧的泡沫历史）    读 Spatial  写 Displacement
//   OceanRipples    compute  局部交互：玩家周围的波纹，每个物理固定步一个（暂停时没有）  读写 RipplesA/B
//   OceanPrepare    graphics 拷贝场景颜色、深度线性化（折射、水深）
//   Ocean           graphics 画海面（大海浪 + 波纹）
// Gerstner 模式没有前四个。
// 容错：compute 链上任何一个被关掉，后面的环节因「必需输入没有内容」被 Render Graph 跳过，
// 海面采样到默认贴图（零位移）—— 退化成平静水面；OceanPrepare 被关掉时海面按深水着色。
//
// 初始频谱 h0 在 CPU 生成（OceanSpectrum.h，可单元测试）。参数变了在后台线程重新生成：
// 拖动滑条时连续的修改会合并（同一时间最多一个生成任务），生成完在主线程上传、替换。

#include "ocean/OceanRipples.h"
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

// 海床：告诉波纹求解器「哪里是岸、水有多深」。海洋模块不 include 地形，由 App 每帧把地形高度图传进来
struct OceanSeabed {
    VkImageView heightMap = VK_NULL_HANDLE; // R32F，每个纹素一个高度采样；空 = 处处是深水
    vec2 origin{0.0f};                      // 纹素 (0,0) 的世界 XZ
    float spacing = 1.0f;                   // 相邻采样的间距（米）
    uint32_t resolution = 0;
};

inline constexpr uint32_t kMaxRippleStepsPerFrame = 4; // 一帧最多推进几步波纹（多出来的丢掉最早的）；= ocean_ripples.hlsl 的 RIPPLE_MAX_STEPS

struct RippleStats {
    uint32_t stepsThisFrame = 0;
    uint32_t droppedSteps = 0; // 一帧里排队超过上限而丢掉的步数（累计）
    float speedLimit = 0.0f;   // 稳定性条件允许的最大波速
    float cellSize = 0.0f;
    vec2 center{0.0f};
};

// 波纹回读（校验用）：同一帧里某一步的输入状态、输出状态和这一步的参数
struct RippleReadback {
    RippleParams params;
    RippleStepInput step;
    bool hadHistory = false;
    RippleGrid input, output;
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

    // ---- 局部交互（波纹） ----
    // 每帧由 App 传入地形高度图（view 只在本帧有效）
    void setSeabed(const OceanSeabed& seabed) { m_seabed = seabed; }
    // 物理固定步里调用：排队推进一步波纹。simTime = 这一步之后的模拟时间；center = 区域中心（玩家位置）
    void queueRippleStep(double simTime, float dt, vec2 center, const std::vector<RippleSource>& sources);
    // 下一步加一个一次性扰动：在 xz 处把水面压下约 depth 米（编辑器按钮、测试用）
    void splash(vec2 xz, float radius, float depth);
    const RippleStats& rippleStats() const { return m_rippleStats; }

    // ---- 校验（自测用，平时不调用） ----
    // 请求回读：之后第一个「组装真正执行」的帧，把位移/导数贴图拷到 CPU 可读的缓冲
    void requestReadback() { m_readbackRequested = true; }
    // 取回读结果（没有则返回空）。会 vkDeviceWaitIdle 等那一帧执行完 —— 只在测试里用
    std::optional<OceanReadback> takeReadback();
    // 用 CPU 参考（直接求和的 DFT）逐点比较 GPU 结果：每个级联随机抽 samples 个点
    static OceanCheck verify(const OceanReadback& readback, uint32_t samples);
    // 波纹：下一个有波纹步的帧，回读第一步的输入和输出；verifyRipples 用 CPU 的 rippleStep 重算一遍逐格比较
    void requestRippleCheck() { m_rippleCheckRequested = true; }
    std::optional<RippleReadback> takeRippleCheck();
    static OceanCheck verifyRipples(const RippleReadback& readback);

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
    // 本帧排队的波纹步 → compute pass；返回海面要采样的最新状态（没有波纹时无效句柄）
    rg::TextureHandle addRipplePasses(rg::RenderGraph& graph);
    void addRippleCopyPass(rg::RenderGraph& graph, const char* name, rg::TextureHandle tex, uint32_t slot);
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
    VkDescriptorSetLayout m_rippleLayout = VK_NULL_HANDLE;
    PipelineHandle m_ripple = kInvalidPipeline;

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

    // 波纹
    struct PendingRipple {
        RippleStepInput input;
        RippleParams params;
        bool reset = false; // 参数变了 / 刚启用：不读上一步的状态，从平静水面开始
    };
    OceanSeabed m_seabed;
    std::vector<PendingRipple> m_pendingRipples; // 物理步排队，下一次画海时变成 compute pass
    std::vector<RippleSource> m_splashes;        // 下一步的一次性扰动（strength 暂存深度，米；排队时换算成加速度）
    std::vector<RippleSource> m_splashFollowUps; // 上一步的水花：这一步加反向加速度抵消速度
    ivec2 m_rippleOrigin{0};
    RippleParams m_rippleParams;                 // 最近一步的参数（海面采样时的网格映射）
    bool m_rippleHasOrigin = false;
    bool m_rippleReset = true;
    bool m_rippleLatestB = false;                // 最新状态在 B（否则在 A）
    bool m_rippleAlive = false;                  // 状态贴图里有有效的波纹（否则海面不采样它）
    double m_rippleStepTime = 0.0;               // 最近一步之后的模拟时间（渲染插值用）
    float m_rippleStepDt = 1.0f / 60.0f;
    RippleStats m_rippleStats;
    std::array<rhi::Buffer, rhi::kFramesInFlight> m_rippleStepBuffers{}; // 每帧每步的扰动源等（常量缓冲）
    bool m_rippleCheckRequested = false;
    bool m_rippleLastHadHistory = false; // 波纹 pass 执行时写，同一帧之后的拷贝 pass 读
    bool m_rippleInputCopied = false;
    rhi::Buffer m_rippleReadbackBuffer;
    std::optional<RippleReadback> m_rippleCheckPending; // 本帧录了校验用的三个 pass：参数先放这里
    std::optional<RippleReadback> m_rippleRecorded;     // 三个 pass 都执行了：等 GPU 执行完再读

    bool m_readbackRequested = false;
    bool m_lastAssembleHadHistory = false; // 组装 pass 执行时写，回读 pass（同一帧、在它之后）读
    rhi::Buffer m_readbackBuffer;
    std::optional<OceanReadback> m_readbackRecorded; // 本帧录制了拷贝（数据要等 GPU 执行完才有效）
};

} // namespace eng::ocean
