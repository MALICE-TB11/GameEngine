#pragma once
// 每帧常量：CPU 端结构体，必须与 shaders/common/frame.hlsli 的 FrameConstants 逐字段一致。
// 规则：全部用 16 字节对齐的类型（mat4 / vec4），避免 HLSL 常量缓冲打包规则带来的隐式填充。
// 改这里时必须同步改 HLSL，static_assert 会帮你检查大小。

#include "math/MathTypes.h"
#include "render/Material.h"

namespace eng {

struct FrameConstants {
    mat4 view;
    mat4 proj;
    mat4 viewProj;
    mat4 invViewProj;
    vec4 cameraPos;     // xyz = 相机世界坐标
    vec4 screen;        // xy = 分辨率，zw = 1/分辨率
    vec4 time;          // x = 秒，y = 帧间隔，z = 帧序号
    // ---- 光照输入（M3）：所有材质（PBR 与 toon）共用这一份 ----
    vec4 sunDirection;  // xyz = 指向太阳的单位向量，w = 太阳照度（lux 的相对值）
    vec4 sunColor;      // rgb = 太阳颜色（线性），w = 太阳角半径（弧度）
    vec4 skyZenith;     // rgb = 天顶颜色（线性辐亮度），w = 天空亮度倍数
    vec4 skyHorizon;    // rgb = 地平线颜色，w = 太阳光晕强度
    vec4 groundColor;   // rgb = 地面反弹颜色（半球环境光的下半部分）
    vec4 exposure;      // x = 曝光倍数，y = 色调映射模式（0=ACES 1=AgX 2=无），z = 环境光强度
    vec4 debug;         // x = 调试视图模式（0=正常 1=法线 2=albedo 3=粗糙度 4=金属度）
};
static_assert(sizeof(FrameConstants) == 64 * 4 + 16 * 10, "FrameConstants 大小与 frame.hlsli 不一致");

// 网格 draw 的 push constant（128 字节上限）：模型矩阵 + 材质参数。
// 法线矩阵（逆转置）不传，在 VS 里用叉积算余子式矩阵得到（见 shaders/common/mesh.hlsli），省 64 字节。
struct DrawPush {
    mat4 model;
    MaterialParams material;
};
static_assert(sizeof(DrawPush) == 128, "push constant 必须正好 128 字节，与 mesh.hlsli 一致");

} // namespace eng
