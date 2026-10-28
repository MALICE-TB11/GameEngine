#pragma once
// 几何查询：射线、包围盒。编辑器用它做「鼠标点选物体」，之后的 AI 视线、子弹检测也能复用。
// （物理查询从 M4 起走 PhysicsWorld；这里是不依赖物理引擎的轻量版本。）

#include "math/MathTypes.h"

#include <algorithm>
#include <limits>
#include <optional>

namespace eng {

struct Ray {
    vec3 origin{0.0f};
    vec3 dir{0.0f, 0.0f, -1.0f}; // 单位向量
};

// 轴对齐包围盒（AABB）与射线求交：「slab」方法。
// 把盒子看成三对平行平面（x/y/z 各一对）。射线进入盒子的时刻 = 三对平面「进入时刻」的最大值，
// 离开时刻 = 「离开时刻」的最小值；进入早于离开才算相交。
// 返回沿射线的距离 t（起点在盒内时为 0），不相交返回 nullopt。
inline std::optional<float> intersectRayAabb(const Ray& ray, const vec3& bmin, const vec3& bmax) {
    float tEnter = 0.0f;
    float tExit = std::numeric_limits<float>::max();
    for (int axis = 0; axis < 3; ++axis) {
        const float o = ray.origin[axis], d = ray.dir[axis];
        if (std::abs(d) < 1e-8f) {
            // 射线与这对平面平行：起点必须夹在两平面之间
            if (o < bmin[axis] || o > bmax[axis]) return std::nullopt;
            continue;
        }
        float t0 = (bmin[axis] - o) / d;
        float t1 = (bmax[axis] - o) / d;
        if (t0 > t1) std::swap(t0, t1);
        tEnter = std::max(tEnter, t0);
        tExit = std::min(tExit, t1);
        if (tEnter > tExit) return std::nullopt;
    }
    return tEnter;
}

// 把局部包围盒的 8 个角变换到世界空间，再取新的轴对齐包围盒（旋转后会变大一些，但对点选足够）
inline void transformAabb(const mat4& m, const vec3& bmin, const vec3& bmax, vec3& outMin, vec3& outMax) {
    outMin = vec3(std::numeric_limits<float>::max());
    outMax = vec3(-std::numeric_limits<float>::max());
    for (int c = 0; c < 8; ++c) {
        const vec3 corner{(c & 1) ? bmax.x : bmin.x, (c & 2) ? bmax.y : bmin.y, (c & 4) ? bmax.z : bmin.z};
        const vec3 w = vec3(m * vec4(corner, 1.0f));
        outMin = glm::min(outMin, w);
        outMax = glm::max(outMax, w);
    }
}

// 屏幕坐标 → 世界空间射线。uv ∈ [0,1]²，(0,0) 为左上角（与引擎的负高度视口约定一致：NDC y 向上）。
// 反投影近平面上的点（反向 Z 下深度 = 1），减去相机位置得到方向。
inline Ray screenRay(const mat4& invViewProj, const vec3& cameraPos, vec2 uv) {
    const vec4 ndc{uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, 1.0f, 1.0f};
    const vec4 p = invViewProj * ndc;
    return {cameraPos, glm::normalize(vec3(p) / p.w - cameraPos)};
}

} // namespace eng
