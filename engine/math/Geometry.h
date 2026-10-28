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

// 视锥：6 个平面（法线朝内），用于剔除完全在视野外的包围盒。
// 从视图投影矩阵直接提取（Gribb–Hartmann）：裁剪空间里「点在视锥内」⇔ -w ≤ x ≤ w、-w ≤ y ≤ w、0 ≤ z ≤ w，
// 每个不等式都是矩阵某两行的线性组合 · 世界坐标 ≥ 0，这个组合就是平面方程。
// 反向 Z 无限远投影的「远平面」退化（处处满足），提取后变成恒真的平面，不影响结果。
struct Frustum {
    vec4 planes[6]; // xyz = 内法线，w = 距离：dot(n, p) + w ≥ 0 表示在内侧

    static Frustum fromViewProj(const mat4& m) {
        auto row = [&](int i) { return vec4(m[0][i], m[1][i], m[2][i], m[3][i]); };
        Frustum f;
        f.planes[0] = row(3) + row(0); // 左
        f.planes[1] = row(3) - row(0); // 右
        f.planes[2] = row(3) + row(1); // 下
        f.planes[3] = row(3) - row(1); // 上
        f.planes[4] = row(3) - row(2); // z ≤ w（反向 Z 的近平面）
        f.planes[5] = row(2);          // z ≥ 0（反向 Z 的远平面；无限远投影时恒真）
        for (vec4& p : f.planes) {
            const float len = glm::length(vec3(p));
            p = len > 1e-6f ? p / len : vec4(0.0f, 0.0f, 0.0f, 1.0f);
        }
        return f;
    }

    // 保守测试：返回 false 时盒子一定在视锥外；返回 true 时可能在内（角落处可能误判为可见，对剔除无害）
    bool intersectsAabb(const vec3& bmin, const vec3& bmax) const {
        for (const vec4& p : planes) {
            // 取盒子在平面法线方向上「最靠前」的角（p-vertex），它都在平面外侧，整个盒子就在外侧
            const vec3 pv{p.x >= 0.0f ? bmax.x : bmin.x, p.y >= 0.0f ? bmax.y : bmin.y, p.z >= 0.0f ? bmax.z : bmin.z};
            if (glm::dot(vec3(p), pv) + p.w < 0.0f) return false;
        }
        return true;
    }
};

// 屏幕坐标 → 世界空间射线。uv ∈ [0,1]²，(0,0) 为左上角（与引擎的负高度视口约定一致：NDC y 向上）。
// 反投影近平面上的点（反向 Z 下深度 = 1），减去相机位置得到方向。
inline Ray screenRay(const mat4& invViewProj, const vec3& cameraPos, vec2 uv) {
    const vec4 ndc{uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, 1.0f, 1.0f};
    const vec4 p = invViewProj * ndc;
    return {cameraPos, glm::normalize(vec3(p) / p.w - cameraPos)};
}

} // namespace eng
