#pragma once
// 交换链：窗口背后的一组图像，GPU 轮流往里画、显示器轮流显示。
//
// 两个常见坑在这里处理：
//   1. 窗口缩放后交换链「过期」（OUT_OF_DATE）必须重建；最小化时尺寸为 0，不能重建，要等恢复。
//   2. 「渲染完成」信号量必须按交换链图像（而不是按帧）分配：present 什么时候真正用完这个信号量
//      应用无从得知，只有等同一张图像再次被 acquire 时才能确定它空闲了。

#include "rhi/VkCommon.h"

#include <vector>

namespace eng::rhi {

class Device;

class Swapchain {
public:
    bool create(Device& device, uint32_t width, uint32_t height, bool vsync);
    // 窗口尺寸变化后调用。内部会 vkDeviceWaitIdle（缩放不频繁，简单可靠优先）
    bool recreate(uint32_t width, uint32_t height, bool vsync);
    void destroy();

    VkSwapchainKHR handle() const { return m_swapchain; }
    VkFormat format() const { return m_format; }
    VkExtent2D extent() const { return m_extent; }
    uint32_t imageCount() const { return static_cast<uint32_t>(m_images.size()); }
    VkImage image(uint32_t i) const { return m_images[i]; }
    VkImageView view(uint32_t i) const { return m_views[i]; }
    VkSemaphore renderFinished(uint32_t i) const { return m_renderFinished[i]; }
    VkPresentModeKHR presentMode() const { return m_presentMode; }
    bool vsync() const { return m_vsync; }

private:
    bool build(uint32_t width, uint32_t height, bool vsync, VkSwapchainKHR oldSwapchain);
    void destroyViews();

    Device* m_device = nullptr;
    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    VkFormat m_format = VK_FORMAT_UNDEFINED;
    VkExtent2D m_extent{};
    VkPresentModeKHR m_presentMode = VK_PRESENT_MODE_FIFO_KHR;
    bool m_vsync = true;
    std::vector<VkImage> m_images;
    std::vector<VkImageView> m_views;
    std::vector<VkSemaphore> m_renderFinished;
};

} // namespace eng::rhi
