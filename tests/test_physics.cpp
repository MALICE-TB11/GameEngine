// 物理封装（PhysicsWorld）与场景同步的行为测试：真的跑 Jolt 模拟，检查结果是否符合物理直觉。
#include "physics/PhysicsWorld.h"
#include "world/SceneDesc.h"

#include <doctest/doctest.h>

#include <vector>

using namespace eng;
using namespace eng::physics;

namespace {

constexpr float kDt = 1.0f / 60.0f;

void simulate(PhysicsWorld& w, float seconds) {
    for (int i = 0; i < int(seconds / kDt); ++i) w.step(kDt);
}

// 一块 20×20 m、表面在 y = 0 的静态地板
BodyId addFloor(PhysicsWorld& w) {
    BodyDesc d;
    d.shape = w.createBox(vec3(10.0f, 0.5f, 10.0f));
    d.position = {0.0f, -0.5f, 0.0f};
    return w.addBody(d);
}

} // namespace

TEST_CASE("物理：球落到地板上停住") {
    PhysicsWorld w;
    REQUIRE(w.init());
    addFloor(w);
    BodyDesc ball;
    ball.shape = w.createSphere(0.5f);
    ball.motion = Motion::Dynamic;
    ball.position = {0.0f, 5.0f, 0.0f};
    const BodyId b = w.addBody(ball);
    REQUIRE(b.valid());

    simulate(w, 0.5f);
    vec3 p;
    quat q;
    REQUIRE(w.getTransform(b, p, q));
    // 自由落体 0.5 秒：h = ½gt² ≈ 1.23 m（允许求解器的小误差）
    CHECK(p.y == doctest::Approx(5.0f - 0.5f * 9.81f * 0.25f).epsilon(0.03));

    simulate(w, 4.0f);
    REQUIRE(w.getTransform(b, p, q));
    CHECK(p.y == doctest::Approx(0.5f).epsilon(0.05)); // 球心 = 地面 + 半径
    CHECK(glm::length(w.linearVelocity(b)) < 0.05f);
}

TEST_CASE("物理：射线检测命中高度场，法线朝上") {
    PhysicsWorld w;
    REQUIRE(w.init());
    // 17×17 的平坦高度场，高度 3 m，间距 1 m，原点在 (-8, -8)
    const uint32_t res = 17;
    std::vector<float> h(res * res, 3.0f);
    const ShapeId hf = w.createHeightfield(h, res, vec2(-8.0f), 1.0f);
    REQUIRE(hf.valid());
    BodyDesc d;
    d.shape = hf;
    REQUIRE(w.addBody(d).valid());

    const auto hit = w.castRay(vec3(0.5f, 10.0f, 0.5f), vec3(0.0f, -1.0f, 0.0f), 50.0f);
    REQUIRE(hit.has_value());
    CHECK(hit->point.y == doctest::Approx(3.0f).epsilon(0.01));
    CHECK(hit->distance == doctest::Approx(7.0f).epsilon(0.01));
    CHECK(hit->normal.y > 0.99f);
    // 打向天空：没有命中
    CHECK_FALSE(w.castRay(vec3(0.0f, 10.0f, 0.0f), vec3(0.0f, 1.0f, 0.0f), 50.0f).has_value());
}

TEST_CASE("物理：角色落地、站稳、按期望速度移动") {
    PhysicsWorld w;
    REQUIRE(w.init());
    addFloor(w);
    const CharacterId c = w.createCharacter({}, vec3(0.0f, 1.0f, 0.0f));
    REQUIRE(c.valid());

    for (int i = 0; i < 90; ++i) w.moveCharacter(c, vec3(0.0f), 0.0f, kDt);
    CharacterState s = w.character(c);
    CHECK(s.grounded);
    CHECK(s.position.y == doctest::Approx(0.0f).epsilon(0.05));

    // 以 3 m/s 向 +X 走 1 秒
    const float x0 = s.position.x;
    for (int i = 0; i < 60; ++i) w.moveCharacter(c, vec3(3.0f, 0.0f, 0.0f), 0.0f, kDt);
    s = w.character(c);
    CHECK(s.position.x - x0 == doctest::Approx(3.0f).epsilon(0.05));
    CHECK(s.grounded);

    // 起跳：离开地面，之后落回
    w.moveCharacter(c, vec3(0.0f), 5.0f, kDt);
    for (int i = 0; i < 10; ++i) w.moveCharacter(c, vec3(0.0f), 0.0f, kDt);
    CHECK(w.character(c).position.y > 0.3f);
    for (int i = 0; i < 120; ++i) w.moveCharacter(c, vec3(0.0f), 0.0f, kDt);
    CHECK(w.character(c).grounded);
}

TEST_CASE("物理：三角网格不能做动态刚体，自动降级为静态") {
    PhysicsWorld w;
    REQUIRE(w.init());
    const std::vector<vec3> pts = {{-1, 0, -1}, {1, 0, -1}, {1, 0, 1}, {-1, 0, 1}};
    const std::vector<uint32_t> idx = {0, 2, 1, 0, 3, 2};
    BodyDesc d;
    d.shape = w.createMesh(pts, idx);
    d.motion = Motion::Dynamic;
    d.position = {0.0f, 3.0f, 0.0f};
    const BodyId b = w.addBody(d);
    REQUIRE(b.valid());
    simulate(w, 1.0f);
    vec3 p;
    quat q;
    REQUIRE(w.getTransform(b, p, q));
    CHECK(p.y == doctest::Approx(3.0f)); // 静态：没有下落
}

TEST_CASE("物理：删除刚体后旧句柄失效") {
    PhysicsWorld w;
    REQUIRE(w.init());
    const BodyId floor = addFloor(w);
    CHECK(w.contains(floor));
    w.removeBody(floor);
    CHECK_FALSE(w.contains(floor));
    vec3 p;
    quat q;
    CHECK_FALSE(w.getTransform(floor, p, q)); // 不崩溃，只返回 false
    w.removeBody(floor);                       // 重复删除也安全
}

TEST_CASE("场景描述：物理模式写入 JSON，默认值省略") {
    SceneDesc s;
    EntityDesc a;
    a.name = "crate";
    a.model = "0123456789abcdef0123456789abcdef";
    a.physics = PhysicsMode::Dynamic;
    EntityDesc b = a;
    b.name = "deco";
    b.physics = PhysicsMode::None;
    s.entities = {a, b};
    const std::string json = sceneToJson(s);
    CHECK(json.find("\"dynamic\"") != std::string::npos);
    CHECK(json.find("\"none\"") == std::string::npos);
    const auto back = sceneFromJson(json);
    REQUIRE(back.has_value());
    CHECK(back->entities[0].physics == PhysicsMode::Dynamic);
    CHECK(back->entities[1].physics == PhysicsMode::None);
    CHECK(physicsModeFromString("bogus") == PhysicsMode::None);
}
