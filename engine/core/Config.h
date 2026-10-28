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

#include <cstdio>
#include <cstdlib>
#include <string>
#include <type_traits>

// ---- float 的 JSON 序列化：按 float 的有效位数（7 位）输出 -----------------------------------
// 默认会先转成 double 再打印 17 位，0.42f 会变成 0.41999998688697815 —— 配置和场景文件要给人读、
// 要进 git 做 diff，这种噪声没有任何信息量。
namespace eng::detail {
inline double floatForJson(float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.7g", double(v));
    return std::strtod(buf, nullptr);
}
} // namespace eng::detail

namespace nlohmann {
template <>
struct adl_serializer<float> {
    static void to_json(json& j, float v) { j = eng::detail::floatForJson(v); }
    static void from_json(const json& j, float& v) { v = static_cast<float>(j.get<double>()); }
};

// ---- GLM 类型的 JSON 序列化：向量写成数组 [x, y, z] ----------------------------------------
template <glm::length_t L, typename T, glm::qualifier Q>
struct adl_serializer<glm::vec<L, T, Q>> {
    static void to_json(json& j, const glm::vec<L, T, Q>& v) {
        j = json::array();
        for (glm::length_t i = 0; i < L; ++i) {
            if constexpr (std::is_same_v<T, float>)
                j.push_back(eng::detail::floatForJson(v[i]));
            else
                j.push_back(v[i]);
        }
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

// 把一个配置值夹到合法范围，被修正时写警告（哪一项、原值、范围）。返回是否修正过。
// 「JSON 类型对、数值不合法」（零 FOV、负的帧时间、超大阴影图）由它兜住：程序照常运行，日志里看得到问题。
// 条件写成 !(lo ≤ v ≤ hi)，这样 NaN 也会被修正成 lo。
template <class T>
bool clampSetting(T& v, T lo, T hi, const char* name) {
    if (v >= lo && v <= hi) return false; // 合法：未修正
    const T old = v;
    v = (v != v) ? lo : std::clamp(v, lo, hi);
    LOG_WARN("配置项 {} = {} 超出范围 [{}, {}]，已修正为 {}", name, old, lo, hi, v);
    return true;
}

} // namespace eng
