#include "core/Config.h"

namespace eng {

ConfigFile::ConfigFile(const std::filesystem::path& projectRelativePath)
    : m_path(fs::resolve(projectRelativePath)) {
    load();
}

bool ConfigFile::load() {
    m_stamp = fs::lastWriteTime(m_path);
    auto text = fs::readText(m_path);
    if (!text) {
        m_lastError = "文件不存在";
        LOG_WARN("配置文件不存在，使用默认值: {}", m_path.string());
        return false;
    }
    try {
        // allow_exceptions=true, ignore_comments=true：允许在 JSON 里写 // 注释，方便给参数加说明
        m_json = nlohmann::json::parse(*text, nullptr, true, true);
        m_lastError.clear();
        LOG_INFO("已加载配置 {}", m_path.filename().string());
        return true;
    } catch (const nlohmann::json::parse_error& e) {
        m_lastError = e.what();
        LOG_ERROR("配置 {} 语法错误，保留旧值: {}", m_path.filename().string(), e.what());
        return false;
    }
}

bool ConfigFile::save() {
    if (!fs::writeText(m_path, m_json.dump(2) + "\n")) {
        LOG_ERROR("保存配置失败: {}", m_path.string());
        return false;
    }
    // 记下自己写入后的时间戳，避免 pollChanged 把自己的保存当成外部修改
    m_stamp = fs::lastWriteTime(m_path);
    LOG_INFO("已保存配置 {}", m_path.filename().string());
    return true;
}

bool ConfigFile::pollChanged() {
    if (fs::lastWriteTime(m_path) == m_stamp) return false;
    return load();
}

} // namespace eng
