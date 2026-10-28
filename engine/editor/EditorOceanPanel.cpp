// 「海洋」面板：实现切换（参考 / 优化）、风浪参数、泡沫、水体着色、网格。
#include "editor/Editor.h"

#include "editor/EditorInternal.h"
#include "editor/EditorTheme.h"
#include "ocean/OceanRenderer.h"

#include <imgui.h>

namespace eng {

using namespace editor;

void Editor::drawOceanPanel() {
    if (!ImGui::Begin(kOceanWindow)) {
        ImGui::End();
        return;
    }
    if (!m_ctx.oceanSettings || !m_ctx.ocean) {
        ImGui::TextDisabled("海洋模块未启用");
        ImGui::End();
        return;
    }
    ocean::OceanSettings& s = *m_ctx.oceanSettings;
    ocean::OceanRenderer& o = *m_ctx.ocean;
    bool c = false;

    c |= ImGui::Checkbox("显示海洋", &s.enabled);
    ImGui::SameLine();
    ImGui::Checkbox("线框", &o.wireframe);
    theme::sectionHeader("实现");
    c |= ImGui::RadioButton("Gerstner（参考）", &s.mode, int(ocean::OceanMode::Gerstner));
    ImGui::SameLine();
    c |= ImGui::RadioButton("FFT（优化）", &s.mode, int(ocean::OceanMode::Fft));
    theme::helpMarker("两种实现用同一份频谱：\n"
                      "Gerstner：从频谱里挑几十个波直接求和，简单、可以在 CPU 精确查询水面高度；\n"
                      "FFT：在 3 个级联 × 256² 的波数网格上逆 FFT，几十万个波同时叠加，细节和泡沫都更真实。\n"
                      "来回切换对比，看看「波的数量」对真实感的影响。");

    const ocean::OceanStats& st = o.stats();
    ImGui::TextDisabled("有效波高 %.2f m · 峰值波长 %.0f m · 周期 %.1f s", st.significantWaveHeight, st.peakWavelength, st.peakPeriod);
    ImGui::TextDisabled("网格 %u 顶点 · 频谱：后台生成 %.1f ms，上传 %.2f ms（共 %u 次）", st.vertices, st.spectrumMs, st.uploadMs,
                        st.rebuilds);

    theme::sectionHeader("风浪");
    c |= ImGui::SliderFloat("风速", &s.windSpeed, 0.5f, 30.0f, "%.1f m/s");
    theme::helpMarker("10 米高处的风速。浪高大约随风速平方增长：\n5 m/s 微浪，10 m/s 中浪，20 m/s 以上是大风浪。");
    c |= ImGui::SliderFloat("风向", &s.windDirectionDeg, 0.0f, 360.0f, "%.0f°");
    c |= ImGui::SliderFloat("风区长度", &s.fetchKm, 1.0f, 1000.0f, "%.0f km", ImGuiSliderFlags_Logarithmic);
    theme::helpMarker("风吹过的海面距离。风区越长，浪越「成熟」：更高、更长。\n小海湾里风再大浪也起不来。");
    c |= ImGui::SliderFloat("波高", &s.heightScale, 0.0f, 3.0f, "×%.2f");
    c |= ImGui::SliderFloat("尖锐度", &s.choppiness, 0.0f, 1.5f, "%.2f");
    theme::helpMarker("水平位移的倍数：水面点向浪峰聚拢 → 浪峰尖、浪谷平。\n太大时水面会自相交，正是泡沫最多的地方。");
    c |= ImGui::SliderFloat("方向集中度", &s.spread, 0.5f, 30.0f, "%.1f");
    theme::helpMarker("越大波浪越整齐地顺着风向；越小越杂乱（各个方向都有浪）。");
    c |= ImGui::SliderFloat("涌浪", &s.swell, 0.0f, 1.0f, "%.2f");
    theme::helpMarker("混入来自远方的长周期涌浪：浪更长、更规整。");
    c |= ImGui::SliderFloat("峰值增强 γ", &s.peakEnhancement, 1.0f, 7.0f, "%.1f");
    c |= ImGui::SliderFloat("水深", &s.depth, 2.0f, 500.0f, "%.0f m", ImGuiSliderFlags_Logarithmic);
    theme::helpMarker("用于色散关系 ω² = gk·tanh(kh)：浅水里长波变慢、能量被压低（TMA 修正）。");
    if (ImGui::CollapsingHeader("级联尺寸")) {
        c |= ImGui::DragFloat3("尺寸 (m)", &s.cascadeSizes.x, 0.5f, 0.5f, 2000.0f, "%.1f");
        theme::helpMarker("三张 256² 的频谱分别覆盖这么大的海面，各自负责一段波长，互不重叠。\n尺寸互不整除，避免看出重复的图案。");
    }
    if (theme::toolButton("↻ 换一组随机相位", false, "同样的统计特征，不同的具体海面")) o.reseed();

    theme::sectionHeader("泡沫");
    c |= ImGui::SliderFloat("起泡阈值", &s.foamBias, -0.5f, 1.5f, "%.2f");
    theme::helpMarker("雅可比行列式 J 衡量水面被挤压的程度：J = 1 平静，J < 0 自相交。\nJ 低于这个阈值就开始产生泡沫；调高 = 更多泡沫。");
    c |= ImGui::SliderFloat("泡沫强度", &s.foamIntensity, 0.0f, 10.0f, "%.1f");
    c |= ImGui::SliderFloat("消散时间", &s.foamDecay, 0.1f, 10.0f, "%.1f s");
    c |= ImGui::SliderFloat("岸边泡沫宽度", &s.shoreFoamWidth, 0.0f, 6.0f, "%.1f m");
    c |= ImGui::SliderFloat("泡沫细节尺寸", &s.foamTiling, 0.5f, 20.0f, "%.1f m");

    theme::sectionHeader("水体");
    c |= ImGui::DragFloat3("吸收 RGB", &s.absorption.x, 0.005f, 0.0f, 3.0f, "%.3f /m");
    theme::helpMarker("光在水里每走 1 米被吸收的比例（Beer–Lambert：透过率 = e^{-σ·距离}）。\n红光吸收最强，所以深水发蓝绿；调大绿色吸收会更蓝，调小所有值水更清。");
    c |= ImGui::ColorEdit3("散射颜色", &s.scatterColor.x, ImGuiColorEditFlags_Float);
    c |= ImGui::ColorEdit3("浪尖透光", &s.sssColor.x, ImGuiColorEditFlags_Float);
    c |= ImGui::SliderFloat("透光强度", &s.sssStrength, 0.0f, 4.0f, "%.2f");
    c |= ImGui::SliderFloat("粗糙度", &s.roughness, 0.01f, 0.4f, "%.3f");
    c |= ImGui::SliderFloat("折射", &s.refraction, 0.0f, 0.2f, "%.3f");
    c |= ImGui::SliderFloat("天空反射", &s.reflection, 0.0f, 2.0f, "%.2f");

    theme::sectionHeader("局部交互（波纹）");
    c |= ImGui::Checkbox("启用", &s.ripples);
    theme::helpMarker("玩家周围一块随玩家移动的网格，每个物理步解一次波动方程：\n"
                      "在水里走动、跳进水里会激起波纹；波纹撞到岸会反射，走到区域边缘被吸收层吃掉。\n"
                      "Render Graph 里的 OceanRipples 是它的 pass（每个物理步一个）。");
    const ocean::RippleStats& rs = o.rippleStats();
    ImGui::TextDisabled("格子 %.2f m · 本帧 %u 步 · 波速上限 %.1f m/s（稳定性条件）", rs.cellSize, rs.stepsThisFrame, rs.speedLimit);
    c |= ImGui::SliderFloat("区域边长", &s.rippleSize, 16.0f, 256.0f, "%.0f m");
    int res = s.rippleResolution == 128 ? 0 : (s.rippleResolution == 512 ? 2 : 1);
    static const char* kResolutions[] = {"128²", "256²", "512²"}; // 不用 "a\0b\0" 形式：\0256 会被当成八进制转义
    if (ImGui::Combo("分辨率", &res, kResolutions, 3)) {
        s.rippleResolution = 128 << res;
        c = true;
    }
    c |= ImGui::SliderFloat("波速", &s.rippleMaxSpeed, 0.5f, 15.0f, "%.1f m/s");
    theme::helpMarker("深水里的传播速度；浅水按 √(g·水深) 自动变慢。\n超过稳定性条件（0.5 × 格子 / 物理步长）时自动截断。");
    c |= ImGui::SliderFloat("阻尼", &s.rippleDamping, 0.0f, 3.0f, "%.2f /s");
    c |= ImGui::SliderFloat("搅动强度", &s.rippleStrength, 0.0f, 30.0f, "%.1f");
    c |= ImGui::SliderFloat("法线强度##ripple", &s.rippleNormal, 0.0f, 4.0f, "%.2f");
    c |= ImGui::SliderFloat("吸收层宽度", &s.rippleSpongeWidth, 0.05f, 0.4f, "%.2f");
    c |= ImGui::SliderFloat("吸收层强度", &s.rippleSpongeStrength, 0.0f, 30.0f, "%.1f /s");
    c |= ImGui::SliderFloat("尾迹泡沫阈值", &s.rippleFoamSlope, 0.0f, 1.0f, "%.2f");
    c |= ImGui::SliderFloat("尾迹泡沫强度", &s.rippleFoamGain, 0.0f, 20.0f, "%.1f");
    c |= ImGui::SliderFloat("尾迹消散时间", &s.rippleFoamDecay, 0.1f, 10.0f, "%.1f s");
    if (theme::toolButton("◉ 在区域中心激起一圈波纹", false, "把水面压下 0.3 米（区域中心 = 玩家位置；在岸上看不到效果）"))
        o.splash(rs.center, 1.0f, 0.3f);

    if (ImGui::CollapsingHeader("网格与参考实现")) {
        c |= ImGui::SliderInt("环数", &s.gridRings, 16, 512);
        c |= ImGui::SliderInt("每环分段", &s.gridSegments, 16, 512);
        c |= ImGui::SliderFloat("覆盖半径", &s.gridOuter, 200.0f, 20000.0f, "%.0f m", ImGuiSliderFlags_Logarithmic);
        c |= ImGui::SliderInt("Gerstner 波数", &s.gerstnerWaves, 1, int(ocean::kMaxGerstnerWaves));
    }
    if (c) o.setSettings(s);
    ImGui::End();
}

} // namespace eng
