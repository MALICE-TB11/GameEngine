#pragma once
// GUID：资产的唯一身份。
//
// 为什么不用文件路径当身份：资产会被改名、移动。如果场景里记的是路径，一改名引用就断了。
// 每个资产第一次被导入时生成一个随机 128 位 GUID，写进旁边的 .meta 文件（提交进 git），
// 之后无论文件挪到哪里，GUID 不变，引用不变 —— 与 Unity 的做法相同。

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace eng {

struct Guid {
    uint64_t hi = 0;
    uint64_t lo = 0;

    static Guid random();
    // 由名字确定性地生成（内置资产、外部临时资产用；同名永远同一个 GUID）
    static Guid fromName(std::string_view name);
    static std::optional<Guid> parse(std::string_view hex32);

    std::string toString() const; // 32 位十六进制
    bool valid() const { return hi != 0 || lo != 0; }
    bool operator==(const Guid&) const = default;
    bool operator<(const Guid& o) const { return hi != o.hi ? hi < o.hi : lo < o.lo; }
};

} // namespace eng

template <>
struct std::hash<eng::Guid> {
    size_t operator()(const eng::Guid& g) const noexcept { return std::hash<uint64_t>()(g.hi ^ (g.lo * 0x9E3779B97F4A7C15ull)); }
};
