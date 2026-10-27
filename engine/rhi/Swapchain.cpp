#include "rhi/Swapchain.h"

#include "core/Log.h"
#include "rhi/Device.h"

#include <algorithm>
#include <string>

namespace eng::rhi {

bool Swapchain::create(Device& device, uint32_t width, uint32_t height, bool vsync) {
    m_device = &device;
    return build(width, height, vsync, VK_NULL_HANDLE);
}

bool Swapchain::recreate(uint32_t width, uint32_t height, bool vsync) {
    // 等 GPU 空闲：旧交换链图像可能还被正在执行的帧引用
    vkDeviceWaitIdle(m_device->device());
    destroyViews();
    VkSwapchainKHR old = m_swapchain;
    m_swapchain = VK_NULL_HANDLE; // build 失败（例如尺寸为 0）时保持为空，调用方据此跳过渲染
    // 把旧交换链传给新交换链（oldSwapchain），驱动可以复用资源，过渡更平滑
    const bool ok = build(width, height, vsync, old);
    if (old) vkDestroySwapchainKHR(m_device->device(), old, nullptr);
    return ok;
}

void Swapchain::destroy() {
    if (!m_device || !m_swapchain) return;
    destroyViews();
    vkDestroySwapchainKHR(m_device->device(), m_swapchain, nullptr);
    m_swapchain = VK_NULL_HANDLE;
}

void Swapchain::destroyViews() {
    VkDevice dev = m_device->device();
    for (VkImageView v : m_views) vkDestroyImageView(dev, v, nullptr);
    for (VkSemaphore s : m_renderFinished) vkDestroySemaphore(dev, s, nullptr);
    m_views.clear();
    m_renderFinished.clear();
    m_images.clear();
}

bool Swapchain::build(uint32_t width, uint32_t height, bool vsync, VkSwapchainKHR oldSwapchain) {
    VkPhysicalDevice phys = m_device->physicalDevice();
    VkSurfaceKHR surface = m_device->surface();
    VkDevice dev = m_device->device();

    VkSurfaceCapabilitiesKHR caps{};
    ENG_CHECK_VK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps));

    // ---- 格式：选 B8G8R8A8_UNORM，而不是 _SRGB ---------------------------------------------
    // 为什么不用 sRGB 格式让硬件自动做 gamma 编码：ImGui 的颜色是按 sRGB 值设计的，
    // 画进 _SRGB 图像会被再编码一次而发白。所以交换链用 UNORM，
    // 由 tonemap pass 在 shader 里手动做线性 → sRGB 编码（见 shaders/common/color.hlsli）。
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &count, formats.data());
    VkSurfaceFormatKHR chosen = formats[0];
    for (const auto& f : formats) {
        if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = f;
            break;
        }
    }

    // ---- 呈现模式 ------------------------------------------------------------------------------
    // FIFO：垂直同步，所有设备都必须支持；MAILBOX：不撕裂且低延迟；IMMEDIATE：可能撕裂，测性能用
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &count, nullptr);
    std::vector<VkPresentModeKHR> modes(count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &count, modes.data());
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (!vsync) {
        if (std::ranges::find(modes, VK_PRESENT_MODE_MAILBOX_KHR) != modes.end())
            mode = VK_PRESENT_MODE_MAILBOX_KHR;
        else if (std::ranges::find(modes, VK_PRESENT_MODE_IMMEDIATE_KHR) != modes.end())
            mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    }

    // ---- 尺寸：currentExtent 为 0xFFFFFFFF 表示「由应用决定」 ---------------------------------
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        extent.width = std::clamp(width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0) return false; // 最小化中

    // 比最少多要一张：避免 CPU 等待驱动释放图像
    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0) imageCount = std::min(imageCount, caps.maxImageCount);

    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface;
    ci.minImageCount = imageCount;
    ci.imageFormat = chosen.format;
    ci.imageColorSpace = chosen.colorSpace;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    // COLOR_ATTACHMENT：最终 pass 渲染进来；TRANSFER_SRC：--capture 截图；TRANSFER_DST：blit/清屏
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = oldSwapchain;
    ENG_CHECK_VK(vkCreateSwapchainKHR(dev, &ci, nullptr, &m_swapchain));
    setDebugName(dev, m_swapchain, "Swapchain");

    m_format = chosen.format;
    m_extent = extent;
    m_presentMode = mode;
    m_vsync = vsync;

    vkGetSwapchainImagesKHR(dev, m_swapchain, &count, nullptr);
    m_images.resize(count);
    vkGetSwapchainImagesKHR(dev, m_swapchain, &count, m_images.data());

    m_views.resize(count);
    m_renderFinished.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        const std::string name = "SwapchainImage" + std::to_string(i);
        setDebugName(dev, m_images[i], name.c_str());

        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = m_images[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = m_format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        ENG_CHECK_VK(vkCreateImageView(dev, &vi, nullptr, &m_views[i]));
        setDebugName(dev, m_views[i], (name + "View").c_str());

        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        ENG_CHECK_VK(vkCreateSemaphore(dev, &si, nullptr, &m_renderFinished[i]));
        setDebugName(dev, m_renderFinished[i], ("RenderFinished" + std::to_string(i)).c_str());
    }

    static const char* kModeNames[] = {"IMMEDIATE", "MAILBOX", "FIFO", "FIFO_RELAXED"};
    LOG_INFO("交换链 {}x{}，{} 张图像，呈现模式 {}", extent.width, extent.height, count,
             mode <= VK_PRESENT_MODE_FIFO_RELAXED_KHR ? kModeNames[mode] : "?");
    return true;
}

} // namespace eng::rhi
