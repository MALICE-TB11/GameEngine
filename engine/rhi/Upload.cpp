#include "rhi/Upload.h"

#include "rhi/Device.h"

#include <algorithm>
#include <bit>
#include <cstring>

namespace eng::rhi {

Buffer createBufferWithData(Device& device, const void* data, VkDeviceSize size, VkBufferUsageFlags usage, const char* name) {
    Buffer staging = createBuffer(device, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemoryUsage::Upload, "Staging");
    std::memcpy(staging.mapped, data, size);
    vmaFlushAllocation(device.allocator(), staging.allocation, 0, VK_WHOLE_SIZE);

    Buffer buffer = createBuffer(device, size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryUsage::GpuOnly, name);
    device.immediateSubmit([&](VkCommandBuffer cmd) {
        const VkBufferCopy region{0, 0, size};
        vkCmdCopyBuffer(cmd, staging.buffer, buffer.buffer, 1, &region);
    });
    // immediateSubmit 内部等待了 fence，且之后使用该缓冲的命令在更晚的提交里，
    // 队列提交顺序 + fence 已保证拷贝完成；顶点读取不需要额外 barrier。
    destroyBuffer(device, staging);
    return buffer;
}

Image createTexture2D(Device& device, const void* rgba8, uint32_t width, uint32_t height, VkFormat format,
                      bool generateMips, const char* name) {
    // mip 级数 = floor(log2(max(w,h))) + 1
    const uint32_t mips = generateMips ? std::bit_width(std::max(width, height)) : 1u;
    ImageDesc desc;
    desc.width = width;
    desc.height = height;
    desc.format = format;
    desc.mipLevels = mips;
    desc.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    desc.name = name;
    Image image = createImage(device, desc);

    const VkDeviceSize size = VkDeviceSize(width) * height * 4;
    Buffer staging = createBuffer(device, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemoryUsage::Upload, "TextureStaging");
    std::memcpy(staging.mapped, rgba8, size);
    vmaFlushAllocation(device.allocator(), staging.allocation, 0, VK_WHOLE_SIZE);

    device.immediateSubmit([&](VkCommandBuffer cmd) {
        auto mipBarrier = [&](uint32_t mip, const ImageState& from, const ImageState& to) {
            VkImageMemoryBarrier2 b = makeImageBarrier(image.image, VK_IMAGE_ASPECT_COLOR_BIT, from, to);
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, 1};
            VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = &b;
            vkCmdPipelineBarrier2(cmd, &dep);
        };
        const ImageState undefined{VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, VK_IMAGE_LAYOUT_UNDEFINED};
        const ImageState dst{VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL};
        const ImageState src{VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
        const ImageState shaderRead{VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

        for (uint32_t m = 0; m < mips; ++m) mipBarrier(m, undefined, dst);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {width, height, 1};
        vkCmdCopyBufferToImage(cmd, staging.buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        // 逐级生成 mip：第 m-1 级（已写好，转为 TRANSFER_SRC）线性缩小一半 blit 到第 m 级
        int32_t w = int32_t(width), h = int32_t(height);
        for (uint32_t m = 1; m < mips; ++m) {
            mipBarrier(m - 1, dst, src);
            const int32_t nw = std::max(1, w / 2), nh = std::max(1, h / 2);
            VkImageBlit blit{};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m - 1, 0, 1};
            blit.srcOffsets[1] = {w, h, 1};
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1};
            blit.dstOffsets[1] = {nw, nh, 1};
            vkCmdBlitImage(cmd, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
            mipBarrier(m - 1, src, shaderRead);
            w = nw;
            h = nh;
        }
        mipBarrier(mips - 1, dst, shaderRead);
    });
    destroyBuffer(device, staging);
    return image;
}

} // namespace eng::rhi
