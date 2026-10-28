#pragma once
// 前向不透明 pass：把所有不透明网格画进 HDR 场景颜色 + 深度。
//
// 每个 draw：push constant = 模型矩阵 + 材质参数；set 1 = 材质的 5 张贴图（缺失的用默认贴图）。
// 容错：材质对应的管线没准备好（shader 编译失败）→ 改用错误管线（品红条纹）绘制。

#include "render/Material.h"
#include "render/PipelineCache.h"
#include "render/RenderGraph.h"
#include "render/RenderTypes.h"

namespace eng {

namespace rhi { class Device; }

class ForwardPass {
public:
    void init(rhi::Device& device, PipelineCache& pipelines, VkFormat colorFormat, VkFormat depthFormat);
    void shutdown();
    // color/depth 用 LOAD：地形等特性 pass 先画（并负责清屏）；没有特性时 Render Graph 自动把 LOAD 改成 CLEAR
    void add(rg::RenderGraph& graph, rg::TextureHandle color, rg::TextureHandle depth, rg::TextureHandle shadowAtlas,
             const DrawList& draws);
    VkDescriptorSetLayout materialSetLayout() const { return m_setLayout; }

    bool wireframe = false;
    Material defaultMaterial; // DrawItem::material 为空时使用

private:
    PipelineHandle pipelineFor(const Material& m) const;

    rhi::Device* m_device = nullptr;
    PipelineCache* m_pipelines = nullptr;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    PipelineHandle m_pbr = kInvalidPipeline;
    PipelineHandle m_pbrDoubleSided = kInvalidPipeline;
    PipelineHandle m_wire = kInvalidPipeline;
    PipelineHandle m_error = kInvalidPipeline;
};

} // namespace eng
