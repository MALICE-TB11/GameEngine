#pragma once
// 高度场：一张规则网格上的高度采样（米）。岛屿和海床是同一张高度场 —— 海平面 y = 0 以下就是海床。
//
// 纯 CPU 数据：渲染（上传成高度贴图）、物理（Jolt 高度场碰撞）、玩法（查询地面高度）共用同一份，
// 保证「看到的地面」和「站立的地面」完全一致。

#include "math/MathTypes.h"

#include <cstdint>
#include <vector>

namespace eng {
class JobSystem;
}

namespace eng::terrain {

struct TerrainSettings {
    // ---- 网格 ----
    uint32_t resolution = 1025; // 每边采样数（2^n + 1：四叉树每一级都能对齐到采样点）
    float worldSize = 1024.0f;  // 覆盖的世界尺寸（米），中心在原点
    // ---- 程序化岛屿 ----
    uint32_t seed = 7;
    vec2 islandCenter{0.0f, -140.0f}; // 岛心的世界 XZ：原点落在岛的南侧低地上，离海岸几十米
    float islandRadius = 230.0f; // 海岸线大致所在的半径
    float coastNoise = 0.35f;    // 海岸线的不规则程度
    float maxHeight = 42.0f;     // 山顶高度
    float seabedDepth = 55.0f;   // 远海海床深度（正数，向下）
    float hillScale = 160.0f;    // 丘陵的特征尺寸（米）
    float plateauRadius = 26.0f; // 原点附近整平的平台（放遗迹、角色出生点）
    float plateauHeight = 2.0f;  // 平台高度：略高于海平面，M5 的海浪不会淹没它
    // ---- 渲染 ----
    float lodDistanceFactor = 2.5f; // 节点尺寸 × 该系数 > 到相机距离时继续细分；越大越精细
    float skirtDepth = 2.0f;        // 裙边下垂深度（米 × 节点级别），遮住相邻 LOD 之间的裂缝
    // ---- 分层材质（高度/坡度阈值，见 shaders/materials/terrain.hlsl） ----
    float sandMaxHeight = 5.0f;   // 沙滩：这个高度以下
    float reefMaxHeight = -1.5f;  // 礁石：海平面以下
    float rockSlope = 0.45f;      // 坡度（1 - normal.y）超过它变成岩石
    float mossMinHeight = 8.0f;   // 苔藓：这个高度以上的缓坡
    vec4 layerTiling{6.0f, 9.0f, 7.0f, 5.0f};       // 沙/岩/礁/苔 每张贴图覆盖多少米
    vec4 layerRoughness{0.9f, 0.8f, 0.65f, 0.85f};  // 沙/岩/礁/苔 的粗糙度（干燥时；潮湿会再降低）
    float blendSharpness = 0.25f; // 基于高度的混合宽度：越小过渡越锐利
};

class Heightfield {
public:
    // jobs 不为空时按行并行生成（100 万个采样 × 十几层噪声，Debug 构建单线程要好几秒）
    void generate(const TerrainSettings& settings, JobSystem* jobs = nullptr);

    // 世界坐标查询（双线性插值）；超出范围时夹到边缘
    float heightAt(float x, float z) const;
    vec3 normalAt(float x, float z) const;

    uint32_t resolution() const { return m_resolution; }
    float worldSize() const { return m_worldSize; }
    float spacing() const { return m_worldSize / float(m_resolution - 1); }
    vec2 origin() const { return vec2(-m_worldSize * 0.5f); } // 采样 (0,0) 的世界 XZ
    float minHeight() const { return m_min; }
    float maxHeight() const { return m_max; }
    const std::vector<float>& samples() const { return m_heights; } // 行优先：index = z * resolution + x
    float sample(uint32_t x, uint32_t z) const { return m_heights[size_t(z) * m_resolution + x]; }

    // 某个矩形区域（采样坐标）内的最小/最大高度：四叉树节点包围盒用
    void rangeMinMax(uint32_t x0, uint32_t z0, uint32_t x1, uint32_t z1, float& outMin, float& outMax) const;

private:
    uint32_t m_resolution = 0;
    float m_worldSize = 0.0f;
    float m_min = 0.0f, m_max = 0.0f;
    std::vector<float> m_heights;
};

} // namespace eng::terrain
