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
// 海面的常量缓冲：Gerstner 波（A、B 各 kMaxGerstnerWaves 个）+ 波纹的网格映射（2 个 vec4）
constexpr uint32_t kParamsBufferSize = (kMaxGerstnerWaves * 2 + 2) * sizeof(vec4);
// 波纹状态：蛙跳格式里 u − u⁻ 是两个很接近的数相减，16 位会把小波纹吃掉，用 32 位
constexpr VkFormat kRippleFormat = VK_FORMAT_R32G32B32A32_SFLOAT;

// 必须与 shaders/ocean/ocean_ripples.hlsl 的 RipplePush 一致
struct RipplePush {
    vec4 grid;   // 格子边长、波速上限、阻尼、n
    vec4 sponge; // 吸收层宽度（格）、吸收层强度、泡沫坡度阈值、泡沫强度
    vec4 flags;  // 泡沫保留系数、有上一步状态、有海床、本步序号
    vec4 seabed; // 高度图原点 xz、间距、分辨率
    vec4 water;  // 平均海面、g、最小水深
};
// 必须与 ocean_ripples.hlsl 的 RippleStepData 一致（全是 vec4，常量缓冲里没有额外的对齐填充）
struct RippleStepGpu {
    vec4 step;   // Δt、shift.x、shift.y、扰动源个数
    vec4 origin; // 区域原点 xz
    vec4 sources[kMaxRippleSources * 2]; // 每个源两个：(位置, 半径, 竖直加速度)、(水平偶极子, -, -)
};
constexpr uint32_t kRippleStepBufferSize = sizeof(RippleStepGpu) * kMaxRippleStepsPerFrame;
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
                                            bind(6, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, vsps), bind(7, sampled, vsps)},
                                           "OceanSurfaceLayout");
    m_rippleLayout = rhi::createSetLayout(
        device, {bind(0, sampled, cs), bind(1, storage, cs), bind(2, sampled, cs), bind(3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, cs)},
        "OceanRippleLayout");

    m_evolve = pipelines.createCompute({"OceanEvolve", "ocean/ocean_evolve", m_evolveLayout});
    m_fft = pipelines.createCompute({"OceanFFT", "ocean/ocean_fft", m_fftLayout});
    m_assemble = pipelines.createCompute({"OceanAssemble", "ocean/ocean_assemble", m_assembleLayout});
    m_ripple = pipelines.createCompute({"OceanRipples", "ocean/ocean_ripples", m_rippleLayout});

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

    for (uint32_t i = 0; i < rhi::kFramesInFlight; ++i) {
        m_waveBuffers[i] = rhi::createBuffer(device, kParamsBufferSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, rhi::MemoryUsage::Upload,
                                             ("OceanParams" + std::to_string(i)).c_str());
        m_rippleStepBuffers[i] = rhi::createBuffer(device, kRippleStepBufferSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                                   rhi::MemoryUsage::Upload, ("OceanRippleSteps" + std::to_string(i)).c_str());
    }
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
    rhi::destroyBuffer(*m_device, m_rippleReadbackBuffer);
    for (rhi::Buffer& b : m_waveBuffers) rhi::destroyBuffer(*m_device, b);
    for (rhi::Buffer& b : m_rippleStepBuffers) rhi::destroyBuffer(*m_device, b);
    for (VkDescriptorSetLayout l : {m_evolveLayout, m_fftLayout, m_assembleLayout, m_prepareLayout, m_surfaceLayout, m_rippleLayout})
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

// ---------------------------------------------------------------- 局部交互：物理步排队

