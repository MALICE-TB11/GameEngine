#include "rhi/Resources.h"

#include "core/Assert.h"
#include "rhi/Device.h"

#include <algorithm>
#include <string>

namespace eng::rhi {

Buffer createBuffer(Device& device, VkDeviceSize size, VkBufferUsageFlags usage, MemoryUsage memory, const char* name) {
    ENG_ASSERT(size > 0, "不能创建大小为 0 的缓冲");
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    switch (memory) {
    case MemoryUsage::GpuOnly: ai.flags = 0; break;
    case MemoryUsage::Upload:
        // 顺序写入：CPU 只写不读，驱动可以用 write-combined 内存，写入更快
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        break;
    case MemoryUsage::Readback:
        // 随机读取：需要带 CPU 缓存的内存，否则逐字节读会极慢
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        break;
    }

    Buffer buf;
    VmaAllocationInfo info{};
    ENG_CHECK_VK(vmaCreateBuffer(device.allocator(), &bi, &ai, &buf.buffer, &buf.allocation, &info));
    buf.size = size;
    buf.mapped = info.pMappedData;
    setDebugName(device.device(), buf.buffer, name);
    vmaSetAllocationName(device.allocator(), buf.allocation, name);
    return buf;
}

void destroyBuffer(Device& device, Buffer& buffer) {
    if (buffer.buffer) vmaDestroyBuffer(device.allocator(), buffer.buffer, buffer.allocation);
    buffer = {};
}

bool isDepthFormat(VkFormat f) {
    return f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT ||
           f == VK_FORMAT_D16_UNORM;
}

VkImageAspectFlags aspectOf(VkFormat f) {
    return isDepthFormat(f) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
}

Image createImage(Device& device, const ImageDesc& desc) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = desc.format;
    ci.extent = {desc.width, desc.height, 1};
    ci.mipLevels = desc.mipLevels;
    ci.arrayLayers = std::max(1u, desc.layers);
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = desc.usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    // render target 给独立的内存块：它们大且常被重建，独占分配避免碎片
    if (desc.usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))
        ai.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

    Image img;
    ENG_CHECK_VK(vmaCreateImage(device.allocator(), &ci, &ai, &img.image, &img.allocation, nullptr));
    img.format = desc.format;
    img.extent = {desc.width, desc.height};
    img.mipLevels = desc.mipLevels;
    img.layers = ci.arrayLayers;
    img.aspect = aspectOf(desc.format);
    setDebugName(device.device(), img.image, desc.name);
    vmaSetAllocationName(device.allocator(), img.allocation, desc.name);

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img.image;
    vi.viewType = img.layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    vi.format = desc.format;
    vi.subresourceRange = {img.aspect, 0, desc.mipLevels, 0, img.layers};
    ENG_CHECK_VK(vkCreateImageView(device.device(), &vi, nullptr, &img.view));
    setDebugName(device.device(), img.view, (std::string(desc.name) + "View").c_str());
    return img;
}

void destroyImage(Device& device, Image& image) {
    if (image.view) vkDestroyImageView(device.device(), image.view, nullptr);
    if (image.image) vmaDestroyImage(device.allocator(), image.image, image.allocation);
    image = {};
}

VkImageMemoryBarrier2 makeImageBarrier(VkImage image, VkImageAspectFlags aspect, const ImageState& from, const ImageState& to) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = from.stage;
    b.srcAccessMask = from.access;
    b.dstStageMask = to.stage;
    b.dstAccessMask = to.access;
    b.oldLayout = from.layout;
    b.newLayout = to.layout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    return b;
}

void imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, const ImageState& from, const ImageState& to) {
    const VkImageMemoryBarrier2 b = makeImageBarrier(image, aspect, from, to);
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

} // namespace eng::rhi
