#include "asset/AssetDatabase.h"

#include "asset/CookedFormats.h"
#include "asset/Cookers.h"
#include "core/FileSystem.h"
#include "core/ImageIO.h"
#include "core/Log.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <thread>

namespace eng::asset {

namespace stdfs = std::filesystem;

std::string AssetDatabase::key(const stdfs::path& p) {
    // 路径统一成小写的规范形式（Windows 文件名不区分大小写）
    std::string s = stdfs::weakly_canonical(p).generic_string();
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

AssetType AssetDatabase::typeForExtension(const stdfs::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (ext == ".glb" || ext == ".gltf") return AssetType::Model;
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga") return AssetType::Texture;
    return AssetType::Unknown;
}

void AssetDatabase::init(const stdfs::path& assetRoot, const stdfs::path& cookedRoot) {
    m_assetRoot = stdfs::weakly_canonical(assetRoot);
    m_cookedRoot = cookedRoot;
    std::error_code ec;
    stdfs::create_directories(m_cookedRoot, ec);
}

nlohmann::json AssetDatabase::defaultSettings(AssetType type) const {
    // 导入设置的默认值；改 .meta 里的值后保存，资产会自动重新烘焙
    if (type == AssetType::Model)
        return {{"optimize", true}, {"maxLods", 4}, {"lodReduction", 0.5}, {"lodMaxError", 0.02}, {"compressTextures", true}};
    if (type == AssetType::Texture) return {{"usage", "color"}, {"generateMips", true}, {"compress", true}};
    return nlohmann::json::object();
}

std::optional<AssetRecord> AssetDatabase::loadOrCreateMeta(const stdfs::path& source, AssetType type) {
    AssetRecord rec;
    rec.type = type;
    rec.source = source;
    rec.meta = source.string() + ".meta";
    rec.displayName = stdfs::relative(source, m_assetRoot).generic_string();

    if (auto text = fs::readText(rec.meta)) {
        try {
            const auto j = nlohmann::json::parse(*text);
            if (auto g = Guid::parse(j.value("guid", std::string{}))) rec.guid = *g;
            rec.settings = defaultSettings(type);
            // 合并：.meta 里有的字段覆盖默认值，缺的字段用默认值（老 .meta 不需要手动升级）
            if (j.contains("settings")) rec.settings.merge_patch(j["settings"]);
        } catch (const std::exception& e) {
            LOG_ERROR("{} 解析失败（{}），保留文件不动，本次按临时资产处理", rec.meta.string(), e.what());
            rec.guid = Guid::fromName(key(source));
            rec.meta.clear();
            rec.transient = true;
            rec.settings = defaultSettings(type);
            return rec;
        }
    }
    if (!rec.guid.valid()) {
        // 第一次见到这个文件：生成 GUID 并写 .meta
        rec.guid = Guid::random();
        rec.settings = defaultSettings(type);
        nlohmann::json j = {{"guid", rec.guid.toString()},
                            {"type", type == AssetType::Model ? "model" : "texture"},
                            {"settings", rec.settings}};
        if (fs::writeText(rec.meta, j.dump(2) + "\n")) LOG_INFO("新资产 {} → {}", rec.displayName, rec.guid.toString());
    }
    return rec;
}

std::optional<Guid> AssetDatabase::registerFile(const stdfs::path& path) {
    const stdfs::path abs = stdfs::weakly_canonical(path);
    const AssetType type = typeForExtension(abs);
    if (type == AssetType::Unknown || !stdfs::exists(abs)) return std::nullopt;
    {
        std::lock_guard lock(m_mutex);
        if (auto it = m_byPath.find(key(abs)); it != m_byPath.end()) return it->second;
    }
    std::optional<AssetRecord> rec;
    const std::string rel = stdfs::relative(abs, m_assetRoot).generic_string();
    if (!rel.empty() && rel.rfind("..", 0) != 0) {
        rec = loadOrCreateMeta(abs, type);
    } else {
        // assets/ 之外的文件（--model 指定的外部路径）：不往用户目录里写 .meta，GUID 由路径决定
        AssetRecord r;
        r.guid = Guid::fromName(key(abs));
        r.type = type;
        r.source = abs;
        r.settings = defaultSettings(type);
        r.displayName = abs.filename().string() + "（外部）";
        r.transient = true;
        rec = r;
    }
    if (!rec) return std::nullopt;
    std::lock_guard lock(m_mutex);
    m_byPath[key(abs)] = rec->guid;
    m_records[rec->guid] = *rec;
    return rec->guid;
}

void AssetDatabase::scan() {
    std::error_code ec;
    if (!stdfs::exists(m_assetRoot, ec)) return;
    size_t count = 0;
    for (auto it = stdfs::recursive_directory_iterator(m_assetRoot, ec); it != stdfs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (it->is_regular_file() && typeForExtension(it->path()) != AssetType::Unknown && registerFile(it->path())) ++count;
    }
    LOG_INFO("资产数据库：{} 个资产", count);
}

std::optional<Guid> AssetDatabase::importExternal(const stdfs::path& file) {
    if (typeForExtension(file) == AssetType::Unknown) {
        LOG_WARN("不支持的资产类型: {}", file.string());
        return std::nullopt;
    }
    // 已经在 assets/ 里：直接登记
    const std::string rel = stdfs::relative(stdfs::weakly_canonical(file), m_assetRoot).generic_string();
    if (!rel.empty() && rel.rfind("..", 0) != 0) return registerFile(file);

    const stdfs::path dstDir = m_assetRoot / "imported";
    std::error_code ec;
    stdfs::create_directories(dstDir, ec);
    stdfs::path dst = dstDir / file.filename();
    for (int i = 1; stdfs::exists(dst); ++i) dst = dstDir / (file.stem().string() + "_" + std::to_string(i) + file.extension().string());
    stdfs::copy_file(file, dst, ec);
    if (ec) {
        LOG_ERROR("复制 {} 失败: {}", file.string(), ec.message());
        return std::nullopt;
    }
    // EXTEND: .gltf 引用的外部 .bin / 贴图没有一起复制；需要时在这里解析 glTF 的 uri 并一起拷贝
    LOG_INFO("已导入 {}", dst.string());
    return registerFile(dst);
}

std::optional<AssetRecord> AssetDatabase::find(Guid guid) const {
    std::lock_guard lock(m_mutex);
    if (auto it = m_records.find(guid); it != m_records.end()) return it->second;
    return std::nullopt;
}

std::optional<Guid> AssetDatabase::findByPath(const stdfs::path& path) const {
    std::lock_guard lock(m_mutex);
    if (auto it = m_byPath.find(key(path)); it != m_byPath.end()) return it->second;
    return std::nullopt;
}

std::vector<AssetRecord> AssetDatabase::records() const {
    std::lock_guard lock(m_mutex);
    std::vector<AssetRecord> out;
    for (const auto& [g, r] : m_records) out.push_back(r);
    std::sort(out.begin(), out.end(), [](const AssetRecord& a, const AssetRecord& b) { return a.displayName < b.displayName; });
    return out;
}

stdfs::path AssetDatabase::cookedPath(Guid guid) const {
    const auto rec = find(guid);
    const char* ext = rec && rec->type == AssetType::Texture ? ".etex" : ".emodel";
    return m_cookedRoot / (guid.toString() + ext);
}

stdfs::file_time_type AssetDatabase::sourceStamp(Guid guid) const {
    const auto rec = find(guid);
    if (!rec) return stdfs::file_time_type::min();
    auto t = fs::lastWriteTime(rec->source);
    if (!rec->meta.empty()) t = std::max(t, fs::lastWriteTime(rec->meta));
    return t;
}

bool AssetDatabase::isStale(Guid guid) const {
    const stdfs::path cooked = cookedPath(guid);
    const auto cookedTime = fs::lastWriteTime(cooked);
    if (cookedTime == stdfs::file_time_type::min() || cookedTime < sourceStamp(guid)) return true;
    // 烘焙器版本变了（算法改动）也要重烘：只读文件头 8 字节
    std::ifstream f(cooked, std::ios::binary);
    std::vector<uint8_t> head(8);
    f.read(reinterpret_cast<char*>(head.data()), 8);
    return !f || !cookedHeaderIsCurrent(head);
}

bool AssetDatabase::ensureCooked(Guid guid, JobSystem* jobs, std::string& error) {
    const auto rec = find(guid);
    if (!rec) {
        error = "未登记的资产 " + guid.toString();
        return false;
    }
    // 同一资产同时只允许一个线程烘焙：其他线程等它完成
    while (true) {
        {
            std::lock_guard lock(m_cookMutex);
            if (!m_cooking.count(guid)) {
                m_cooking.insert(guid);
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    struct Release {
        AssetDatabase* db;
        Guid g;
        ~Release() {
            std::lock_guard lock(db->m_cookMutex);
            db->m_cooking.erase(g);
        }
    } release{this, guid};

    if (!isStale(guid)) return true;

    const auto start = std::chrono::steady_clock::now();
    std::vector<uint8_t> bytes;
    if (rec->type == AssetType::Model) {
        auto data = loadGltfData(rec->source);
        if (!data) {
            error = "导入 glTF 失败: " + rec->source.string();
            return false;
        }
        ModelCookSettings s;
        s.optimize = rec->settings.value("optimize", true);
        s.maxLods = rec->settings.value("maxLods", 4u);
        s.lodReduction = rec->settings.value("lodReduction", 0.5f);
        s.lodMaxError = rec->settings.value("lodMaxError", 0.02f);
        s.compressTextures = rec->settings.value("compressTextures", true);
        if (!cookModel(*data, s, jobs, error)) return false;
        bytes = writeCookedModel(*data);
    } else if (rec->type == AssetType::Texture) {
        auto img = loadImageRGBA8(rec->source);
        if (!img) {
            error = "读取图片失败: " + rec->source.string();
            return false;
        }
        TextureData t;
        t.name = rec->source.stem().string();
        const std::string usage = rec->settings.value("usage", std::string("color"));
        t.usage = usage == "normal" ? TextureUsage::Normal : usage == "data" ? TextureUsage::Data : TextureUsage::Color;
        t.format = t.usage == TextureUsage::Color ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        t.width = uint32_t(img->width);
        t.height = uint32_t(img->height);
        t.mips.push_back(std::move(img->rgba8));
        TextureCookSettings ts;
        ts.generateMips = rec->settings.value("generateMips", true);
        ts.compress = rec->settings.value("compress", true);
        if (!cookTexture(t, ts, jobs, error)) return false;
        bytes = writeCookedTexture(t);
    } else {
        error = "未知资产类型";
        return false;
    }

    // 先写临时文件再改名：烘焙到一半崩溃不会留下损坏的缓存
    const stdfs::path out = cookedPath(guid);
    const stdfs::path tmp = out.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        if (!f) {
            error = "写入烘焙文件失败: " + tmp.string();
            return false;
        }
    }
    std::error_code ec;
    stdfs::rename(tmp, out, ec);
    if (ec) {
        error = "重命名烘焙文件失败: " + ec.message();
        return false;
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    LOG_INFO("烘焙 {} 完成：{:.1f} KB，用时 {:.0f} ms", rec->displayName, double(bytes.size()) / 1024.0, ms);
    return true;
}

} // namespace eng::asset
