#pragma once
// 日志：spdlog 的薄封装。
// 除了写控制台和文件，还把最近的日志保存在内存里，给 ImGui 的日志面板显示；
// 同时统计 error 级别的条数——冒烟测试（--frames N）以「没有任何 error」作为通过条件，
// validation layer 的报错也走 error 级别，所以它们会自动让冒烟测试失败。

#include <spdlog/spdlog.h>

#include <cstdint>
#include <string>
#include <vector>

namespace eng::log {

struct Entry {
    spdlog::level::level_enum level;
    std::string text;
};

void init(const std::string& logFilePath);
void shutdown();

// 最近的若干条日志（拷贝一份，线程安全）
std::vector<Entry> recent();
// 清空「最近日志」缓冲（日志面板的「清空」按钮）；不影响日志文件和 error 计数
void clearRecent();
// 自启动以来 error/critical 级别的日志条数
uint32_t errorCount();

} // namespace eng::log

#define LOG_TRACE(...) ::spdlog::trace(__VA_ARGS__)
#define LOG_DEBUG(...) ::spdlog::debug(__VA_ARGS__)
#define LOG_INFO(...) ::spdlog::info(__VA_ARGS__)
#define LOG_WARN(...) ::spdlog::warn(__VA_ARGS__)
#define LOG_ERROR(...) ::spdlog::error(__VA_ARGS__)
