#include "render/passes/ShadowPass.h"

#include "render/FrameConstants.h"
#include "render/FrameContext.h"
#include "render/Mesh.h"

#include <algorithm>

namespace eng {

void ShadowPass::init(PipelineCache& pipelines, VkFormat depthFormat) {
    m_pipelines = &pipelines;
    GraphicsPipelineDesc d;
    d.name = "ShadowDepth";
    d.shader = "passes/shadow_depth";
    d.state.depthFormat = depthFormat; // 没有颜色附件：只写深度
    d.state.depthTest = true;
    d.state.depthWrite = true;
    // 不剔除背面：地面这类单面薄片从背后看也要能挡光；痤疮由采样时的法线偏移处理
    d.state.cullMode = VK_CULL_MODE_NONE;
    setMeshVertexInput(d.state);
    // 深度 pass 只读位置：去掉法线/UV/切线属性（validation 会对「声明了却没用」的属性给出警告）
    std::erase_if(d.state.vertexAttributes, [](const VkVertexInputAttributeDescription& a) { return a.location != 0; });
    m_pipeline = pipelines.createGraphics(d);
}

void ShadowPass::setCascadeViewport(VkCommandBuffer cmd, uint32_t cascade, uint32_t atlasSize) {
    const float half = float(atlasSize / 2);
    const float x = float(cascade & 1) * half, y = float(cascade >> 1) * half;
    const VkViewport vp{x, y + half, half, -half, 0.0f, 1.0f};
    const VkRect2D sc{{int32_t(x), int32_t(y)}, {atlasSize / 2, atlasSize / 2}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
}

void ShadowPass::add(rg::RenderGraph& graph, rg::TextureHandle atlas, const DrawList& draws) {
    graph.addPass("Shadows", rg::PassType::Graphics)
        .depth(atlas, rg::LoadOp::Clear)
        .require([this] { return m_pipelines->ready(m_pipeline); })
        .execute([this, &draws](rg::PassContext& ctx) {
            m_pipelines->bind(ctx.cmd, m_pipeline, ctx.frame->globalSet);
            for (uint32_t c = 0; c < kShadowCascades; ++c) {
                setCascadeViewport(ctx.cmd, c, ctx.extent.width);
                // EXTEND: 每个级联先做视锥剔除，只画落在该级联光源视锥里的物体
                for (const DrawItem& item : draws.opaque) {
                    if (!item.mesh) continue;
                    DrawPush push;
                    push.model = item.model;
                    push.material.flags.w = float(c); // 借用材质参数的空位传级联序号
                    m_pipelines->pushConstants(ctx.cmd, m_pipeline, &push, sizeof(push));
                    // 阴影用较粗的 LOD 也看不出区别：至少用第 1 级（如果有）
                    drawMesh(ctx.cmd, *item.mesh, std::min<uint32_t>(item.lod + 1, uint32_t(item.mesh->lods.size() - 1)));
                }
            }
        });
}

} // namespace eng
