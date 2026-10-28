#pragma once
// 海洋：作为 RenderFeature 挂在「半透明」阶段（天空之后、色调映射之前）。
//
// 每帧的 pass（FFT 模式）：
//   OceanSpectrum   compute  h0 → 时刻 t 的频谱（位移 + 导数，三个级联）
//   OceanFFTRows    compute  横向逆 FFT
//   OceanFFTColumns compute  纵向逆 FFT
//   OceanAssemble   compute  → 位移/导数贴图，泡沫累积（读上一帧的位移贴图，A/B 交替）
//   OceanPrepare    graphics 拷贝场景颜色、深度线性化（折射、水深）
//   Ocean           graphics 画海面
// Gerstner 模式只有后两个 pass。任何一个被关掉，下游都会拿到默认资源，海面退化成平面但不会崩。
//
// 初始频谱 h0 在 CPU 生成（OceanSpectrum.h，可单元测试），参数变了才重新生成并上传。

#include "ocean/OceanSettings.h"
#include "ocean/OceanSpectrum.h"
#include "render/PipelineCache.h"
#include "render/RenderFeature.h"
#include "rhi/Resources.h"

#include <array>

namespace eng {
class JobSystem;
}

namespace eng::ocean {

struct OceanStats {
    float significantWaveHeight = 0.0f; // 有效波高 Hs（米）
    float peakWavelength = 0.0f;        // 峰值波长（米）
    float peakPeriod = 0.0f;            // 峰值周期（秒）
    float spectrumMs = 0.0f;            // 最近一次 CPU 生成频谱的耗时
    uint32_t vertices = 0;
};

class OceanRenderer final : public RenderFeature {
public:
    bool init(rhi::Device& device, PipelineCache& pipelines, JobSystem* jobs, VkFormat colorFormat, VkFormat depthFormat);
    void shutdown();

    // 频谱相关的参数变了会在下一帧重新生成 h0；着色参数立即生效
    void setSettings(const OceanSettings& s);
    const OceanSettings& settings() const { return m_settings; }
    void setFoamTexture(const rhi::Image* image) { m_foamTex = image; } // 每帧由 App 从 AssetManager 取（可能为空）
    void reseed() { ++m_seed; m_spectrumDirty = true; }                 // 换一组随机相位

    const char* name() const override { return "Ocean"; }
    void addTransparentPasses(rg::RenderGraph& graph, const SceneTargets& targets) override;

    // CPU 端水面高度（Gerstner 参考实现；FFT 模式下是同一频谱的近似）。M6 的浮力与 Medium::Query 用
    float heightAt(float x, float z, float time) const;

    bool wireframe = false;
    const OceanStats& stats() const { return m_stats; }

private:
    struct SimPush;
    struct SurfacePush;
    void rebuildSpectrum();
    void rebuildGrid();

    rhi::Device* m_device = nullptr;
    PipelineCache* m_pipelines = nullptr;
    JobSystem* m_jobs = nullptr;
    VkFormat m_colorFormat = VK_FORMAT_UNDEFINED;
    OceanSettings m_settings;
    OceanSettings m_builtFrom; // 当前 h0 / Gerstner 波是用哪组参数生成的
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

    rhi::Image m_h0; // 初始频谱（RGBA32F，3 层）
    std::vector<GerstnerWave> m_waves;
    std::array<rhi::Buffer, rhi::kFramesInFlight> m_waveBuffers{}; // 每个在途帧一份，CPU 写时 GPU 不会正在读
    rhi::Buffer m_indexBuffer;
    uint32_t m_indexCount = 0;
    int m_gridRings = 0, m_gridSegments = 0;
    const rhi::Image* m_foamTex = nullptr;
    OceanStats m_stats;
};

} // namespace eng::ocean
