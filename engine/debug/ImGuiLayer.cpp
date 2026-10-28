#include "debug/ImGuiLayer.h"

#include "core/FileSystem.h"
#include "core/Log.h"
#include "platform/Window.h"
#include "rhi/Device.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

#include <filesystem>
#include <string>

namespace eng {
namespace {
std::string g_iniPath; // ImGui 只保存指针，字符串必须一直活着
}

std::filesystem::path ImGuiLayer::layoutIniPath() {
    // 文件名带版本：停靠布局改版后换个名字，旧的浮动窗口布局不会干扰新布局
    return fs::projectRoot() / "build" / "imgui_editor_v3.ini"; // v3：M5 新增「海洋」面板
}

bool ImGuiLayer::init(rhi::Device& device, Window& window, VkFormat colorFormat, uint32_t imageCount) {
    m_device = &device;

    // ImGui 自己的描述符池（只用来放字体贴图和用户贴图的 combined image sampler）
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64}};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT; // ImGui 会单独释放描述符集
    pci.maxSets = 64;
    pci.poolSizeCount = 1;
    pci.pPoolSizes = sizes;
    ENG_CHECK_VK(vkCreateDescriptorPool(device.device(), &pci, nullptr, &m_pool));
    rhi::setDebugName(device.device(), m_pool, "ImGuiDescriptorPool");

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // 编辑器习惯：只能拖标题栏移动窗口，否则在视口里拖鼠标会把整个窗口拖走
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    g_iniPath = layoutIniPath().string();
    io.IniFilename = g_iniPath.c_str();
    ImGui::StyleColorsDark();

    // 日志和面板里有中文，默认字体没有中文字形。用系统自带的微软雅黑，找不到就退回默认字体。
    const char* cjkFont = "C:/Windows/Fonts/msyh.ttc";
    if (std::filesystem::exists(cjkFont)) {
        // 「常用 2500 字」不含 渲、帧 等技术词汇用字；全量字库（2 万字）又会让字体图集膨胀到几十 MB。
        // 折中：常用字 + 本引擎会用到的专业词汇用字。
        // 另外，源码字符串里实际用到的其余汉字由 tools/update_ui_glyphs.py 扫描生成到 UiGlyphs.inc。
        // EXTEND: 新增界面文字后出现「?」，运行 python tools/update_ui_glyphs.py 再重新构建。
#include "debug/UiGlyphs.inc"
        static const char* kExtraGlyphs =
            "渲帧清屏缓栅瞬焦散噪糙曝映射纹贴潮汐浪沫涌岸礁苔藓岩砂崖穴遗迹漂吸衰减折菲涅尔雅谐插值钳截捕获着描"
            "阴影漫镜框拓扑剔遮锥裁雾霾烘焙载兜灰阶伽饱亮湿润附胶囊碰撞约束积蓝绿紫橙粉"
            "垂乘累溢栈队序琐碎卸韧逾魂淤矩阵叉矢锐钝褪隙缝罩捆缩攒";
        static ImVector<ImWchar> ranges; // ImGui 在构建图集前一直引用它，必须是静态的
        if (ranges.empty()) {
            ImFontGlyphRangesBuilder builder;
            builder.AddRanges(io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
            builder.AddText(kExtraGlyphs);
            builder.AddText(kUiExtraGlyphs);
            builder.BuildRanges(&ranges);
        }
        ImFontConfig cfg;
        cfg.OversampleH = 1; // 中文字形多，关掉水平超采样能让图集小一半，16px 下看不出差别
        io.Fonts->AddFontFromFileTTF(cjkFont, 17.0f, &cfg, ranges.Data);

        // 图标：合并 Windows 自带的 Segoe UI Symbol（箭头、几何图形、杂项符号、装饰符号），
        // 这样按钮可以写成 "▶ 运行"、"☀ 环境" 而不需要额外的图标字体文件。
        // EXTEND: 想要更丰富的图标，可换成 Font Awesome / Material Symbols 等图标字体（以同样方式合并）。
        const char* symbolFont = "C:/Windows/Fonts/seguisym.ttf";
        if (std::filesystem::exists(symbolFont)) {
            // 箭头、技术符号、制表符（│）、几何图形、杂项符号、装饰符号
            static const ImWchar kIconRanges[] = {0x2190, 0x21FF, 0x2300, 0x23FF, 0x2500, 0x25FF, 0x2600, 0x27BF, 0};
            ImFontConfig icons;
            icons.MergeMode = true;   // 合并进上一个字体：同一行文字里中文和图标混排
            icons.OversampleH = 1;
            icons.GlyphOffset.y = 1.0f; // 符号字体的基线略高，往下挪一点与中文对齐
            io.Fonts->AddFontFromFileTTF(symbolFont, 16.0f, &icons, kIconRanges);
        }
    } else {
        LOG_WARN("未找到中文字体 {}，ImGui 中文会显示为 ?", cjkFont);
        io.Fonts->AddFontDefault();
    }

    // install_callbacks=true：ImGui 安装 GLFW 回调，并把之前已安装的回调（Window 的滚轮回调）串起来
    ImGui_ImplGlfw_InitForVulkan(window.handle(), true);

    // ImGui 用 dynamic rendering 时需要知道目标附件格式（代替 VkRenderPass）
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &colorFormat;

    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = VK_API_VERSION_1_3;
    info.Instance = device.instance();
    info.PhysicalDevice = device.physicalDevice();
    info.Device = device.device();
    info.QueueFamily = device.graphicsFamily();
    info.Queue = device.graphicsQueue();
    info.DescriptorPool = m_pool;
    info.MinImageCount = imageCount;
    info.ImageCount = imageCount;
    info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    info.UseDynamicRendering = true;
    info.PipelineRenderingCreateInfo = rendering;
    info.CheckVkResultFn = [](VkResult r) {
        if (r < 0) LOG_ERROR("ImGui Vulkan 后端错误: {}", rhi::toString(r));
    };
    if (!ImGui_ImplVulkan_Init(&info)) {
        LOG_ERROR("ImGui Vulkan 后端初始化失败");
        return false;
    }
    return true;
}

void ImGuiLayer::shutdown() {
    if (!m_device) return;
    vkDeviceWaitIdle(m_device->device());
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    vkDestroyDescriptorPool(m_device->device(), m_pool, nullptr);
    m_device = nullptr;
}

void ImGuiLayer::beginFrame() {
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    m_frameEnded = false;
}

void ImGuiLayer::endFrame() {
    ImGui::Render();
    m_frameEnded = true;
}

void ImGuiLayer::render(VkCommandBuffer cmd) {
    if (!m_frameEnded) return;
    if (ImDrawData* dd = ImGui::GetDrawData()) ImGui_ImplVulkan_RenderDrawData(dd, cmd);
}

bool ImGuiLayer::wantsMouse() const {
    return ImGui::GetIO().WantCaptureMouse;
}

bool ImGuiLayer::wantsKeyboard() const {
    return ImGui::GetIO().WantCaptureKeyboard;
}

} // namespace eng
