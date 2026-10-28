#pragma once
// 海洋参数。全部可在编辑器「海洋」面板里实时调节，保存到 config/engine.json 的 "ocean" 节。
//
// 「风浪」一节决定海浪的统计特征（JONSWAP 频谱）：风越大、风区越长，浪越高、越长；
// 「波高」和「尖锐度」是在物理结果之上的艺术控制。

#include "math/MathTypes.h"

#include <cstdint>

namespace eng::ocean {

// 两种实现：参考（简单、正确、CPU 可查询）与优化（真实感、GPU）
enum class OceanMode : int {
    Gerstner = 0, // 参考实现：从同一份频谱里采样几十个 Gerstner 波，顶点着色器直接求和
    Fft = 1,      // 优化实现：三级联 256² 频谱 → GPU 逆 FFT，几十万个波同时叠加
};

inline constexpr uint32_t kOceanCascades = 3;
inline constexpr uint32_t kOceanFftSize = 256; // 必须与 shaders/ocean/ocean_common.hlsli 的 OCEAN_N 一致
inline constexpr uint32_t kMaxGerstnerWaves = 64;

struct OceanSettings {
    bool enabled = true;
    int mode = int(OceanMode::Fft);

    // ---- 风浪（JONSWAP / TMA 频谱） ----
    float windSpeed = 9.0f;           // 10 米高处风速（m/s）
    float windDirectionDeg = 250.0f;  // 浪的传播方向：0° = +X，90° = +Z
    float fetchKm = 150.0f;           // 风区长度（风吹过的海面距离）：越长浪越成熟、越高
    float depth = 40.0f;              // 平均水深（米），用于浅水色散关系与 TMA 修正
    float peakEnhancement = 3.3f;     // JONSWAP 的 γ：越大能量越集中在峰值频率（浪更规整）
    float spread = 6.0f;              // 方向集中度 s（cos^2s 分布）：越大越顺着风向，越小越「乱」
    float swell = 0.0f;               // 0~1：混入一部分来自远方的长周期涌浪（更规整、更长）
    float heightScale = 1.0f;         // 波高倍数（艺术控制）
    float choppiness = 1.0f;          // 水平位移倍数：越大浪尖越尖，过大会自相交（泡沫最多的地方）
    vec4 cascadeSizes{250.0f, 37.0f, 5.3f, 0.0f}; // 三个级联覆盖的海面尺寸（米），互不整除避免重复感

    // ---- 泡沫 ----
    float foamBias = 0.9f;      // 雅可比行列式低于它就开始起泡（1 = 平静水面，< 0 = 水面自相交）
    float foamIntensity = 3.0f; // 新生泡沫的强度
    float foamDecay = 2.5f;     // 泡沫消散的时间常数（秒）
    float shoreFoamWidth = 2.0f; // 岸边泡沫带宽度（米，按水深算）
    float foamTiling = 3.0f;     // 泡沫细节贴图覆盖多少米

    // ---- 着色 ----
    vec3 absorption{0.45f, 0.09f, 0.06f}; // 按 RGB 的吸收系数（1/米）：红光最先被吃掉，深水发蓝绿
    vec3 scatterColor{0.01f, 0.07f, 0.09f}; // 水体散射颜色（线性，乘以天光/阳光）
    vec3 sssColor{0.08f, 0.40f, 0.32f};     // 浪尖透光的颜色
    float sssStrength = 1.0f;
    float roughness = 0.06f;      // 水面微观粗糙度（太阳高光的大小）
    float refraction = 0.04f;     // 折射扭曲强度（屏幕空间）
    float reflection = 1.0f;      // 天空反射强度

    // ---- 网格 ----
    int gridRings = 192;      // 以相机为中心的极坐标网格：环数
    int gridSegments = 256;   // 每环的分段数
    float gridInner = 0.5f;   // 第一环半径（米）
    float gridOuter = 6000.0f; // 最外环半径（米）：再远就是地平线
    int gerstnerWaves = 32;   // 参考实现的波数

    // ---- 局部交互（玩家、物体搅动出的波纹；见 OceanRipples.h） ----
    bool ripples = true;
    int rippleSolver = 1;             // 0 = 波动方程（参考），1 = 浅水方程（水量守恒、能冲上沙滩、有流速）
    float rippleSize = 64.0f;         // 模拟区域边长（米），以玩家为中心、随玩家移动
    int rippleResolution = 256;       // 每边格子数（128 / 256 / 512）：格子越小，能表示的波纹越短
    float rippleMaxSpeed = 4.0f;      // 深水中波纹的传播速度上限（m/s），浅水按 √(g·h) 变慢；还受稳定性条件限制
    float rippleDamping = 0.3f;       // 全局阻尼（1/秒）：波纹自然消失的快慢
    float rippleSpongeWidth = 0.15f;  // 区域边缘吸收层的宽度（占边长的比例）：波纹在这里被「吃掉」，不会从边界反弹
    float rippleSpongeStrength = 6.0f; // 吸收层最外圈的阻尼（1/秒）
    float rippleStrength = 1.0f;      // 物体搅动水面的强度：推开的水量 = 它 × 速度 × 入水比例（无量纲）
    float rippleNormal = 1.0f;        // 波纹对法线的贡献倍数（艺术控制）
    float rippleFoamSlope = 0.12f;    // 波纹坡度超过它开始起泡（尾迹泡沫）
    float rippleFoamGain = 4.0f;      // 尾迹泡沫的强度
    float rippleFoamDecay = 1.5f;     // 尾迹泡沫的消散时间（秒）
};

} // namespace eng::ocean
