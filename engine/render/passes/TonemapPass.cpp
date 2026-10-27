#include "render/passes/TonemapPass.h"

#include "render/FrameContext.h"
#include "rhi/Descriptors.h"
#include "rhi/Device.h"

namespace eng {

void TonemapPass::init(rhi::Device& device, PipelineCache& pipelines, VkFormat outputFormat) {
    m_device = &device;
    m_pipelines = &pipelines;
    // set 1：binding 0 = HDR 场景颜色
    m_setLayout = rhi::createSetLayout(device, {{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}},
                                       "TonemapSetLayout");
    GraphicsPipelineDesc d;
    d.name = "Tonemap";
    d.shader = "passes/tonemap";
    d.state.colorFormats = {outputFormat};
    d.set1 = m_setLayout;
    m_pipeline = pipelines.createGraphics(d);
}

void TonemapPass::shutdown() {
    vkDestroyDescriptorSetLayout(m_device->device(), m_setLayout, nullptr);
}

void TonemapPass::add(rg::RenderGraph& graph, rg::TextureHandle hdr, rg::TextureHandle output) {
    graph.addPass("Tonemap", rg::PassType::Graphics)
        .sampled(hdr)
        // DONT_CARE：全屏三角形会覆盖每个像素，旧内容不需要加载
        .color(output, rg::LoadOp::DontCare)
        .require([this] { return m_pipelines->ready(m_pipeline); })
        .execute([this, hdr](rg::PassContext& ctx) {
            VkDescriptorSet set = ctx.frame->descriptors->allocate(m_setLayout);
            // ctx.view()：如果上游 Forward 被关掉，这里拿到的是默认黑图
            rhi::DescriptorWriter().sampledImage(0, ctx.view(hdr)).update(ctx.frame->device, set);
            m_pipelines->bind(ctx.cmd, m_pipeline, ctx.frame->globalSet);
            m_pipelines->bindSet1(ctx.cmd, m_pipeline, set);
            vkCmdDraw(ctx.cmd, 3, 1, 0, 0); // 全屏三角形，顶点由 SV_VertexID 生成
        });
}

} // namespace eng
