#pragma once
// 编辑器配色与控件辅助。
// 主题「深海」：深蓝灰底色 + 青色强调色（呼应海洋题材），选中/激活状态一律用强调色，
// 警告橙、错误红，与日志、状态栏的颜色语义一致。

#include <imgui.h>

namespace eng::theme {

// 语义颜色：所有面板共用，改这里即可统一换肤
inline const ImVec4 kAccent{0.05f, 0.68f, 0.76f, 1.0f};
inline const ImVec4 kAccentDim{0.03f, 0.40f, 0.46f, 1.0f};
inline const ImVec4 kAccentBright{0.35f, 0.90f, 0.95f, 1.0f};
inline const ImVec4 kOk{0.45f, 0.90f, 0.60f, 1.0f};
inline const ImVec4 kWarn{1.00f, 0.75f, 0.30f, 1.0f};
inline const ImVec4 kError{1.00f, 0.42f, 0.42f, 1.0f};
inline const ImVec4 kTextDim{0.48f, 0.55f, 0.63f, 1.0f};
inline const ImVec4 kSelection{1.00f, 0.55f, 0.15f, 1.0f}; // 选中物体的橙色（与视口高亮一致）

void apply();

// 工具栏按钮：active 时用强调色填充（开关类按钮）；tooltip 悬停时显示
bool toolButton(const char* label, bool active = false, const char* tooltip = nullptr);
// 区块标题：强调色小字 + 细分隔线
void sectionHeader(const char* text);
// 灰色帮助问号，悬停显示说明
void helpMarker(const char* text);

} // namespace eng::theme