void OceanRenderer::queueRippleStep(double simTime, float dt, vec2 center, const std::vector<RippleSource>& sources) {
    if (!m_settings.enabled || !m_settings.ripples || dt <= 0.0f) {
        m_rippleReset = true; // 关掉期间的状态作废：重新打开时从平静水面开始
        m_splashes.clear();
        m_splashFollowUps.clear();
        return;
    }
    PendingRipple r;
    r.params = makeRippleParams(m_settings, dt);
    // 网格映射变了（区域尺寸 / 分辨率）：旧状态的格子对不上新格子，作废
    if (r.params.n != m_rippleParams.n || r.params.cellSize != m_rippleParams.cellSize) m_rippleReset = true;
    const ivec2 origin = rippleOriginCell(center, r.params);
    r.input.dt = dt;
    // 原点按整格对齐，移动了几格就让 shader 读上一步状态时偏移几格（瞬移超过整个网格也没关系：全部读到平静水面）
    r.input.shift = (m_rippleHasOrigin && !m_rippleReset) ? origin - m_rippleOrigin : ivec2(0);
    r.input.origin = vec2(origin) * r.params.cellSize;
    for (const RippleSource& src : sources)
        if (r.input.sourceCount < kMaxRippleSources) r.input.sources[r.input.sourceCount++] = src;
    // 一次性扰动（水花）：这一步加速度 -depth/Δt²，水面下沉 depth 米（见 rippleStep 的 Δt²·f 项）；
    // 下一步再加一个反向的，把这一步带来的速度抵消掉 —— 结果是「静止的凹坑」，之后像石子入水一样向外扩散成一圈。
    // 只有第一步的话，水面带着 -depth/Δt 的速度一直往下走，二维里凹坑会越陷越深，而不是变成波纹
    for (const RippleSource& sp : m_splashFollowUps)
        if (r.input.sourceCount < kMaxRippleSources) r.input.sources[r.input.sourceCount++] = {sp.position, sp.radius, sp.strength / (dt * dt)};
    m_splashFollowUps.clear();
    for (const RippleSource& sp : m_splashes)
        if (r.input.sourceCount < kMaxRippleSources) {
            r.input.sources[r.input.sourceCount++] = {sp.position, sp.radius, -sp.strength / (dt * dt)};
            m_splashFollowUps.push_back(sp);
        }
    m_splashes.clear();
    r.reset = m_rippleReset;

    m_rippleReset = false;
    m_rippleOrigin = origin;
    m_rippleHasOrigin = true;
    m_rippleParams = r.params;
    m_rippleStepTime = simTime;
    m_rippleStepDt = dt;
    if (m_pendingRipples.size() >= kMaxRippleStepsPerFrame) {
        // 一帧里排了太多步（画海的那一帧之前物理补了很多步）：丢掉最早的一步。
        // 它的区域移动要并到下一步里，否则下一步的偏移是相对于一个 GPU 从没算过的原点
        PendingRipple& next = m_pendingRipples[1];
        next.input.shift += m_pendingRipples.front().input.shift;
        next.reset = next.reset || m_pendingRipples.front().reset;
        m_pendingRipples.erase(m_pendingRipples.begin());
        ++m_rippleStats.droppedSteps;
    }
    m_pendingRipples.push_back(r);
}

