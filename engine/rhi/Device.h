#pragma once
// Vulkan 设备：实例、物理设备选择、逻辑设备、队列、VMA 分配器，以及两个贯穿全引擎的机制：
//   1. 延迟销毁（deferDestroy）：GPU 可能还在用的对象不能立刻销毁，要等到它所在的帧确实执行完。
//   2. 立即提交（immediateSubmit）：上传网格/贴图这类一次性命令，提交后同步等待完成。

#include "rhi/VkCommon.h"

#include <vk_mem_alloc.h>

#include <deque>
#include <functional>
#include <string>

struct GLFWwindow;

namespace eng::rhi {

// 同时「在路上」的帧数（CPU 录制第 N+1 帧时 GPU 还在执行第 N 帧）。
// 2 是延迟与吞吐的平衡点：1 会让 CPU/GPU 串行互等，3 会多一帧输入延迟、多一份每帧资源。
inline constexpr uint32_t kFramesInFlight = 2;

struct DeviceSettings {
    std::string appName = "Ocean Engine";
    bool validation = true;
    // 同步验证：检查 barrier 是否缺失/多余。Render Graph 的 barrier 推导靠它来验证。
    bool syncValidation = true;
};

class Device {
public:
    bool init(const DeviceSettings& settings, GLFWwindow* window);
    void shutdown();

    VkInstance instance() const { return m_instance; }
    VkPhysicalDevice physicalDevice() const { return m_physicalDevice; }
    VkDevice device() const { return m_device; }
    VkSurfaceKHR surface() const { return m_surface; }
    VkQueue graphicsQueue() const { return m_graphicsQueue; }
    uint32_t graphicsFamily() const { return m_graphicsFamily; }
    VmaAllocator allocator() const { return m_allocator; }
    const VkPhysicalDeviceProperties& properties() const { return m_properties; }
    bool validationEnabled() const { return m_validation; }
    bool memoryBudgetSupported() const { return m_memoryBudget; }

    // ---- 延迟销毁 ------------------------------------------------------------------------
    // 每帧开头（等完该帧 fence 之后）调用，执行 kFramesInFlight 帧之前登记的销毁任务。
    void beginFrame(uint64_t frameNumber);
    // 登记一个销毁任务，在当前帧的命令确定执行完后才运行
    void deferDestroy(std::function<void()> fn);
    // 关机或 vkDeviceWaitIdle 之后调用：立刻执行所有待销毁任务
    void flushDeferred();

    // ---- 一次性同步提交 --------------------------------------------------------------------
    void immediateSubmit(const std::function<void(VkCommandBuffer)>& record);

    // 显存使用（需要 VK_EXT_memory_budget），单位字节
    void queryMemory(uint64_t& usage, uint64_t& budget) const;

private:
    bool m_validation = false;
    bool m_memoryBudget = false;
    VkInstance m_instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_messenger = VK_NULL_HANDLE;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties m_properties{};
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_graphicsQueue = VK_NULL_HANDLE;
    uint32_t m_graphicsFamily = 0;
    VmaAllocator m_allocator = VK_NULL_HANDLE;

    VkCommandPool m_immediatePool = VK_NULL_HANDLE;
    VkFence m_immediateFence = VK_NULL_HANDLE;

    struct Deferred {
        uint64_t frame;
        std::function<void()> fn;
    };
    std::deque<Deferred> m_deferred;
    uint64_t m_frameNumber = 0;
};

} // namespace eng::rhi
