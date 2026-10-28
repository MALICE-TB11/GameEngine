#pragma once
// 局部交互：玩家、物体搅动出的波纹。这里是纯 CPU 的参考实现（可单元测试）；
// GPU 版与这里一行对一行，自测（--selftest ocean）逐格比较两者：
//   波动方程 rippleStep  ↔ shaders/ocean/ocean_ripples.hlsl
//   浅水方程 shallowStep ↔ shaders/ocean/ocean_shallow_flux.hlsl + ocean_shallow_depth.hlsl
//
// 为什么需要单独一层：大海浪（FFT / Gerstner）是「预先按频谱生成的统计海面」，它不知道岛在哪、玩家在哪。
// 波纹层叠加在大海浪之上：以玩家为中心的一块 N×N 网格，每个物理固定步推进一步（暂停 / 慢放时也停 / 慢）。
// 两种求解器，编辑器里切换对比：
//
// ---- 波动方程（参考，最简单） ----
//   ∂²u/∂t² = c²∇²u − γ·∂u/∂t                u：水面起伏（米）；c：波速；γ：阻尼
//   蛙跳格式：u⁺ = u + (u − u⁻)(1 − γΔt) + c²Δt²·∇²u          ∇²u ≈ (左 + 右 + 下 + 上 − 4u) / Δx²
//   岸：水深 ≤ kRippleMinDepth 的格子没有水，邻居把它当成「和自己一样高」（法向梯度为零）→ 波纹被反射
//
// ---- 浅水方程（交错网格上的线性化动量 + 守恒的质量） ----
//   每条格子边上存一个有符号的体积流量 F（m³/s，Fx 在格子与右邻居之间，Fz 在格子与上邻居之间）：
//     动量：F⁺ = F(1 − γΔt) + Δt·g·d_边·(h_左 − h_右)          h = 床面 + 水深（水面的绝对高度）
//     质量：d⁺ = d + Δt·(流入 − 流出)/Δx²
//   比波动方程多的：水量严格守恒、水能冲上沙滩再退回（湿/干格子自然处理）、有流速（给 M6 的 Medium::Query）。
//   线性、水深处处 ≥ h_cap 时两者的离散方程完全相同（单元测试逐格比较），区别只在岸边和大振幅。
//   为什么不用经典的「虚拟管道」（每个格子 4 根单向管子）：两根反向的管子同时有流量时推力加倍，
//   波速落在 √(gh) 与 √(2gh) 之间（原型里测到快 25%）。每条边一个有符号流量就没有这个问题。
//   不会出现负水深：每条边流出的量不超过上游格子水量的 1/4（四条边加起来也不会抽干）。
//
// ---- 两者共用 ----
// 波速：c² = min(g·h, c_max²)。浅水里波按 √(gh) 变慢（靠近岸边会转向、堆高）；
//   深水里真实的小波纹有色散（短的慢、长的快），这两个模型都表示不了，用 c_max 截断成「看起来对」的速度。
//   浅水方程里这个截断加在「边上的有效水深」d_边 = min(d, h_cap)，h_cap = c_max²/g，真实水量不变。
// 稳定性（CFL 条件）：c·Δt/Δx ≤ 1/√2；留一半余量取 0.5，c_max 超过时自动截断。
// 区域边缘：吸收层（海绵层）里速度阻尼 γ 按 ramp² 增大，波纹走进去就被吃掉。只阻尼「速度」（波动方程的 u − u⁻、
//   浅水方程的流量），不要把高度往 0 拉：那样会凭空产生速度，把能量困在吸收层里（单元测试测到 40% 降不下去）。
// 区域跟着玩家走：原点按整格对齐。移动了几格，读上一步状态时就偏移几格（移出去的丢掉，移进来的是平静水面）。
// 扰动源是「体积流量」：每秒往这块水面加（正）或减（负）多少米的水，见 RippleSource。

#include "ocean/OceanSettings.h"

#include <array>
#include <vector>

