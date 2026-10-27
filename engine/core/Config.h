#pragma once
// 参数文件（config/*.json）。
//
// 用法：每个模块把可调参数写成一个普通结构体，并用
//   NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Settings, a, b, c)
// 声明序列化。WITH_DEFAULT 的意思是「JSON 里缺的字段用结构体默认值」——
// 所以给结构体加新字段不需要同步改 JSON，老文件照样能读。
//
//   ConfigFile cfg("config/engine.json");
//   auto window = cfg.get<WindowSettings>("window");   // 读一节
//   cfg.set("window", window); cfg.save();             // ImGui 改完后保存
//   if (cfg.pollChanged(now)) { ...重新 get... }        // 外部编辑器改了文件 → 热加载
//
// 容错：文件缺失或 JSON 语法错误时保留上一次成功读取的内容并写 error 日志，程序继续跑。

#include "core/FileSystem.h"
#include "core/Log.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include <string>

// ---- GLM 类型的 JSON 序列化：向量写成数组 [x, y, z] ----------------------------------------
namespace nlohmann {
template <glm::length_t L, typename T, glm::qualifier Q>
struct adl_serializer<glm::vec<L, T, Q>> {
    static void to_json(json& j, const glm::vec<L, T, Q>& v) {
        j = json::array();
        for (glm::length_t i = 0; i < L; ++i) j.push_back(v[i]);
    }
    static void from_json(const json& j, glm::vec<L, T, Q>& v) {
        for (glm::length_t i = 0; i < L && i < static_cast<glm::length_t>(j.size()); ++i) v[i] = j[i].get<T>();
    }
};
} // namespace nlohmann

namespace eng {

class ConfigFile {
public:
    ConfigFile() = default;
    explicit ConfigFile(const std::filesystem::path& projectRelativePath);

    bool load();
    bool save();
    // 文件时间戳变了就重新加载，返回是否加载成功
    bool pollChanged();

    const std::filesystem::path& path() const { return m_path; }
    const std::string& lastError() const { return m_lastError; }

    template <class T>
    T get(const char* section) const {
        T value{};
        if (!m_json.contains(section)) return value;
        try {
            value = m_json.at(section).get<T>();
        } catch (const std::exception& e) {
            // 类型不对（比如把数字写成了字符串）：用默认值，并说明是哪一节出错
            LOG_ERROR("配置 {} 的 [{}] 解析失败，使用默认值: {}", m_path.filename().string(), section, e.what());
        }
        return value;
    }

    template <class T>
    void set(const char* section, const T& value) {
        m_json[section] = value;
    }

private:
    std::filesystem::path m_path;
    nlohmann::json m_json = nlohmann::json::object();
    std::filesystem::file_time_type m_stamp{};
    std::string m_lastError;
};

} // namespace eng
