// VMA 是单头文件库：在且仅在一个 .cpp 里定义 VMA_IMPLEMENTATION。
// VMA_STATIC_VULKAN_FUNCTIONS=0 / VMA_DYNAMIC_VULKAN_FUNCTIONS=1 在 CMake 里定义：
// 让 VMA 通过我们传入的 vkGetInstanceProcAddr/vkGetDeviceProcAddr 自己取函数指针（与 volk 配合）。
#include "rhi/VkCommon.h"

#define VMA_IMPLEMENTATION
#pragma warning(push, 0)
#include <vk_mem_alloc.h>
#pragma warning(pop)
