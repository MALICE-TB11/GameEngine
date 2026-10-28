#pragma once
// 局部交互：玩家、物体搅动出的波纹。这里是纯 CPU 的参考实现（可单元测试）；
// GPU 版 shaders/ocean/ocean_ripples.hlsl 与 rippleStep 一行对一行，自测（--selftest ocean）逐格比较两者。
//
// 为什么需要单独一层：大海浪（FFT / Gerstner）是「预先按频谱生成的统计海面」，它不知道岛在哪、玩家在哪。
// 波纹层叠加在大海浪之上：以玩家为中心的一块 N×N 网格，每个物理固定步解一次二维波动方程
//   ∂²u/∂t² = c²∇²u − γ·∂u/∂t + f
//   u：水面相对大海浪的起伏（米）；c：波速；γ：阻尼；f：物体推动水面的加速度
// 离散（蛙跳格式，Δt = 物理步长，所以暂停 / 慢放时波纹也停 / 慢）：
//   u⁺ = u + (u − u⁻)(1 − γΔt) + c²Δt²·∇²u + Δt²·f          ∇²u ≈ (左 + 右 + 下 + 上 − 4u) / Δx²
// 边界：
//   岸：水深 ≤ kRippleMinDepth 的格子没有水。邻居算拉普拉斯时把它当成「和自己一样高」（法向梯度为零）——
//       波纹撞到岸上会反射回来
//   区域边缘：吸收层（海绵层）。越靠近边缘阻尼越大，波纹走进去就被吃掉，不会从这条「假边界」反弹
// 波速：c² = min(g·h, c_max²)。浅水里波按 √(gh) 变慢（靠近岸边会转向、堆高）；
//   深水里真实的小波纹有色散（短的慢、长的快），线性波动方程表示不了，用 c_max 截断成「看起来对」的速度。
//   EXTEND: 第二种求解器（浅水方程或 iWave）在这里加，见 docs/06-海洋.md「局部交互」
// 稳定性（CFL 条件）：二维蛙跳要求 c·Δt/Δx ≤ 1/√2；留一半余量取 0.5，c_max 超过时自动截断。
// 区域跟着玩家走：原点按整格对齐。移动了几格，读上一步状态时就偏移几格（移出去的丢掉，移进来的是平静水面）。

#include "ocean/OceanSettings.h"

#include <array>
#include <vector>

namespace eng::ocean {

inline constexpr uint32_t kMaxRippleSources = 7; // 每步最多几个扰动源（GPU 常量缓冲按它分配）
inline constexpr float kRippleMinDepth = 0.05f;  // 水深小于它的格子视为岸（米）
inline constexpr float kRippleCfl = 0.5f;        // 稳定性系数：c·Δt/Δx 的上限（理论上限 1/√2）

// 一个扰动源：在 position 附近、半径 radius 的高斯范围 G 内推动水面，加速度（m/s²）
//   f = G · (strength + push·d / radius)        d = 格子中心 - position
// strength：竖直方向，整片往上（正）或往下（负）推 —— 物体入水 / 出水
// push：水平方向的「偶极子」：前面推高、后面拉低，总体积为零 —— 物体在水里走动时的船首波和尾迹。
// 为什么不用「一直往下压」：二维波动方程里持续的单向推力会让下陷的体积不断增长（或在整个区域里形成对数形的大坑），
// 而不是激起向外传播的波纹；所有源在一个来回里的净体积都应该接近零。
struct RippleSource {
    vec2 position{0.0f};
    float radius = 0.5f;
    float strength = 0.0f;
    vec2 push{0.0f};
};

// 一个固定步的输入
struct RippleStepInput {
    float dt = 1.0f / 60.0f;
    ivec2 shift{0};    // 区域原点这一步移动了几格（读上一步状态时的偏移）
    vec2 origin{0.0f}; // 区域原点：格子 (0,0) 左下角的世界 XZ
    uint32_t sourceCount = 0;
    std::array<RippleSource, kMaxRippleSources> sources{};
};

// 由设置换算出的求解参数（GPU 用同一组数）
struct RippleParams {
    uint32_t n = 256;          // 每边格子数
    float cellSize = 0.25f;    // 格子边长（米）
    float maxSpeed = 4.0f;     // 波速上限（已按 CFL 条件截断）
    float damping = 0.3f;      // γ（1/秒）
    float spongeCells = 38.0f; // 吸收层宽度（格）
    float spongeStrength = 6.0f;
    float foamSlope = 0.12f;
    float foamGain = 4.0f;
    float foamDecay = 1.5f;
};
RippleParams makeRippleParams(const OceanSettings& s, float dt);
// CFL 条件允许的最大波速
float rippleSpeedLimit(float cellSize, float dt);
// 以 center 为中心、按整格对齐的区域原点（格子坐标；世界坐标 = 格子坐标 × cellSize）
ivec2 rippleOriginCell(vec2 center, const RippleParams& p);

// 网格状态：每格 vec4 = (u 本步, u 上一步, 尾迹泡沫, 水深) —— 与 GPU 的 RGBA32F 贴图逐位对应。
// 下标 = j·n + i，i 沿 +X、j 沿 +Z。
using RippleGrid = std::vector<vec4>;

// 推进一步。in：上一步的状态（hasHistory = false 时当作全是平静水面）；depth：本步网格每格的水深（米）
void rippleStep(const RippleGrid& in, bool hasHistory, const std::vector<float>& depth, const RippleParams& p,
                const RippleStepInput& step, RippleGrid& out);

// 离散能量 Σ[((u − u⁻)/Δt)² + c²|∇u|²]·Δx²（测试用：没有阻尼、波纹没碰到边界时应近似守恒）
double rippleEnergy(const RippleGrid& g, const std::vector<float>& depth, const RippleParams& p, float dt);

} // namespace eng::ocean
