#include "render/Renderer.h"

#include "core/ImageIO.h"
#include "core/Log.h"
#include "math/MathTypes.h"
#include "platform/Window.h"
#include "render/FrameContext.h"

#include <cstring>
#include <string>
#include <vector>

namespace eng {

bool Renderer::init(rhi::Device& device, Window& window, const RendererSettings& settings) {
    m_device = &device;
    m_window = &window;
    m_settings = settings;

    const ivec2 fb = window.framebufferSize();
    if (!m_swapchain.create(device, uint32_t(fb.x), uint32_t(fb.y), settings.vsync)) return false;

    VkDevice dev = device.device();
    for (uint32_t i = 0; i < rhi::kFramesInFlight; ++i) {
        FrameResources& f = m_frames[i];
        const std::string idx = std::to_string(i);
        // 每帧一个命令池：帧开始时整池重置，比逐个重置命令缓冲更便宜
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pci.queueFamilyIndex = device.graphicsFamily();
        ENG_CHECK_VK(vkCreateCommandPool(dev, &pci, nullptr, &f.pool));
        rhi::setDebugName(dev, f.pool, ("FramePool" + idx).c_str());

        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = f.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        ENG_CHECK_VK(vkAllocateCommandBuffers(dev, &ai, &f.cmd));
        rhi::setDebugName(dev, f.cmd, ("FrameCmd" + idx).c_str());

        // fence 初始为「已触发」：第一帧等待它时立即返回，不需要特判
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        ENG_CHECK_VK(vkCreateFence(dev, &fci, nullptr, &f.inFlight));
        rhi::setDebugName(dev, f.inFlight, ("FrameFence" + idx).c_str());

        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        ENG_CHECK_VK(vkCreateSemaphore(dev, &sci, nullptr, &f.imageAvailable));
        rhi::setDebugName(dev, f.imageAvailable, ("ImageAvailable" + idx).c_str());

        f.descriptors.init(device, ("FrameDescriptors" + idx + "_").c_str());
        // 每帧一份常量缓冲：CPU 写第 N+1 帧的常量时，GPU 可能还在读第 N 帧的
        f.constants = rhi::createBuffer(device, sizeof(FrameConstants), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                        rhi::MemoryUsage::Upload, ("FrameConstants" + idx).c_str());
    }

    m_defaults.init(device);
    createGlobalDescriptors();
    m_pipelines.init(device, m_globalLayout);
    m_graph.init(device, m_defaults);
    m_profiler.init(device);

    m_forward.init(device, m_pipelines, kSceneColorFormat, kDepthFormat);
    m_shadowPass.init(m_pipelines, kDepthFormat);
    m_sky.init(m_pipelines, kSceneColorFormat, kDepthFormat);
    m_tonemap.init(device, m_pipelines, m_swapchain.format());
    m_fxaa.init(device, m_pipelines, m_swapchain.format());
    return true;
}

void Renderer::createGlobalDescriptors() {
    VkDevice dev = m_device->device();
    // set 0 的采样器用「不可变采样器」（immutable sampler）：直接烘焙进布局，永远不需要写描述符
    const VkSampler linearWrap = m_defaults.linearWrap();
    const VkSampler linearClamp = m_defaults.linearClamp();
    const VkSampler pointClamp = m_defaults.pointClamp();
    const VkSampler shadowCompare = m_defaults.shadowCompare();
    m_globalLayout = rhi::createSetLayout(*m_device,
                                          {
                                              {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_ALL, nullptr},
                                              {1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_ALL, &linearWrap},
                                              {2, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_ALL, &linearClamp},
                                              {3, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_ALL, &pointClamp},
                                              {4, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_ALL, &shadowCompare},
                                          },
                                          "GlobalSetLayout");

    const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, rhi::kFramesInFlight},
                                          {VK_DESCRIPTOR_TYPE_SAMPLER, 4 * rhi::kFramesInFlight}};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = rhi::kFramesInFlight;
    pci.poolSizeCount = 2;
    pci.pPoolSizes = sizes;
    ENG_CHECK_VK(vkCreateDescriptorPool(dev, &pci, nullptr, &m_globalPool));
    rhi::setDebugName(dev, m_globalPool, "GlobalDescriptorPool");

    for (uint32_t i = 0; i < rhi::kFramesInFlight; ++i) {
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = m_globalPool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &m_globalLayout;
        ENG_CHECK_VK(vkAllocateDescriptorSets(dev, &ai, &m_frames[i].globalSet));
        rhi::setDebugName(dev, m_frames[i].globalSet, ("GlobalSet" + std::to_string(i)).c_str());
        // 只需写一次：之后每帧只更新缓冲内容，描述符本身不变
        rhi::DescriptorWriter().uniformBuffer(0, m_frames[i].constants.buffer, 0, sizeof(FrameConstants)).update(dev, m_frames[i].globalSet);
    }
}