namespace eng::ocean {

inline constexpr uint32_t kMaxRippleSources = 7; // 每步最多几个扰动源（GPU 常量缓冲按它分配）
inline constexpr float kRippleMinDepth = 0.05f;  // 波动方程：水深小于它的格子视为岸（米）
inline constexpr float kShallowWetDepth = 0.02f; // 浅水方程：水柱比它薄就画成「干的」（波纹高度取 0，藏在地形下面）
inline constexpr float kRippleCfl = 0.5f;        // 稳定性系数：c·Δt/Δx 的上限（理论上限 1/√2）

enum class RippleSolver : int {
    Wave = 0,    // 波动方程（参考）
    Shallow = 1, // 浅水方程
};

// 一个扰动源：在 position 附近、半径 radius 的高斯范围 G 内往水面加 / 减水，体积流量（m/s = 每秒多少米的水）
//   q = G · (strength + push·d / radius)        d = 格子中心 - position
// strength：竖直方向。物体往下沉，排开的水从周围冒出来（正）；往上走，水回填（负）。一来一回抵消
// push：水平方向的「偶极子」：前面加水（船首波）、后面减水（尾迹），总量为零 —— 物体在水里走动时把水推开。
//   推导：物体在水里占的「厚度」B = 入水深度 s × 高斯，以速度 v 移动时 q = -v·∇B = (2s/r)·(v·d/r)·G，即 push = (2s/r)·v
// 水花：一步里 q = -深度/Δt，正好挖出一个静止的凹坑，之后像石子入水一样扩散成一圈。
// 为什么是体积流量而不是加速度：两个求解器都能直接用（浅水方程的质量方程本来就有这一项）；
// 而「持续的单向加速度」在二维波动方程里会让凹坑越陷越深（第一版就是这样，看到的是一团泡沫而不是波纹）。
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
    RippleSolver solver = RippleSolver::Wave;
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
// 扰动源在点 x 处的体积流量之和（m/s）
float rippleSourceRate(const RippleStepInput& step, vec2 x);

// 网格状态：每格一个 vec4，与 GPU 的 RGBA32F 贴图逐位对应。下标 = j·n + i，i 沿 +X、j 沿 +Z。
//   波动方程：(u 本步, u 上一步, 尾迹泡沫, 静水深)
//   浅水方程：(波纹高度 η, η 上一步, 尾迹泡沫, 高出静水的水量 δ)   —— 流量另存一张：(Fx, Fz, 0, 0)
// 浅水方程存 δ 而不是总水深 d：深海里 d ≈ 40 m，float 只能分辨约 4 µm，毫米级的波纹每步的变化会被舍入吃掉一大半；
// 存 δ 时水面高度 h = max(床面, 海面) + δ 也是精确的（静水时相邻格子的 h 完全相等，不会凭空流动）。
using RippleGrid = std::vector<vec4>;

// 波动方程推进一步。in：上一步的状态（hasHistory = false 时当作全是平静水面）；depth：本步网格每格的静水深（米）
void rippleStep(const RippleGrid& in, bool hasHistory, const std::vector<float>& depth, const RippleParams& p,
                const RippleStepInput& step, RippleGrid& out);

// 浅水方程推进一步。bed：本步网格每格的床面高度；waterLevel：平均海面高度。
// EXTEND: 动量的平流项 (v·∇)v（半拉格朗日）—— 现在是线性化动量，浪在浅滩上卷曲、破碎前变陡这类非线性效果没有；
//         第三种求解器 iWave（深水小波纹的正确色散），见 docs/06-海洋.md「扩展点」
// GPU 分两个 pass：先算所有边的流量（outFlux），再用流量更新水量（outState）
void shallowStep(const RippleGrid& inState, const RippleGrid& inFlux, bool hasHistory, const std::vector<float>& bed, float waterLevel,
                 const RippleParams& p, const RippleStepInput& step, RippleGrid& outState, RippleGrid& outFlux);
// 浅水方程某格的流速（m/s，格子中心；两侧边上的流量平均后除以过水断面）。M6 的 Medium::Query 用
vec2 shallowVelocity(const RippleGrid& state, const RippleGrid& flux, const std::vector<float>& bed, float waterLevel,
                     const RippleParams& p, int i, int j);

// 离散能量 Σ[((u − u⁻)/Δt)² + c²|∇u|²]·Δx²（测试用：没有阻尼、波纹没碰到边界时应近似守恒）
double rippleEnergy(const RippleGrid& g, const std::vector<float>& depth, const RippleParams& p, float dt);

} // namespace eng::ocean
