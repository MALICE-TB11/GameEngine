#pragma once
// RenderDoc in-app 集成：程序内一键捕获当前帧，然后用 RenderDoc 查看每个 pass、barrier、资源内容。
//
// 两种使用方式：
//   1. 从 RenderDoc 界面启动 sandbox → renderdoc.dll 已被注入，直接拿 API
//   2. 正常启动 + config 中 debug.renderdoc = true → 我们自己加载 renderdoc.dll
// 注意：必须在创建 VkInstance 之前加载，RenderDoc 才能挂钩 Vulkan。
// 捕获快捷键 F12（RenderDoc 自己监听），也可以点调试面板的按钮。

#include <string>

namespace eng {

class RenderDocCapture {
public:
    bool init(bool loadIfNotInjected);
    bool available() const { return m_api != nullptr; }
    void triggerCapture();
    // 打开 RenderDoc 界面并连接到本进程（之后捕获会自动出现在界面里）
    void launchReplayUI();
    uint32_t numCaptures() const;
    std::string lastCapturePath() const;

private:
    void* m_api = nullptr; // RENDERDOC_API_1_6_0*，避免在头文件里暴露 renderdoc_app.h
};

} // namespace eng
