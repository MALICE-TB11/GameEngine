#pragma once
// 文件读写 + 轮询式文件监视（热重载用）。
// 所有「工程内路径」（config/、shaders/、assets/）都相对源码根目录解析，
// 这样无论从哪个工作目录启动 sandbox，热重载改的都是你在编辑器里打开的那份文件。

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace eng::fs {

namespace stdfs = std::filesystem;

stdfs::path projectRoot();
// "config/engine.json" → <源码根>/config/engine.json；绝对路径原样返回
stdfs::path resolve(const stdfs::path& projectRelative);
// 构建时编译好的 SPIR-V 所在目录（build/shaders）
stdfs::path shaderBinaryDir();

std::optional<std::vector<uint8_t>> readBinary(const stdfs::path& path);
std::optional<std::string> readText(const stdfs::path& path);
bool writeText(const stdfs::path& path, std::string_view text);
// 文件不存在时返回 file_time_type::min()，调用方不需要特判
stdfs::file_time_type lastWriteTime(const stdfs::path& path);

// 轮询式文件监视：每隔 interval 检查一次时间戳。
// 为什么不用 ReadDirectoryChangesW：轮询几十个文件的开销可以忽略，而且逻辑简单、跨平台、不需要线程。
// EXTEND: 监视的文件很多时（例如上千个贴图）再换成系统通知 API。
class FileWatcher {
public:
    using Callback = std::function<void(const stdfs::path&)>;

    // 返回 id，可用于 unwatch
    uint32_t watch(const stdfs::path& path, Callback onChanged);
    void unwatch(uint32_t id);
    // 每帧调用；距上次检查不足 intervalSeconds 时直接返回
    void poll(double nowSeconds, double intervalSeconds = 0.25);

private:
    struct Entry {
        uint32_t id;
        stdfs::path path;
        stdfs::file_time_type stamp;
        Callback callback;
    };
    std::vector<Entry> m_entries;
    uint32_t m_nextId = 1;
    double m_lastPoll = -1.0;
};

} // namespace eng::fs
