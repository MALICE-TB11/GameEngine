#include "core/Log.h"

#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <atomic>
#include <deque>
#include <filesystem>
#include <mutex>

#include <windows.h>

namespace eng::log {
namespace {

// 环形缓冲 sink：保存最近 kMaxEntries 条日志
class RingSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    static constexpr size_t kMaxEntries = 512;

    std::vector<Entry> snapshot() {
        std::lock_guard lock(mutex_);
        return {m_entries.begin(), m_entries.end()};
    }
    void clear() {
        std::lock_guard lock(mutex_);
        m_entries.clear();
    }

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        spdlog::memory_buf_t buf;
        formatter_->format(msg, buf);
        std::string text(buf.data(), buf.size());
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
        m_entries.push_back({msg.level, std::move(text)});
        if (m_entries.size() > kMaxEntries) m_entries.pop_front();
    }
    void flush_() override {}

private:
    std::deque<Entry> m_entries;
};

// 只计数 error/critical 的 sink
class ErrorCounterSink final : public spdlog::sinks::base_sink<spdlog::details::null_mutex> {
public:
    std::atomic<uint32_t> count{0};

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        if (msg.level >= spdlog::level::err) ++count;
    }
    void flush_() override {}
};

std::shared_ptr<RingSink> g_ring;
std::shared_ptr<ErrorCounterSink> g_errors;

} // namespace

void init(const std::string& logFilePath) {
    // 日志里有中文。Windows 控制台默认代码页是 GBK(936)，不切到 UTF-8 会显示乱码。
    SetConsoleOutputCP(CP_UTF8);
    std::filesystem::create_directories(std::filesystem::path(logFilePath).parent_path());

    auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    auto file = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logFilePath, /*truncate=*/true);
    g_ring = std::make_shared<RingSink>();
    g_errors = std::make_shared<ErrorCounterSink>();
    g_ring->set_pattern("%H:%M:%S.%e [%l] %v");

    auto logger = std::make_shared<spdlog::logger>("engine", spdlog::sinks_init_list{console, file, g_ring, g_errors});
    logger->set_pattern("%H:%M:%S.%e [%^%l%$] %v");
    logger->set_level(spdlog::level::debug);
    logger->flush_on(spdlog::level::warn); // 崩溃前的警告/错误一定要落盘
    spdlog::set_default_logger(logger);
}

void shutdown() {
    spdlog::shutdown();
}

std::vector<Entry> recent() {
    return g_ring ? g_ring->snapshot() : std::vector<Entry>{};
}

void clearRecent() {
    if (g_ring) g_ring->clear();
}

uint32_t errorCount() {
    return g_errors ? g_errors->count.load() : 0;
}

} // namespace eng::log
