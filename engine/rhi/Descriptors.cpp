#include "rhi/Descriptors.h"

#include "rhi/Device.h"

#include <string>

namespace eng::rhi {

VkDescriptorSetLayout createSetLayout(Device& device, std::initializer_list<VkDescriptorSetLayoutBinding> bindings, const char* name) {
    VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.bindingCount = static_cast<uint32_t>(bindings.size());
    ci.pBindings = bindings.begin();
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    ENG_CHECK_VK(vkCreateDescriptorSetLayout(device.device(), &ci, nullptr, &layout));
    setDebugName(device.device(), layout, name);
    return layout;
}

void DescriptorAllocator::init(Device& device, const char* name) {
    m_device = &device;
    m_name = name;
    m_pools.push_back(newPool());
}

void DescriptorAllocator::shutdown() {
    if (!m_device) return; // init 失败（或从没 init）时也能安全调用
    for (VkDescriptorPool p : m_pools) vkDestroyDescriptorPool(m_device->device(), p, nullptr);
    m_pools.clear();
}

void DescriptorAllocator::reset() {
    // 调用者保证这一帧的 GPU 工作已结束（已等过该帧 fence）
    for (VkDescriptorPool p : m_pools) vkResetDescriptorPool(m_device->device(), p, 0);
    m_current = 0;
}

VkDescriptorPool DescriptorAllocator::newPool() {
    // 各类型的数量按「一帧够用」粗估；不够时 allocate 会自动开新池
    const VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 512},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 64},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 256},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 64},
        {VK_DESCRIPTOR_TYPE_SAMPLER, 32},
    };
    VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    ci.maxSets = 256;
    ci.poolSizeCount = static_cast<uint32_t>(std::size(sizes));
    ci.pPoolSizes = sizes;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    ENG_CHECK_VK(vkCreateDescriptorPool(m_device->device(), &ci, nullptr, &pool));
    setDebugName(m_device->device(), pool, (std::string(m_name) + std::to_string(m_pools.size())).c_str());
    return pool;
}

VkDescriptorSet DescriptorAllocator::allocate(VkDescriptorSetLayout layout) {
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &layout;
    while (true) {
        ai.descriptorPool = m_pools[m_current];
        VkDescriptorSet set = VK_NULL_HANDLE;
        const VkResult r = vkAllocateDescriptorSets(m_device->device(), &ai, &set);
        if (r == VK_SUCCESS) return set;
        if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL) ENG_CHECK_VK(r);
        // 当前池满：换下一个，没有就新建
        if (++m_current == m_pools.size()) m_pools.push_back(newPool());
    }
}

DescriptorWriter& DescriptorWriter::sampledImage(uint32_t binding, VkImageView view, VkImageLayout layout) {
    m_images.push_back({VK_NULL_HANDLE, view, layout});
    m_entries.push_back({binding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, true, m_images.size() - 1});
    return *this;
}

DescriptorWriter& DescriptorWriter::storageImage(uint32_t binding, VkImageView view) {
    m_images.push_back({VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL});
    m_entries.push_back({binding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, true, m_images.size() - 1});
    return *this;
}

DescriptorWriter& DescriptorWriter::uniformBuffer(uint32_t binding, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize range) {
    m_buffers.push_back({buffer, offset, range});
    m_entries.push_back({binding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, false, m_buffers.size() - 1});
    return *this;
}

DescriptorWriter& DescriptorWriter::storageBuffer(uint32_t binding, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize range) {
    m_buffers.push_back({buffer, offset, range});
    m_entries.push_back({binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, false, m_buffers.size() - 1});
    return *this;
}

void DescriptorWriter::update(VkDevice device, VkDescriptorSet set) {
    // 到这里 m_images/m_buffers 不会再扩容，取地址是安全的
    std::vector<VkWriteDescriptorSet> writes;
    writes.reserve(m_entries.size());
    for (const Entry& e : m_entries) {
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set;
        w.dstBinding = e.binding;
        w.descriptorCount = 1;
        w.descriptorType = e.type;
        if (e.isImage)
            w.pImageInfo = &m_images[e.index];
        else
            w.pBufferInfo = &m_buffers[e.index];
        writes.push_back(w);
    }
    vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

} // namespace eng::rhi
