#include "rhi/GpuProfiler.h"

#include <string>

namespace eng::rhi {

void GpuProfiler::init(Device& device) {
    m_device = &device;
    // timestampPeriod：GPU 时钟一跳是多少纳秒
    m_periodNs = device.properties().limits.timestampPeriod;
    m_supported = device.properties().limits.timestampComputeAndGraphics == VK_TRUE;
    if (!m_supported) return;
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        VkQueryPoolCreateInfo ci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        ci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        ci.queryCount = kMaxScopes * 2;
        ENG_CHECK_VK(vkCreateQueryPool(device.device(), &ci, nullptr, &m_slots[i].pool));
        setDebugName(device.device(), m_slots[i].pool, ("GpuTimestamps" + std::to_string(i)).c_str());
        // 新建的查询处于未定义状态，必须先重置才能写入（需要 Vulkan 1.2 的 hostQueryReset 特性）
        vkResetQueryPool(device.device(), m_slots[i].pool, 0, kMaxScopes * 2);
    }
}

void GpuProfiler::shutdown() {
    if (!m_device) return; // init 失败（或从没 init）时也能安全调用
    for (Slot& s : m_slots)
        if (s.pool) vkDestroyQueryPool(m_device->device(), s.pool, nullptr);
    m_slots = {};
}

void GpuProfiler::beginFrame(uint32_t frameSlot) {
    m_current = frameSlot;
    if (!m_supported) return;
    Slot& slot = m_slots[frameSlot];
    if (slot.used && !slot.names.empty()) {
        const uint32_t count = static_cast<uint32_t>(slot.names.size()) * 2;
        std::vector<uint64_t> ticks(count);
        // 不带 WAIT 标志：调用者已等过 fence，结果应当可用；万一不可用（VK_NOT_READY）就跳过这一帧
        const VkResult r = vkGetQueryPoolResults(m_device->device(), slot.pool, 0, count, ticks.size() * sizeof(uint64_t),
                                                 ticks.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
        if (r == VK_SUCCESS) {
            m_results.clear();
            m_totalMs = 0.0f;
            for (size_t i = 0; i < slot.names.size(); ++i) {
                const float ms = float(double(ticks[i * 2 + 1] - ticks[i * 2]) * m_periodNs * 1e-6);
                m_results.push_back({slot.names[i], ms});
                m_totalMs += ms;
            }
        }
    }
    vkResetQueryPool(m_device->device(), slot.pool, 0, kMaxScopes * 2);
    slot.names.clear();
    slot.used = true;
}

uint32_t GpuProfiler::begin(VkCommandBuffer cmd, const char* name) {
    if (!m_supported) return UINT32_MAX;
    Slot& slot = m_slots[m_current];
    if (slot.names.size() >= kMaxScopes) return UINT32_MAX;
    const uint32_t scope = static_cast<uint32_t>(slot.names.size());
    slot.names.emplace_back(name);
    // ALL_COMMANDS：等前面所有命令都到这一点后再写时间，测到的是「这个 pass 开始」的时刻
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, slot.pool, scope * 2);
    return scope;
}

void GpuProfiler::end(VkCommandBuffer cmd, uint32_t scope) {
    if (scope == UINT32_MAX) return;
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, m_slots[m_current].pool, scope * 2 + 1);
}

} // namespace eng::rhi
