#pragma once
// 渲染特性接口：地形、海洋、植被等「特性模块」通过它把自己的 pass 挂进渲染器的 Render Graph。
//
// 为什么需要它：Renderer 在 render 模块里，地形/海洋在更上层的模块里。
// 依赖只能向下，Renderer 不能 include 地形；反过来，地形实现这个接口，App 把它注册给 Renderer。
// Renderer 在固定的「阶段」调用各特性，阶段顺序决定了 pass 顺序。
//
// EXTEND: M5 海洋在 addTransparentPasses 里画水面；M6.5 大气在 addSkyPasses 之类的新阶段里加体积云。

#include "render/RenderGraph.h"
#include "render/Shadows.h"

namespace eng {

class PipelineCache;
struct RenderView;

// 一帧里所有特性都可能用到的共享目标与数据
struct SceneTargets {
    rg::TextureHandle color;       // HDR 场景颜色
    rg::TextureHandle depth;       // 场景深度（反向 Z）
    rg::TextureHandle shadowAtlas; // 级联阴影图集
    const ShadowCascades* cascades = nullptr;
    const ShadowSettings* shadowSettings = nullptr;
    const RenderView* view = nullptr;
    VkExtent2D sceneExtent{};
};

class RenderFeature {
public:
    virtual ~RenderFeature() = default;
    virtual const char* name() const = 0;
    // 往阴影图集里画投影物（在网格阴影 pass 之后，LOAD 图集）
    virtual void addShadowPasses(rg::RenderGraph&, const SceneTargets&) {}
    // 不透明几何（在网格 Forward pass 之前；第一个写 color/depth 的 pass 负责 CLEAR）
    virtual void addOpaquePasses(rg::RenderGraph&, const SceneTargets&) {}
    // 天空之后、色调映射之前（半透明、水面）
    virtual void addTransparentPasses(rg::RenderGraph&, const SceneTargets&) {}
};

} // namespace eng
