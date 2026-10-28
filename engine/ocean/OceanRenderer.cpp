#include "ocean/OceanRenderer.h"

#include "core/JobSystem.h"
#include "core/Log.h"
#include "render/DefaultResources.h"
#include "render/FrameContext.h"
#include "render/RenderTypes.h"
#include "rhi/Descriptors.h"
#include "rhi/Device.h"
#include "rhi/Upload.h"

#include <chrono>
#include <cstring>
#include <string>

namespace eng::ocean {

// 必须与 shaders/ocean/ocean_common.hlsli 的 OceanSimPush 一致
struct OceanRenderer::SimPush {
    vec4 time;  // 时间、帧间隔、水深、尖锐度
    vec4 sizes; // 三个级联的尺寸
    vec4 foam;  // 起泡阈值、强度、本帧保留系数、FFT 方向
};

// 必须与 shaders/ocean/ocean_surface.hlsl 的 OceanSurfacePush 一致
struct OceanRenderer::SurfacePush {
    vec4 sizes;
    vec4 grid;
    vec4 gridDims;
    vec4 water;
    vec4 absorption;
    vec4 scatter;
    vec4 sss;
    vec4 foam;
};

namespace {

constexpr VkFormat kSpectrumFormat = VK_FORMAT_R32G32B32A32_SFLOAT; // FFT 需要 32 位精度（16 位会出现明显噪点）
constexpr VkFormat kDisplacementFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kLinearDepthFormat = VK_FORMAT_R32_SFLOAT;
constexpr uint32_t kWaveBufferSize = kMaxGerstnerWaves * 2 * sizeof(vec4);

// 频谱参数变了才需要重新生成 h0（着色参数直接走 push constant，不用重建）
bool spectrumChanged(const OceanSettings& a, const OceanSettings& b) {
    return a.windSpeed != b.windSpeed || a.windDirectionDeg != b.windDirectionDeg || a.fetchKm != b.fetchKm || a.depth != b.depth ||
           a.peakEnhancement != b.peakEnhancement || a.spread != b.spread || a.swell != b.swell || a.heightScale != b.heightScale ||
           a.choppiness != b.choppiness || a.cascadeSizes != b.cascadeSizes || a.gerstnerWaves != b.gerstnerWaves;
}

} // namespace

bool OceanRenderer::init(rhi::Device& device, PipelineCache& pipelines, JobSystem* jobs, VkFormat colorFormat, VkFormat depthFormat) {
    m_device = &device;
    m_pipelines = &pipelines;
    m_jobs = jobs;
    m_colorFormat = colorFormat;

    auto bind = [](uint32_t b, VkDescriptorType t, VkShaderStageFlags stages) {
        return VkDescriptorSetLayoutBinding{b, t, 1, stages, nullptr};
    };
    const VkDescriptorType sampled = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, storage = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    const VkShaderStageFlags cs = VK_SHADER_STAGE_COMPUTE_BIT;
    const VkShaderStageFlags vsps = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    m_evolveLayout = rhi::createSetLayout(device, {bind(0, sampled, cs), bind(1, storage, cs), bind(2, storage, cs)}, "OceanEvolveLayout");
    m_fftLayout = rhi::createSetLayout(device, {bind(0, storage, cs), bind(1, storage, cs), bind(2, storage, cs), bind(3, storage, cs)},
                                       "OceanFftLayout");
    m_assembleLayout = rhi::createSetLayout(
        device, {bind(0, storage, cs), bind(1, storage, cs), bind(2, sampled, cs), bind(3, storage, cs), bind(4, storage, cs)},
        "OceanAssembleLayout");
    m_prepareLayout = rhi::createSetLayout(device, {bind(0, sampled, VK_SHADER_STAGE_FRAGMENT_BIT), bind(1, sampled, VK_SHADER_STAGE_FRAGMENT_BIT)},
                                           "OceanPrepareLayout");
    m_surfaceLayout = rhi::createSetLayout(device,
                                           {bind(0, sampled, vsps), bind(1, sampled, VK_SHADER_STAGE_FRAGMENT_BIT),
                                            bind(2, sampled, VK_SHADER_STAGE_FRAGMENT_BIT), bind(3, sampled, VK_SHADER_STAGE_FRAGMENT_BIT),
                                            bind(4, sampled, VK_SHADER_STAGE_FRAGMENT_BIT), bind(5, sampled, VK_SHADER_STAGE_FRAGMENT_BIT),
                                            bind(6, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, vsps)},
                                           "OceanSurfaceLayout");

    m_evolve = pipelines.createCompute({"OceanEvolve", "ocean/ocean_evolve", m_evolveLayout});
    m_fft = pipelines.createCompute({"OceanFFT", "ocean/ocean_fft", m_fftLayout});
    m_assemble = pipelines.createCompute({"OceanAssemble", "ocean/ocean_assemble", m_assembleLayout});

    GraphicsPipelineDesc prep;
    prep.name = "OceanPrepare";
    prep.shader = "ocean/ocean_prepare";
    prep.state.colorFormats = {colorFormat, kLinearDepthFormat};
    prep.set1 = m_prepareLayout;
    m_prepare = pipelines.createGraphics(prep);

    GraphicsPipelineDesc surf;
    surf.name = "OceanSurface";
    surf.shader = "ocean/ocean_surface";
    surf.state.colorFormats = {colorFormat};
    surf.state.depthFormat = depthFormat;
    surf.state.depthTest = true;
    surf.state.depthWrite = true;
    surf.state.cullMode = VK_CULL_MODE_NONE; // 水下也要看得到水面
    surf.set1 = m_surfaceLayout;
    m_surface = pipelines.createGraphics(surf);
    surf.name = "OceanSurfaceWireframe";
    surf.state.polygonMode = VK_POLYGON_MODE_LINE;
    m_surfaceWire = pipelines.createGraphics(surf);

    for (uint32_t i = 0; i < rhi::kFramesInFlight; ++i)
        m_waveBuffers[i] = rhi::createBuffer(device, kWaveBufferSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, rhi::MemoryUsage::Upload,
                                             ("OceanWaves" + std::to_string(i)).c_str());
    rebuildGrid();
    return true;
}

void OceanRenderer::shutdown() {
    if (!m_device) return;
    rhi::destroyImage(*m_device, m_h0);
    rhi::destroyBuffer(*m_device, m_indexBuffer);
    for (rhi::Buffer& b : m_waveBuffers) rhi::destroyBuffer(*m_device, b);
    for (VkDescriptorSetLayout l : {m_evolveLayout, m_fftLayout, m_assembleLayout, m_prepareLayout, m_surfaceLayout})
        vkDestroyDescriptorSetLayout(m_device->device(), l, nullptr);
    m_device = nullptr;
}

void OceanRenderer::setSettings(const OceanSettings& s) {
    if (spectrumChanged(s, m_builtFrom)) m_spectrumDirty = true;
    const bool gridChanged = s.gridRings != m_settings.gridRings || s.gridSegments != m_settings.gridSegments;
    m_settings = s;
    if (gridChanged && m_device) rebuildGrid();
}

void OceanRenderer::rebuildGrid() {
    const int rings = std::max(m_settings.gridRings, 3), segments = std::max(m_settings.gridSegments, 8);
    std::vector<uint32_t> idx;
    idx.reserve(size_t(rings - 1) * segments * 6);
    const uint32_t stride = uint32_t(segments) + 1; // 每环多一个顶点（角度 2π 处），接缝处 UV 不跳变
    for (uint32_t r = 0; r + 1 < uint32_t(rings); ++r)
        for (uint32_t s = 0; s < uint32_t(segments); ++s) {
            const uint32_t a = r * stride + s, b = a + 1, c = a + stride, d = c + 1;
            // 正面朝上（逆时针）：角度从 +X 转向 +Z、半径向外，(径向 × 切向) 指向 -Y，所以要反过来排。
            // 绕序错了的话从上面看到的全是背面，PS 会把法线翻到水下 —— 整片海只剩菲涅尔反射、泡沫发褐
            idx.insert(idx.end(), {a, d, c, a, b, d});
        }
    if (m_indexBuffer) {
        auto old = std::make_shared<rhi::Buffer>(m_indexBuffer);
        m_device->deferDestroy([dev = m_device, old] { rhi::destroyBuffer(*dev, *old); });
    }
    m_indexBuffer = rhi::createBufferWithData(*m_device, idx.data(), idx.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                              "OceanGridIB");
    m_indexCount = uint32_t(idx.size());
    m_gridRings = rings;
    m_gridSegments = segments;
    m_stats.vertices = uint32_t(rings) * stride;
}

void OceanRenderer::rebuildSpectrum() {
    const auto t0 = std::chrono::steady_clock::now();
    const uint32_t n = kOceanFftSize;
    std::vector<vec4> all(size_t(n) * n * kOceanCascades);
    auto generate = [&](uint32_t begin, uint32_t end) {
        for (uint32_t c = begin; c < end; ++c) {
            const std::vector<vec4> h0 = generateCascadeSpectrum(m_settings, c, n, m_seed);
            std::copy(h0.begin(), h0.end(), all.begin() + ptrdiff_t(c) * n * n);
        }
    };
    if (m_jobs) m_jobs->parallelFor(kOceanCascades, 1, generate); // 三个级联互不相关，并行生成
    else generate(0, kOceanCascades);

    if (m_h0) {
        auto old = std::make_shared<rhi::Image>(m_h0);
        m_device->deferDestroy([dev = m_device, old] { rhi::destroyImage(*dev, *old); });
    }
    m_h0 = rhi::createTextureArray(*m_device, kSpectrumFormat, n, n, kOceanCascades, all.data(), sizeof(vec4), "OceanH0");
    m_waves = generateGerstnerWaves(m_settings, uint32_t(std::max(m_settings.gerstnerWaves, 1)), m_seed);

    const float wp = peakFrequency(m_settings);
    m_stats.significantWaveHeight = significantWaveHeight(m_settings);
    m_stats.peakWavelength = 2.0f * kPi * kGravity / (wp * wp); // 深水近似 λ = 2πg/ω²
    m_stats.peakPeriod = 2.0f * kPi / wp;
    m_stats.spectrumMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
    m_builtFrom = m_settings;
    m_spectrumDirty = false;
}

float OceanRenderer::heightAt(float x, float z, float time) const {
    return gerstnerHeightAt(m_waves, x, z, time);
}

void OceanRenderer::addTransparentPasses(rg::RenderGraph& graph, const SceneTargets& targets) {
    if (!m_settings.enabled || !targets.view || !m_device) return;
    if (m_spectrumDirty || !m_h0) rebuildSpectrum();
    ++m_frame;

    const RenderView& view = *targets.view;
    const OceanSettings& s = m_settings;
    const bool fft = s.mode == int(OceanMode::Fft);
    const uint32_t n = kOceanFftSize;

    SimPush sim;
    sim.time = vec4(float(view.time), view.dt, s.depth, s.choppiness);
    sim.sizes = s.cascadeSizes;
    sim.foam = vec4(s.foamBias, s.foamIntensity, std::exp(-view.dt / std::max(s.foamDecay, 0.01f)), 0.0f);

    rg::TextureHandle disp{}, deriv{};
    if (fft) {
        auto cascadeArray = [&](const char* name, VkFormat format, bool persistent) {
            rg::TextureDesc d;
            d.name = name;
            d.format = format;
            d.width = d.height = n;
            d.layers = kOceanCascades;
            d.persistent = persistent;
            return graph.createTexture(d);
        };
        const rg::TextureHandle spec0 = cascadeArray("OceanSpectrum0", kSpectrumFormat, false);
        const rg::TextureHandle spec1 = cascadeArray("OceanSpectrum1", kSpectrumFormat, false);
        const rg::TextureHandle tmp0 = cascadeArray("OceanFFTTemp0", kSpectrumFormat, false);
        const rg::TextureHandle tmp1 = cascadeArray("OceanFFTTemp1", kSpectrumFormat, false);
        // 位移贴图 A/B 交替：本帧写一张，读上一帧写的另一张里的泡沫（持久贴图，内容跨帧保留）
        const bool odd = (m_frame & 1) != 0;
        disp = cascadeArray(odd ? "OceanDisplacementA" : "OceanDisplacementB", kDisplacementFormat, true);
        const rg::TextureHandle prev = cascadeArray(odd ? "OceanDisplacementB" : "OceanDisplacementA", kDisplacementFormat, true);
        deriv = cascadeArray("OceanDerivatives", kDisplacementFormat, false);

        graph.addPass("OceanSpectrum", rg::PassType::Compute)
            .storageWrite(spec0)
            .storageWrite(spec1)
            .require([this] { return m_pipelines->ready(m_evolve); })
            .execute([this, spec0, spec1, sim, n](rg::PassContext& ctx) {
                m_pipelines->bind(ctx.cmd, m_evolve, ctx.frame->globalSet);
                VkDescriptorSet set = ctx.frame->descriptors->allocate(m_evolveLayout);
                rhi::DescriptorWriter().sampledImage(0, m_h0.view).storageImage(1, ctx.view(spec0)).storageImage(2, ctx.view(spec1)).update(ctx.frame->device, set);
                m_pipelines->bindSet1(ctx.cmd, m_evolve, set);
                m_pipelines->pushConstants(ctx.cmd, m_evolve, &sim, sizeof(sim));
                vkCmdDispatch(ctx.cmd, n / 8, n / 8, kOceanCascades);
            });

        auto fftPass = [&](const char* name, rg::TextureHandle in0, rg::TextureHandle in1, rg::TextureHandle out0, rg::TextureHandle out1,
                           float direction) {
            SimPush p = sim;
            p.foam.w = direction;
            graph.addPass(name, rg::PassType::Compute)
                .storageRead(in0)
                .storageRead(in1)
                .storageWrite(out0)
                .storageWrite(out1)
                .require([this] { return m_pipelines->ready(m_fft); })
                .execute([this, in0, in1, out0, out1, p, n](rg::PassContext& ctx) {
                    // 上游被关掉时输入是默认贴图，不能当 storage image 绑定：直接跳过，下游同样会跳过
                    if (!ctx.hasContent(in0) || !ctx.hasContent(in1)) return;
                    m_pipelines->bind(ctx.cmd, m_fft, ctx.frame->globalSet);
                    VkDescriptorSet set = ctx.frame->descriptors->allocate(m_fftLayout);
                    rhi::DescriptorWriter()
                        .storageImage(0, ctx.view(in0))
                        .storageImage(1, ctx.view(in1))
                        .storageImage(2, ctx.view(out0))
                        .storageImage(3, ctx.view(out1))
                        .update(ctx.frame->device, set);
                    m_pipelines->bindSet1(ctx.cmd, m_fft, set);
                    m_pipelines->pushConstants(ctx.cmd, m_fft, &p, sizeof(p));
                    vkCmdDispatch(ctx.cmd, 1, n, kOceanCascades); // 每行（列）一个线程组
                });
        };
        fftPass("OceanFFTRows", spec0, spec1, tmp0, tmp1, 0.0f);
        fftPass("OceanFFTColumns", tmp0, tmp1, spec0, spec1, 1.0f);

        graph.addPass("OceanAssemble", rg::PassType::Compute)
            .storageRead(spec0)
            .storageRead(spec1)
            .sampled(prev)
            .storageWrite(disp)
            .storageWrite(deriv)
            .require([this] { return m_pipelines->ready(m_assemble); })
            .execute([this, spec0, spec1, prev, disp, deriv, sim, n](rg::PassContext& ctx) {
                if (!ctx.hasContent(spec0) || !ctx.hasContent(spec1)) return;
                m_pipelines->bind(ctx.cmd, m_assemble, ctx.frame->globalSet);
                VkDescriptorSet set = ctx.frame->descriptors->allocate(m_assembleLayout);
                rhi::DescriptorWriter()
                    .storageImage(0, ctx.view(spec0))
                    .storageImage(1, ctx.view(spec1))
                    .sampledImage(2, ctx.view(prev)) // 第一帧没有上一帧：默认黑色 = 没有泡沫
                    .storageImage(3, ctx.view(disp))
                    .storageImage(4, ctx.view(deriv))
                    .update(ctx.frame->device, set);
                m_pipelines->bindSet1(ctx.cmd, m_assemble, set);
                m_pipelines->pushConstants(ctx.cmd, m_assemble, &sim, sizeof(sim));
                vkCmdDispatch(ctx.cmd, n / 8, n / 8, kOceanCascades);
            });
    }

    // ---- 场景颜色/深度的拷贝（折射与水深） ----
    rg::TextureDesc cd;
    cd.name = "OceanSceneColor";
    cd.format = m_colorFormat;
    cd.width = targets.sceneExtent.width;
    cd.height = targets.sceneExtent.height;
    const rg::TextureHandle colorCopy = graph.createTexture(cd);
    rg::TextureDesc dd = cd;
    dd.name = "OceanSceneDepth";
    dd.format = kLinearDepthFormat;
    dd.fallback = rg::Fallback::White; // 没有深度信息时当作「很远」：整片按深水处理
    const rg::TextureHandle depthLinear = graph.createTexture(dd);

    const float nearPlane = view.nearPlane;
    graph.addPass("OceanPrepare", rg::PassType::Graphics)
        .sampled(targets.color)
        .sampled(targets.depth)
        .color(colorCopy, rg::LoadOp::DontCare)
        .color(depthLinear, rg::LoadOp::DontCare)
        .require([this] { return m_pipelines->ready(m_prepare); })
        .execute([this, targets, nearPlane](rg::PassContext& ctx) {
            m_pipelines->bind(ctx.cmd, m_prepare, ctx.frame->globalSet);
            VkDescriptorSet set = ctx.frame->descriptors->allocate(m_prepareLayout);
            rhi::DescriptorWriter().sampledImage(0, ctx.view(targets.color)).sampledImage(1, ctx.view(targets.depth)).update(ctx.frame->device, set);
            m_pipelines->bindSet1(ctx.cmd, m_prepare, set);
            const vec4 push(nearPlane, 0.0f, 0.0f, 0.0f);
            m_pipelines->pushConstants(ctx.cmd, m_prepare, &push, sizeof(push));
            vkCmdDraw(ctx.cmd, 3, 1, 0, 0);
        });

    // ---- 海面 ----
    SurfacePush sp;
    sp.sizes = vec4(vec3(s.cascadeSizes), fft ? 1.0f : 0.0f);
    sp.grid = vec4(view.position.x, view.position.z, std::max(s.gridInner, 0.01f), std::max(s.gridOuter, s.gridInner + 1.0f));
    sp.gridDims = vec4(float(m_gridRings), float(m_gridSegments), float(m_waves.size()), m_stats.significantWaveHeight);
    sp.water = vec4(s.refraction, s.roughness, s.sssStrength, s.shoreFoamWidth);
    sp.absorption = vec4(s.absorption, s.reflection);
    sp.scatter = vec4(s.scatterColor, std::max(s.foamTiling, 0.1f));
    sp.sss = vec4(s.sssColor, float(view.time));
    sp.foam = vec4(s.foamBias, s.foamIntensity, s.choppiness, 0.0f);

    auto pass = graph.addPass("Ocean", rg::PassType::Graphics);
    pass.color(targets.color, rg::LoadOp::Load)
        .depth(targets.depth, rg::LoadOp::Load)
        .sampled(colorCopy)
        .sampled(depthLinear)
        .sampled(targets.shadowAtlas);
    if (fft) pass.sampled(disp).sampled(deriv);
    pass.require([this] { return m_pipelines->ready(wireframe ? m_surfaceWire : m_surface); })
        .execute([this, targets, colorCopy, depthLinear, disp, deriv, fft, sp](rg::PassContext& ctx) {
            const PipelineHandle p = wireframe ? m_surfaceWire : m_surface;
            const DefaultResources& def = *ctx.frame->defaults;
            // Gerstner 波参数：每个在途帧一份缓冲，写本帧的那份
            const rhi::Buffer& waves = m_waveBuffers[ctx.frame->slot];
            std::vector<vec4> data(kMaxGerstnerWaves * 2, vec4(0.0f));
            for (size_t i = 0; i < m_waves.size() && i < kMaxGerstnerWaves; ++i) {
                const GerstnerWave& w = m_waves[i];
                data[i] = vec4(w.direction, w.k, w.omega);
                data[kMaxGerstnerWaves + i] = vec4(w.amplitude, w.phase, w.steepness, 0.0f);
            }
            std::memcpy(waves.mapped, data.data(), kWaveBufferSize);
            vmaFlushAllocation(m_device->allocator(), waves.allocation, 0, VK_WHOLE_SIZE);

            m_pipelines->bind(ctx.cmd, p, ctx.frame->globalSet);
            VkDescriptorSet set = ctx.frame->descriptors->allocate(m_surfaceLayout);
            rhi::DescriptorWriter()
                .sampledImage(0, fft ? ctx.view(disp) : def.blackArray().view)
                .sampledImage(1, fft ? ctx.view(deriv) : def.blackArray().view)
                .sampledImage(2, ctx.view(colorCopy))
                .sampledImage(3, ctx.view(depthLinear))
                .sampledImage(4, ctx.view(targets.shadowAtlas))
                .sampledImage(5, m_foamTex ? m_foamTex->view : def.white().view)
                .uniformBuffer(6, waves.buffer, 0, kWaveBufferSize)
                .update(ctx.frame->device, set);
            m_pipelines->bindSet1(ctx.cmd, p, set);
            m_pipelines->pushConstants(ctx.cmd, p, &sp, sizeof(sp));
            vkCmdBindIndexBuffer(ctx.cmd, m_indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(ctx.cmd, m_indexCount, 1, 0, 0, 0);
        });
}

} // namespace eng::ocean