void Renderer::shutdown() {
    if (!m_device) return;
    VkDevice dev = m_device->device();
    vkDeviceWaitIdle(dev);
    m_device->flushDeferred();
    m_tonemap.shutdown();
    m_fxaa.shutdown();
    m_forward.shutdown();
    m_profiler.shutdown();
    m_graph.shutdown();
    m_pipelines.shutdown();
    vkDestroyDescriptorPool(dev, m_globalPool, nullptr);
    vkDestroyDescriptorSetLayout(dev, m_globalLayout, nullptr);
    m_defaults.shutdown();
    rhi::destroyBuffer(*m_device, m_captureBuffer);
    for (FrameResources& f : m_frames) {
        f.descriptors.shutdown();
        rhi::destroyBuffer(*m_device, f.constants);
        vkDestroySemaphore(dev, f.imageAvailable, nullptr);
        vkDestroyFence(dev, f.inFlight, nullptr);
        vkDestroyCommandPool(dev, f.pool, nullptr);
    }
    m_swapchain.destroy();
    m_device = nullptr;
}

bool Renderer::recreateSwapchain() {
    const ivec2 fb = m_window->framebufferSize();
    if (fb.x == 0 || fb.y == 0) return false;
    return m_swapchain.recreate(uint32_t(fb.x), uint32_t(fb.y), m_settings.vsync);
}

void Renderer::updateFrameConstants(FrameResources& frame, const RenderView& view) {
    FrameConstants c{};
    c.view = view.view;
    c.proj = view.proj;
    c.viewProj = view.proj * view.view;
    c.invViewProj = glm::inverse(c.viewProj);
    c.cameraPos = vec4(view.position, 1.0f);
    const VkExtent2D e = sceneExtent();
    c.screen = vec4(float(e.width), float(e.height), 1.0f / float(e.width), 1.0f / float(e.height));
    c.time = vec4(float(view.time), view.dt, float(m_frameNumber), 0.0f);

    // 光照输入：所有材质共享这一份（见 shaders/common/lighting.hlsli）
    const LightingSettings& L = m_lighting;
    c.sunDirection = vec4(L.sunDirection(), L.sunIntensity);
    c.sunColor = vec4(L.sunColor, glm::radians(L.sunAngularRadiusDeg));
    c.skyZenith = vec4(L.skyZenith, L.skyIntensity);
    c.skyHorizon = vec4(L.skyHorizon, L.sunHalo);
    c.groundColor = vec4(L.groundColor, 0.0f);
    c.exposure = vec4(L.exposure, float(L.tonemapper), L.ambientIntensity, 0.0f);
    c.debug = vec4(float(m_settings.debugView), 0.0f, 0.0f, 0.0f);

    // 级联阴影：每帧根据相机视锥与太阳方向重新计算
    m_cascades = computeCascades(view.view, view.fovY, float(e.width) / float(std::max(e.height, 1u)), view.nearPlane,
                                 L.sunDirection(), m_shadowSettings);
    for (uint32_t i = 0; i < kShadowCascades; ++i) c.shadowViewProj[i] = m_cascades.viewProj[i];
    c.cascadeSplits = m_cascades.splits;
    c.cascadeTexel = m_cascades.texelWorldSize;
    // 太阳落到地平线以下时没有直射光，也就不需要阴影
    const bool shadowsOn = m_shadowSettings.enabled && L.sunElevationDeg > -2.0f;
    c.shadowParams = vec4(m_shadowSettings.depthBias, m_shadowSettings.normalBias, m_shadowSettings.pcfRadius, shadowsOn ? 1.0f : 0.0f);
    std::memcpy(frame.constants.mapped, &c, sizeof(c));
    // Upload 内存可能不是 HOST_COHERENT：flush 保证 GPU 看到 CPU 的写入
    vmaFlushAllocation(m_device->allocator(), frame.constants.allocation, 0, VK_WHOLE_SIZE);
}

