#include "render/passes/ForwardPass.h"

#include "render/DefaultResources.h"
#include "render/FrameConstants.h"
#include "render/FrameContext.h"
#include "render/Mesh.h"
#include "rhi/Descriptors.h"
#include "rhi/Device.h"

#include <algorithm>
#include <unordered_map>

namespace eng {

void ForwardPass::init(rhi::Device& device, PipelineCache& pipelines, VkFormat colorFormat, VkFormat depthFormat) {
    m_device = &device;
    m_pipelines = &pipelines;

    // set 1：材质贴图。binding 号与 shaders/materials/pbr_standard.hlsl 一致
    auto tex = [](uint32_t b) {
        return VkDescriptorSetLayoutBinding{b, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    };
    // binding 5 = 阴影图集：每个材质集都带上它，阴影 pass 被关掉时 ctx.view() 自动换成「处处照亮」的默认图
    m_setLayout = rhi::createSetLayout(device, {tex(0), tex(1), tex(2), tex(3), tex(4), tex(5)}, "MaterialSetLayout");

    GraphicsPipelineDesc d;
    d.state.colorFormats = {colorFormat};
    d.state.depthFormat = depthFormat;
    d.state.depthTest = true;
    d.state.depthWrite = true;
    d.state.cullMode = VK_CULL_MODE_BACK_BIT;
    d.set1 = m_setLayout;
    setMeshVertexInput(d.state);

    d.name = "ForwardPBR";
    d.shader = "materials/pbr_standard";
    m_pbr = pipelines.createGraphics(d);

    // 双面材质（树叶、布料）：关闭背面剔除。shader 里会翻转背面的法线。
    d.name = "ForwardPBRDoubleSided";
    d.state.cullMode = VK_CULL_MODE_NONE;
    m_pbrDoubleSided = pipelines.createGraphics(d);

    d.name = "ForwardWireframe";
    d.state.polygonMode = VK_POLYGON_MODE_LINE;
    m_wire = pipelines.createGraphics(d);

    // 错误管线：不依赖贴图和材质参数，尽量保证它总能编译成功
    d.name = "ForwardError";
    d.shader = "materials/error";
    d.state.polygonMode = VK_POLYGON_MODE_FILL;
    d.state.cullMode = VK_CULL_MODE_BACK_BIT;
    m_error = pipelines.createGraphics(d);

    defaultMaterial.name = "Default";
    defaultMaterial.params.baseColor = {0.8f, 0.8f, 0.8f, 1.0f};
    defaultMaterial.params.pbr = {0.0f, 0.6f, 1.0f, 0.5f};
}

void ForwardPass::shutdown() {
    if (!m_device) return; // init 失败（或从没 init）时也能安全调用
    vkDestroyDescriptorSetLayout(m_device->device(), m_setLayout, nullptr);
}

PipelineHandle ForwardPass::pipelineFor(const Material& m) const {
    // EXTEND: M7 的 toon 材质在这里按 m.shadingModel() 选择 toon 管线
    PipelineHandle wanted = wireframe ? m_wire : (m.doubleSided ? m_pbrDoubleSided : m_pbr);
    return m_pipelines->ready(wanted) ? wanted : m_error;
}

void ForwardPass::add(rg::RenderGraph& graph, rg::TextureHandle color, rg::TextureHandle depth, rg::TextureHandle shadowAtlas,
                      const DrawList& draws) {
    graph.addPass("Forward", rg::PassType::Graphics)
        .color(color, rg::LoadOp::Load)
        .depth(depth, rg::LoadOp::Load)
        .sampled(shadowAtlas)
        // 正常管线和错误管线至少有一个能用才执行；都坏了就跳过，下游读到默认黑图
        .require([this] { return m_pipelines->ready(m_pbr) || m_pipelines->ready(m_error); })
        .execute([this, &draws, shadowAtlas](rg::PassContext& ctx) {
            const DefaultResources& def = *ctx.frame->defaults;
            const VkImageView shadowView = ctx.view(shadowAtlas);
            // 同一帧里同一材质只分配并写一次描述符集
            std::unordered_map<const Material*, VkDescriptorSet> sets;
            auto materialSet = [&, shadowView](const Material& m) {
                if (auto it = sets.find(&m); it != sets.end()) return it->second;
                VkDescriptorSet set = ctx.frame->descriptors->allocate(m_setLayout);
                auto view = [](const rhi::Image* img, const rhi::Image& fallback) { return img ? img->view : fallback.view; };
                rhi::DescriptorWriter()
                    .sampledImage(0, view(m.baseColorTex, def.white()))
                    .sampledImage(1, view(m.normalTex, def.flatNormal()))
                    .sampledImage(2, view(m.metalRoughTex, def.white())) // 白 = 粗糙度/金属度由参数决定
                    .sampledImage(3, view(m.occlusionTex, def.white()))  // 白 = 无遮蔽
                    .sampledImage(4, view(m.emissiveTex, def.white()))   // 白 × emissive 参数
                    .sampledImage(5, shadowView)
                    .update(ctx.frame->device, set);
                sets.emplace(&m, set);
                return set;
            };

            PipelineHandle bound = kInvalidPipeline;
            for (const DrawItem& item : draws.opaque) {
                if (!item.mesh) continue;
                const Material& mat = item.material ? *item.material : defaultMaterial;
                const PipelineHandle p = pipelineFor(mat);
                if (p != bound) {
                    m_pipelines->bind(ctx.cmd, p, ctx.frame->globalSet);
                    bound = p;
                }
                m_pipelines->bindSet1(ctx.cmd, p, materialSet(mat));
                DrawPush push;
                push.model = item.model;
                push.material = item.paramsOverride ? *item.paramsOverride : mat.params;
                if (item.highlight > 0.0f) {
                    // 选中高亮：叠加一点橙色自发光。只改这次 draw 的 push constant，不改材质本身
                    const vec3 kHighlight{1.0f, 0.45f, 0.1f};
                    push.material.emissive += vec4(kHighlight * item.highlight / std::max(push.material.flags.y, 1.0f), 0.0f);
                    push.material.flags.y = std::max(push.material.flags.y, 1.0f);
                }
                m_pipelines->pushConstants(ctx.cmd, p, &push, sizeof(push));
                drawMesh(ctx.cmd, *item.mesh, item.lod);
            }
        });
}

} // namespace eng
