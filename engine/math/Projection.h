#pragma once
// 投影矩阵：反向 Z（reverse-Z）+ 无限远平面。
//
// 为什么用反向 Z：浮点数在 0 附近最密。普通投影把近处映射到 0、远处映射到 1，
// 而透视除法本身已经让大部分精度集中在近处，两者叠加后远处精度极差（海面远景会严重 z-fighting）。
// 反向 Z 把近平面映射到 1、无穷远映射到 0，浮点精度分布与透视分布正好互相抵消，
// 几乎整个可视距离都有均匀精度。代价：深度清屏值要用 0，深度比较用 GREATER。
//
// 为什么用无限远平面：海洋场景视距很远，不想调 far；反向 Z 下无限远平面几乎不损失精度。

#include "math/MathTypes.h"

#include <cmath>

namespace eng {

// 右手系、看向 -Z、深度范围 [0,1]（近=1，无穷远=0）
// 推导：z_clip = near，w_clip = -z_view，所以 depth = near / (-z_view)。
inline mat4 perspectiveReverseZ(float fovYRadians, float aspect, float zNear) {
    const float f = 1.0f / std::tan(fovYRadians * 0.5f);
    mat4 m(0.0f);
    m[0][0] = f / aspect; // GLM 是列主序：m[列][行]
    m[1][1] = f;
    m[2][3] = -1.0f; // w_clip = -z_view
    m[3][2] = zNear; // z_clip = near
    return m;
}

} // namespace eng
