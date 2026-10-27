#pragma once
// CPU → GPU 数据上传（同步，适合加载阶段）。
// 显存（DEVICE_LOCAL）通常 CPU 不可直接写：先写进 CPU 可见的 staging 缓冲，再由 GPU 拷贝过去。
// EXTEND: 运行时流式加载（大世界）时改成异步：专用传输队列 + 环形 staging 缓冲 + timeline 信号量。

#include "rhi/Resources.h"

namespace eng::rhi {

class Device;

// 创建 GPU 缓冲并上传数据
Buffer createBufferWithData(Device& device, const void* data, VkDeviceSize size, VkBufferUsageFlags usage, const char* name);

// 创建 2D 贴图并上传 RGBA8 像素；generateMips=true 时用逐级 blit 生成完整 mip 链。
// 返回的贴图处于 SHADER_READ_ONLY_OPTIMAL 布局。
Image createTexture2D(Device& device, const void* rgba8, uint32_t width, uint32_t height, VkFormat format,
                      bool generateMips, const char* name);

} // namespace eng::rhi
