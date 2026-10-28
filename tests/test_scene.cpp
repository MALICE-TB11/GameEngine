#include "world/SceneDesc.h"

#include <doctest/doctest.h>

using namespace eng;

TEST_CASE("场景描述：JSON 往返一致（含旋转与材质覆盖）") {
    SceneDesc s;
    EntityDesc e;
    e.name = "ruins/Pillar_R";
    e.position = {1.5f, 0.0f, -3.0f};
    e.rotation = glm::angleAxis(glm::radians(30.0f), vec3(0, 1, 0));
    e.scale = vec3(2.0f);
    e.model = "0123456789abcdef0123456789abcdef";
    e.instance = 2;
    e.visible = false;
    MaterialParams p;
    p.baseColor = {0.5f, 0.4f, 0.3f, 1.0f};
    e.material = p;
    s.entities.push_back(e);

    const auto back = sceneFromJson(sceneToJson(s));
    REQUIRE(back.has_value());
    REQUIRE(back->entities.size() == 1);
    const EntityDesc& r = back->entities[0];
    CHECK(r.name == e.name);
    CHECK(r.position.x == doctest::Approx(1.5f));
    CHECK(glm::dot(r.rotation, e.rotation) == doctest::Approx(1.0f));
    CHECK(r.scale.y == doctest::Approx(2.0f));
    CHECK(r.model == e.model);
    CHECK(r.instance == 2);
    CHECK_FALSE(r.visible);
    REQUIRE(r.material.has_value());
    CHECK(r.material->baseColor.y == doctest::Approx(0.4f));
}

TEST_CASE("场景描述：格式错误的实体被跳过，其余照常加载") {
    const char* json = R"({
        "version": 1,
        "entities": [
            { "name": "ok", "model": "0123456789abcdef0123456789abcdef" },
            { "name": "缺少 model 字段" },
            { "name": "position 类型错误", "model": "0123456789abcdef0123456789abcdef", "position": "abc" }
        ]
    })";
    const auto s = sceneFromJson(json);
    REQUIRE(s.has_value());
    REQUIRE(s->entities.size() == 1);
    CHECK(s->entities[0].name == "ok");
    CHECK_FALSE(s->entities[0].material.has_value());
}

TEST_CASE("场景描述：JSON 语法错误返回空并给出原因") {
    std::string error;
    CHECK_FALSE(sceneFromJson("{ not json", &error).has_value());
    CHECK_FALSE(error.empty());
}
