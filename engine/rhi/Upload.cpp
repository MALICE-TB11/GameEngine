#include "rhi/Upload.h"

#include "rhi/Device.h"

#include <algorithm>
#include <bit>
#include <cstring>

namespace eng::rhi {

namespace {
// 上传的贴图会被哪些阶段读取：顶点着色器（地形高度图）、片元着色器（材质）、计算着色器（M5 起的 FFT 海洋等）。
// barrier 的目标阶段必须覆盖所有实际读者，否则没被覆盖的那个读者与上传之间没有同步。
constexpr VkPipelineStageFlags2 kTextureConsumers =
    VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
} // namespace

Buffer createBufferWithData(Device& device, const void* data, VkDeviceSize size, VkBufferUsageFlags usage, const char* name) {
    Buffer staging = createBuffer(device, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemoryUsage::Upload, "Staging");
    std::memcpy(staging.mapped, data, size);
    vmaFlushAllocation(device.allocator(), staging.allocation, 0, VK_WHOLE_SIZE);

    Buffer buffer = createBuffer(device, size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryUsage::GpuOnly, name);
    device.immediateSubmit([&](VkCommandBuffer cmd) {
        const VkBufferCopy region{0, 0, size};
        vkCmdCopyBuffer(cmd, staging.buffer, buffer.buffer, 1, &region);
        // 「执行完成」≠「后续读取可见」：fence 只告诉 CPU 拷贝做完了，并不会把拷贝的写入
        // 对之后提交里的顶点/索引/着色器读取变得可见。必须有一个内存依赖覆盖实际的消费者。
        // pipeline barrier 的第二个同步范围包括同一队列上「提交顺序更晚」的所有命令，所以放在这里就够了。
        // 消费者不确定（顶点、索引、uniform、storage 都可能）→ 目标用 ALL_COMMANDS / MEMORY_READ，一次性上传不在乎这点开销。
        VkMemoryBarrier2 visible{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        visible.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        visible.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        visible.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        visible.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &visible;
        vkCmdPipelineBarrier2(cmd, &dep);
    });
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
        const ImageState shaderRead{kTextureConsumers,
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

Image createTextureFromMips(Device& device, VkFormat format, uint32_t width, uint32_t height,
                            const std::vector<std::vector<uint8_t>>& mips, const char* name) {
    ImageDesc desc;
    desc.width = width;
    desc.height = height;
    desc.format = format;
    desc.mipLevels = static_cast<uint32_t>(mips.size());
    desc.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    desc.name = name;
    Image image = createImage(device, desc);

    // 所有 mip 拼进一个 staging 缓冲，一次提交拷完
    VkDeviceSize total = 0;
    for (const auto& m : mips) total += m.size();
    Buffer staging = createBuffer(device, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemoryUsage::Upload, "MipStaging");
    std::vector<VkBufferImageCopy> regions;
    VkDeviceSize offset = 0;
    for (uint32_t m = 0; m < mips.size(); ++m) {
        std::memcpy(static_cast<uint8_t*>(staging.mapped) + offset, mips[m].data(), mips[m].size());
        VkBufferImageCopy r{};
        r.bufferOffset = offset;
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1};
        // 图像尺寸写真实的 mip 尺寸（可以小于 4×4 的压缩块），驱动按块对齐读取缓冲
        r.imageExtent = {std::max(1u, width >> m), std::max(1u, height >> m), 1};
        regions.push_back(r);
        offset += mips[m].size();
    }
    vmaFlushAllocation(device.allocator(), staging.allocation, 0, VK_WHOLE_SIZE);

    device.immediateSubmit([&](VkCommandBuffer cmd) {
        const ImageState undefined{VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, VK_IMAGE_LAYOUT_UNDEFINED};
        const ImageState dst{VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL};
        const ImageState shaderRead{kTextureConsumers,
                                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        imageBarrier(cmd, image.image, VK_IMAGE_ASPECT_COLOR_BIT, undefined, dst);
        vkCmdCopyBufferToImage(cmd, staging.buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               static_cast<uint32_t>(regions.size()), regions.data());
        imageBarrier(cmd, image.image, VK_IMAGE_ASPECT_COLOR_BIT, dst, shaderRead);
    });
    destroyBuffer(device, staging);
    return image;
}

Image createTextureArray(Device& device, VkFormat format, uint32_t width, uint32_t height, uint32_t layers, const void* data,
                         uint32_t bytesPerTexel, const char* name) {
    ImageDesc desc;
    desc.width = width;
    desc.height = height;
    desc.format = format;
    desc.layers = layers;
    desc.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    desc.name = name;
    Image image = createImage(device, desc);

    const VkDeviceSize size = VkDeviceSize(width) * height * layers * bytesPerTexel;
    Buffer staging = createBuffer(device, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemoryUsage::Upload, "ArrayStaging");
    std::memcpy(staging.mapped, data, size);
    vmaFlushAllocation(device.allocator(), staging.allocation, 0, VK_WHOLE_SIZE);

    device.immediateSubmit([&](VkCommandBuffer cmd) {
        const ImageState undefined{VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, VK_IMAGE_LAYOUT_UNDEFINED};
        const ImageState dst{VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL};
        const ImageState shaderRead{kTextureConsumers, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        imageBarrier(cmd, image.image, VK_IMAGE_ASPECT_COLOR_BIT, undefined, dst);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers}; // 缓冲里各层紧挨着，一次拷完
        region.imageExtent = {width, height, 1};
        vkCmdCopyBufferToImage(cmd, staging.buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        imageBarrier(cmd, image.image, VK_IMAGE_ASPECT_COLOR_BIT, dst, shaderRead);
    });
    destroyBuffer(device, staging);
    return image;
}

} // namespace eng::rhi
