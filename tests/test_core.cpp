#include "core/BinaryIO.h"
#include "core/Guid.h"
#include "core/JobSystem.h"

#include <doctest/doctest.h>

#include <atomic>

using namespace eng;

TEST_CASE("GUID：随机生成不重复，字符串往返一致") {
    const Guid a = Guid::random(), b = Guid::random();
    CHECK(a.valid());
    CHECK_FALSE(a == b);
    const auto parsed = Guid::parse(a.toString());
    REQUIRE(parsed.has_value());
    CHECK(*parsed == a);
    CHECK(a.toString().size() == 32);
}

TEST_CASE("GUID：由名字生成是确定的，非法字符串解析失败") {
    CHECK(Guid::fromName("builtin/cube") == Guid::fromName("builtin/cube"));
    CHECK_FALSE(Guid::fromName("builtin/cube") == Guid::fromName("builtin/sphere"));
    CHECK_FALSE(Guid::parse("xyz").has_value());
    CHECK_FALSE(Guid::parse("0123456789abcdef0123456789abcdeg").has_value());
}

TEST_CASE("二进制读写：往返一致，截断的数据读取失败而不崩溃") {
    BinaryWriter w;
    w.write(uint32_t(42));
    w.writeString("海洋");
    w.writeArray(std::vector<float>{1.0f, 2.0f, 3.0f});
    BinaryReader r(w.data());
    CHECK(r.read<uint32_t>() == 42);
    CHECK(r.readString() == "海洋");
    CHECK(r.readArray<float>() == std::vector<float>{1.0f, 2.0f, 3.0f});
    CHECK(r.ok());
    CHECK(r.atEnd());

    std::vector<uint8_t> truncated(w.data().begin(), w.data().end() - 5);
    BinaryReader bad(truncated);
    bad.read<uint32_t>();
    bad.readString();
    CHECK(bad.readArray<float>().empty());
    CHECK_FALSE(bad.ok());
}

TEST_CASE("Job 系统：parallelFor 覆盖每个元素恰好一次") {
    JobSystem jobs;
    jobs.init(4);
    std::vector<std::atomic<int>> hits(10000);
    jobs.parallelFor(10000, 64, [&](uint32_t b, uint32_t e) {
        for (uint32_t i = b; i < e; ++i) ++hits[i];
    });
    bool allOnce = true;
    for (auto& h : hits) allOnce &= h.load() == 1;
    CHECK(allOnce);
    jobs.shutdown();
}

TEST_CASE("Job 系统：主线程队列按顺序执行，且只在 pump 时执行") {
    JobSystem jobs;
    std::vector<int> order;
    jobs.runOnMainThread([&] { order.push_back(1); });
    jobs.runOnMainThread([&] { order.push_back(2); });
    CHECK(order.empty());
    CHECK(jobs.pumpMainThread(1) == 1);
    CHECK(order == std::vector<int>{1});
    jobs.pumpMainThread();
    CHECK(order == std::vector<int>{1, 2});
}
