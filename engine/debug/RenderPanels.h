#pragma once
// 渲染相关的调试面板（可停靠窗口）：
//   Render Graph 面板 —— 每个 pass 的开关、状态、GPU 耗时、被替换成默认贴图的输入；资源列表；RenderDoc
//   Shader 面板      —— 每条管线的编译状态与错误信息、手动重编
//   错误横幅         —— 有 shader 错误时显示红字（游戏模式下用；编辑器模式下显示在视口和状态栏）

namespace eng {

class Renderer;
class PipelineCache;
class RenderDocCapture;

inline constexpr const char* kRenderGraphWindow = "▦ Render Graph###RenderGraph";
inline constexpr const char* kShaderWindow = "✎ Shader###Shader";

void drawRenderGraphPanel(Renderer& renderer, RenderDocCapture* renderdoc, bool* open);
void drawShaderPanel(PipelineCache& pipelines, bool* open);
void drawShaderErrorBanner(const PipelineCache& pipelines);

} // namespace eng
