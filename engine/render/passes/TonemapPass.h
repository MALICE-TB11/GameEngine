#pragma once
// 色调映射 pass：HDR 场景颜色 → 交换链（LDR，sRGB 编码）。
// M2 只做曝光 + sRGB 编码；M3 加入 ACES / AgX 曲线（shaders/common/tonemap.hlsli）。

#include "render/PipelineCache.h"
#include "render/RenderGraph.h"

namespace eng {

namespace rhi { class Device; }

class TonemapPass {
public:
    void init(rhi::Device& device, PipelineCache& pipelines, VkFormat outputFormat);
    void shutdown();
    void add(rg::RenderGraph& graph, rg::TextureHandle hdr, rg::TextureHandle output);

private:
    rhi::Device* m_device = nullptr;
    PipelineCache* m_pipelines = nullptr;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    PipelineHandle m_pipeline = kInvalidPipeline;
};

} // namespace eng
