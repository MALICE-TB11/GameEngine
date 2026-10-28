#pragma once
// 地形渲染：作为 RenderFeature 挂进渲染器（阴影阶段 + 不透明阶段）。
//
// GPU 资源：
//   高度贴图  R32F，1025²，每个纹素一个高度采样（VS 里 Load 精确读取）
//   法线贴图  RGBA8 + mip，由 CPU 从高度场算出（PS 采样，远处的粗网格也有完整的光照细节）
//   网格块    只有一个索引缓冲（32×32 格 + 裙边），顶点由 SV_VertexID 生成，所有节点共用
//   分层贴图  沙/岩/礁/苔 各一张颜色（alpha = 高度）+ 一张法线，经资源层加载（缺失时用默认贴图）
//
// 每帧：四叉树选节点（视锥剔除 + 距离 LOD）→ 每个节点一次 drawIndexed，push constant 告诉 VS 节点的位置与尺寸。
// EXTEND: 节点很多时改成实例化绘制（节点参数放进存储缓冲）或 GPU 驱动的四叉树遍历。

#include "asset/AssetManager.h"
#include "render/PipelineCache.h"
#include "render/RenderFeature.h"
#include "terrain/Heightfield.h"
#include "terrain/TerrainQuadtree.h"

#include <array>

namespace eng::terrain {

// 每个网格块每边的格数。必须与 shaders/terrain/terrain_common.hlsli 的 kPatchQuads 一致
inline constexpr uint32_t kPatchQuads = 32;

// 分层材质的贴图：0 沙、1 岩、2 礁、3 苔
struct TerrainLayerTextures {
    std::array<asset::TextureHandle, 4> albedo{};
    std::array<asset::TextureHandle, 4> normal{};
};

struct TerrainStats {
    uint32_t nodes = 0;
    uint32_t shadowNodes = 0; // 四个级联加起来实际画的节点数
    uint64_t triangles = 0;
    std::array<uint32_t, 8> perLevel{}; // 每个四叉树级别选中了几个节点
};

class TerrainRenderer final : public RenderFeature {
public:
    bool init(rhi::Device& device, PipelineCache& pipelines, asset::AssetManager& assets, VkFormat colorFormat, VkFormat depthFormat);
    void shutdown();

    // 上传高度场并重建四叉树（生成或重新生成地形后调用）
    bool setHeightfield(const Heightfield& hf);
    void setSettings(const TerrainSettings& s) { m_settings = s; }
    void setLayerTextures(const TerrainLayerTextures& t) { m_layers = t; }

    const char* name() const override { return "Terrain"; }
    void addShadowPasses(rg::RenderGraph& graph, const SceneTargets& targets) override;
    void addOpaquePasses(rg::RenderGraph& graph, const SceneTargets& targets) override;

    bool enabled = true;
    bool castShadows = true;
    bool wireframe = false;
    bool showLod = false; // 按四叉树级别着色（调 LOD 参数时很直观）
    const TerrainStats& stats() const { return m_stats; }

private:
    struct Push;
    Push makePush(const TerrainNode& node, uint32_t cascade) const;
    VkDescriptorSet materialSet(rg::PassContext& ctx, rg::TextureHandle shadowAtlas);

    rhi::Device* m_device = nullptr;
    PipelineCache* m_pipelines = nullptr;
    asset::AssetManager* m_assets = nullptr;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;    // 主 pass：高度、法线、8 张分层贴图、阴影图集
    VkDescriptorSetLayout m_shadowLayout = VK_NULL_HANDLE; // 阴影 pass：只有高度
    PipelineHandle m_pipeline = kInvalidPipeline;
    PipelineHandle m_wirePipeline = kInvalidPipeline;
    PipelineHandle m_shadowPipeline = kInvalidPipeline;

    rhi::Buffer m_indexBuffer;
    uint32_t m_indexCount = 0;
    rhi::Image m_heightTex;
    rhi::Image m_normalTex;
    uint32_t m_resolution = 0;
    float m_spacing = 1.0f;
    vec2 m_origin{0.0f};

    TerrainSettings m_settings;
    TerrainLayerTextures m_layers;
    TerrainQuadtree m_quadtree;
    std::vector<TerrainNode> m_nodes;
    std::vector<TerrainNode> m_shadowNodes;
    TerrainStats m_stats;
};

} // namespace eng::terrain
