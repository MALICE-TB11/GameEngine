#pragma once
// 渲染器：每帧的 CPU/GPU 同步、交换链、每帧常量，以及用 Render Graph 组织各个 pass。
//
// 一帧的生命周期（kFramesInFlight = 2 时，CPU 最多领先 GPU 一帧）：
//   1. 等待本帧槽位的 fence   —— 确保 GPU 执行完了两帧前用这个槽位提交的命令
//   2. acquire 交换链图像     —— imageAvailable 信号量在图像真正可写时触发
//   3. 构建 Render Graph      —— 声明本帧的 pass 及其读写关系
//   4. 执行 Render Graph      —— 自动插入 barrier、录制命令
//   5. submit + present
//
// 加一个新 pass 的步骤（EXTEND）：
//   在 render/passes/ 新建 XxxPass（init 里建管线，add 里声明读写 + 执行函数），
//   然后在 Renderer::buildGraph 里按顺序调用它的 add。barrier 与开关面板会自动生效。

#include "math/MathTypes.h"
#include "render/Camera.h"
#include "render/DefaultResources.h"
#include "render/FrameConstants.h"
#include "render/PipelineCache.h"
#include "render/RenderGraph.h"
#include "render/RenderTypes.h"
#include "render/passes/ForwardPass.h"
#include "render/passes/TonemapPass.h"
#include "rhi/Descriptors.h"
#include "rhi/Device.h"
#include "rhi/GpuProfiler.h"
#include "rhi/Resources.h"
#include "rhi/Swapchain.h"

#include <array>
#include <filesystem>
#include <functional>

namespace eng {

class Window;

struct RendererSettings {
    bool vsync = true;
    vec4 clearColor{0.02f, 0.05f, 0.08f, 1.0f}; // 场景背景（线性 HDR 值）
    float sunElevationDeg = 35.0f;
    float sunAzimuthDeg = 45.0f;
    float exposure = 1.0f;
};

class Renderer {
public:
    // 场景颜色与深度的格式：HDR 需要超过 1 的亮度，16 位浮点足够且比 32 位省一半带宽
    static constexpr VkFormat kSceneColorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    static constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

    bool init(rhi::Device& device, Window& window, const RendererSettings& settings);
    void shutdown();

    void setSettings(const RendererSettings& s) { m_settings = s; }
    const RendererSettings& settings() const { return m_settings; }

    // 在最终画面上叠加 UI（ImGui）。回调在 dynamic rendering 作用域内调用，目标是交换链图像。
    using OverlayFn = std::function<void(VkCommandBuffer)>;
    void setOverlay(OverlayFn fn) { m_overlay = std::move(fn); }

    // 渲染并呈现一帧。返回 false 表示本帧被跳过（交换链重建中、窗口最小化）。
    bool drawFrame(const RenderView& view, const DrawList& draws);

    // 下一帧呈现前把画面保存为 PNG（--capture 与调试面板用）
    void requestCapture(const std::filesystem::path& path) { m_capturePath = path; }

    const rhi::Swapchain& swapchain() const { return m_swapchain; }
    uint64_t frameNumber() const { return m_frameNumber; }
    rg::RenderGraph& graph() { return m_graph; }
    PipelineCache& pipelines() { return m_pipelines; }
    const rhi::GpuProfiler& profiler() const { return m_profiler; }
    ForwardPass& forwardPass() { return m_forward; }

private:
    struct FrameResources {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence inFlight = VK_NULL_HANDLE;
        VkSemaphore imageAvailable = VK_NULL_HANDLE;
        rhi::DescriptorAllocator descriptors;
        rhi::Buffer constants; // FrameConstants，CPU 每帧写入
        VkDescriptorSet globalSet = VK_NULL_HANDLE;
    };

    bool recreateSwapchain();
    void createGlobalDescriptors();
    void updateFrameConstants(FrameResources& frame, const RenderView& view);
    void buildGraph(uint32_t imageIndex, const DrawList& draws);
    void addCapturePass(rg::TextureHandle backbuffer, uint32_t imageIndex);
    void finishCapture();

    rhi::Device* m_device = nullptr;
    Window* m_window = nullptr;
    RendererSettings m_settings;
    rhi::Swapchain m_swapchain;
    std::array<FrameResources, rhi::kFramesInFlight> m_frames{};
    uint64_t m_frameNumber = 0;
    OverlayFn m_overlay;

    DefaultResources m_defaults;
    PipelineCache m_pipelines;
    rg::RenderGraph m_graph;
    rhi::GpuProfiler m_profiler;
    VkDescriptorSetLayout m_globalLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_globalPool = VK_NULL_HANDLE;

    ForwardPass m_forward;
    TonemapPass m_tonemap;

    std::filesystem::path m_capturePath;
    rhi::Buffer m_captureBuffer;
    bool m_captureRecorded = false;
};

} // namespace eng
