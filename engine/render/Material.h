#pragma once
// 材质：参数 + 贴图。
//
// 材质参数走 push constant（每次 draw 64 字节），而不是常量缓冲：
//   - ImGui 里拖动滑条，下一帧立刻生效，不需要处理「GPU 还在读旧缓冲」的同步问题
//   - 不需要为每个材质管理一块显存
// 贴图走 set 1（每帧按材质分配描述符集），缺失的贴图用默认贴图（白 / 平坦法线 / 黑）代替。
//
// 着色模型（ShadingModel）决定 shader 里用哪个光照映射：PBR 给环境，Toon 给角色（M7）。
// 两者拿到的是同一份 LightingInputs —— 「风格化 = 物理光照之上的映射」。

#include "math/MathTypes.h"

#include <string>

namespace eng {

namespace rhi { struct Image; }

enum class ShadingModel : uint32_t {
    PBR = 0,
    Toon = 1, // EXTEND: M7
};

// GPU 布局：必须与 shaders/common/surface.hlsli 的 MaterialParams 一致
struct MaterialParams {
    vec4 baseColor{1.0f};                       // 线性 RGBA
    vec4 emissive{0.0f, 0.0f, 0.0f, 1.0f};      // rgb = 自发光（线性，可 >1），w = 法线贴图强度
    vec4 pbr{0.0f, 0.5f, 1.0f, 0.5f};           // x = 金属度，y = 粗糙度，z = AO 强度，w = alpha 裁剪阈值
    vec4 flags{0.0f};                           // x = ShadingModel，y = 自发光强度倍数
};
static_assert(sizeof(MaterialParams) == 64, "MaterialParams 必须是 64 字节（push constant 的后半部分）");

struct Material {
    std::string name = "Default";
    MaterialParams params;
    // 为空表示「没有这张贴图」，绑定时用默认贴图
    const rhi::Image* baseColorTex = nullptr;  // sRGB
    const rhi::Image* normalTex = nullptr;     // 切线空间，线性
    const rhi::Image* metalRoughTex = nullptr; // glTF 约定：G = 粗糙度，B = 金属度
    const rhi::Image* occlusionTex = nullptr;  // R = AO
    const rhi::Image* emissiveTex = nullptr;   // sRGB
    bool doubleSided = false;

    ShadingModel shadingModel() const { return static_cast<ShadingModel>(uint32_t(params.flags.x)); }
};

} // namespace eng
