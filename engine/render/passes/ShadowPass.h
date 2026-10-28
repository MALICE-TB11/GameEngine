#pragma once
// 阴影 pass：从太阳方向把所有不透明网格画进阴影图集的 4 个象限（每个级联一个象限），只写深度。

#include "render/PipelineCache.h"
#include "render/RenderGraph.h"
#include "render/RenderTypes.h"
#include "render/Shadows.h"

namespace eng {

class ShadowPass {
public:
    void init(PipelineCache& pipelines, VkFormat depthFormat);
    void add(rg::RenderGraph& graph, rg::TextureHandle atlas, const DrawList& draws);

    // 每个级联在图集中的视口（负高度：与场景渲染一致的 Y 翻转约定）
    static void setCascadeViewport(VkCommandBuffer cmd, uint32_t cascade, uint32_t atlasSize);

private:
    PipelineCache* m_pipelines = nullptr;
    PipelineHandle m_pipeline = kInvalidPipeline;
};

} // namespace eng