bool Renderer::drawFrame(const RenderView& view, const DrawList& draws) {
    // 窗口缩放或 vsync 设置变化：先重建交换链，本帧跳过
    if (m_window->consumeResized() || !m_swapchain.handle() || m_swapchain.vsync() != m_settings.vsync) {
        recreateSwapchain();
        return false;
    }

    VkDevice dev = m_device->device();
    const uint32_t slot = uint32_t(m_frameNumber % rhi::kFramesInFlight);
    FrameResources& frame = m_frames[slot];

    // 1. 等待这个槽位上一次提交的命令执行完毕
    ENG_CHECK_VK(vkWaitForFences(dev, 1, &frame.inFlight, VK_TRUE, UINT64_MAX));
    m_device->beginFrame(m_frameNumber); // 现在可以安全销毁两帧前登记的对象了
    m_pipelines.pollHotReload(view.time);

    // 2. 获取交换链图像
    uint32_t imageIndex = 0;
    VkResult acq = vkAcquireNextImageKHR(dev, m_swapchain.handle(), UINT64_MAX, frame.imageAvailable, VK_NULL_HANDLE, &imageIndex);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
        // 注意：此时还没 reset fence，下一帧等待它会立即返回，不会死锁
        recreateSwapchain();
        return false;
    }
    if (acq != VK_SUBOPTIMAL_KHR) ENG_CHECK_VK(acq);

    // 确定要提交了才 reset fence（如果在 acquire 失败前 reset，fence 永远不会再被触发 → 死锁）
    ENG_CHECK_VK(vkResetFences(dev, 1, &frame.inFlight));
    ENG_CHECK_VK(vkResetCommandPool(dev, frame.pool, 0));
    frame.descriptors.reset();
    m_profiler.beginFrame(slot);
    updateFrameConstants(frame, view);

    // 3+4. 构建并执行 Render Graph
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    ENG_CHECK_VK(vkBeginCommandBuffer(frame.cmd, &bi));

    m_currentView = &view;
    buildGraph(imageIndex, draws);
    FrameContext ctx;
    ctx.slot = slot;
    ctx.frameNumber = m_frameNumber;
    ctx.descriptors = &frame.descriptors;
    ctx.globalSet = frame.globalSet;
    ctx.pipelines = &m_pipelines;
    ctx.defaults = &m_defaults;
    ctx.device = dev;
    m_graph.execute(frame.cmd, ctx, &m_profiler);

    ENG_CHECK_VK(vkEndCommandBuffer(frame.cmd));

    // 5. 提交
    // waitSemaphore 的 stageMask = COLOR_ATTACHMENT_OUTPUT：只有「写颜色附件」这一步需要等图像可用，
    // 之前的顶点处理等阶段可以提前开始。Render Graph 给交换链设定的初始阶段与之一致，构成依赖链。
    VkSemaphoreSubmitInfo waitInfo{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    waitInfo.semaphore = frame.imageAvailable;
    waitInfo.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSemaphoreSubmitInfo signalInfo{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signalInfo.semaphore = m_swapchain.renderFinished(imageIndex);
    signalInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo cmdInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmdInfo.commandBuffer = frame.cmd;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.waitSemaphoreInfoCount = 1;
    si.pWaitSemaphoreInfos = &waitInfo;
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cmdInfo;
    si.signalSemaphoreInfoCount = 1;
    si.pSignalSemaphoreInfos = &signalInfo;
    ENG_CHECK_VK(vkQueueSubmit2(m_device->graphicsQueue(), 1, &si, frame.inFlight));

    if (m_captureRecorded) {
        // 截图：等这一帧执行完再读回（只在截图那一帧同步，平时不影响性能）
        ENG_CHECK_VK(vkWaitForFences(dev, 1, &frame.inFlight, VK_TRUE, UINT64_MAX));
        finishCapture();
    }

    VkSwapchainKHR sc = m_swapchain.handle();
    VkSemaphore renderFinished = m_swapchain.renderFinished(imageIndex);
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &renderFinished;
    pi.swapchainCount = 1;
    pi.pSwapchains = &sc;
    pi.pImageIndices = &imageIndex;
    VkResult pres = vkQueuePresentKHR(m_device->graphicsQueue(), &pi);
    ++m_frameNumber;
    if (pres == VK_ERROR_OUT_OF_DATE_KHR || pres == VK_SUBOPTIMAL_KHR) {
        recreateSwapchain();
    } else {
        ENG_CHECK_VK(pres);
    }
    return true;
}

namespace {
// 编辑器视口贴图：格式与交换链相同，这样 Tonemap 管线（按交换链格式创建）可以直接写它
rg::TextureDesc viewportDesc(VkFormat format, VkExtent2D extent) {
    rg::TextureDesc d;
    d.name = "ViewportColor";
    d.format = format;
    d.width = extent.width;
    d.height = extent.height;
    d.fallback = rg::Fallback::Black;
    return d;
}
} // namespace

VkImageView Renderer::useViewportOutput(uint32_t width, uint32_t height) {
    m_viewportOutput = true;
    // 面板被拖得很小时也保留最小尺寸，避免创建 0 尺寸贴图
    m_viewportExtent = {std::max(width, 16u), std::max(height, 16u)};
    return m_graph.preparePooledTexture(viewportDesc(m_swapchain.format(), m_viewportExtent),
                                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
}

void Renderer::useBackbufferOutput() {
    m_viewportOutput = false;
}

VkExtent2D Renderer::sceneExtent() const {
    return m_viewportOutput ? m_viewportExtent : m_swapchain.extent();
}

void Renderer::buildGraph(uint32_t imageIndex, const DrawList& draws) {
    const VkExtent2D extent = m_swapchain.extent();
    const VkExtent2D scene = sceneExtent();
    m_graph.beginFrame(extent, m_frameNumber);

    // 交换链图像：初始阶段 = submit 等待 imageAvailable 的阶段；帧末必须是 PRESENT_SRC 布局
    const rg::TextureHandle backbuffer = m_graph.importTexture(
        "Backbuffer", m_swapchain.image(imageIndex), m_swapchain.view(imageIndex), m_swapchain.format(), extent,
        {VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_NONE, VK_IMAGE_LAYOUT_UNDEFINED},
        {VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR});

    rg::TextureDesc colorDesc;
    colorDesc.name = "SceneColor";
    colorDesc.format = kSceneColorFormat;
    colorDesc.width = scene.width; // 场景分辨率跟随输出目标（视口面板或整个窗口）
    colorDesc.height = scene.height;
    colorDesc.fallback = rg::Fallback::Black;
    const vec4 c = m_settings.clearColor;
    colorDesc.clear.color = {{c.r, c.g, c.b, c.a}};
    const rg::TextureHandle sceneColor = m_graph.createTexture(colorDesc);

    rg::TextureDesc depthDesc;
    depthDesc.name = "SceneDepth";
    depthDesc.format = kDepthFormat;
    depthDesc.width = scene.width;
    depthDesc.height = scene.height;
    depthDesc.fallback = rg::Fallback::None;
    depthDesc.clear.depthStencil = {0.0f, 0}; // reverse-Z：0 = 无穷远
    const rg::TextureHandle depth = m_graph.createTexture(depthDesc);

    // 阴影图集：4 个级联各占一个象限。关掉阴影 pass 时，读取它的 pass 自动拿到「深度 0」的默认图 → 处处照亮
    rg::TextureDesc shadowDesc;
    shadowDesc.name = "ShadowAtlas";
    shadowDesc.format = kDepthFormat;
    shadowDesc.width = shadowDesc.height = m_shadowSettings.atlasSize;
    shadowDesc.fallback = rg::Fallback::DepthFar;
    shadowDesc.clear.depthStencil = {0.0f, 0};
    const rg::TextureHandle shadowAtlas = m_graph.createTexture(shadowDesc);

    SceneTargets targets;
    targets.color = sceneColor;
    targets.depth = depth;
    targets.shadowAtlas = shadowAtlas;
    targets.cascades = &m_cascades;
    targets.shadowSettings = &m_shadowSettings;
    targets.view = m_currentView;
    targets.sceneExtent = scene;

    // ---- pass 顺序 = 执行顺序 ----
    // 1. 阴影：网格 → 各特性（地形……）
    if (m_shadowSettings.enabled) {
        m_shadowPass.add(m_graph, shadowAtlas, draws);
        for (RenderFeature* f : m_features) f->addShadowPasses(m_graph, targets);
    }
    // 2. 不透明：各特性先画（第一个写入者负责清屏），再画网格
    for (RenderFeature* f : m_features) f->addOpaquePasses(m_graph, targets);
    m_forward.add(m_graph, sceneColor, depth, shadowAtlas, draws);
    // 3. 天空填充剩余像素；4. 半透明（水面……）
    m_sky.add(m_graph, sceneColor, depth);
    for (RenderFeature* f : m_features) f->addTransparentPasses(m_graph, targets);

    // 5. 色调映射（+ FXAA）→ 输出（编辑器视口贴图或交换链）
    const rg::TextureHandle output =
        m_viewportOutput ? m_graph.createTexture(viewportDesc(m_swapchain.format(), scene)) : backbuffer;
    if (m_settings.fxaa) {
        rg::TextureDesc ldrDesc;
        ldrDesc.name = "LDR";
        ldrDesc.format = m_swapchain.format();
        ldrDesc.width = scene.width;
        ldrDesc.height = scene.height;
        const rg::TextureHandle ldr = m_graph.createTexture(ldrDesc);
        m_tonemap.add(m_graph, sceneColor, ldr);
        m_fxaa.add(m_graph, ldr, output);
    } else {
        m_tonemap.add(m_graph, sceneColor, output);
    }

    // 6. 界面
    if (m_viewportOutput) {
        // 编辑器模式：UI pass 采样视口贴图（ImGui::Image）并画整个编辑器界面。
        // 「采样」声明让图在 UI 之前把视口贴图转成 SHADER_READ_ONLY 布局。
        m_graph.addPass("UI", rg::PassType::Graphics)
            .sampled(output)
            .color(backbuffer, rg::LoadOp::Clear)
            .execute([this](rg::PassContext& ctx) {
                if (m_overlay) m_overlay(ctx.cmd);
            });
    } else {
        m_graph.addPass("UI", rg::PassType::Graphics)
            .color(backbuffer, rg::LoadOp::Load) // 叠加在场景上；若前面的 pass 都被关，图会自动改成 CLEAR
            .execute([this](rg::PassContext& ctx) {
                if (m_overlay) m_overlay(ctx.cmd);
            });
    }
    if (!m_capturePath.empty()) addCapturePass(backbuffer, imageIndex);
}

void Renderer::addCapturePass(rg::TextureHandle backbuffer, uint32_t imageIndex) {
    const VkExtent2D ext = m_swapchain.extent();
    const VkDeviceSize size = VkDeviceSize(ext.width) * ext.height * 4;
    if (m_captureBuffer.size < size) {
        rhi::destroyBuffer(*m_device, m_captureBuffer);
        m_captureBuffer = rhi::createBuffer(*m_device, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::MemoryUsage::Readback, "CaptureReadback");
    }
    m_captureRecorded = true;
    const VkImage image = m_swapchain.image(imageIndex);
    m_graph.addPass("Capture", rg::PassType::Transfer)
        .transferSrc(backbuffer) // 图会自动把交换链图像转成 TRANSFER_SRC 布局，之后再转成 PRESENT
        .execute([this, image, ext](rg::PassContext& ctx) {
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {ext.width, ext.height, 1};
            vkCmdCopyImageToBuffer(ctx.cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_captureBuffer.buffer, 1, &region);
            // fence 只保证 GPU 执行完，不保证 GPU 的写入对 CPU 可见；
            // 必须有一个 dst 为 HOST 的 barrier，把拷贝结果「推」到 CPU 可见的域。
            VkMemoryBarrier2 toHost{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            toHost.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            toHost.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            toHost.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
            toHost.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
            VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dep.memoryBarrierCount = 1;
            dep.pMemoryBarriers = &toHost;
            vkCmdPipelineBarrier2(ctx.cmd, &dep);
        });
}

void Renderer::finishCapture() {
    m_captureRecorded = false;
    const VkExtent2D ext = m_swapchain.extent();
    const size_t pixels = size_t(ext.width) * ext.height;
    // Readback 内存可能不是 HOST_COHERENT，读之前要 invalidate，确保 CPU 看到 GPU 写入的数据
    vmaInvalidateAllocation(m_device->allocator(), m_captureBuffer.allocation, 0, VK_WHOLE_SIZE);
    const auto* src = static_cast<const uint8_t*>(m_captureBuffer.mapped);
    std::vector<uint8_t> rgba(pixels * 4);
    const bool bgra = m_swapchain.format() == VK_FORMAT_B8G8R8A8_UNORM || m_swapchain.format() == VK_FORMAT_B8G8R8A8_SRGB;
    for (size_t i = 0; i < pixels; ++i) {
        rgba[i * 4 + 0] = src[i * 4 + (bgra ? 2 : 0)];
        rgba[i * 4 + 1] = src[i * 4 + 1];
        rgba[i * 4 + 2] = src[i * 4 + (bgra ? 0 : 2)];
        rgba[i * 4 + 3] = 255;
    }
    if (writePngRGBA8(m_capturePath, int(ext.width), int(ext.height), rgba.data()))
        LOG_INFO("截图已保存: {}", m_capturePath.string());
    else
        LOG_ERROR("截图保存失败: {}", m_capturePath.string());
    m_capturePath.clear();
}

} // namespace eng
