#pragma once
// 断言。
//   ENG_ASSERT(cond, msg)：程序员错误（不变量被破坏），Debug/Release 都检查，失败即终止。
//   可恢复的错误（文件缺失、shader 编译失败……）不要用断言，返回 false/optional 并写日志，
//   让上层用默认资源兜底——这是整个引擎「改坏了也不全崩」的基础。

namespace eng::detail {
[[noreturn]] void assertFailed(const char* expr, const char* msg, const char* file, int line);
}

#define ENG_ASSERT(cond, msg)                                                \
    do {                                                                     \
        if (!(cond)) ::eng::detail::assertFailed(#cond, msg, __FILE__, __LINE__); \
    } while (0)
