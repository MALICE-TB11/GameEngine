#pragma once
// Vulkan 公共头：所有 rhi 以上的代码都通过它使用 Vulkan。
//
// 为什么用 volk 而不是链接 vulkan-1.lib：
//   通过 loader 的导出函数调用 vkCmdDraw 等设备函数，每次都要经过 loader 的分发跳板。
//   volk 在创建设备后直接取设备级函数指针（volkLoadDevice），省掉这一层；
//   同时我们不需要在链接期依赖 vulkan-1.lib，缺驱动时也能优雅报错而不是启动即崩。

#include <volk.h>

#include <cstdint>

namespace eng::rhi {

const char* toString(VkResult result);

// 设置调试名。RenderDoc 和 validation 报错里会显示这个名字，而不是一串十六进制句柄。
// 「所有 Vulkan 对象都要有名字」是本引擎的调试约定。
void setDebugName(VkDevice device, VkObjectType type, uint64_t handle, const char* name);

template <class T> struct ObjectType;
#define ENG_VK_OBJECT_TYPE(Handle, Enum) \
    template <> struct ObjectType<Handle> { static constexpr VkObjectType value = Enum; };
ENG_VK_OBJECT_TYPE(VkBuffer, VK_OBJECT_TYPE_BUFFER)
ENG_VK_OBJECT_TYPE(VkImage, VK_OBJECT_TYPE_IMAGE)
ENG_VK_OBJECT_TYPE(VkImageView, VK_OBJECT_TYPE_IMAGE_VIEW)
ENG_VK_OBJECT_TYPE(VkSampler, VK_OBJECT_TYPE_SAMPLER)
ENG_VK_OBJECT_TYPE(VkPipeline, VK_OBJECT_TYPE_PIPELINE)
ENG_VK_OBJECT_TYPE(VkPipelineLayout, VK_OBJECT_TYPE_PIPELINE_LAYOUT)
ENG_VK_OBJECT_TYPE(VkShaderModule, VK_OBJECT_TYPE_SHADER_MODULE)
ENG_VK_OBJECT_TYPE(VkDescriptorSetLayout, VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT)
ENG_VK_OBJECT_TYPE(VkDescriptorPool, VK_OBJECT_TYPE_DESCRIPTOR_POOL)
ENG_VK_OBJECT_TYPE(VkDescriptorSet, VK_OBJECT_TYPE_DESCRIPTOR_SET)
ENG_VK_OBJECT_TYPE(VkCommandPool, VK_OBJECT_TYPE_COMMAND_POOL)
ENG_VK_OBJECT_TYPE(VkCommandBuffer, VK_OBJECT_TYPE_COMMAND_BUFFER)
ENG_VK_OBJECT_TYPE(VkFence, VK_OBJECT_TYPE_FENCE)
ENG_VK_OBJECT_TYPE(VkSemaphore, VK_OBJECT_TYPE_SEMAPHORE)
ENG_VK_OBJECT_TYPE(VkQueryPool, VK_OBJECT_TYPE_QUERY_POOL)
ENG_VK_OBJECT_TYPE(VkQueue, VK_OBJECT_TYPE_QUEUE)
ENG_VK_OBJECT_TYPE(VkSwapchainKHR, VK_OBJECT_TYPE_SWAPCHAIN_KHR)
#undef ENG_VK_OBJECT_TYPE

template <class T>
void setDebugName(VkDevice device, T handle, const char* name) {
    setDebugName(device, ObjectType<T>::value, reinterpret_cast<uint64_t>(handle), name);
}

// 命令缓冲里的调试标签：RenderDoc 的事件树按这些标签分组（每个 render graph pass 一个）
void beginLabel(VkCommandBuffer cmd, const char* name, const float color[4] = nullptr);
void endLabel(VkCommandBuffer cmd);

[[noreturn]] void vkCheckFailed(VkResult result, const char* expr, const char* file, int line);

} // namespace eng::rhi

// 包住返回 VkResult 的调用。失败视为不可恢复（设备丢失、显存耗尽等），直接终止并给出位置。
#define ENG_CHECK_VK(expr)                                                              \
    do {                                                                                \
        const VkResult eng_vk_result_ = (expr);                                         \
        if (eng_vk_result_ < 0) ::eng::rhi::vkCheckFailed(eng_vk_result_, #expr, __FILE__, __LINE__); \
    } while (0)
