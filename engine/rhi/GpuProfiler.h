#pragma once
// GPU 计时：在每个 pass 前后写 timestamp，读回后得到每个 pass 在 GPU 上的耗时。
//
// 为什么不能用 CPU 计时：CPU 只是「录制」命令，真正执行在 GPU 上、而且晚一到两帧。
// timestamp query 让 GPU 在执行到某个位置时把自己的时钟写进查询池。
// 读回时机：等过某帧的 fence 之后，那一帧的查询结果一定可用，所以结果总是「两帧前」的。

#include "rhi/Device.h"

#include <array>
#include <string>
#include <vector>

namespace eng::rhi {

class GpuProfiler {
public:
    static constexpr uint32_t kMaxScopes = 64;

    struct Result {
        std::string name;
        float ms = 0.0f;
    };

    void init(Device& device);
    void shutdown();

    // 每帧开头调用（已等过该槽位 fence）：读回这个槽位上次的结果，然后在 CPU 端重置查询池
    void beginFrame(uint32_t frameSlot);
    // 返回 scope 下标，传给 end；查询数用完时返回 UINT32_MAX（不计时但不出错）
    uint32_t begin(VkCommandBuffer cmd, const char* name);
    void end(VkCommandBuffer cmd, uint32_t scope);

    const std::vector<Result>& results() const { return m_results; }
    float totalMs() const { return m_totalMs; }

private:
    struct Slot {
        VkQueryPool pool = VK_NULL_HANDLE;
        std::vector<std::string> names;
        bool used = false;
    };
    Device* m_device = nullptr;
    std::array<Slot, kFramesInFlight> m_slots{};
    uint32_t m_current = 0;
    float m_periodNs = 1.0f;
    bool m_supported = false;
    std::vector<Result> m_results;
    float m_totalMs = 0.0f;
};

} // namespace eng::rhi
