#pragma once
// 描述符（descriptor）：告诉 shader「binding N 对应哪个缓冲/贴图」。
//
// 本引擎的约定（所有 shader 都遵守，见 shaders/common/frame.hlsli）：
//   set 0：全局，每帧一份 —— FrameConstants 常量缓冲 + 固定的采样器
//   set 1：每个 pass / 每个材质自己的资源，每帧从「帧描述符池」临时分配
//   push constant：每次 draw 的小数据（模型矩阵等），128 字节
//
// 为什么 set 1 每帧重新分配而不是缓存：Render Graph 的瞬态贴图、被关掉的 pass 换成默认贴图……
// 每帧绑定的东西都可能不同。每帧整池 reset 再分配，逻辑最简单，开销也很小（几十个 set）。
// EXTEND: 物体数量上万时，改成 bindless（一个大数组 + 下标），避免每个 draw 一个 set。

#include "rhi/VkCommon.h"

#include <initializer_list>
#include <vector>

namespace eng::rhi {

class Device;

VkDescriptorSetLayout createSetLayout(Device& device, std::initializer_list<VkDescriptorSetLayoutBinding> bindings, const char* name);

// 每帧一个：帧开始时 reset，之后随用随分配；池子满了自动再开一个
class DescriptorAllocator {
public:
    void init(Device& device, const char* name);
    void shutdown();
    void reset();
    VkDescriptorSet allocate(VkDescriptorSetLayout layout);

private:
    VkDescriptorPool newPool();

    Device* m_device = nullptr;
    const char* m_name = "";
    std::vector<VkDescriptorPool> m_pools;
    size_t m_current = 0;
};

// 收集若干写入，一次 vkUpdateDescriptorSets 提交
class DescriptorWriter {
public:
    DescriptorWriter& sampledImage(uint32_t binding, VkImageView view, VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    DescriptorWriter& storageImage(uint32_t binding, VkImageView view);
    DescriptorWriter& uniformBuffer(uint32_t binding, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize range);
    DescriptorWriter& storageBuffer(uint32_t binding, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize range);
    void update(VkDevice device, VkDescriptorSet set);

private:
    // 写入信息要在 update 时仍然有效，所以用 vector 存起来（预留空间避免扩容导致指针失效）
    struct Entry {
        uint32_t binding;
        VkDescriptorType type;
        bool isImage;
        size_t index;
    };
    std::vector<VkDescriptorImageInfo> m_images;
    std::vector<VkDescriptorBufferInfo> m_buffers;
    std::vector<Entry> m_entries;
};

} // namespace eng::rhi
