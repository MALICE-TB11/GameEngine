#pragma once
// 渲染器的输入：相机视图 + 一组 draw。由 App/World 每帧填写，渲染器只读。

#include "math/MathTypes.h"

#include <vector>

namespace eng {

struct GpuMesh;
struct Material;
struct MaterialParams;

struct RenderView {
    mat4 view{1.0f};
    mat4 proj{1.0f};
    vec3 position{0.0f};
    double time = 0.0;
    float dt = 0.0f;
};

struct DrawItem {
    const GpuMesh* mesh = nullptr;
    mat4 model{1.0f};
    const Material* material = nullptr; // nullptr = 默认材质（贴图与参数都来自它）
    const MaterialParams* paramsOverride = nullptr; // 非空时用它代替 material 的参数（实体级材质覆盖）
    uint32_t lod = 0;                   // 网格 LOD 级别（由场景按屏幕误差选择）
    float highlight = 0.0f;             // 编辑器选中高亮强度（0 = 不高亮）
};

struct DrawList {
    std::vector<DrawItem> opaque;
    // EXTEND: 透明物体（水下气泡、玻璃）需要单独列表并按距离排序
};

} // namespace eng
