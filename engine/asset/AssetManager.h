#pragma once
// 运行时资产管理：用句柄引用已加载的模型，后台加载，热重载，失败兜底。
//
// 为什么用句柄而不是指针：模型被热重载（源文件改了 → 重新烘焙 → 重新上传）时，
// GPU 资源会整个换掉。如果场景里存的是 Model* / GpuMesh*，旧指针就悬空了。
// 句柄只是「槽位下标」，槽位里的内容可以换，句柄永远有效；每次使用时再通过句柄取当前内容。
//
// 加载流程（不阻塞主线程）：
//   主线程 loadModel(guid) → 立即返回句柄，状态 Loading
//   Job 线程：必要时烘焙 → 读取烘焙文件 → 解析成 ModelData
//   主线程（下一帧 update 时）：上传 GPU → 状态 Ready
// Loading / Failed 期间 model(handle) 返回「兜底模型」（品红棋盘立方体），场景照常渲染。

#include "asset/AssetDatabase.h"
#include "render/Model.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace eng {
class JobSystem;
namespace rhi {
class Device;
struct Image;
} // namespace rhi
} // namespace eng

namespace eng::asset {

struct ModelHandle {
    uint32_t index = UINT32_MAX;
    bool valid() const { return index != UINT32_MAX; }
    bool operator==(const ModelHandle&) const = default;
};

enum class AssetStatus { Loading, Ready, Failed };

// 内置程序化模型：不需要文件，GUID 固定（Guid::fromName("builtin/cube") 等），可以写进场景文件
enum class BuiltinModel { Cube, Sphere, Plane };

class AssetManager {
public:
    void init(rhi::Device& device, AssetDatabase& db, JobSystem& jobs, const rhi::Image& errorTexture);
    void shutdown();

    ModelHandle loadModel(Guid guid); // 已加载过则直接返回同一个句柄
    ModelHandle builtin(BuiltinModel which) const;
    static Guid builtinGuid(BuiltinModel which);

    // 总是返回可用的模型：未就绪或失败时返回兜底模型
    const Model& model(ModelHandle h) const;
    Model* mutableModel(ModelHandle h); // 编辑器改材质参数用；未就绪返回 nullptr
    AssetStatus status(ModelHandle h) const;
    const std::string& error(ModelHandle h) const;
    Guid guid(ModelHandle h) const;
    std::string name(ModelHandle h) const;

    // 主线程每帧调用：完成后台加载的 GPU 上传，检查源文件变化并热重载
    void update(double nowSeconds);
    void reload(ModelHandle h);
    uint32_t pendingCount() const { return m_pending; }

    struct Info {
        ModelHandle handle;
        Guid guid;
        std::string name;
        AssetStatus status;
        std::string error;
        bool builtin;
    };
    std::vector<Info> loaded() const;
    AssetDatabase& database() { return *m_db; }

private:
    struct Slot {
        Guid guid;
        std::string name;
        std::unique_ptr<Model> model;
        AssetStatus status = AssetStatus::Loading;
        std::string error;
        bool builtin = false;
        bool inFlight = false;
        std::filesystem::file_time_type stamp{}; // 加载时源文件的时间戳
    };
    void startLoad(uint32_t index);
    void createBuiltins();

    rhi::Device* m_device = nullptr;
    AssetDatabase* m_db = nullptr;
    JobSystem* m_jobs = nullptr;
    std::vector<Slot> m_slots;
    std::unordered_map<Guid, uint32_t> m_byGuid;
    std::unique_ptr<Model> m_fallback;
    uint32_t m_pending = 0;
    double m_lastPoll = -1.0;
    ModelHandle m_builtins[3];
};

} // namespace eng::asset
