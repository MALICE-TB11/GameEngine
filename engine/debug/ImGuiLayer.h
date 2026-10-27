#pragma once
// Dear ImGui 的初始化与每帧调用。
// ImGui 是「立即模式」UI：每帧重新描述整个界面，不保存控件对象——
// 所以调试面板可以直接读写引擎里的参数结构体，不需要任何绑定代码。

#include "rhi/VkCommon.h"

namespace eng {

class Window;
namespace rhi { class Device; }

class ImGuiLayer {
public:
    bool init(rhi::Device& device, Window& window, VkFormat colorFormat, uint32_t imageCount);
    void shutdown();

    void beginFrame();                 // 开始描述 UI
    void endFrame();                   // 结束描述，生成绘制数据
    void render(VkCommandBuffer cmd);  // 在当前 dynamic rendering 作用域内绘制

    // ImGui 正在使用鼠标/键盘时（例如拖动滑条），相机等游戏输入应忽略
    bool wantsMouse() const;
    bool wantsKeyboard() const;

private:
    rhi::Device* m_device = nullptr;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    bool m_frameEnded = false;
};

} // namespace eng
