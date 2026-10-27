#include "core/FileSystem.h"

#include "core/Log.h"

#include <fstream>

namespace eng::fs {

stdfs::path projectRoot() {
    return stdfs::path(ENGINE_ROOT_DIR);
}

stdfs::path resolve(const stdfs::path& projectRelative) {
    if (projectRelative.is_absolute()) return projectRelative;
    return projectRoot() / projectRelative;
}

stdfs::path shaderBinaryDir() {
    return stdfs::path(ENGINE_SHADER_BINARY_DIR);
}

std::optional<std::vector<uint8_t>> readBinary(const stdfs::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return std::nullopt;
    const auto size = static_cast<size_t>(file.tellg());
    std::vector<uint8_t> data(size);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
    if (!file) return std::nullopt;
    return data;
}

std::optional<std::string> readText(const stdfs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

bool writeText(const stdfs::path& path, std::string_view text) {
    std::error_code ec;
    stdfs::create_directories(path.parent_path(), ec);
    // 先写临时文件再改名：写到一半崩溃也不会留下半个 config 文件
    const stdfs::path tmp = path.string() + ".tmp";
    {
        std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        if (!file) return false;
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!file) return false;
    }
    stdfs::rename(tmp, path, ec);
    if (ec) {
        LOG_ERROR("写入 {} 失败: {}", path.string(), ec.message());
        return false;
    }
    return true;
}

stdfs::file_time_type lastWriteTime(const stdfs::path& path) {
    std::error_code ec;
    auto t = stdfs::last_write_time(path, ec);
    return ec ? stdfs::file_time_type::min() : t;
}

uint32_t FileWatcher::watch(const stdfs::path& path, Callback onChanged) {
    const uint32_t id = m_nextId++;
    m_entries.push_back({id, path, lastWriteTime(path), std::move(onChanged)});
    return id;
}

void FileWatcher::unwatch(uint32_t id) {
    std::erase_if(m_entries, [id](const Entry& e) { return e.id == id; });
}

void FileWatcher::poll(double nowSeconds, double intervalSeconds) {
    if (m_lastPoll >= 0.0 && nowSeconds - m_lastPoll < intervalSeconds) return;
    m_lastPoll = nowSeconds;
    // 先收集再回调：回调里可能会 watch/unwatch，直接在遍历中调用会让迭代器失效
    std::vector<std::pair<stdfs::path, Callback>> fired;
    for (auto& e : m_entries) {
        const auto t = lastWriteTime(e.path);
        if (t != e.stamp) {
            e.stamp = t;
            fired.emplace_back(e.path, e.callback);
        }
    }
    for (auto& [path, cb] : fired) cb(path);
}

} // namespace eng::fs
