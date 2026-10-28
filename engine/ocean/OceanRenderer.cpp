#include "ocean/OceanRenderer.h"

#include "core/JobSystem.h"
#include "core/Log.h"
#include "render/DefaultResources.h"
#include "render/FrameContext.h"
#include "render/RenderTypes.h"
#include "rhi/Descriptors.h"
#include "rhi/Device.h"
#include "rhi/Upload.h"

#include <glm/gtc/packing.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <random>
#include <string>

namespace eng::ocean {

// 必须与 shaders/ocean/ocean_common.hlsli 的 OceanSimPush 一致
struct OceanRenderer::SimPush {
    vec4 time;  // 相位时间、-、-、尖锐度
    vec4 sizes; // 三个级联的尺寸
    vec4 foam;  // 起泡阈值、强度、本帧保留系数、有没有上一帧泡沫
    vec4 fft;   // FFT 方向
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

// 后台正在生成的一份频谱
struct OceanRenderer::PendingSpectrum {
    std::shared_ptr<OceanSpectrumData> data;
    std::shared_ptr<JobCounter> counter;
    std::chrono::steady_clock::time_point start;
    std::atomic<bool> failed{false};
};

namespace {

constexpr VkFormat kSpectrumFormat = VK_FORMAT_R32G32B32A32_SFLOAT; // FFT 需要 32 位精度（16 位会出现明显噪点）
constexpr VkFormat kOmegaFormat = VK_FORMAT_R32_SFLOAT;
constexpr VkFormat kDisplacementFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kFoamFormat = VK_FORMAT_R32_SFLOAT; // r32f 是 storage image 的基础格式，不需要额外的设备特性
constexpr VkFormat kLinearDepthFormat = VK_FORMAT_R32_SFLOAT;
constexpr uint32_t kWaveBufferSize = kMaxGerstnerWaves * 2 * sizeof(vec4);
// 着色器的标志位（ocean_surface.hlsl 的 OCEAN_FLAG_*）
constexpr uint32_t kFlagFft = 1, kFlagScene = 2;
// 岸边泡沫「一推一退」的角频率（rad/s）：纯视觉参数。相位在 CPU 用 double 累计后取模再传，长时间运行不失精度
constexpr double kShoreSurfFrequency = 1.3;

// 频谱参数变了才需要重新生成 h0（着色参数直接走 push constant，不用重建）
bool spectrumChanged(const OceanSettings& a, const OceanSettings& b) {
    return a.windSpeed != b.windSpeed || a.windDirectionDeg != b.windDirectionDeg || a.fetchKm != b.fetchKm || a.depth != b.depth ||
           a.peakEnhancement != b.peakEnhancement || a.spread != b.spread || a.swell != b.swell || a.heightScale != b.heightScale ||
           a.choppiness != b.choppiness || a.cascadeSizes != b.cascadeSizes || a.gerstnerWaves != b.gerstnerWaves;
}

float msSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
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
    m_evolveLayout = rhi::createSetLayout(device, {bind(0, sampled, cs), bind(1, storage, cs), bind(2, storage, cs), bind(3, sampled, cs)},
                                          "OceanEvolveLayout");
    m_fftLayout = rhi::createSetLayout(device, {bind(0, storage, cs), bind(1, storage, cs), bind(2, storage, cs), bind(3, storage, cs)},
                                       "OceanFftLayout");
    m_assembleLayout = rhi::createSetLayout(device,
                                            {bind(0, storage, cs), bind(1, storage, cs), bind(2, sampled, cs), bind(3, storage, cs),
                                             bind(4, storage, cs), bind(5, storage, cs)},
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
    // 后台任务只写它自己持有的数据（shared_ptr），不碰本对象；等它结束只是为了不让线程池在关机时还有活
    if (m_pending && m_jobs) m_jobs->wait(*m_pending->counter);
    m_pending.reset();
    rhi::destroyImage(*m_device, m_h0);
    rhi::destroyImage(*m_device, m_omega);
    rhi::destroyBuffer(*m_device, m_indexBuffer);
    rhi::destroyBuffer(*m_device, m_readbackBuffer);
    for (rhi::Buffer& b : m_waveBuffers) rhi::destroyBuffer(*m_device, b);
    for (VkDescriptorSetLayout l : {m_evolveLayout, m_fftLayout, m_assembleLayout, m_prepareLayout, m_surfaceLayout})
        vkDestroyDescriptorSetLayout(m_device->device(), l, nullptr);
    m_device = nullptr;
}

void OceanRenderer::setSettings(const OceanSettings& s) {
    if (spectrumChanged(s, m_requestedFrom)) m_spectrumDirty = true;
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

// ---------------------------------------------------------------- 频谱：后台生成、主线程上传

void OceanRenderer::launchSpectrumJob() {
    auto job = std::make_shared<PendingSpectrum>();
    job->data = std::make_shared<OceanSpectrumData>();
    job->data->settings = m_settings;
    job->data->seed = m_seed;
    const size_t cells = size_t(kOceanFftSize) * kOceanFftSize;
    job->data->h0.resize(cells * kOceanCascades);
    job->data->omega.resize(cells * kOceanCascades);
    job->counter = std::make_shared<JobCounter>();
    job->start = std::chrono::steady_clock::now();
    m_requestedFrom = m_settings;
    m_spectrumDirty = false;

    // 每个级联一个任务、Gerstner 波和统计量一个任务：各写各的区间，不需要加锁。
    // 任务只捕获 job（shared_ptr），不捕获 this：就算本对象先没了，任务也只是写一份没人要的数据
    auto work = [job](uint32_t task) {
        try {
            OceanSpectrumData& d = *job->data;
            const uint32_t n = kOceanFftSize;
            if (task < kOceanCascades) {
                const std::vector<vec4> h0 = generateCascadeSpectrum(d.settings, task, n, d.seed);
                const std::vector<float> omega = cascadeFrequencies(d.settings, task, n);
                std::copy(h0.begin(), h0.end(), d.h0.begin() + ptrdiff_t(task) * n * n);
                std::copy(omega.begin(), omega.end(), d.omega.begin() + ptrdiff_t(task) * n * n);
            } else {
                d.waves = generateGerstnerWaves(d.settings, uint32_t(std::max(d.settings.gerstnerWaves, 1)), d.seed);
                d.significantWaveHeight = significantWaveHeight(d.settings);
            }
        } catch (const std::exception& e) {
            // 在任务自己的边界把异常变成「失败」：主线程看到后保留旧频谱
            LOG_ERROR("海洋频谱生成异常: {}", e.what());
            job->failed = true;
        }
    };
    for (uint32_t t = 0; t <= kOceanCascades; ++t) {
        if (m_jobs) m_jobs->submit([work, t] { work(t); }, job->counter);
        else work(t);
    }
    m_pending = std::move(job);
}

void OceanRenderer::updateSpectrum() {
    // 1. 参数变了、而且没有正在生成的任务 → 发起一个。正在生成时参数又变了：只记下 dirty，
    //    等这个完成后再发 —— 拖动滑条时中间的很多组参数被合并掉，后台同一时间最多一个任务
    if (m_spectrumDirty && !m_pending) launchSpectrumJob();
    if (!m_pending) return;
    // 2. 启动时还没有任何频谱：只能等（只发生一次），否则头几帧没有海面
    if (!m_spectrum && m_jobs) m_jobs->wait(*m_pending->counter);
    if (!m_pending->counter->done()) return;

    // 3. 完成 → 在主线程上传（Vulkan 命令只在主线程录制）
    const std::shared_ptr<PendingSpectrum> job = std::move(m_pending);
    m_pending.reset();
    if (job->failed) {
        LOG_ERROR("海洋频谱生成失败，继续使用上一份");
        return;
    }
    job->data->generateMs = msSince(job->start);
    uploadSpectrum(job->data);
}

void OceanRenderer::uploadSpectrum(std::shared_ptr<const OceanSpectrumData> data) {
    const auto t0 = std::chrono::steady_clock::now();
    const uint32_t n = kOceanFftSize;
    // 旧贴图可能还被在途的帧使用：延迟到 GPU 用完再销毁
    for (rhi::Image* image : {&m_h0, &m_omega})
        if (*image) {
            auto old = std::make_shared<rhi::Image>(*image);
            m_device->deferDestroy([dev = m_device, old] { rhi::destroyImage(*dev, *old); });
        }
    // EXTEND: 这里是同步上传（等 GPU 拷完，约 1 ms）。需要更平滑时改成在帧内的 transfer pass 里拷贝，不等待
    m_h0 = rhi::createTextureArray(*m_device, kSpectrumFormat, n, n, kOceanCascades, data->h0.data(), sizeof(vec4), "OceanH0");
    m_omega = rhi::createTextureArray(*m_device, kOmegaFormat, n, n, kOceanCascades, data->omega.data(), sizeof(float), "OceanOmega");

    const float wp = peakFrequency(data->settings);
    m_stats.significantWaveHeight = data->significantWaveHeight;
    m_stats.peakWavelength = 2.0f * kPi * kGravity / (wp * wp); // 深水近似 λ = 2πg/ω²
    m_stats.peakPeriod = 2.0f * kPi / wp;
    m_stats.spectrumMs = data->generateMs;
    m_stats.uploadMs = msSince(t0);
    ++m_stats.rebuilds;
    m_spectrum = std::move(data);
}

float OceanRenderer::heightAt(float x, float z, double simTime) const {
    return m_spectrum ? gerstnerHeightAt(m_spectrum->waves, x, z, oceanPhaseTime(simTime)) : 0.0f;
}

// ---------------------------------------------------------------- 每帧的 pass

void OceanRenderer::addTransparentPasses(rg::RenderGraph& graph, const SceneTargets& targets) {
    if (!m_settings.enabled || !targets.view || !m_device) return;
    updateSpectrum();
    if (!m_spectrum) return; // 第一份频谱生成失败：不画海，但不影响其他 pass
    ++m_frame;

    const RenderView& view = *targets.view;
    const OceanSettings& s = m_settings;
    const std::shared_ptr<const OceanSpectrumData> spectrum = m_spectrum; // 本帧用的频谱（lambda 里持有，保证一致）
    // 贴图是按这组级联尺寸生成的：新尺寸的频谱还在后台生成时，继续用旧尺寸才对得上
    const vec4 sizes = spectrum->settings.cascadeSizes;
    const bool fft = s.mode == int(OceanMode::Fft);
    const uint32_t n = kOceanFftSize;
    // 海浪用模拟时间（暂停时停住），对循环周期取模后再转 float，见 OceanSpectrum.h 的 kOceanRepeatPeriod
    const float phaseTime = oceanPhaseTime(view.simTime);

    SimPush sim;
    sim.time = vec4(phaseTime, 0.0f, 0.0f, s.choppiness);
    sim.sizes = sizes;
    sim.foam = vec4(s.foamBias, s.foamIntensity, std::exp(-view.simDt / std::max(s.foamDecay, 0.01f)), 0.0f);
    sim.fft = vec4(0.0f);

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
        // 频域、中间结果、空间域各用各的贴图：任何一步被关掉，下一步都会因为「必需输入没有内容」被图跳过
        const rg::TextureHandle spec0 = cascadeArray("OceanSpectrum0", kSpectrumFormat, false);
        const rg::TextureHandle spec1 = cascadeArray("OceanSpectrum1", kSpectrumFormat, false);
        const rg::TextureHandle tmp0 = cascadeArray("OceanFFTTemp0", kSpectrumFormat, false);
        const rg::TextureHandle tmp1 = cascadeArray("OceanFFTTemp1", kSpectrumFormat, false);
        const rg::TextureHandle space0 = cascadeArray("OceanSpatial0", kSpectrumFormat, false);
        const rg::TextureHandle space1 = cascadeArray("OceanSpatial1", kSpectrumFormat, false);
        // 位移/导数只代表「本帧」：组装没执行时海面读到默认贴图（平静水面），而不是之前残留的旧位移
        disp = cascadeArray("OceanDisplacement", kDisplacementFormat, false);
        deriv = cascadeArray("OceanDerivatives", kDisplacementFormat, false);
        // 泡沫历史 A/B 交替：本帧写一张，读上一帧写的另一张（持久贴图，内容跨帧保留）
        const bool odd = (m_frame & 1) != 0;
        const rg::TextureHandle foam = cascadeArray(odd ? "OceanFoamA" : "OceanFoamB", kFoamFormat, true);
        const rg::TextureHandle prevFoam = cascadeArray(odd ? "OceanFoamB" : "OceanFoamA", kFoamFormat, true);

        graph.addPass("OceanSpectrum", rg::PassType::Compute)
            .storageWrite(spec0)
            .storageWrite(spec1)
            .require([this] { return m_pipelines->ready(m_evolve); })
            .execute([this, spec0, spec1, sim, n](rg::PassContext& ctx) {
                m_pipelines->bind(ctx.cmd, m_evolve, ctx.frame->globalSet);
                VkDescriptorSet set = ctx.frame->descriptors->allocate(m_evolveLayout);
                rhi::DescriptorWriter()
                    .sampledImage(0, m_h0.view)
                    .storageImage(1, ctx.view(spec0))
                    .storageImage(2, ctx.view(spec1))
                    .sampledImage(3, m_omega.view)
                    .update(ctx.frame->device, set);
                m_pipelines->bindSet1(ctx.cmd, m_evolve, set);
                m_pipelines->pushConstants(ctx.cmd, m_evolve, &sim, sizeof(sim));
                vkCmdDispatch(ctx.cmd, n / 8, n / 8, kOceanCascades);
            });

        auto fftPass = [&](const char* name, rg::TextureHandle in0, rg::TextureHandle in1, rg::TextureHandle out0, rg::TextureHandle out1,
                           float direction) {
            SimPush p = sim;
            p.fft.x = direction;
            // 输入是 storage 读取（必需输入）：上游没产生它时图会直接跳过本 pass，这里不用再检查
            graph.addPass(name, rg::PassType::Compute)
                .storageRead(in0)
                .storageRead(in1)
                .storageWrite(out0)
                .storageWrite(out1)
                .require([this] { return m_pipelines->ready(m_fft); })
                .execute([this, in0, in1, out0, out1, p, n](rg::PassContext& ctx) {
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
        fftPass("OceanFFTColumns", tmp0, tmp1, space0, space1, 1.0f);

        graph.addPass("OceanAssemble", rg::PassType::Compute)
            .storageRead(space0)
            .storageRead(space1)
            .sampled(prevFoam) // 可选输入：没有历史时换成默认贴图，shader 由 foam.w 得知、不去读它
            .storageWrite(disp)
            .storageWrite(deriv)
            .storageWrite(foam)
            .require([this] { return m_pipelines->ready(m_assemble); })
            .execute([this, space0, space1, prevFoam, disp, deriv, foam, sim, n](rg::PassContext& ctx) {
                SimPush p = sim;
                m_lastAssembleHadHistory = ctx.hasContent(prevFoam);
                p.foam.w = m_lastAssembleHadHistory ? 1.0f : 0.0f;
                m_pipelines->bind(ctx.cmd, m_assemble, ctx.frame->globalSet);
                VkDescriptorSet set = ctx.frame->descriptors->allocate(m_assembleLayout);
                rhi::DescriptorWriter()
                    .storageImage(0, ctx.view(space0))
                    .storageImage(1, ctx.view(space1))
                    .sampledImage(2, ctx.view(prevFoam))
                    .storageImage(3, ctx.view(disp))
                    .storageImage(4, ctx.view(deriv))
                    .storageImage(5, ctx.view(foam))
                    .update(ctx.frame->device, set);
                m_pipelines->bindSet1(ctx.cmd, m_assemble, set);
                m_pipelines->pushConstants(ctx.cmd, m_assemble, &p, sizeof(p));
                vkCmdDispatch(ctx.cmd, n / 8, n / 8, kOceanCascades);
            });

        if (m_readbackRequested) addReadbackPass(graph, disp, deriv, spectrum, phaseTime);
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
    // 没有拷贝时海面 shader 由标志位得知、按深水处理，不读这张默认贴图的值（任何颜色都不是「很远」）
    dd.fallback = rg::Fallback::Black;
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
    sp.sizes = vec4(vec3(sizes), 0.0f); // w（标志位）在 execute 里按实际可用的输入填
    sp.grid = vec4(view.position.x, view.position.z, std::max(s.gridInner, 0.01f), std::max(s.gridOuter, s.gridInner + 1.0f));
    sp.gridDims = vec4(float(m_gridRings), float(m_gridSegments), float(spectrum->waves.size()), spectrum->significantWaveHeight);
    sp.water = vec4(s.refraction, s.roughness, s.sssStrength, s.shoreFoamWidth);
    sp.absorption = vec4(s.absorption, s.reflection);
    sp.scatter = vec4(s.scatterColor, std::max(s.foamTiling, 0.1f));
    sp.sss = vec4(s.sssColor, phaseTime);
    sp.foam = vec4(s.foamBias, s.foamIntensity, s.choppiness, float(std::fmod(view.simTime * kShoreSurfFrequency, 2.0 * kPi)));

    auto pass = graph.addPass("Ocean", rg::PassType::Graphics);
    pass.color(targets.color, rg::LoadOp::Load)
        .depth(targets.depth, rg::LoadOp::Load)
        .sampled(colorCopy)
        .sampled(depthLinear)
        .sampled(targets.shadowAtlas);
    if (fft) pass.sampled(disp).sampled(deriv);
    pass.require([this] { return m_pipelines->ready(wireframe ? m_surfaceWire : m_surface); })
        .execute([this, targets, colorCopy, depthLinear, disp, deriv, fft, sp, spectrum](rg::PassContext& ctx) {
            const PipelineHandle p = wireframe ? m_surfaceWire : m_surface;
            const DefaultResources& def = *ctx.frame->defaults;
            SurfacePush push = sp;
            const bool scene = ctx.hasContent(colorCopy) && ctx.hasContent(depthLinear);
            push.sizes.w = float((fft ? kFlagFft : 0u) | (scene ? kFlagScene : 0u));

            // Gerstner 波参数：每个在途帧一份缓冲，写本帧的那份
            const rhi::Buffer& waves = m_waveBuffers[ctx.frame->slot];
            std::vector<vec4> data(kMaxGerstnerWaves * 2, vec4(0.0f));
            for (size_t i = 0; i < spectrum->waves.size() && i < kMaxGerstnerWaves; ++i) {
                const GerstnerWave& w = spectrum->waves[i];
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
            m_pipelines->pushConstants(ctx.cmd, p, &push, sizeof(push));
            vkCmdBindIndexBuffer(ctx.cmd, m_indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(ctx.cmd, m_indexCount, 1, 0, 0, 0);
        });
}

// ---------------------------------------------------------------- 回读与校验

void OceanRenderer::addReadbackPass(rg::RenderGraph& graph, rg::TextureHandle disp, rg::TextureHandle deriv,
                                    std::shared_ptr<const OceanSpectrumData> spectrum, float phaseTime) {
    const uint32_t n = kOceanFftSize;
    const VkDeviceSize arrayBytes = VkDeviceSize(n) * n * kOceanCascades * 8; // RGBA16F = 8 字节/纹素
    if (m_readbackBuffer.size < arrayBytes * 2) {
        if (m_readbackBuffer) {
            auto old = std::make_shared<rhi::Buffer>(m_readbackBuffer);
            m_device->deferDestroy([dev = m_device, old] { rhi::destroyBuffer(*dev, *old); });
        }
        m_readbackBuffer = rhi::createBuffer(*m_device, arrayBytes * 2, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::MemoryUsage::Readback,
                                             "OceanReadback");
    }
    OceanReadback info;
    info.spectrum = std::move(spectrum);
    info.phaseTime = phaseTime;
    info.lambda = m_settings.choppiness;
    info.foamBias = m_settings.foamBias;
    info.foamIntensity = m_settings.foamIntensity;
    // 拷贝源是必需输入：组装没执行（被关掉、缺输入）时图会跳过本 pass，请求保留到下一次
    graph.addPass("OceanReadback", rg::PassType::Transfer)
        .transferSrc(disp)
        .transferSrc(deriv)
        .execute([this, disp, deriv, n, arrayBytes, info](rg::PassContext& ctx) mutable {
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, kOceanCascades}; // 各层在缓冲里紧挨着
            region.imageExtent = {n, n, 1};
            vkCmdCopyImageToBuffer(ctx.cmd, ctx.image(disp), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_readbackBuffer.buffer, 1, &region);
            region.bufferOffset = arrayBytes;
            vkCmdCopyImageToBuffer(ctx.cmd, ctx.image(deriv), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_readbackBuffer.buffer, 1, &region);
            // 与截图相同：fence 只保证执行完，要让 CPU 看到写入还需要一个 dst = HOST 的内存依赖
            VkMemoryBarrier2 toHost{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            toHost.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            toHost.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            toHost.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
            toHost.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
            VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dep.memoryBarrierCount = 1;
            dep.pMemoryBarriers = &toHost;
            vkCmdPipelineBarrier2(ctx.cmd, &dep);

            info.hadHistory = m_lastAssembleHadHistory; // 组装在本 pass 之前执行，标志已经写好
            m_readbackRecorded = std::move(info);
            m_readbackRequested = false;
        });
}

std::optional<OceanReadback> OceanRenderer::takeReadback() {
    if (!m_readbackRecorded || !m_device) return std::nullopt;
    vkDeviceWaitIdle(m_device->device()); // 测试专用：等录制了拷贝的那一帧执行完
    // Readback 内存可能不是 HOST_COHERENT：读之前 invalidate
    vmaInvalidateAllocation(m_device->allocator(), m_readbackBuffer.allocation, 0, VK_WHOLE_SIZE);
    OceanReadback rb = std::move(*m_readbackRecorded);
    m_readbackRecorded.reset();
    const size_t texels = size_t(kOceanFftSize) * kOceanFftSize * kOceanCascades;
    const auto* src = static_cast<const uint64_t*>(m_readbackBuffer.mapped);
    rb.displacement.resize(texels);
    rb.derivatives.resize(texels);
    for (size_t i = 0; i < texels; ++i) {
        // RGBA16F 一个纹素 8 字节：R 在最低的 16 位（小端），与 unpackHalf4x16 的顺序一致
        rb.displacement[i] = glm::unpackHalf4x16(src[i]);
        rb.derivatives[i] = glm::unpackHalf4x16(src[texels + i]);
    }
    return rb;
}

OceanCheck OceanRenderer::verify(const OceanReadback& rb, uint32_t samples) {
    OceanCheck out;
    if (!rb.spectrum) {
        out.summary = "回读里没有频谱";
        return out;
    }
    const OceanSpectrumData& spec = *rb.spectrum;
    const uint32_t n = kOceanFftSize;
    const size_t cells = size_t(n) * n;

    // 1. 所有纹素：不能有 NaN/Inf，泡沫在 [0, 1]
    for (size_t i = 0; i < rb.displacement.size(); ++i) {
        const vec4 d = rb.displacement[i], g = rb.derivatives[i];
        for (int c = 0; c < 4; ++c)
            if (!std::isfinite(d[c]) || !std::isfinite(g[c])) ++out.nonFinite;
        out.foamError = std::max(out.foamError, std::max(-d.w, d.w - 1.0f));
    }

    // 2. 随机抽样，与 CPU 直接求和逐点比较。误差除以该分量的均方根：
    //    位移是 half 精度（相对误差约 5e-4），加上 GPU 的 float 运算，正常情况下远小于 1%；
    //    约定不一致（传播方向、符号校正、打包顺序）会让误差达到 100% 量级
    std::mt19937 rng(20261028u);
    static const char* kNames[7] = {"λDx", "y", "λDz", "∂y/∂x", "∂y/∂z", "λ∂Dx/∂x", "λ∂Dz/∂z"};
    std::string worst;
    for (uint32_t c = 0; c < kOceanCascades; ++c) {
        const std::vector<vec4> h0(spec.h0.begin() + ptrdiff_t(c * cells), spec.h0.begin() + ptrdiff_t((c + 1) * cells));
        const std::vector<float> omega(spec.omega.begin() + ptrdiff_t(c * cells), spec.omega.begin() + ptrdiff_t((c + 1) * cells));
        const float L = spec.settings.cascadeSizes[int(c)];
        double sumSq[7] = {}, maxErr[7] = {};
        for (uint32_t i = 0; i < samples; ++i) {
            const uint32_t x = rng() % n, z = rng() % n;
            const CascadeSample ref = evaluateCascade(h0, omega, n, L, rb.lambda, vec2(float(x), float(z)) * (L / float(n)), rb.phaseTime);
            const vec4 gd = rb.displacement[c * cells + size_t(z) * n + x];
            const vec4 gg = rb.derivatives[c * cells + size_t(z) * n + x];
            const float want[7] = {ref.displacement.x, ref.displacement.y, ref.displacement.z, ref.derivatives.x,
                                   ref.derivatives.y,  ref.derivatives.z,  ref.derivatives.w};
            const float got[7] = {gd.x, gd.y, gd.z, gg.x, gg.y, gg.z, gg.w};
            for (int k = 0; k < 7; ++k) {
                sumSq[k] += double(want[k]) * want[k];
                maxErr[k] = std::max(maxErr[k], double(std::abs(got[k] - want[k])));
            }
            // 泡沫：第一帧没有历史 → 正好等于新泡沫；之后 = max(衰减的旧泡沫, 新泡沫) ≥ 新泡沫
            const float fresh = std::clamp((rb.foamBias - ref.displacement.w) * rb.foamIntensity, 0.0f, 1.0f);
            out.foamError = std::max(out.foamError, rb.hadHistory ? fresh - gd.w : std::abs(gd.w - fresh));
        }
        for (int k = 0; k < 7; ++k) {
            const double rms = std::sqrt(sumSq[k] / std::max(samples, 1u));
            if (rms > 1e-7 && float(maxErr[k] / rms) > out.maxError) {
                out.maxError = float(maxErr[k] / rms);
                worst = "（最大处：级联 " + std::to_string(c) + " 的 " + kNames[k] + "，均方根 " + std::to_string(rms) + "）";
            }
        }
    }
    // 阈值：相对误差 2%、泡沫 0.03（泡沫是雅可比的陡峭函数，雅可比的 half 误差会被强度放大）
    out.ok = out.nonFinite == 0 && out.maxError < 0.02f && out.foamError < 0.03f;
    out.summary = "相对误差 " + std::to_string(out.maxError) + worst + "，泡沫误差 " + std::to_string(out.foamError) + "，非有限值 " +
                  std::to_string(out.nonFinite) + (rb.hadHistory ? "（有泡沫历史）" : "（无泡沫历史）");
    return out;
}

} // namespace eng::ocean
