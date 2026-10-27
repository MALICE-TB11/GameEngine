#include "core/Time.h"

#include <doctest/doctest.h>

using namespace eng;

TEST_CASE("固定步长：时间累积到一步才执行") {
    FixedStepLoop loop({60.0, 5, 0.25});
    const double dt = loop.fixedDt();
    CHECK(loop.advance(dt * 0.5) == 0);
    CHECK(loop.alpha() == doctest::Approx(0.5));
    CHECK(loop.advance(dt * 0.6) == 1);
    CHECK(loop.alpha() == doctest::Approx(0.1).epsilon(1e-6));
}

TEST_CASE("固定步长：长帧补多步，但不超过上限") {
    FixedStepLoop loop({60.0, 3, 0.25});
    CHECK(loop.advance(loop.fixedDt() * 2.0 + 1e-9) == 2);
    // 0.2 秒 = 12 步，上限 3 步，剩余时间被丢弃（防止死亡螺旋）
    CHECK(loop.advance(0.2) == 3);
    CHECK(loop.alpha() < 1.0);
}

TEST_CASE("固定步长：单帧时间被钳制到 maxFrameTime") {
    FixedStepLoop loop({10.0, 100, 0.25});
    // 断点停了 10 秒：只计入 0.25 秒 = 2 步（10Hz），而不是 100 步
    CHECK(loop.advance(10.0) == 2);
}

TEST_CASE("固定步长：总模拟时间与真实时间一致（未触发上限时）") {
    FixedStepLoop loop({50.0, 10, 0.25});
    int total = 0;
    for (int i = 0; i < 1000; ++i) total += loop.advance(1.0 / 144.0);
    const double simulated = total * loop.fixedDt() + loop.alpha() * loop.fixedDt();
    CHECK(simulated == doctest::Approx(1000.0 / 144.0).epsilon(1e-9));
}
