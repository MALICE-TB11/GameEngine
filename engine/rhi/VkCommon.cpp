#include "rhi/VkCommon.h"

#include "core/Log.h"

#include <cstdlib>

namespace eng::rhi {

const char* toString(VkResult r) {
    switch (r) {
#define CASE(x) \
    case x: return #x;
        CASE(VK_SUCCESS)
        CASE(VK_NOT_READY)
        CASE(VK_TIMEOUT)
        CASE(VK_INCOMPLETE)
        CASE(VK_SUBOPTIMAL_KHR)
        CASE(VK_ERROR_OUT_OF_HOST_MEMORY)
        CASE(VK_ERROR_OUT_OF_DEVICE_MEMORY)
        CASE(VK_ERROR_INITIALIZATION_FAILED)
        CASE(VK_ERROR_DEVICE_LOST)
        CASE(VK_ERROR_MEMORY_MAP_FAILED)
        CASE(VK_ERROR_LAYER_NOT_PRESENT)
        CASE(VK_ERROR_EXTENSION_NOT_PRESENT)
        CASE(VK_ERROR_FEATURE_NOT_PRESENT)
        CASE(VK_ERROR_INCOMPATIBLE_DRIVER)
        CASE(VK_ERROR_TOO_MANY_OBJECTS)
        CASE(VK_ERROR_FORMAT_NOT_SUPPORTED)
        CASE(VK_ERROR_SURFACE_LOST_KHR)
        CASE(VK_ERROR_OUT_OF_DATE_KHR)
        CASE(VK_ERROR_VALIDATION_FAILED_EXT)
#undef CASE
    default: return "VK_ERROR_<unknown>";
    }
}

void setDebugName(VkDevice device, VkObjectType type, uint64_t handle, const char* name) {
    // 没有启用 VK_EXT_debug_utils 时函数指针为空，静默跳过
    if (!vkSetDebugUtilsObjectNameEXT || !handle || !name) return;
    VkDebugUtilsObjectNameInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
    info.objectType = type;
    info.objectHandle = handle;
    info.pObjectName = name;
    vkSetDebugUtilsObjectNameEXT(device, &info);
}

void beginLabel(VkCommandBuffer cmd, const char* name, const float color[4]) {
    if (!vkCmdBeginDebugUtilsLabelEXT) return;
    VkDebugUtilsLabelEXT label{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
    label.pLabelName = name;
    if (color)
        for (int i = 0; i < 4; ++i) label.color[i] = color[i];
    vkCmdBeginDebugUtilsLabelEXT(cmd, &label);
}

void endLabel(VkCommandBuffer cmd) {
    if (vkCmdEndDebugUtilsLabelEXT) vkCmdEndDebugUtilsLabelEXT(cmd);
}

void vkCheckFailed(VkResult result, const char* expr, const char* file, int line) {
    spdlog::critical("Vulkan 调用失败: {} 返回 {}\n  位置: {}:{}", expr, toString(result), file, line);
    spdlog::default_logger()->flush();
    std::abort();
}

} // namespace eng::rhi
