#include "math/Projection.h"
#include "math/Transform.h"

#include <doctest/doctest.h>

using namespace eng;

namespace {
// 视空间点 → NDC 深度
float ndcDepth(const mat4& proj, float zView) {
    const vec4 clip = proj * vec4(0.0f, 0.0f, zView, 1.0f);
    return clip.z / clip.w;
}
} // namespace

TEST_CASE("reverse-Z 投影：近平面深度为 1，越远越接近 0") {
    const float zNear = 0.1f;
    const mat4 p = perspectiveReverseZ(glm::radians(60.0f), 16.0f / 9.0f, zNear);
    CHECK(ndcDepth(p, -zNear) == doctest::Approx(1.0f));
    CHECK(ndcDepth(p, -1.0f) == doctest::Approx(0.1f));
    CHECK(ndcDepth(p, -10000.0f) < 1e-4f);
    // 单调：越远深度越小（所以深度测试要用 GREATER）
    CHECK(ndcDepth(p, -5.0f) > ndcDepth(p, -6.0f));
}

TEST_CASE("reverse-Z 投影：视野边缘映射到 NDC 的 ±1") {
    const float fov = glm::radians(90.0f);
    const mat4 p = perspectiveReverseZ(fov, 1.0f, 0.1f);
    // fov 90° 时，z=-1 处上边缘 y=1
    const vec4 clip = p * vec4(0.0f, 1.0f, -1.0f, 1.0f);
    CHECK(clip.y / clip.w == doctest::Approx(1.0f));
}

TEST_CASE("Transform::matrix 按 T*R*S 组合") {
    Transform t;
    t.position = {1, 2, 3};
    t.rotation = glm::angleAxis(glm::radians(90.0f), vec3(0, 1, 0));
    t.scale = vec3(2.0f);
    const vec3 p = vec3(t.matrix() * vec4(1, 0, 0, 1));
    // (1,0,0) 缩放 → (2,0,0)，绕 Y 转 90° → (0,0,-2)，平移 → (1,2,1)
    CHECK(p.x == doctest::Approx(1.0f));
    CHECK(p.y == doctest::Approx(2.0f));
    CHECK(p.z == doctest::Approx(1.0f));
}

TEST_CASE("Transform::lerp 端点与中点") {
    Transform a, b;
    b.position = {10, 0, 0};
    b.rotation = glm::angleAxis(glm::radians(90.0f), vec3(0, 1, 0));
    const Transform m0 = Transform::lerp(a, b, 0.0f);
    const Transform m1 = Transform::lerp(a, b, 1.0f);
    const Transform mh = Transform::lerp(a, b, 0.5f);
    CHECK(m0.position.x == doctest::Approx(0.0f));
    CHECK(m1.position.x == doctest::Approx(10.0f));
    CHECK(mh.position.x == doctest::Approx(5.0f));
    // slerp 中点应是 45°
    CHECK(glm::degrees(glm::angle(mh.rotation)) == doctest::Approx(45.0f).epsilon(1e-3));
}

TEST_CASE("remap / saturate") {
    CHECK(remap(5.0f, 0.0f, 10.0f, 100.0f, 200.0f) == doctest::Approx(150.0f));
    CHECK(saturate(-1.0f) == 0.0f);
    CHECK(saturate(2.0f) == 1.0f);
}
