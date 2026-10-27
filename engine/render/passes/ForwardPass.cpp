#include "render/passes/ForwardPass.h"

#include "render/FrameConstants.h"
#include "render/FrameContext.h"
#include "render/Mesh.h"

namespace eng {

void ForwardPass::init(PipelineCache& pipelines, VkFormat colorFormat, VkFormat depthFormat) {
    m_pipelines = &pipelines;

    GraphicsPipelineDesc d;
    d.state.colorFormats = {colorFormat};
    d.state.depthFormat = depthFormat;
    d.state.depthTest = true;
    d.state.depthWrite = true;
    d.state.cullMode = VK_CULL_MODE_BACK_BIT;
    setMeshVertexInput(d.state);

    d.name = "ForwardLit";
    d.shader = "materials/debug_lit";
    m_lit = pipelines.createGraphics(d);

    d.name = "ForwardWireframe";
    d.state.polygonMode = VK_POLYGON_MODE_LINE;
    m_wire = pipelines.createGraphics(d);

    // 错误管线：不依赖任何贴图或材质参数，尽量保证它总能编译成功
    d.name = "ForwardError";
    d.shader = "materials/error";
    d.state.polygonMode = VK_POLYGON_MODE_FILL;
    m_error = pipelines.createGraphics(d);
}

void ForwardPass::add(rg::RenderGraph& graph, rg::TextureHandle color, rg::TextureHandle depth, const DrawList& draws) {
    graph.addPass("Forward", rg::PassType::Graphics)
        .color(color, rg::LoadOp::Clear)
        .depth(depth, rg::LoadOp::Clear)
        // 正常管线和错误管线至少有一个能用才执行；都坏了就跳过，下游读到默认黑图
        .require([this] { return m_pipelines->ready(m_lit) || m_pipelines->ready(m_error); })
        .execute([this, &draws](rg::PassContext& ctx) {
            const PipelineHandle wanted = wireframe ? m_wire : m_lit;
            const PipelineHandle p = m_pipelines->ready(wanted) ? wanted : m_error;
            m_pipelines->bind(ctx.cmd, p, ctx.frame->globalSet);
            for (const DrawItem& item : draws.opaque) {
                if (!item.mesh) continue;
                DrawPush push;
                push.model = item.model;
                push.normalMatrix = glm::transpose(glm::inverse(item.model));
                m_pipelines->pushConstants(ctx.cmd, p, &push, sizeof(push));
                drawMesh(ctx.cmd, *item.mesh);
            }
        });
}

} // namespace eng
