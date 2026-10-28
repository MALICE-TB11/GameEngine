#pragma once
// FXAA 抗锯齿：在色调映射之后的 LDR 图像上，沿检测到的边缘方向做一次模糊。
// 便宜（约 0.1 ms）、不需要历史帧和运动矢量；代价是会略微糊掉细小纹理。
// EXTEND: M6.5 引入 TAA（体积云也需要时间累积），届时 FXAA 作为低配选项保留。

#include "render/PipelineCache.h"
#include "render/RenderGraph.h"

namespace eng {

namespace rhi { class Device; }

class FxaaPass {
public:
    void init(rhi::Device& device, PipelineCache& pipelines, VkFormat outputFormat);
    void shutdown();
    void add(rg::RenderGraph& graph, rg::TextureHandle ldr, rg::TextureHandle output);

private:
    rhi::Device* m_device = nullptr;
    PipelineCache* m_pipelines = nullptr;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    PipelineHandle m_pipeline = kInvalidPipeline;
};

} // namespace eng
