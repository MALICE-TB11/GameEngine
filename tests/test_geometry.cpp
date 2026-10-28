#include "math/Geometry.h"
#include "math/Projection.h"

#include <doctest/doctest.h>

using namespace eng;

TEST_CASE("射线-AABB：正面命中返回进入距离") {
    const Ray r{{0, 0, 5}, {0, 0, -1}};
    const auto t = intersectRayAabb(r, vec3(-1), vec3(1));
    REQUIRE(t.has_value());
    CHECK(*t == doctest::Approx(4.0f));
}

TEST_CASE("射线-AABB：擦过和背向都不命中") {
    CHECK_FALSE(intersectRayAabb({{0, 2.5f, 5}, {0, 0, -1}}, vec3(-1), vec3(1)).has_value());
    CHECK_FALSE(intersectRayAabb({{0, 0, 5}, {0, 0, 1}}, vec3(-1), vec3(1)).has_value());
}

TEST_CASE("射线-AABB：起点在盒内时距离为 0；平行于某轴也能正确处理") {
    const auto inside = intersectRayAabb({{0, 0, 0}, {1, 0, 0}}, vec3(-1), vec3(1));
    REQUIRE(inside.has_value());
    CHECK(*inside == doctest::Approx(0.0f));
    // 方向分量为 0（平行于 yz 平面）且起点 x 在范围内
    CHECK(intersectRayAabb({{0.5f, 0, 5}, {0, 0, -1}}, vec3(-1), vec3(1)).has_value());
}

TEST_CASE("transformAabb：旋转 45° 后包围盒变大") {
    const mat4 rot = glm::rotate(mat4(1.0f), glm::radians(45.0f), vec3(0, 1, 0));
    vec3 mn, mx;
    transformAabb(rot, vec3(-1), vec3(1), mn, mx);
    CHECK(mx.x == doctest::Approx(std::sqrt(2.0f)));
    CHECK(mx.y == doctest::Approx(1.0f));
}

TEST_CASE("screenRay：屏幕中心的射线就是相机前方") {
    const mat4 view = glm::lookAt(vec3(0, 0, 5), vec3(0, 0, 0), vec3(0, 1, 0));
    const mat4 proj = perspectiveReverseZ(glm::radians(60.0f), 16.0f / 9.0f, 0.1f);
    const Ray r = screenRay(glm::inverse(proj * view), vec3(0, 0, 5), vec2(0.5f));
    CHECK(r.dir.z == doctest::Approx(-1.0f));
    // 屏幕上方（uv.y 小）的射线朝上
    const Ray up = screenRay(glm::inverse(proj * view), vec3(0, 0, 5), vec2(0.5f, 0.1f));
    CHECK(up.dir.y > 0.0f);
}
