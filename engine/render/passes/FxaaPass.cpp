#include "render/passes/FxaaPass.h"

#include "render/FrameContext.h"
#include "rhi/Descriptors.h"
#include "rhi/Device.h"

namespace eng {

void FxaaPass::init(rhi::Device& device, PipelineCache& pipelines, VkFormat outputFormat) {
    m_device = &device;
    m_pipelines = &pipelines;
    m_setLayout = rhi::createSetLayout(device, {{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}},
                                       "FxaaSetLayout");
    GraphicsPipelineDesc d;
    d.name = "FXAA";
    d.shader = "passes/fxaa";
    d.state.colorFormats = {outputFormat};
    d.set1 = m_setLayout;
    m_pipeline = pipelines.createGraphics(d);
}

void FxaaPass::shutdown() {
    if (!m_device) return; // init 失败（或从没 init）时也能安全调用
    vkDestroyDescriptorSetLayout(m_device->device(), m_setLayout, nullptr);
}

void FxaaPass::add(rg::RenderGraph& graph, rg::TextureHandle ldr, rg::TextureHandle output) {
    graph.addPass("FXAA", rg::PassType::Graphics)
        .sampled(ldr)
        .color(output, rg::LoadOp::DontCare)
        .require([this] { return m_pipelines->ready(m_pipeline); })
        .execute([this, ldr](rg::PassContext& ctx) {
            VkDescriptorSet set = ctx.frame->descriptors->allocate(m_setLayout);
            rhi::DescriptorWriter().sampledImage(0, ctx.view(ldr)).update(ctx.frame->device, set);
            m_pipelines->bind(ctx.cmd, m_pipeline, ctx.frame->globalSet);
            m_pipelines->bindSet1(ctx.cmd, m_pipeline, set);
            vkCmdDraw(ctx.cmd, 3, 1, 0, 0);
        });
}

} // namespace eng
