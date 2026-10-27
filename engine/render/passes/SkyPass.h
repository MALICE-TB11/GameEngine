#pragma once
// 天空 pass：在 Forward 之后，只填充「深度仍为 0（无穷远）」的像素。
// 为什么放在 Forward 之后而不是之前：先画不透明物体，天空只在没被遮住的像素上计算，
// 被山和建筑挡住的部分直接被深度测试剔除，省掉天空 shader 的开销。

#include "render/PipelineCache.h"
#include "render/RenderGraph.h"

namespace eng {

class SkyPass {
public:
    void init(PipelineCache& pipelines, VkFormat colorFormat, VkFormat depthFormat);
    void add(rg::RenderGraph& graph, rg::TextureHandle color, rg::TextureHandle depth);

private:
    PipelineCache* m_pipelines = nullptr;
    PipelineHandle m_pipeline = kInvalidPipeline;
};

} // namespace eng
