#pragma once
// 渲染相关的调试面板：
//   Render Graph 面板 —— 每个 pass 的开关、状态、GPU 耗时、被替换成默认贴图的输入
//   Shader 面板      —— 每条管线的编译状态与错误信息、手动重编
//   错误横幅         —— 有 shader 错误时在屏幕顶部显示红字，避免「改坏了没发现」

namespace eng {

class Renderer;
class PipelineCache;
class RenderDocCapture;

void drawRenderGraphPanel(Renderer& renderer, RenderDocCapture* renderdoc);
void drawShaderPanel(PipelineCache& pipelines);
void drawShaderErrorBanner(const PipelineCache& pipelines);

} // namespace eng
