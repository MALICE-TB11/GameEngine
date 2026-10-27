#include "rhi/Device.h"

#include "core/Log.h"

#include <VkBootstrap.h>
#include <GLFW/glfw3.h>

namespace eng::rhi {
namespace {

// validation layer 的回调：按严重级别转发到日志。
// error 级别会被日志系统计数，冒烟测试据此判定失败。
VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    const char* msg = data && data->pMessage ? data->pMessage : "";
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        LOG_ERROR("[Vulkan] {}", msg);
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        LOG_WARN("[Vulkan] {}", msg);
    // 返回 VK_FALSE：不中断触发消息的那个 Vulkan 调用
    return VK_FALSE;
}

} // namespace

bool Device::init(const DeviceSettings& settings, GLFWwindow* window) {
    // ---- 1. 加载 vulkan-1.dll 并取得 vkGetInstanceProcAddr ----------------------------------
    if (volkInitialize() != VK_SUCCESS) {
        LOG_ERROR("找不到 Vulkan 运行时（vulkan-1.dll），请更新显卡驱动");
        return false;
    }

    // ---- 2. 实例 ---------------------------------------------------------------------------
    auto sysInfo = vkb::SystemInfo::get_system_info(vkGetInstanceProcAddr);
    if (!sysInfo) {
        LOG_ERROR("查询 Vulkan 系统信息失败: {}", sysInfo.error().message());
        return false;
    }
    m_validation = settings.validation && sysInfo->validation_layers_available;
    if (settings.validation && !m_validation) LOG_WARN("未找到 validation layer（Vulkan SDK 没装？），已禁用验证");

    vkb::InstanceBuilder ib(vkGetInstanceProcAddr);
    ib.set_app_name(settings.appName.c_str())
        .set_engine_name("OceanEngine")
        .require_api_version(1, 3, 0)
        .request_validation_layers(m_validation);
    if (m_validation) {
        ib.set_debug_callback(&debugCallback)
            .set_debug_messenger_severity(VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                          VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
            .set_debug_messenger_type(VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                      VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                      VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT);
        if (settings.syncValidation)
            ib.add_validation_feature_enable(VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT);
    } else if (sysInfo->debug_utils_available) {
        // 没开验证也要 debug utils：RenderDoc 里的对象名和 pass 标签靠它
        ib.enable_extension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
    auto instRet = ib.build();
    if (!instRet) {
        LOG_ERROR("创建 VkInstance 失败: {}", instRet.error().message());
        return false;
    }
    vkb::Instance vkbInstance = instRet.value();
    m_instance = vkbInstance.instance;
    m_messenger = vkbInstance.debug_messenger;
    // 加载实例级函数（含 vkCreateDevice、表面相关函数等）
    volkLoadInstance(m_instance);

    // ---- 3. 表面（窗口与 Vulkan 的连接点） --------------------------------------------------
    ENG_CHECK_VK(glfwCreateWindowSurface(m_instance, window, nullptr, &m_surface));

    // ---- 4. 物理设备：要求 Vulkan 1.3 + 必需特性，优先独显 -----------------------------------
    // 笔记本有核显 + 独显时，Vulkan 会枚举出两块 GPU；不指定的话可能选到核显。
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE; // 不再需要 VkRenderPass/VkFramebuffer，直接指定附件开始渲染
    f13.synchronization2 = VK_TRUE; // 更清晰的 barrier API：stage 与 access 写在同一个结构里
    // dxc 以 vulkan1.3 为目标时，把 HLSL 的 discard 编译成 OpDemoteToHelperInvocation：
    // 被丢弃的像素继续作为「辅助调用」参与导数计算，避免 alpha 裁剪边缘的 mip 选择出错。
    f13.shaderDemoteToHelperInvocation = VK_TRUE;
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.hostQueryReset = VK_TRUE; // GPU 计时：在 CPU 端重置 timestamp query
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    f11.shaderDrawParameters = VK_TRUE; // dxc 把 SV_InstanceID 翻译成 InstanceIndex - BaseInstance，需要它
    VkPhysicalDeviceFeatures f10{};
    f10.samplerAnisotropy = VK_TRUE; // 地形/海面斜视角贴图清晰度
    f10.fillModeNonSolid = VK_TRUE;  // 线框模式（调试地形 LOD）

    vkb::PhysicalDeviceSelector selector(vkbInstance);
    selector.set_surface(m_surface)
        .set_minimum_version(1, 3)
        .prefer_gpu_device_type(vkb::PreferredDeviceType::discrete)
        .set_required_features(f10)
        .set_required_features_11(f11)
        .set_required_features_12(f12)
        .set_required_features_13(f13);
    auto physRet = selector.select();
    if (!physRet) {
        LOG_ERROR("没有满足要求的 GPU（需要 Vulkan 1.3）: {}", physRet.error().message());
        return false;
    }
    vkb::PhysicalDevice vkbPhys = physRet.value();
    // 显存预算扩展：调试面板显示「用了多少显存」，用来守住 2GB 预算
    m_memoryBudget = vkbPhys.enable_extension_if_present(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    m_physicalDevice = vkbPhys.physical_device;
    m_properties = vkbPhys.properties;
    LOG_INFO("GPU: {}（驱动 {:#x}，Vulkan {}.{}.{}）", m_properties.deviceName, m_properties.driverVersion,
             VK_API_VERSION_MAJOR(m_properties.apiVersion), VK_API_VERSION_MINOR(m_properties.apiVersion),
             VK_API_VERSION_PATCH(m_properties.apiVersion));
    if (m_properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) LOG_WARN("当前使用的不是独立显卡，性能可能不足");

    // ---- 5. 逻辑设备与队列 ---------------------------------------------------------------------
    auto devRet = vkb::DeviceBuilder(vkbPhys).build();
    if (!devRet) {
        LOG_ERROR("创建 VkDevice 失败: {}", devRet.error().message());
        return false;
    }
    vkb::Device vkbDevice = devRet.value();
    m_device = vkbDevice.device;
    // 直接取设备级函数指针，绕过 loader 分发
    volkLoadDevice(m_device);

    // 只用一个图形队列：它同时能做 compute、transfer 和 present。
    // EXTEND: M5 的 FFT 海面可以挪到异步 compute 队列，那时再加第二个队列和跨队列同步。
    m_graphicsQueue = vkbDevice.get_queue(vkb::QueueType::graphics).value();
    m_graphicsFamily = vkbDevice.get_queue_index(vkb::QueueType::graphics).value();
    VkBool32 presentSupported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(m_physicalDevice, m_graphicsFamily, m_surface, &presentSupported);
    if (!presentSupported) {
        LOG_ERROR("图形队列不支持呈现到窗口（这种硬件很少见，需要单独的 present 队列）");
        return false;
    }
    setDebugName(m_device, m_graphicsQueue, "GraphicsQueue");

    // ---- 6. VMA 显存分配器 ---------------------------------------------------------------------
    // 为什么用 VMA：Vulkan 限制分配次数（maxMemoryAllocationCount 通常 4096），
    // 每个缓冲/贴图单独 vkAllocateMemory 很快就会用完；VMA 负责从大块内存里子分配。
    VmaVulkanFunctions vmaFns{};
    vmaFns.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    vmaFns.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo aci{};
    aci.instance = m_instance;
    aci.physicalDevice = m_physicalDevice;
    aci.device = m_device;
    aci.vulkanApiVersion = VK_API_VERSION_1_3;
    aci.pVulkanFunctions = &vmaFns;
    if (m_memoryBudget) aci.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
    ENG_CHECK_VK(vmaCreateAllocator(&aci, &m_allocator));

    // ---- 7. 立即提交用的命令池 ------------------------------------------------------------------
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = m_graphicsFamily;
    ENG_CHECK_VK(vkCreateCommandPool(m_device, &pci, nullptr, &m_immediatePool));
    setDebugName(m_device, m_immediatePool, "ImmediatePool");
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    ENG_CHECK_VK(vkCreateFence(m_device, &fci, nullptr, &m_immediateFence));
    setDebugName(m_device, m_immediateFence, "ImmediateFence");

    LOG_INFO("Vulkan 初始化完成（validation: {}）", m_validation ? "开" : "关");
    return true;
}

void Device::shutdown() {
    if (!m_device) return;
    vkDeviceWaitIdle(m_device);
    flushDeferred();
    vkDestroyFence(m_device, m_immediateFence, nullptr);
    vkDestroyCommandPool(m_device, m_immediatePool, nullptr);
    vmaDestroyAllocator(m_allocator);
    vkDestroyDevice(m_device, nullptr);
    vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
    if (m_messenger) vkDestroyDebugUtilsMessengerEXT(m_instance, m_messenger, nullptr);
    vkDestroyInstance(m_instance, nullptr);
    m_device = VK_NULL_HANDLE;
    volkFinalize();
}

void Device::beginFrame(uint64_t frameNumber) {
    m_frameNumber = frameNumber;
    // 调用者已经等过「kFramesInFlight 帧之前那一帧」的 fence，所以那时登记的对象 GPU 已不再使用
    while (!m_deferred.empty() && m_deferred.front().frame + kFramesInFlight <= frameNumber) {
        m_deferred.front().fn();
        m_deferred.pop_front();
    }
}

void Device::deferDestroy(std::function<void()> fn) {
    m_deferred.push_back({m_frameNumber, std::move(fn)});
}

void Device::flushDeferred() {
    for (auto& d : m_deferred) d.fn();
    m_deferred.clear();
}

void Device::immediateSubmit(const std::function<void(VkCommandBuffer)>& record) {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = m_immediatePool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    ENG_CHECK_VK(vkAllocateCommandBuffers(m_device, &ai, &cmd));

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    ENG_CHECK_VK(vkBeginCommandBuffer(cmd, &bi));
    record(cmd);
    ENG_CHECK_VK(vkEndCommandBuffer(cmd));

    VkCommandBufferSubmitInfo cbsi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbsi.commandBuffer = cmd;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbsi;
    ENG_CHECK_VK(vkQueueSubmit2(m_graphicsQueue, 1, &si, m_immediateFence));
    ENG_CHECK_VK(vkWaitForFences(m_device, 1, &m_immediateFence, VK_TRUE, UINT64_MAX));
    ENG_CHECK_VK(vkResetFences(m_device, 1, &m_immediateFence));
    vkFreeCommandBuffers(m_device, m_immediatePool, 1, &cmd);
}

void Device::queryMemory(uint64_t& usage, uint64_t& budget) const {
    usage = budget = 0;
    VmaBudget budgets[VK_MAX_MEMORY_HEAPS]{};
    vmaGetHeapBudgets(m_allocator, budgets);
    const VkPhysicalDeviceMemoryProperties* props = nullptr;
    vmaGetMemoryProperties(m_allocator, &props);
    // 只统计显存堆（DEVICE_LOCAL），共享内存不算进「显存预算」
    for (uint32_t i = 0; i < props->memoryHeapCount; ++i) {
        if (props->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            usage += budgets[i].usage;
            budget += budgets[i].budget;
        }
    }
}

} // namespace eng::rhi
