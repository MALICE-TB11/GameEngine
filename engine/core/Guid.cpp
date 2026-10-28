#include "core/Guid.h"

#include <cstdio>
#include <mutex>
#include <random>

namespace eng {

Guid Guid::random() {
    // random_device 提供种子，mt19937_64 生成；加锁是因为可能在多个 Job 线程里同时导入资产
    static std::mutex mutex;
    static std::mt19937_64 rng{std::random_device{}() ^ (uint64_t(std::random_device{}()) << 32)};
    std::lock_guard lock(mutex);
    Guid g;
    do {
        g.hi = rng();
        g.lo = rng();
    } while (!g.valid());
    return g;
}

Guid Guid::fromName(std::string_view name) {
    // 两路不同种子的 FNV-1a 64 位哈希拼成 128 位；再过一遍 splitmix64 打散低熵
    auto fnv = [&](uint64_t h) {
        for (unsigned char c : name) {
            h ^= c;
            h *= 0x100000001B3ull;
        }
        return h;
    };
    auto mix = [](uint64_t x) {
        x += 0x9E3779B97F4A7C15ull;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
        return x ^ (x >> 31);
    };
    return {mix(fnv(0xCBF29CE484222325ull)), mix(fnv(0x84222325CBF29CE4ull))};
}

std::optional<Guid> Guid::parse(std::string_view s) {
    if (s.size() != 32) return std::nullopt;
    Guid g;
    for (size_t i = 0; i < 32; ++i) {
        const char c = s[i];
        uint64_t v = 0;
        if (c >= '0' && c <= '9') v = uint64_t(c - '0');
        else if (c >= 'a' && c <= 'f') v = uint64_t(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v = uint64_t(c - 'A' + 10);
        else return std::nullopt;
        uint64_t& part = i < 16 ? g.hi : g.lo;
        part = (part << 4) | v;
    }
    return g;
}

std::string Guid::toString() const {
    char buf[33];
    std::snprintf(buf, sizeof(buf), "%016llx%016llx", static_cast<unsigned long long>(hi), static_cast<unsigned long long>(lo));
    return buf;
}

} // namespace eng
