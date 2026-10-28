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

// 实体的物理参与方式（M4）。碰撞体由模型几何自动生成，见 world/ScenePhysics.h
enum class PhysicsMode : uint8_t {
    None,    // 不参与物理（纯装饰）
    Static,  // 静态碰撞体：三角网格，精确贴合模型，永远不动
    Dynamic, // 动态刚体：模型顶点的凸包，受重力和碰撞驱动
};
const char* physicsModeToString(PhysicsMode mode);
PhysicsMode physicsModeFromString(const std::string& s); // 未知字符串 → None

struct EntityDesc {
    std::string name;
    vec3 position{0.0f};
    quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    vec3 scale{1.0f};
    std::string model;     // 资产 GUID（32 位十六进制）
    uint32_t instance = 0;
    bool visible = true;
    std::optional<MaterialParams> material;
    PhysicsMode physics = PhysicsMode::None;
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
