#pragma once
// 管线缓存 + shader 热重载。
//
// 每条管线登记一次（描述：用哪个 shader、什么固定功能状态），之后用 PipelineHandle 引用。
// 热重载：定期检查 shader 源文件（含 include）的时间戳，变了就用 dxc 重新编译并重建管线：
//   成功 → 新管线替换旧管线（旧的延迟销毁，因为前一两帧可能还在用）
//   失败 → 保留旧管线继续渲染，错误信息显示在 ImGui「Shader」面板
// 启动时就编译失败（没有旧管线可用）→ ready() 返回 false：
//   Render Graph 会跳过依赖它的 pass，材质会改用错误管线（品红棋盘）。
//
// 管线布局约定：set 0 = 全局（FrameConstants + 采样器），set 1 = 每个管线自己的资源，
// push constant = 128 字节，对所有阶段可见。统一的 set 0 和 push constant 让不同管线的布局「兼容」，
// 切换管线后 set 0 不需要重新绑定。

#include "render/ShaderCompiler.h"
#include "rhi/Pipeline.h"

#include <filesystem>
#include <string>
#include <vector>

namespace eng {

namespace rhi { class Device; }

using PipelineHandle = uint32_t;
inline constexpr PipelineHandle kInvalidPipeline = UINT32_MAX;
inline constexpr uint32_t kPushConstantSize = 128; // Vulkan 保证所有设备至少支持 128 字节

struct GraphicsPipelineDesc {
    std::string name;
    std::string shader; // 例如 "passes/composite"
    rhi::GraphicsState state;
    VkDescriptorSetLayout set1 = VK_NULL_HANDLE;
};

struct ComputePipelineDesc {
    std::string name;
    std::string shader;
    VkDescriptorSetLayout set1 = VK_NULL_HANDLE;
};

class PipelineCache {
public:
    void init(rhi::Device& device, VkDescriptorSetLayout globalLayout);
    void shutdown();

    PipelineHandle createGraphics(const GraphicsPipelineDesc& desc);
    PipelineHandle createCompute(const ComputePipelineDesc& desc);

    bool ready(PipelineHandle h) const;
    VkPipeline pipeline(PipelineHandle h) const;
    VkPipelineLayout layout(PipelineHandle h) const;
    // 绑定管线 + set 0
    void bind(VkCommandBuffer cmd, PipelineHandle h, VkDescriptorSet globalSet) const;
    void bindSet1(VkCommandBuffer cmd, PipelineHandle h, VkDescriptorSet set) const;
    void pushConstants(VkCommandBuffer cmd, PipelineHandle h, const void* data, uint32_t size) const;

    // ---- 热重载 ----
    void pollHotReload(double nowSeconds);
    void reloadAll();
    bool autoReload = true;

    struct Status {
        std::string name;
        std::string shader;
        bool ready;
        std::string error; // 最近一次编译错误；空 = 正常
    };
    std::vector<Status> status() const;
    uint32_t errorCount() const;
    uint32_t reloadCount() const { return m_reloadCount; }

private:
    struct Entry {
        bool compute = false;
        GraphicsPipelineDesc graphics;
        ComputePipelineDesc computeDesc;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        std::filesystem::file_time_type stamp{};
        std::string error;
    };

    VkPipelineLayout makeLayout(VkDescriptorSetLayout set1, const std::string& name);
    VkPipeline build(Entry& e, bool forceCompile, std::string& error);
    void reload(Entry& e);
    const std::string& shaderOf(const Entry& e) const { return e.compute ? e.computeDesc.shader : e.graphics.shader; }
    const std::string& nameOf(const Entry& e) const { return e.compute ? e.computeDesc.name : e.graphics.name; }

    rhi::Device* m_device = nullptr;
    VkDescriptorSetLayout m_globalLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_emptyLayout = VK_NULL_HANDLE;
    ShaderCompiler m_compiler;
    std::vector<Entry> m_entries;
    double m_lastPoll = -1.0;
    uint32_t m_reloadCount = 0;
};

} // namespace eng