void OceanRenderer::splash(vec2 xz, float radius, float depth) {
    m_splashes.push_back({xz, radius, depth}); // strength 暂存深度，排队时换算成加速度
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

    // ---- 局部交互（波纹） ----
    const rg::TextureHandle ripples = addRipplePasses(graph);
    // 海面常量缓冲里的波纹映射：区域原点、边长、插值系数（渲染时刻在最近两步之间的位置）
    const float rippleCell = m_rippleParams.cellSize;
    const float alpha = std::clamp(1.0f - float(m_rippleStepTime - view.simTime) / std::max(m_rippleStepDt, 1e-4f), 0.0f, 1.0f);
    const vec4 ripple0(vec2(m_rippleOrigin) * rippleCell, rippleCell * float(m_rippleParams.n), alpha);
    const vec4 ripple1(ripples.valid() ? 1.0f : 0.0f, s.rippleNormal, 1.0f / float(m_rippleParams.n), 0.0f);

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
    if (ripples.valid()) pass.sampled(ripples);
    pass.require([this] { return m_pipelines->ready(wireframe ? m_surfaceWire : m_surface); })
        .execute([this, targets, colorCopy, depthLinear, disp, deriv, fft, sp, spectrum, ripples, ripple0, ripple1](rg::PassContext& ctx) {
            const PipelineHandle p = wireframe ? m_surfaceWire : m_surface;
            const DefaultResources& def = *ctx.frame->defaults;
            SurfacePush push = sp;
            const bool scene = ctx.hasContent(colorCopy) && ctx.hasContent(depthLinear);
            push.sizes.w = float((fft ? kFlagFft : 0u) | (scene ? kFlagScene : 0u));

            // Gerstner 波参数 + 波纹映射：每个在途帧一份缓冲，写本帧的那份
            const rhi::Buffer& waves = m_waveBuffers[ctx.frame->slot];
            std::vector<vec4> data(kMaxGerstnerWaves * 2 + 2, vec4(0.0f));
            for (size_t i = 0; i < spectrum->waves.size() && i < kMaxGerstnerWaves; ++i) {
                const GerstnerWave& w = spectrum->waves[i];
                data[i] = vec4(w.direction, w.k, w.omega);
                data[kMaxGerstnerWaves + i] = vec4(w.amplitude, w.phase, w.steepness, 0.0f);
            }
            data[kMaxGerstnerWaves * 2] = ripple0;
            data[kMaxGerstnerWaves * 2 + 1] = ripple1;
            std::memcpy(waves.mapped, data.data(), kParamsBufferSize);
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
                .uniformBuffer(6, waves.buffer, 0, kParamsBufferSize)
                .sampledImage(7, ripples.valid() ? ctx.view(ripples) : def.black().view)
                .update(ctx.frame->device, set);
            m_pipelines->bindSet1(ctx.cmd, p, set);
            m_pipelines->pushConstants(ctx.cmd, p, &push, sizeof(push));
            vkCmdBindIndexBuffer(ctx.cmd, m_indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(ctx.cmd, m_indexCount, 1, 0, 0, 0);
        });
}

// ---------------------------------------------------------------- 局部交互：compute pass

rg::TextureHandle OceanRenderer::addRipplePasses(rg::RenderGraph& graph) {
    m_rippleStats.stepsThisFrame = 0;
    m_rippleStats.cellSize = m_rippleParams.cellSize;
    m_rippleStats.speedLimit = rippleSpeedLimit(m_rippleParams.cellSize, m_rippleStepDt);
    m_rippleStats.center = (vec2(m_rippleOrigin) + float(m_rippleParams.n / 2)) * m_rippleParams.cellSize;
    // 这一帧的步会不会真的执行，构建图的时候就要知道：最新状态在 A 还是 B 取决于「实际写了哪一张」。
    // 被关掉 / 管线没准备好 → 不排步、海面不采样波纹，状态作废（重新打开时从平静水面开始）。
    // 波纹 pass 只有采样输入（可选），不会因为缺输入被图跳过，所以这两个条件就够了
    if (!m_settings.ripples) {
        m_pendingRipples.clear();
        m_rippleAlive = false;
        m_rippleReset = true;
        return {};
    }
    const uint32_t n = m_rippleParams.n;
    rg::TextureDesc d;
    d.format = kRippleFormat;
    d.width = d.height = n;
    d.persistent = true; // 状态本身就是「上一步的结果」：A/B 交替，内容跨帧保留
    d.name = "OceanRipplesA";
    const rg::TextureHandle texA = graph.createTexture(d);
    d.name = "OceanRipplesB";
    const rg::TextureHandle texB = graph.createTexture(d);

    if (!graph.passEnabled("OceanRipples") || !m_pipelines->ready(m_ripple)) {
        // 仍然声明一个（不会执行的）pass：面板靠本帧的 pass 列表显示开关，不声明就没法再打开它
        graph.addPass("OceanRipples", rg::PassType::Compute)
            .sampled(texA)
            .storageWrite(texB)
            .require([this] { return m_pipelines->ready(m_ripple); })
            .execute([](rg::PassContext&) {});
        m_pendingRipples.clear();
        m_rippleAlive = false;
        m_rippleReset = true;
        return {};
    }
    if (m_pendingRipples.empty() && !m_rippleAlive) return {};

    const bool check = m_rippleCheckRequested && !m_rippleRecorded; // 上一次的结果还没被取走时不再录
    for (uint32_t k = 0; k < m_pendingRipples.size(); ++k) {
        const PendingRipple& r = m_pendingRipples[k];
        const rg::TextureHandle in = m_rippleLatestB ? texB : texA;
        const rg::TextureHandle out = m_rippleLatestB ? texA : texB;

        RipplePush push;
        push.grid = vec4(r.params.cellSize, r.params.maxSpeed, r.params.damping, float(n));
        push.sponge = vec4(r.params.spongeCells, r.params.spongeStrength, r.params.foamSlope, r.params.foamGain);
        push.flags = vec4(std::exp(-r.input.dt / r.params.foamDecay), 0.0f, m_seabed.heightMap ? 1.0f : 0.0f, float(k));
        push.seabed = vec4(m_seabed.origin, m_seabed.spacing, float(m_seabed.resolution));
        push.water = vec4(0.0f, kGravity, kRippleMinDepth, 0.0f); // 平均海面在 y = 0

        RippleStepGpu gpu{};
        gpu.step = vec4(r.input.dt, float(r.input.shift.x), float(r.input.shift.y), float(r.input.sourceCount));
        gpu.origin = vec4(r.input.origin, 0.0f, 0.0f);
        for (uint32_t i = 0; i < r.input.sourceCount; ++i) {
            const RippleSource& src = r.input.sources[i];
            gpu.sources[2 * i] = vec4(src.position, src.radius, src.strength);
            gpu.sources[2 * i + 1] = vec4(src.push, 0.0f, 0.0f);
        }

        if (check && k == 0) addRippleCopyPass(graph, "OceanRippleCheckIn", in, 0);
        const VkImageView seabed = m_seabed.heightMap; // 地形的贴图：只在本帧内用（App 每帧重新传）
        graph.addPass("OceanRipples", rg::PassType::Compute) // 一帧里的每一步同名：面板上一个开关管全部
            .sampled(in) // 可选输入：没有内容（第一步）时 shader 由 flags.y 得知，不去读
            .storageWrite(out)
            .require([this] { return m_pipelines->ready(m_ripple); })
            .execute([this, in, out, push, gpu, k, n, seabed, reset = r.reset](rg::PassContext& ctx) {
                RipplePush p = push;
                const bool history = !reset && ctx.hasContent(in);
                p.flags.y = history ? 1.0f : 0.0f;
                m_rippleLastHadHistory = history;
                const rhi::Buffer& steps = m_rippleStepBuffers[ctx.frame->slot];
                std::memcpy(static_cast<char*>(steps.mapped) + k * sizeof(RippleStepGpu), &gpu, sizeof(gpu));
                vmaFlushAllocation(m_device->allocator(), steps.allocation, k * sizeof(RippleStepGpu), sizeof(RippleStepGpu));
                const DefaultResources& def = *ctx.frame->defaults;
                m_pipelines->bind(ctx.cmd, m_ripple, ctx.frame->globalSet);
                VkDescriptorSet set = ctx.frame->descriptors->allocate(m_rippleLayout);
                rhi::DescriptorWriter()
                    .sampledImage(0, ctx.view(in))
                    .storageImage(1, ctx.view(out))
                    .sampledImage(2, seabed ? seabed : def.black().view)
                    .uniformBuffer(3, steps.buffer, 0, kRippleStepBufferSize)
                    .update(ctx.frame->device, set);
                m_pipelines->bindSet1(ctx.cmd, m_ripple, set);
                m_pipelines->pushConstants(ctx.cmd, m_ripple, &p, sizeof(p));
                vkCmdDispatch(ctx.cmd, (n + 7) / 8, (n + 7) / 8, 1);
            });
        if (check && k == 0) {
            addRippleCopyPass(graph, "OceanRippleCheckOut", out, 1);
            m_rippleCheckPending = {r.params, r.input, false, {}, {}};
        }
        m_rippleLatestB = !m_rippleLatestB;
        ++m_rippleStats.stepsThisFrame;
    }
    m_pendingRipples.clear();
    m_rippleAlive = true;
    return m_rippleLatestB ? texB : texA;
}

void OceanRenderer::addRippleCopyPass(rg::RenderGraph& graph, const char* name, rg::TextureHandle tex, uint32_t slot) {
    const uint32_t n = m_rippleParams.n;
    const VkDeviceSize bytes = VkDeviceSize(n) * n * sizeof(vec4);
    if (m_rippleReadbackBuffer.size < bytes * 2) {
        if (m_rippleReadbackBuffer) {
            auto old = std::make_shared<rhi::Buffer>(m_rippleReadbackBuffer);
            m_device->deferDestroy([dev = m_device, old] { rhi::destroyBuffer(*dev, *old); });
        }
        m_rippleReadbackBuffer = rhi::createBuffer(*m_device, bytes * 2, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::MemoryUsage::Readback,
                                                   "OceanRippleReadback");
    }
    // 拷贝源是必需输入：输入状态还没有内容（第一步）时图会跳过「输入」这一份，这一次校验就不记录
    graph.addPass(name, rg::PassType::Transfer)
        .transferSrc(tex)
        .execute([this, tex, n, bytes, slot](rg::PassContext& ctx) {
            VkBufferImageCopy region{};
            region.bufferOffset = bytes * slot;
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {n, n, 1};
            vkCmdCopyImageToBuffer(ctx.cmd, ctx.image(tex), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_rippleReadbackBuffer.buffer, 1, &region);
            VkMemoryBarrier2 toHost{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            toHost.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            toHost.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            toHost.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
            toHost.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
            VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dep.memoryBarrierCount = 1;
            dep.pMemoryBarriers = &toHost;
            vkCmdPipelineBarrier2(ctx.cmd, &dep);
            if (slot == 0) {
                m_rippleInputCopied = true;
            } else if (m_rippleInputCopied && m_rippleCheckPending) {
                // 输入、这一步、输出三个 pass 都执行了：记录下来，等 GPU 执行完再读
                m_rippleRecorded = std::move(*m_rippleCheckPending);
                m_rippleRecorded->hadHistory = m_rippleLastHadHistory;
                m_rippleCheckRequested = false;
            }
        });
    if (slot == 0) m_rippleInputCopied = false;
}

std::optional<RippleReadback> OceanRenderer::takeRippleCheck() {
    if (!m_rippleRecorded || !m_device) return std::nullopt;
    vkDeviceWaitIdle(m_device->device()); // 测试专用：等录制了拷贝的那一帧执行完
    vmaInvalidateAllocation(m_device->allocator(), m_rippleReadbackBuffer.allocation, 0, VK_WHOLE_SIZE);
    RippleReadback rb = std::move(*m_rippleRecorded);
    m_rippleRecorded.reset();
    const size_t cells = size_t(rb.params.n) * rb.params.n;
    const auto* src = static_cast<const vec4*>(m_rippleReadbackBuffer.mapped);
    rb.input.assign(src, src + cells);
    rb.output.assign(src + cells, src + 2 * cells);
    return rb;
}

OceanCheck OceanRenderer::verifyRipples(const RippleReadback& rb) {
    OceanCheck out;
    // 水深取 GPU 写在输出第 4 个分量里的值：海床高度图在 GPU 上，CPU 只验证「同样的输入 → 同样的输出」
    std::vector<float> depth(rb.output.size());
    for (size_t i = 0; i < depth.size(); ++i) depth[i] = rb.output[i].w;
    RippleGrid cpu;
    rippleStep(rb.input, rb.hadHistory, depth, rb.params, rb.step, cpu);

    double sumSq = 0.0, maxErr = 0.0, maxPrevErr = 0.0;
    for (size_t i = 0; i < cpu.size(); ++i) {
        const vec4 g = rb.output[i];
        for (int c = 0; c < 4; ++c)
            if (!std::isfinite(g[c])) ++out.nonFinite;
        sumSq += double(cpu[i].x) * cpu[i].x;
        maxErr = std::max(maxErr, double(std::abs(g.x - cpu[i].x)));
        maxPrevErr = std::max(maxPrevErr, double(std::abs(g.y - cpu[i].y)));
        out.foamError = std::max(out.foamError, std::abs(g.z - cpu[i].z));
    }
    const double rms = std::sqrt(sumSq / std::max<size_t>(cpu.size(), 1));
    out.maxError = float(maxErr / std::max(rms, 1e-9));
    // u 的误差相对均方根 0.1%（GPU 的 exp 与 FMA 和 CPU 略有不同）；u⁻ 是直接拷过去的，必须完全相同
    out.ok = out.nonFinite == 0 && rms > 1e-6 && out.maxError < 1e-3f && maxPrevErr == 0.0 && out.foamError < 1e-3f;
    out.summary = "波纹 u 相对误差 " + std::to_string(out.maxError) + "（均方根 " + std::to_string(rms) + " m），泡沫误差 " +
                  std::to_string(out.foamError) + "，非有限值 " + std::to_string(out.nonFinite) + (rb.hadHistory ? "" : "（无历史）");
    if (rms <= 1e-6) out.summary += "；水面几乎平静，校验没有意义";
    return out;
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
