#include "core/Assert.h"

#include "core/Log.h"

#include <cstdlib>
#include <intrin.h>
#include <windows.h>

namespace eng::detail {

void assertFailed(const char* expr, const char* msg, const char* file, int line) {
    spdlog::critical("断言失败: {} ({})\n  位置: {}:{}", expr, msg ? msg : "", file, line);
    spdlog::default_logger()->flush();
    // 在调试器里运行时直接断在出错处，比看调用栈日志方便得多
    if (IsDebuggerPresent()) __debugbreak();
    std::abort();
}

} // namespace eng::detail
