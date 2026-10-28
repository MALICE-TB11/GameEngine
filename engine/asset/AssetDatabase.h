#pragma once
// 资产数据库：源资产 ↔ GUID ↔ 烘焙产物 的登记簿。
//
//   assets/models/ruins.glb          源资产（提交进 git）
//   assets/models/ruins.glb.meta     GUID + 导入设置（提交进 git；第一次扫描时自动生成）
//   build/cooked/<guid>.emodel       烘焙产物（不提交；源文件、.meta 或烘焙器版本变了就重新生成）
//
// 线程安全：ensureCooked 会在 Job 线程里调用，所有成员访问都加锁。

#include "core/Guid.h"
#include "core/JobSystem.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace eng::asset {

enum class AssetType { Model, Texture, Unknown };

struct AssetRecord {
    Guid guid;
    AssetType type = AssetType::Unknown;
    std::filesystem::path source;   // 绝对路径
    std::filesystem::path meta;     // 外部临时资产（不在 assets/ 下）为空
    nlohmann::json settings;        // 导入设置（来自 .meta）
    std::string displayName;        // 相对 assets/ 的路径，界面上显示
    bool transient = false;         // 外部文件：不写 .meta，GUID 由路径决定
};

class AssetDatabase {
public:
    void init(const std::filesystem::path& assetRoot, const std::filesystem::path& cookedRoot);

    // 扫描 assets/ 下所有支持的文件，补齐 .meta 并登记
    void scan();
    // 登记单个文件（assets/ 内：读/建 .meta；assets/ 外：临时登记，不写任何文件）
    std::optional<Guid> registerFile(const std::filesystem::path& path);
    // 把外部文件复制到 assets/imported/ 再登记（编辑器拖放导入）
    std::optional<Guid> importExternal(const std::filesystem::path& file);

    std::optional<AssetRecord> find(Guid guid) const;
    std::optional<Guid> findByPath(const std::filesystem::path& path) const;
    std::vector<AssetRecord> records() const;

    std::filesystem::path cookedPath(Guid guid) const;
    // 源文件或 .meta 的最后修改时间（取较新者），热重载据此判断
    std::filesystem::file_time_type sourceStamp(Guid guid) const;
    bool isStale(Guid guid) const;
    // 必要时烘焙。可以在 Job 线程调用；同一个资产不会被同时烘焙两次
    bool ensureCooked(Guid guid, JobSystem* jobs, std::string& error);

    static AssetType typeForExtension(const std::filesystem::path& path);
    const std::filesystem::path& assetRoot() const { return m_assetRoot; }

private:
    nlohmann::json defaultSettings(AssetType type, const std::filesystem::path& source) const;
    std::optional<AssetRecord> loadOrCreateMeta(const std::filesystem::path& source, AssetType type);
    static std::string key(const std::filesystem::path& p);

    std::filesystem::path m_assetRoot;
    std::filesystem::path m_cookedRoot;
    mutable std::mutex m_mutex;
    std::unordered_map<Guid, AssetRecord> m_records;
    std::unordered_map<std::string, Guid> m_byPath;
    std::mutex m_cookMutex;
    std::unordered_set<Guid> m_cooking;
};

} // namespace eng::asset
