#include "debug/RenderDocCapture.h"

#include "core/FileSystem.h"
#include "core/Log.h"

#include <renderdoc_app.h>
#include <windows.h>

#include <vector>

namespace eng {
namespace {
RENDERDOC_API_1_6_0* api(void* p) {
    return static_cast<RENDERDOC_API_1_6_0*>(p);
}
} // namespace

bool RenderDocCapture::init(bool loadIfNotInjected) {
    HMODULE mod = GetModuleHandleA("renderdoc.dll"); // 从 RenderDoc 界面启动时已经注入
    if (!mod && loadIfNotInjected) {
        // 按安装目录加载。找不到就算了：RenderDoc 是可选工具
        mod = LoadLibraryA("C:\\Program Files\\RenderDoc\\renderdoc.dll");
        if (!mod) LOG_WARN("未找到 RenderDoc（C:\\Program Files\\RenderDoc），帧捕获不可用");
    }
    if (!mod) return false;

    auto getApi = reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(mod, "RENDERDOC_GetAPI"));
    if (!getApi || getApi(eRENDERDOC_API_Version_1_6_0, &m_api) != 1) {
        LOG_WARN("RenderDoc API 版本不匹配，帧捕获不可用");
        m_api = nullptr;
        return false;
    }
    const std::string tmpl = (fs::projectRoot() / "captures" / "ocean").string();
    api(m_api)->SetCaptureFilePathTemplate(tmpl.c_str());
    RENDERDOC_InputButton key = eRENDERDOC_Key_F12;
    api(m_api)->SetCaptureKeys(&key, 1);
    // 不显示 RenderDoc 在画面左上角的覆盖文字（我们有自己的面板）
    api(m_api)->MaskOverlayBits(eRENDERDOC_Overlay_None, eRENDERDOC_Overlay_None);
    LOG_INFO("RenderDoc 已就绪：按 F12 捕获一帧，保存到 captures/");
    return true;
}

void RenderDocCapture::triggerCapture() {
    if (m_api) api(m_api)->TriggerCapture();
}

void RenderDocCapture::launchReplayUI() {
    if (!m_api) return;
    if (api(m_api)->IsTargetControlConnected()) return;
    api(m_api)->LaunchReplayUI(1, nullptr);
}

uint32_t RenderDocCapture::numCaptures() const {
    return m_api ? api(m_api)->GetNumCaptures() : 0;
}

std::string RenderDocCapture::lastCapturePath() const {
    const uint32_t n = numCaptures();
    if (n == 0) return {};
    uint32_t len = 0;
    api(m_api)->GetCapture(n - 1, nullptr, &len, nullptr);
    std::vector<char> path(len + 1);
    api(m_api)->GetCapture(n - 1, path.data(), &len, nullptr);
    return path.data();
}

} // namespace eng
