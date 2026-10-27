#include "render/passes/SkyPass.h"

#include "render/FrameContext.h"

namespace eng {

void SkyPass::init(PipelineCache& pipelines, VkFormat colorFormat, VkFormat depthFormat) {
    m_pipelines = &pipelines;
    GraphicsPipelineDesc d;
    d.name = "Sky";
    d.shader = "passes/sky";
    d.state.colorFormats = {colorFormat};
    d.state.depthFormat = depthFormat;
    // 全屏三角形的深度是 0（反向 Z 的无穷远）。GREATER_OR_EQUAL：只有深度缓冲里仍是 0 的像素能通过
    d.state.depthTest = true;
    d.state.depthWrite = false;
    d.state.depthCompare = VK_COMPARE_OP_GREATER_OR_EQUAL;
    m_pipeline = pipelines.createGraphics(d);
}

void SkyPass::add(rg::RenderGraph& graph, rg::TextureHandle color, rg::TextureHandle depth) {
    graph.addPass("Sky", rg::PassType::Graphics)
        .color(color, rg::LoadOp::Load)
        .depthReadOnly(depth) // 只读深度：可以和后续读取深度的 pass 共享布局
        .require([this] { return m_pipelines->ready(m_pipeline); })
        .execute([this](rg::PassContext& ctx) {
            m_pipelines->bind(ctx.cmd, m_pipeline, ctx.frame->globalSet);
            vkCmdDraw(ctx.cmd, 3, 1, 0, 0);
        });
}

} // namespace eng
