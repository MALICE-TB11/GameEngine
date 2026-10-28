#include "terrain/TerrainRenderer.h"

#include "core/Log.h"
#include "render/DefaultResources.h"
#include "render/FrameContext.h"
#include "render/RenderTypes.h"
#include "render/passes/ShadowPass.h"
#include "rhi/Descriptors.h"
#include "rhi/Device.h"
#include "rhi/Upload.h"

namespace eng::terrain {

// 必须与 shaders/terrain/terrain_common.hlsli 的 TerrainPush 一致
struct TerrainRenderer::Push {
    vec4 node;
    vec4 terrain;
    vec4 layers;
    vec4 tiling;
    vec4 misc;
    vec4 roughness;
};

namespace {

// 网格块的索引缓冲：(N+1)² 个网格顶点 + 4 条边 × (N+1) 个裙边顶点（顶点本身由 VS 根据序号生成）
std::vector<uint32_t> buildPatchIndices() {
    const uint32_t n = kPatchQuads, n1 = n + 1;
    std::vector<uint32_t> idx;
    idx.reserve(size_t(n) * n * 6 + size_t(4) * n * 6);
    // 每个格子沿 a(x,z) → d(x+1,z+1) 的对角线切成两个三角形 —— 与 Jolt 高度场、Heightfield::heightAt
    // 完全相同的切法。四个角高度不共面时，两种对角线给出的表面不一样（差可达几厘米），
    // 切法不一致就会出现「脚底悬空 / 陷进地里」。逆时针为正面（从 +Y 往下看）。
    for (uint32_t z = 0; z < n; ++z)
        for (uint32_t x = 0; x < n; ++x) {
            const uint32_t a = z * n1 + x, b = a + 1, c = a + n1, d = c + 1;
            idx.insert(idx.end(), {a, c, d, a, d, b});
        }

    // 裙边：每条边上「网格顶点 → 正下方的裙边顶点」组成一排竖直四边形，正面朝外。
    // 四条边的遍历方向不同，绕序用叉积判断一下，比手写四套索引不容易出错
    auto gridPos = [&](uint32_t edge, uint32_t i) {
        const uint32_t gx = edge == 0 ? i : (edge == 1 ? n : (edge == 2 ? i : 0));
        const uint32_t gz = edge == 0 ? 0 : (edge == 1 ? i : (edge == 2 ? n : i));
        return uvec2(gx, gz);
    };
    const vec3 outward[4] = {{0, 0, -1}, {1, 0, 0}, {0, 0, 1}, {-1, 0, 0}};
    for (uint32_t edge = 0; edge < 4; ++edge)
        for (uint32_t i = 0; i < n; ++i) {
            const uvec2 g0 = gridPos(edge, i), g1 = gridPos(edge, i + 1);
            const uint32_t t0 = g0.y * n1 + g0.x, t1 = g1.y * n1 + g1.x; // 边上的网格顶点
            const uint32_t s0 = n1 * n1 + edge * n1 + i, s1 = s0 + 1;     // 对应的裙边顶点
            const vec3 p0(float(g0.x), 0.0f, float(g0.y)), p1(float(g1.x), 0.0f, float(g1.y)), q0 = p0 - vec3(0, 1, 0);
            const bool flip = glm::dot(glm::cross(p1 - p0, q0 - p0), outward[edge]) < 0.0f;
            if (!flip) idx.insert(idx.end(), {t0, t1, s0, t1, s1, s0});
            else idx.insert(idx.end(), {t0, s0, t1, t1, s0, s1});
        }
    return idx;
}

} // namespace

bool TerrainRenderer::init(rhi::Device& device, PipelineCache& pipelines, asset::AssetManager& assets, VkFormat colorFormat,
                           VkFormat depthFormat) {
    m_device = &device;
    m_pipelines = &pipelines;
    m_assets = &assets;

    auto tex = [](uint32_t b, VkShaderStageFlags stages) {
        return VkDescriptorSetLayoutBinding{b, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, stages, nullptr};
    };
    const VkShaderStageFlags ps = VK_SHADER_STAGE_FRAGMENT_BIT;
    m_setLayout = rhi::createSetLayout(device,
                                       {tex(0, VK_SHADER_STAGE_VERTEX_BIT), tex(1, ps), tex(2, ps), tex(3, ps), tex(4, ps), tex(5, ps),
                                        tex(6, ps), tex(7, ps), tex(8, ps), tex(9, ps), tex(10, ps)},
                                       "TerrainSetLayout");
    m_shadowLayout = rhi::createSetLayout(device, {tex(0, VK_SHADER_STAGE_VERTEX_BIT)}, "TerrainShadowSetLayout");

    GraphicsPipelineDesc d;
    d.name = "Terrain";
    d.shader = "terrain/terrain";
    d.state.colorFormats = {colorFormat};
    d.state.depthFormat = depthFormat;
    d.state.depthTest = true;
    d.state.depthWrite = true;
    d.state.cullMode = VK_CULL_MODE_BACK_BIT;
    d.set1 = m_setLayout;
    m_pipeline = pipelines.createGraphics(d);
    d.name = "TerrainWireframe";
    d.state.polygonMode = VK_POLYGON_MODE_LINE;
    m_wirePipeline = pipelines.createGraphics(d);

    GraphicsPipelineDesc s;
    s.name = "TerrainShadow";
    s.shader = "terrain/terrain_shadow";
    s.state.depthFormat = depthFormat;
    s.state.depthTest = true;
    s.state.depthWrite = true;
    s.state.cullMode = VK_CULL_MODE_NONE; // 与网格阴影一致：不剔除，痤疮交给法线偏移
    s.set1 = m_shadowLayout;
    m_shadowPipeline = pipelines.createGraphics(s);

    const std::vector<uint32_t> indices = buildPatchIndices();
    m_indexCount = uint32_t(indices.size());
    m_indexBuffer = rhi::createBufferWithData(device, indices.data(), indices.size() * sizeof(uint32_t),
                                              VK_BUFFER_USAGE_INDEX_BUFFER_BIT, "TerrainPatchIB");
    return true;
}

void TerrainRenderer::shutdown() {
    if (!m_device) return;
    rhi::destroyBuffer(*m_device, m_indexBuffer);
    rhi::destroyImage(*m_device, m_heightTex);
    rhi::destroyImage(*m_device, m_normalTex);
    vkDestroyDescriptorSetLayout(m_device->device(), m_setLayout, nullptr);
    vkDestroyDescriptorSetLayout(m_device->device(), m_shadowLayout, nullptr);
    m_device = nullptr;
}

bool TerrainRenderer::setHeightfield(const Heightfield& hf) {
    if (!m_quadtree.build(hf, kPatchQuads)) return false;
    const uint32_t res = hf.resolution();
    m_resolution = res;
    m_spacing = hf.spacing();
    m_origin = hf.origin();

    // 法线：对相邻采样做中心差分。和 Heightfield::normalAt 同样的公式，只是直接按采样下标算
    std::vector<uint32_t> normals(size_t(res) * res);
    const float inv2s = 1.0f / (2.0f * m_spacing);
    for (uint32_t z = 0; z < res; ++z)
        for (uint32_t x = 0; x < res; ++x) {
            const float dx = (hf.sample(std::min(x + 1, res - 1), z) - hf.sample(x > 0 ? x - 1 : 0, z)) * inv2s;
            const float dz = (hf.sample(x, std::min(z + 1, res - 1)) - hf.sample(x, z > 0 ? z - 1 : 0)) * inv2s;
            const vec3 n = glm::normalize(vec3(-dx, 1.0f, -dz)) * 0.5f + 0.5f;
            normals[size_t(z) * res + x] = uint32_t(n.x * 255.0f + 0.5f) | (uint32_t(n.y * 255.0f + 0.5f) << 8) |
                                           (uint32_t(n.z * 255.0f + 0.5f) << 16) | (255u << 24);
        }

    // 旧贴图可能还在被在途的帧使用：延迟销毁
    if (m_heightTex) {
        auto oldH = std::make_shared<rhi::Image>(m_heightTex), oldN = std::make_shared<rhi::Image>(m_normalTex);
        m_device->deferDestroy([dev = m_device, oldH, oldN] {
            rhi::destroyImage(*dev, *oldH);
            rhi::destroyImage(*dev, *oldN);
        });
    }
    // R32F 每纹素 4 字节，与 RGBA8 相同，可以直接复用 createTexture2D 的上传路径（不生成 mip）
    m_heightTex = rhi::createTexture2D(*m_device, hf.samples().data(), res, res, VK_FORMAT_R32_SFLOAT, false, "TerrainHeight");
    m_normalTex = rhi::createTexture2D(*m_device, normals.data(), res, res, VK_FORMAT_R8G8B8A8_UNORM, true, "TerrainNormal");
    LOG_INFO("地形已上传：{}² 采样，间距 {:.2f} m，高度 {:.1f} ~ {:.1f} m，四叉树 {} 级", res, m_spacing, hf.minHeight(),
             hf.maxHeight(), m_quadtree.maxLevel() + 1);
    return true;
}

TerrainRenderer::Push TerrainRenderer::makePush(const TerrainNode& node, uint32_t cascade) const {
    static_assert(sizeof(Push) == 96, "TerrainPush 布局必须与 HLSL 一致");
    const TerrainSettings& s = m_settings;
    Push p;
    p.node = vec4(node.origin, node.size, float(node.level));
    p.terrain = vec4(m_origin, m_spacing, float(m_resolution));
    p.layers = vec4(s.sandMaxHeight, s.reefMaxHeight, s.rockSlope, s.mossMinHeight);
    p.tiling = glm::max(s.layerTiling, vec4(0.01f)); // shader 里做除数
    p.misc = vec4(s.blendSharpness, s.skirtDepth, float(cascade), showLod ? 1.0f : 0.0f);
    p.roughness = s.layerRoughness;
    return p;
}

VkDescriptorSet TerrainRenderer::materialSet(rg::PassContext& ctx, rg::TextureHandle shadowAtlas) {
    const DefaultResources& def = *ctx.frame->defaults;
    // 分层贴图还没加载完（或加载失败）时用默认贴图：白色 + 平坦法线，地形照样能画
    auto layer = [&](asset::TextureHandle h, const rhi::Image& fallback) {
        const rhi::Image* img = m_assets->texture(h);
        return img ? img->view : fallback.view;
    };
    VkDescriptorSet set = ctx.frame->descriptors->allocate(m_setLayout);
    rhi::DescriptorWriter w;
    w.sampledImage(0, m_heightTex.view).sampledImage(1, m_normalTex.view);
    for (uint32_t i = 0; i < 4; ++i) {
        w.sampledImage(2 + i, layer(m_layers.albedo[i], def.white()));
        w.sampledImage(6 + i, layer(m_layers.normal[i], def.flatNormal()));
    }
    w.sampledImage(10, ctx.view(shadowAtlas));
    w.update(ctx.frame->device, set);
    return set;
}

void TerrainRenderer::addOpaquePasses(rg::RenderGraph& graph, const SceneTargets& targets) {
    m_stats.nodes = 0;
    m_stats.triangles = 0;
    m_stats.perLevel.fill(0);
    if (!enabled || !m_quadtree.valid() || !targets.view) return;

    const RenderView& view = *targets.view;
    const Frustum frustum = Frustum::fromViewProj(view.proj * view.view);
    m_quadtree.select(view.position, m_settings.lodDistanceFactor, &frustum, 0.0f, m_nodes);
    m_stats.nodes = uint32_t(m_nodes.size());
    m_stats.triangles = uint64_t(m_nodes.size()) * (m_indexCount / 3);
    for (const TerrainNode& n : m_nodes) ++m_stats.perLevel[std::min<uint32_t>(n.level, 7)];

    const rg::TextureHandle atlas = targets.shadowAtlas;
    graph.addPass("Terrain", rg::PassType::Graphics)
        .color(targets.color, rg::LoadOp::Load) // 第一个写入者：图会自动把 LOAD 换成 CLEAR
        .depth(targets.depth, rg::LoadOp::Load)
        .sampled(atlas)
        .require([this] { return m_pipelines->ready(wireframe ? m_wirePipeline : m_pipeline); })
        .execute([this, atlas](rg::PassContext& ctx) {
            const PipelineHandle p = wireframe ? m_wirePipeline : m_pipeline;
            m_pipelines->bind(ctx.cmd, p, ctx.frame->globalSet);
            m_pipelines->bindSet1(ctx.cmd, p, materialSet(ctx, atlas));
            vkCmdBindIndexBuffer(ctx.cmd, m_indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
            for (const TerrainNode& n : m_nodes) {
                const Push push = makePush(n, 0);
                m_pipelines->pushConstants(ctx.cmd, p, &push, sizeof(push));
                vkCmdDrawIndexed(ctx.cmd, m_indexCount, 1, 0, 0, 0);
            }
        });
}

void TerrainRenderer::addShadowPasses(rg::RenderGraph& graph, const SceneTargets& targets) {
    m_stats.shadowNodes = 0;
    if (!enabled || !castShadows || !m_quadtree.valid() || !targets.view || !targets.cascades || !targets.shadowSettings) return;

    // 阴影不做相机视锥剔除（相机背后的山也会把影子投进画面），但只取阴影距离附近、且更粗的 LOD
    const ShadowSettings& ss = *targets.shadowSettings;
    m_quadtree.select(targets.view->position, m_settings.lodDistanceFactor * 0.5f, nullptr, ss.maxDistance + ss.casterExtent * 0.5f,
                      m_shadowNodes);

    const ShadowCascades* cascades = targets.cascades;
    graph.addPass("TerrainShadows", rg::PassType::Graphics)
        .depth(targets.shadowAtlas, rg::LoadOp::Load) // 叠加在网格阴影之上
        .require([this] { return m_pipelines->ready(m_shadowPipeline); })
        .execute([this, cascades](rg::PassContext& ctx) {
            m_pipelines->bind(ctx.cmd, m_shadowPipeline, ctx.frame->globalSet);
            VkDescriptorSet set = ctx.frame->descriptors->allocate(m_shadowLayout);
            rhi::DescriptorWriter().sampledImage(0, m_heightTex.view).update(ctx.frame->device, set);
            m_pipelines->bindSet1(ctx.cmd, m_shadowPipeline, set);
            vkCmdBindIndexBuffer(ctx.cmd, m_indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
            for (uint32_t c = 0; c < kShadowCascades; ++c) {
                ShadowPass::setCascadeViewport(ctx.cmd, c, ctx.extent.width);
                // 每个级联只画落在它正交视锥里的节点：光源空间里 8 个角全在 [-1,1]² 某一侧之外就跳过
                const Frustum f = Frustum::fromViewProj(cascades->viewProj[c]);
                for (const TerrainNode& n : m_shadowNodes) {
                    const vec3 bmin{n.origin.x, n.minY, n.origin.y}, bmax{n.origin.x + n.size, n.maxY, n.origin.y + n.size};
                    if (!f.intersectsAabb(bmin, bmax)) continue;
                    const Push push = makePush(n, c);
                    m_pipelines->pushConstants(ctx.cmd, m_shadowPipeline, &push, sizeof(push));
                    vkCmdDrawIndexed(ctx.cmd, m_indexCount, 1, 0, 0, 0);
                    ++m_stats.shadowNodes;
                }
            }
        });
}

} // namespace eng::terrain
