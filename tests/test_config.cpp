#include "core/Config.h"

#include <doctest/doctest.h>

#include <filesystem>

namespace {
struct TestSettings {
    float speed = 3.0f;
    int count = 7;
    glm::vec3 color{1.0f, 0.5f, 0.25f};
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(TestSettings, speed, count, color)

std::filesystem::path tempFile(const char* name) {
    return std::filesystem::temp_directory_path() / name;
}
} // namespace

TEST_CASE("配置：缺失字段使用默认值") {
    const auto path = tempFile("eng_test_cfg1.json");
    eng::fs::writeText(path, R"({ "s": { "speed": 9.5 } })");
    eng::ConfigFile cfg(path);
    const auto s = cfg.get<TestSettings>("s");
    CHECK(s.speed == doctest::Approx(9.5f));
    CHECK(s.count == 7);
    CHECK(s.color.y == doctest::Approx(0.5f));
}

TEST_CASE("配置：语法错误时保留上一次成功的值") {
    const auto path = tempFile("eng_test_cfg2.json");
    eng::fs::writeText(path, R"({ "s": { "count": 42 } })");
    eng::ConfigFile cfg(path);
    CHECK(cfg.get<TestSettings>("s").count == 42);
    eng::fs::writeText(path, R"({ "s": { "count": 43 )"); // 坏 JSON
    CHECK_FALSE(cfg.load());
    CHECK(cfg.get<TestSettings>("s").count == 42);
    CHECK_FALSE(cfg.lastError().empty());
}

TEST_CASE("配置：保存后再读回一致，glm 向量序列化为数组") {
    const auto path = tempFile("eng_test_cfg3.json");
    std::filesystem::remove(path);
    eng::ConfigFile cfg(path);
    TestSettings s;
    s.speed = 1.25f;
    s.color = {0.1f, 0.2f, 0.3f};
    cfg.set("s", s);
    REQUIRE(cfg.save());
    eng::ConfigFile cfg2(path);
    const auto r = cfg2.get<TestSettings>("s");
    CHECK(r.speed == doctest::Approx(1.25f));
    CHECK(r.color.z == doctest::Approx(0.3f));
}

TEST_CASE("配置：类型错误的节使用默认值而不是崩溃") {
    const auto path = tempFile("eng_test_cfg4.json");
    eng::fs::writeText(path, R"({ "s": { "count": "not a number" } })");
    eng::ConfigFile cfg(path);
    CHECK(cfg.get<TestSettings>("s").count == 7);
}
