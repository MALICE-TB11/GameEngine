// 地形（高度场、四叉树）、视锥、级联阴影矩阵：都是纯 CPU 逻辑，不需要 GPU。
#include "math/Projection.h"
#include "render/Shadows.h"
#include "terrain/Heightfield.h"
#include "terrain/TerrainQuadtree.h"

#include <doctest/doctest.h>

#include <cmath>

using namespace eng;

namespace {

// 小号地形：129² 采样覆盖 256 m（间距 2 m），岛心在 (0, -30)
terrain::TerrainSettings smallIsland() {
    terrain::TerrainSettings s;
    s.resolution = 129;
    s.worldSize = 256.0f;
    s.islandCenter = {0.0f, -30.0f};
    s.islandRadius = 70.0f;
    s.plateauRadius = 10.0f;
    s.plateauHeight = 2.0f;
    return s;
}

} // namespace

TEST_CASE("高度场：原点平台被整平，远海是海床") {
    terrain::Heightfield hf;
    hf.generate(smallIsland());
    CHECK(hf.resolution() == 129);
    CHECK(hf.spacing() == doctest::Approx(2.0f));
    CHECK(hf.heightAt(0.0f, 0.0f) == doctest::Approx(2.0f).epsilon(0.01));
    CHECK(hf.heightAt(5.0f, -4.0f) == doctest::Approx(2.0f).epsilon(0.01));
    // 平台上法线朝上
    CHECK(hf.normalAt(0.0f, 0.0f).y > 0.999f);
    // 地图角落离岛很远：在海平面以下
    CHECK(hf.heightAt(120.0f, 120.0f) < 0.0f);
    CHECK(hf.minHeight() < 0.0f);
    CHECK(hf.maxHeight() > 2.0f);
}

TEST_CASE("高度场：极端参数（平台半径 0、岛屿半径极小）不产生 NaN") {
    terrain::TerrainSettings s = smallIsland();
    s.plateauRadius = 0.0f;
    terrain::Heightfield hf;
    hf.generate(s);
    for (float h : hf.samples()) REQUIRE(std::isfinite(h));
    s.plateauRadius = 10.0f;
    s.islandRadius = 0.001f;
    hf.generate(s);
    for (float h : hf.samples()) REQUIRE(std::isfinite(h));
}

TEST_CASE("高度场：采样点上精确、采样点之间按三角形插值") {
    terrain::Heightfield hf;
    hf.generate(smallIsland());
    const vec2 o = hf.origin();
    const float s = hf.spacing();
    // 正好落在采样点上
    CHECK(hf.heightAt(o.x + 10 * s, o.y + 20 * s) == doctest::Approx(hf.sample(10, 20)));
    // 两个采样点正中间 = 两者平均
    const float mid = hf.heightAt(o.x + 10.5f * s, o.y + 20 * s);
    CHECK(mid == doctest::Approx((hf.sample(10, 20) + hf.sample(11, 20)) * 0.5f));
    // 超出范围夹到边缘，不越界
    CHECK(hf.heightAt(-1e6f, 1e6f) == doctest::Approx(hf.sample(0, hf.resolution() - 1)));
}

TEST_CASE("四叉树：选中的节点恰好铺满整个地形，没有重叠") {
    terrain::Heightfield hf;
    hf.generate(smallIsland());
    terrain::TerrainQuadtree qt;
    REQUIRE(qt.build(hf, 32)); // 128 / 32 = 4 → 2 级细分
    CHECK(qt.maxLevel() == 2);

    std::vector<terrain::TerrainNode> nodes;
    qt.select(vec3(0.0f, 5.0f, 0.0f), 2.5f, nullptr, 0.0f, nodes);
    REQUIRE(!nodes.empty());
    double area = 0.0;
    for (const auto& n : nodes) area += double(n.size) * n.size;
    CHECK(area == doctest::Approx(256.0 * 256.0));
    for (size_t i = 0; i < nodes.size(); ++i)
        for (size_t j = i + 1; j < nodes.size(); ++j) {
            const auto &a = nodes[i], &b = nodes[j];
            const float ox = std::min(a.origin.x + a.size, b.origin.x + b.size) - std::max(a.origin.x, b.origin.x);
            const float oz = std::min(a.origin.y + a.size, b.origin.y + b.size) - std::max(a.origin.y, b.origin.y);
            CHECK_FALSE((ox > 1e-3f && oz > 1e-3f)); // 只能共边，不能有面积重叠
        }
}

