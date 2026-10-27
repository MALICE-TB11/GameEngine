#pragma once
// GPU 资源：Buffer 与 Image（VMA 分配），以及 image barrier 辅助函数。
// 刻意做成「普通结构体 + 自由函数」而不是 RAII 类：
// GPU 资源的销毁时机由「GPU 是否还在用」决定（见 Device::deferDestroy），
// C++ 析构函数的时机由 CPU 作用域决定，两者不一致，RAII 反而容易在错误的时刻销毁。

#include "rhi/VkCommon.h"

#include <vk_mem_alloc.h>

namespace eng::rhi {

class Device;

enum class MemoryUsage {
    GpuOnly,  // 显存，CPU 不可见：顶点缓冲、贴图、render target
    Upload,   // CPU 写 → GPU 读：staging、每帧常量
    Readback, // GPU 写 → CPU 读：截图、GPU 数据回读
};

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    void* mapped = nullptr; // Upload/Readback 内存创建时就持久映射，直接读写
    explicit operator bool() const { return buffer != VK_NULL_HANDLE; }
};

Buffer createBuffer(Device& device, VkDeviceSize size, VkBufferUsageFlags usage, MemoryUsage memory, const char* name);
void destroyBuffer(Device& device, Buffer& buffer);

struct ImageDesc {
    uint32_t width = 1;
    uint32_t height = 1;
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    uint32_t mipLevels = 1;
    const char* name = "Image";
};

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    uint32_t mipLevels = 1;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    explicit operator bool() const { return image != VK_NULL_HANDLE; }
};

Image createImage(Device& device, const ImageDesc& desc);
void destroyImage(Device& device, Image& image);

bool isDepthFormat(VkFormat format);
VkImageAspectFlags aspectOf(VkFormat format);

// 一个 image 的「同步状态」：最后一次访问发生在哪个管线阶段、以什么方式访问、处于什么布局。
// barrier 本质上就是「从状态 A 过渡到状态 B」：
//   srcStage/srcAccess：等哪些之前的操作完成、让它们的写入可用（available）
//   dstStage/dstAccess：之后哪些操作要等待、让写入对它们可见（visible）
//   oldLayout/newLayout：图像在内存中的排布方式（不同用途有不同的最优排布）
struct ImageState {
    VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2 access = VK_ACCESS_2_NONE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

VkImageMemoryBarrier2 makeImageBarrier(VkImage image, VkImageAspectFlags aspect, const ImageState& from, const ImageState& to);
void imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, const ImageState& from, const ImageState& to);

} // namespace eng::rhi
