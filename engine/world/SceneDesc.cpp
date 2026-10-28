#include "world/SceneDesc.h"

#include "core/Config.h" // glm 向量的 JSON 序列化
#include "core/FileSystem.h"
#include "core/Log.h"

#include <nlohmann/json.hpp>

namespace eng {

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(MaterialParams, baseColor, emissive, pbr, flags)

namespace {

nlohmann::json entityToJson(const EntityDesc& e) {
    nlohmann::json j = {{"name", e.name},
                        {"position", e.position},
                        // 四元数存成 [x, y, z, w]（与 glTF 一致）
                        {"rotation", vec4(e.rotation.x, e.rotation.y, e.rotation.z, e.rotation.w)},
                        {"scale", e.scale},
                        {"model", e.model},
                        {"instance", e.instance},
                        {"visible", e.visible}};
    if (e.material) j["material"] = *e.material;
    return j;
}

EntityDesc entityFromJson(const nlohmann::json& j) {
    EntityDesc e;
    e.name = j.value("name", std::string("Entity"));
    if (j.contains("position")) e.position = j["position"].get<vec3>();
    if (j.contains("rotation") && j["rotation"].size() == 4) {
        const auto& r = j["rotation"];
        e.rotation = glm::normalize(quat(r[3].get<float>(), r[0].get<float>(), r[1].get<float>(), r[2].get<float>()));
    }
    if (j.contains("scale")) e.scale = j["scale"].get<vec3>();
    e.model = j.at("model").get<std::string>(); // 没有模型的实体没有意义：缺了就让这一项失败
    e.instance = j.value("instance", 0u);
    e.visible = j.value("visible", true);
    if (j.contains("material")) e.material = j["material"].get<MaterialParams>();
    return e;
}

} // namespace

std::string sceneToJson(const SceneDesc& scene) {
    nlohmann::json j;
    j["version"] = scene.version;
    j["entities"] = nlohmann::json::array();
    for (const EntityDesc& e : scene.entities) j["entities"].push_back(entityToJson(e));
    return j.dump(2) + "\n";
}

std::optional<SceneDesc> sceneFromJson(const std::string& text, std::string* error) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(text, nullptr, true, true);
    } catch (const std::exception& ex) {
        if (error) *error = ex.what();
        return std::nullopt;
    }
    SceneDesc scene;
    scene.version = j.value("version", 1);
    if (!j.contains("entities") || !j["entities"].is_array()) return scene;
    size_t index = 0;
    for (const auto& ej : j["entities"]) {
        try {
            scene.entities.push_back(entityFromJson(ej));
        } catch (const std::exception& ex) {
            LOG_WARN("场景第 {} 个实体格式错误，已跳过: {}", index, ex.what());
        }
        ++index;
    }
    return scene;
}

bool saveSceneFile(const std::filesystem::path& path, const SceneDesc& scene) {
    if (!fs::writeText(path, sceneToJson(scene))) {
        LOG_ERROR("保存场景失败: {}", path.string());
        return false;
    }
    LOG_INFO("场景已保存: {}（{} 个实体）", path.filename().string(), scene.entities.size());
    return true;
}

std::optional<SceneDesc> loadSceneFile(const std::filesystem::path& path) {
    auto text = fs::readText(path);
    if (!text) return std::nullopt;
    std::string error;
    auto scene = sceneFromJson(*text, &error);
    if (!scene) LOG_ERROR("场景文件 {} 解析失败: {}", path.string(), error);
    return scene;
}

} // namespace eng
