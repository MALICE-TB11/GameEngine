// 「地形与物理」面板：地形 LOD / 分层材质 / 岛屿生成参数，物理世界统计。
#include "editor/Editor.h"

#include "editor/EditorInternal.h"
#include "editor/EditorTheme.h"
#include "physics/PhysicsWorld.h"
#include "terrain/TerrainRenderer.h"
#include "world/ScenePhysics.h"

#include <imgui.h>

namespace eng {

using namespace editor;

void Editor::drawWorldPanel() {
    if (!ImGui::Begin(kWorldWindow)) {
        ImGui::End();
        return;
    }

    if (m_ctx.terrain && m_ctx.terrainSettings) {
        terrain::TerrainRenderer& tr = *m_ctx.terrain;
        terrain::TerrainSettings& ts = *m_ctx.terrainSettings;
        bool changed = false;

        theme::sectionHeader("地形渲染");
        ImGui::Checkbox("显示地形", &tr.enabled);
        ImGui::SameLine();
        ImGui::Checkbox("投射阴影", &tr.castShadows);
        ImGui::Checkbox("线框", &tr.wireframe);
        ImGui::SameLine();
        ImGui::Checkbox("按 LOD 着色", &tr.showLod);
        changed |= ImGui::SliderFloat("LOD 距离系数", &ts.lodDistanceFactor, 0.5f, 8.0f, "%.2f");
        theme::helpMarker("节点到相机的距离 < 节点边长 × 系数 时继续细分。\n调大 = 近处更精细、三角形更多；配合「按 LOD 着色」观察。");
        changed |= ImGui::SliderFloat("裙边深度", &ts.skirtDepth, 0.0f, 8.0f, "%.1f × 网格间距");
        theme::helpMarker("相邻节点 LOD 不同时，边上会出现细小裂缝。\n裙边是节点四周向下垂的一圈网格，从下面把裂缝挡住。设为 0 可以看到裂缝。");

        const terrain::TerrainStats& st = tr.stats();
        ImGui::TextDisabled("本帧 %u 个节点 · %.1fk 三角形 · 阴影 %u 次绘制", st.nodes, double(st.triangles) / 1000.0, st.shadowNodes);
        std::string levels;
        for (size_t i = 0; i < st.perLevel.size(); ++i)
            if (st.perLevel[i]) levels += "L" + std::to_string(i) + "×" + std::to_string(st.perLevel[i]) + "  ";
        ImGui::TextDisabled("%s", levels.c_str());

        theme::sectionHeader("分层材质");
        changed |= ImGui::SliderFloat("沙滩上限", &ts.sandMaxHeight, -2.0f, 20.0f, "%.1f m");
        changed |= ImGui::SliderFloat("礁石上限", &ts.reefMaxHeight, -20.0f, 2.0f, "%.1f m");
        changed |= ImGui::SliderFloat("苔藓起始", &ts.mossMinHeight, 0.0f, 40.0f, "%.1f m");
        changed |= ImGui::SliderFloat("岩石坡度", &ts.rockSlope, 0.05f, 0.95f, "%.2f");
        theme::helpMarker("坡度 = 1 - 法线.y：0 是平地，1 是垂直的崖壁。超过阈值的地方变成岩石。");
        changed |= ImGui::SliderFloat("高度混合宽度", &ts.blendSharpness, 0.01f, 1.0f, "%.2f");
        theme::helpMarker("各层贴图的 alpha 存的是「高度」。混合时只保留最高的一档，\n宽度越小过渡越锐利（石头从沙里冒出来），越大越像普通的半透明混合。");
        changed |= ImGui::DragFloat4("贴图尺寸 沙/岩/礁/苔", &ts.layerTiling.x, 0.1f, 0.5f, 50.0f, "%.1f m");
        changed |= ImGui::SliderFloat4("粗糙度 沙/岩/礁/苔", &ts.layerRoughness.x, 0.05f, 1.0f, "%.2f");
        if (changed) tr.setSettings(ts);

        theme::sectionHeader("岛屿生成");
        int seed = int(ts.seed);
        if (ImGui::InputInt("随机种子", &seed)) ts.seed = uint32_t(std::max(seed, 0));
        ImGui::DragFloat2("岛心位置 XZ", &ts.islandCenter.x, 1.0f, -400.0f, 400.0f, "%.0f m");
        theme::helpMarker("原点附近总会被整平成平台（放遗迹和出生点）。\n岛心离原点越远，平台越靠近海岸。");
        ImGui::SliderFloat("岛屿半径", &ts.islandRadius, 60.0f, 450.0f, "%.0f m");
        ImGui::SliderFloat("海岸不规则度", &ts.coastNoise, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("山顶高度", &ts.maxHeight, 0.0f, 120.0f, "%.0f m");
        ImGui::SliderFloat("海床深度", &ts.seabedDepth, 5.0f, 150.0f, "%.0f m");
        ImGui::SliderFloat("丘陵尺度", &ts.hillScale, 30.0f, 400.0f, "%.0f m");
        ImGui::SliderFloat("平台半径", &ts.plateauRadius, 0.0f, 80.0f, "%.0f m");
        ImGui::SliderFloat("平台高度", &ts.plateauHeight, -5.0f, 20.0f, "%.1f m");
        if (theme::toolButton("↻ 重新生成", false, "用上面的参数重新生成高度场；渲染与碰撞体一起更新") && m_ctx.regenerateTerrain)
            m_ctx.regenerateTerrain();
        ImGui::SameLine();
        ImGui::TextDisabled("分辨率 %u² · %.0f m", ts.resolution, ts.worldSize);
    }

    if (m_ctx.physics && m_ctx.physics->initialized()) {
        theme::sectionHeader("物理（Jolt）");
        const physics::PhysicsStats ps = m_ctx.physics->stats();
        ImGui::Text("刚体 %u（活动 %u）· 形状 %u · 角色 %u", ps.bodies, ps.activeBodies, ps.shapes, ps.characters);
        ImGui::TextDisabled("最近一步 %.3f ms（CPU）", ps.stepMs);
        if (m_ctx.scenePhysics) {
            const ScenePhysics::Stats& ss = m_ctx.scenePhysics->stats();
            ImGui::TextDisabled("场景实体：静态 %u · 动态 %u（休眠 %u）· 等待加载 %u", ss.staticBodies, ss.dynamicBodies, ss.sleeping,
                                ss.waiting);
        }
        vec3 g = m_ctx.physics->gravity();
        if (ImGui::SliderFloat("重力", &g.y, -30.0f, 0.0f, "%.2f m/s²")) m_ctx.physics->setGravity(g);
        theme::helpMarker("动态刚体静止一段时间后会「休眠」，Jolt 不再求解它们，直到被碰到。\n在检视器里把实体的「碰撞」设为动态即可让它受物理驱动。");
    }

    if (m_ctx.worldPanelExtra) {
        theme::sectionHeader("玩家");
        m_ctx.worldPanelExtra();
    }
    ImGui::End();
}

} // namespace eng
