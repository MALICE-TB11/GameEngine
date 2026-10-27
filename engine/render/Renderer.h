#pragma once
// 渲染器：每帧的 CPU/GPU 同步、交换链获取与呈现、截图。
//
// 一帧的生命周期（kFramesInFlight = 2 时，CPU 最多领先 GPU 一帧）：
//   1. 等待本帧槽位的 fence   —— 确保 GPU 执行完了两帧前用这个槽位提交的命令
//   2. acquire 交换链图像     —— 拿到要画的图像下标；imageAvailable 信号量在图像真正可写时触发
//   3. 录制命令               —— M1：清屏 + ImGui
//   4. submit                 —— 等 imageAvailable，完成后触发 renderFinished[图像] 与 fence
//   5. present                —— 等 renderFinished[图像]，把图像交给显示器

#include "math/MathTypes.h"
#include "rhi/Device.h"
#include "rhi/Resources.h"
#include "rhi/Swapchain.h"

#include <array>
#include <filesystem>
#include <functional>

namespace eng {

class Window;

struct RendererSettings {
    bool vsync = true;
    vec4 clearColor{0.02f, 0.05f, 0.08f, 1.0f};
};

class Renderer {
public:
    bool init(rhi::Device& device, Window& window, const RendererSettings& settings);
    void shutdown();

    void setSettings(const RendererSettings& s) { m_settings = s; }
    const RendererSettings& settings() const { return m_settings; }

    // 在最终画面上叠加 UI（ImGui）。回调在 dynamic rendering 作用域内调用，目标是交换链图像。
    using OverlayFn = std::function<void(VkCommandBuffer)>;
    void setOverlay(OverlayFn fn) { m_overlay = std::move(fn); }

    // 渲染并呈现一帧。返回 false 表示本帧被跳过（交换链重建中、窗口最小化）。
    bool drawFrame();

    // 下一帧呈现前把画面保存为 PNG（--capture 与调试面板用）
    void requestCapture(const std::filesystem::path& path) { m_capturePath = path; }

    const rhi::Swapchain& swapchain() const { return m_swapchain; }
    uint64_t frameNumber() const { return m_frameNumber; }

private:
    struct FrameResources {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence inFlight = VK_NULL_HANDLE;
        VkSemaphore imageAvailable = VK_NULL_HANDLE;
    };

    bool recreateSwapchain();
    void recordFrame(VkCommandBuffer cmd, uint32_t imageIndex);
    void recordCapture(VkCommandBuffer cmd, uint32_t imageIndex);
    void finishCapture();

    rhi::Device* m_device = nullptr;
    Window* m_window = nullptr;
    RendererSettings m_settings;
    rhi::Swapchain m_swapchain;
    std::array<FrameResources, rhi::kFramesInFlight> m_frames{};
    uint64_t m_frameNumber = 0;
    OverlayFn m_overlay;

    std::filesystem::path m_capturePath;
    rhi::Buffer m_captureBuffer;
};

} // namespace eng