TEST_CASE("四叉树：相机附近最细，远处更粗；包围盒包含真实高度") {
    terrain::Heightfield hf;
    hf.generate(smallIsland());
    terrain::TerrainQuadtree qt;
    REQUIRE(qt.build(hf, 32));
    std::vector<terrain::TerrainNode> nodes;
    const vec3 cam(-100.0f, 5.0f, -100.0f); // 靠近一个角
    qt.select(cam, 1.0f, nullptr, 0.0f, nodes);
    const terrain::TerrainNode* nearest = nullptr;
    const terrain::TerrainNode* farthest = nullptr;
    float dmin = 1e30f, dmax = -1.0f;
    for (const auto& n : nodes) {
        const float d = glm::length(vec2(cam.x, cam.z) - (n.origin + n.size * 0.5f));
        if (d < dmin) dmin = d, nearest = &n;
        if (d > dmax) dmax = d, farthest = &n;
        // 节点包围盒覆盖其中心的真实高度
        const float h = hf.heightAt(n.origin.x + n.size * 0.5f, n.origin.y + n.size * 0.5f);
        CHECK(h >= n.minY - 1e-3f);
        CHECK(h <= n.maxY + 1e-3f);
    }
    REQUIRE(nearest);
    CHECK(nearest->level == qt.maxLevel());
    CHECK(farthest->level < nearest->level);
}

TEST_CASE("四叉树：分辨率不合法时拒绝构建") {
    terrain::TerrainSettings s = smallIsland();
    s.resolution = 100; // 99 不是 32 × 2^n
    terrain::Heightfield hf;
    hf.generate(s);
    terrain::TerrainQuadtree qt;
    CHECK_FALSE(qt.build(hf, 32));
    CHECK_FALSE(qt.valid());
}

TEST_CASE("视锥：前方可见、身后与侧面被剔除") {
    const mat4 view = glm::lookAt(vec3(0.0f), vec3(0.0f, 0.0f, -1.0f), vec3(0.0f, 1.0f, 0.0f));
    const mat4 proj = perspectiveReverseZ(glm::radians(60.0f), 16.0f / 9.0f, 0.1f);
    const Frustum f = Frustum::fromViewProj(proj * view);
    CHECK(f.intersectsAabb(vec3(-1, -1, -11), vec3(1, 1, -9)));      // 正前方
    CHECK(f.intersectsAabb(vec3(-1, -1, -100001), vec3(1, 1, -99999))); // 无限远投影：多远都不会被远平面剔除
    CHECK_FALSE(f.intersectsAabb(vec3(-1, -1, 9), vec3(1, 1, 11)));  // 身后
    CHECK_FALSE(f.intersectsAabb(vec3(50, -1, -11), vec3(52, 1, -9))); // 右侧视野外
    CHECK(f.intersectsAabb(vec3(-1000, -1, -20), vec3(1000, 1, -10))); // 横跨视锥的大盒子
}

TEST_CASE("级联阴影：反向 Z 正交投影把近平面映射到 1、远平面映射到 0") {
    const mat4 p = orthoReverseZ(-10, 10, -10, 10, 1.0f, 101.0f);
    const vec4 nearPt = p * vec4(0, 0, -1.0f, 1);
    const vec4 farPt = p * vec4(0, 0, -101.0f, 1);
    CHECK(nearPt.z == doctest::Approx(1.0f));
    CHECK(farPt.z == doctest::Approx(0.0f));
    CHECK((p * vec4(10, 0, -50, 1)).x == doctest::Approx(1.0f));
}

TEST_CASE("级联阴影：切分递增、最后一级等于阴影距离、近级联更精细") {
    ShadowSettings s;
    s.maxDistance = 120.0f;
    const mat4 view = glm::lookAt(vec3(0, 2, 0), vec3(0, 2, -1), vec3(0, 1, 0));
    const vec3 sun = glm::normalize(vec3(0.3f, 0.8f, 0.2f));
    const ShadowCascades c = computeCascades(view, glm::radians(60.0f), 16.0f / 9.0f, 0.1f, sun, s);
    CHECK(c.splits.x > 0.1f);
    CHECK(c.splits.x < c.splits.y);
    CHECK(c.splits.y < c.splits.z);
    CHECK(c.splits.z < c.splits.w);
    CHECK(c.splits.w == doctest::Approx(120.0f));
    CHECK(c.texelWorldSize.x < c.texelWorldSize.w);
    // 相机正前方 1 米处的点落在第 0 级联的图内
    const vec4 clip = c.viewProj[0] * vec4(0, 2, -1, 1);
    CHECK(std::abs(clip.x) <= 1.0f);
    CHECK(std::abs(clip.y) <= 1.0f);
    CHECK(clip.z >= 0.0f);
    CHECK(clip.z <= 1.0f);
}
