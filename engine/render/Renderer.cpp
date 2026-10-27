#include "render/Renderer.h"

#include "core/ImageIO.h"
#include "core/Log.h"
#include "platform/Window.h"

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
    }
    return true;
}

void Renderer::shutdown() {
    if (!m_device) return;
    VkDevice dev = m_device->device();
    vkDeviceWaitIdle(dev);
    rhi::destroyBuffer(*m_device, m_captureBuffer);
    for (FrameResources& f : m_frames) {
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

bool Renderer::drawFrame() {
    // 窗口缩放或 vsync 设置变化：先重建交换链，本帧跳过
    if (m_window->consumeResized() || !m_swapchain.handle() || m_swapchain.vsync() != m_settings.vsync) {
        recreateSwapchain();
        return false;
    }

    VkDevice dev = m_device->device();
    FrameResources& frame = m_frames[m_frameNumber % rhi::kFramesInFlight];

    // 1. 等待这个槽位上一次提交的命令执行完毕
    ENG_CHECK_VK(vkWaitForFences(dev, 1, &frame.inFlight, VK_TRUE, UINT64_MAX));
    m_device->beginFrame(m_frameNumber); // 现在可以安全销毁两帧前登记的对象了

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

    // 3. 录制
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    ENG_CHECK_VK(vkBeginCommandBuffer(frame.cmd, &bi));
    recordFrame(frame.cmd, imageIndex);
    ENG_CHECK_VK(vkEndCommandBuffer(frame.cmd));

    // 4. 提交
    // waitSemaphore 的 stageMask = COLOR_ATTACHMENT_OUTPUT：只有「写颜色附件」这一步需要等图像可用，
    // 之前的顶点处理等阶段可以提前开始。
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

    if (!m_capturePath.empty() && m_captureBuffer) {
        // 截图：等这一帧执行完再读回（只在截图那一帧同步，平时不影响性能）
        ENG_CHECK_VK(vkWaitForFences(dev, 1, &frame.inFlight, VK_TRUE, UINT64_MAX));
        finishCapture();
    }

    // 5. 呈现
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

void Renderer::recordFrame(VkCommandBuffer cmd, uint32_t imageIndex) {
    VkImage image = m_swapchain.image(imageIndex);

    // 交换链图像：UNDEFINED → COLOR_ATTACHMENT_OPTIMAL。
    // 旧布局写 UNDEFINED 表示「不关心原内容」，驱动可以跳过保留数据的工作（反正马上要清屏）。
    // src 阶段是 COLOR_ATTACHMENT_OUTPUT：与 submit 时等待 imageAvailable 的阶段相同，
    // 这样布局转换一定发生在图像可用之后。
    const rhi::ImageState undefinedState{VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_NONE, VK_IMAGE_LAYOUT_UNDEFINED};
    const rhi::ImageState colorState{VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT,
                                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    rhi::imageBarrier(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, undefinedState, colorState);

    // dynamic rendering：直接指定附件开始渲染，不需要预先创建 VkRenderPass/VkFramebuffer
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = m_swapchain.view(imageIndex);
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    const vec4 c = m_settings.clearColor;
    color.clearValue.color = {{c.r, c.g, c.b, c.a}};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, m_swapchain.extent()};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;

    rhi::beginLabel(cmd, "ClearAndOverlay");
    vkCmdBeginRendering(cmd, &ri);
    if (m_overlay) m_overlay(cmd);
    vkCmdEndRendering(cmd);
    rhi::endLabel(cmd);

    if (!m_capturePath.empty()) recordCapture(cmd, imageIndex);

    // → PRESENT_SRC：呈现引擎要求的布局。dst 阶段写 NONE：present 不是管线阶段，
    // 它与渲染的同步由 renderFinished 信号量负责。
    const rhi::ImageState presentState{VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR};
    const rhi::ImageState current = m_capturePath.empty()
        ? colorState
        : rhi::ImageState{VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
    rhi::imageBarrier(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, current, presentState);
}

void Renderer::recordCapture(VkCommandBuffer cmd, uint32_t imageIndex) {
    const VkExtent2D ext = m_swapchain.extent();
    const VkDeviceSize size = VkDeviceSize(ext.width) * ext.height * 4;
    if (m_captureBuffer.size < size) {
        rhi::destroyBuffer(*m_device, m_captureBuffer);
        m_captureBuffer = rhi::createBuffer(*m_device, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::MemoryUsage::Readback, "CaptureReadback");
    }
    VkImage image = m_swapchain.image(imageIndex);
    rhi::imageBarrier(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT,
                      {VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                      {VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL});
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {ext.width, ext.height, 1};
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_captureBuffer.buffer, 1, &region);

    // fence 只保证 GPU 执行完，不保证 GPU 的写入对 CPU 可见；
    // 必须有一个 dst 为 HOST 的 barrier，把拷贝结果「推」到 CPU 可见的域。
    VkMemoryBarrier2 toHost{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    toHost.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    toHost.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    toHost.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    toHost.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &toHost;
    vkCmdPipelineBarrier2(cmd, &dep);
}

void Renderer::finishCapture() {
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
