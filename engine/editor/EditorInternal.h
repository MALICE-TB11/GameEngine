#pragma once
// 编辑器内部共用的常量与小工具（Editor.cpp 与 EditorPanels.cpp 共享）。
//
// 窗口名格式「图标 标题###固定ID」：### 之后的部分决定窗口 ID，
// 所以改标题文字/图标不会让已保存的停靠布局失效。

namespace eng::editor {

inline constexpr const char* kViewportWindow = "◆ 视口###Viewport";
inline constexpr const char* kOutlinerWindow = "☰ 场景大纲###Outliner";
inline constexpr const char* kAssetsWindow = "▤ 资源###Assets";
inline constexpr const char* kInspectorWindow = "◉ 检视器###Inspector";
inline constexpr const char* kEnvironmentWindow = "☀ 环境光照###Environment";
inline constexpr const char* kRenderSettingsWindow = "◐ 渲染设置###RenderSettings";
inline constexpr const char* kProjectWindow = "⚙ 项目设置###Project";
inline constexpr const char* kWorldWindow = "▲ 地形与物理###World";
inline constexpr const char* kOceanWindow = "♒ 海洋###Ocean";

inline constexpr const char* kDebugViewNames[] = {"正常光照", "法线", "Albedo", "粗糙度", "金属度", "AO"};
inline constexpr const char* kTonemapperNames[] = {"ACES（Hill 拟合）", "AgX", "无（直接截断）"};
inline constexpr float kTimeScales[] = {0.1f, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f};

} // namespace eng::editor
