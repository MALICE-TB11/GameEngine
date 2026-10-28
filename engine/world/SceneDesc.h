#pragma once
// 场景描述：场景文件（scenes/*.scene.json）在内存中的纯数据形式。
//
// 与 Scene 分开的原因：Scene 持有运行时句柄（需要 GPU、资产管理器），没法在单元测试里直接构造；
// SceneDesc 只是数据，可以独立测试 JSON 读写，Scene 再负责「描述 ↔ 运行时」的转换。

#include "math/MathTypes.h"
#include "render/Material.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace eng {

struct EntityDesc {
    std::string name;
    vec3 position{0.0f};
    quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    vec3 scale{1.0f};
    std::string model;     // 资产 GUID（32 位十六进制）
    uint32_t instance = 0;
    bool visible = true;
    std::optional<MaterialParams> material;
};

struct SceneDesc {
    int version = 1;
    std::vector<EntityDesc> entities;
};

std::string sceneToJson(const SceneDesc& scene);
// 容错：单个实体字段错误时跳过该实体并写警告，而不是整个场景加载失败
std::optional<SceneDesc> sceneFromJson(const std::string& text, std::string* error = nullptr);

bool saveSceneFile(const std::filesystem::path& path, const SceneDesc& scene);
std::optional<SceneDesc> loadSceneFile(const std::filesystem::path& path);

} // namespace eng
