#pragma once
// 前向不透明 pass：把所有不透明网格画进 HDR 场景颜色 + 深度。
// 管线没准备好（shader 编译失败）时，这些物体改用错误管线（品红条纹）绘制。

#include "render/PipelineCache.h"
#include "render/RenderGraph.h"
#include "render/RenderTypes.h"

namespace eng {

class ForwardPass {
public:
    void init(PipelineCache& pipelines, VkFormat colorFormat, VkFormat depthFormat);
    void add(rg::RenderGraph& graph, rg::TextureHandle color, rg::TextureHandle depth, const DrawList& draws);

    bool wireframe = false;

private:
    PipelineCache* m_pipelines = nullptr;
    PipelineHandle m_lit = kInvalidPipeline;
    PipelineHandle m_wire = kInvalidPipeline;
    PipelineHandle m_error = kInvalidPipeline;
};

} // namespace eng
