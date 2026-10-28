#include "asset/AssetManager.h"

#include "asset/CookedFormats.h"
#include "core/FileSystem.h"
#include "core/JobSystem.h"
#include "core/Log.h"
#include "rhi/Device.h"
#include "rhi/Upload.h"

namespace eng::asset {
namespace {

// 单网格 + 单材质 + 单实例的模型（内置几何体、兜底模型都用它）
Model makeSingleMeshModel(rhi::Device& device, const std::string& name, const MeshData& mesh, const Material& material) {
    ModelData data;
    data.name = name;
    data.meshNames.push_back(name);
    data.meshes.push_back(mesh);
    MaterialDesc md;
    md.name = material.name;
    md.params = material.params;
    data.materials.push_back(md);
    data.instances.push_back({0, 0, mat4(1.0f), name});
    return uploadModel(device, data);
}

} // namespace

Guid AssetManager::builtinGuid(BuiltinModel which) {
    switch (which) {
    case BuiltinModel::Cube: return Guid::fromName("builtin/cube");
    case BuiltinModel::Sphere: return Guid::fromName("builtin/sphere");
    case BuiltinModel::Capsule: return Guid::fromName("builtin/capsule");
    default: return Guid::fromName("builtin/plane");
    }
}

void AssetManager::init(rhi::Device& device, AssetDatabase& db, JobSystem& jobs, const rhi::Image& errorTexture) {
    m_device = &device;
    m_db = &db;
    m_jobs = &jobs;

    // 兜底模型：品红棋盘立方体。加载中或加载失败的资产都显示它 —— 醒目，一看就知道「这里缺东西」
    Material err;
    err.name = "AssetFallback";
    err.params.baseColor = vec4(1.0f);
    err.params.pbr = {0.0f, 0.8f, 1.0f, 0.5f};
    m_fallback = std::make_unique<Model>(makeSingleMeshModel(device, "AssetFallback", makeCube(0.5f), err));
    m_fallback->materials[0].baseColorTex = &errorTexture;
    createBuiltins();
}

void AssetManager::createBuiltins() {
    Material def;
    def.name = "Default";
    def.params.baseColor = {0.8f, 0.8f, 0.8f, 1.0f};
    def.params.pbr = {0.0f, 0.6f, 1.0f, 0.5f};
    struct B {
        BuiltinModel which;
        const char* name;
        MeshData mesh;
    };
    B builtins[] = {{BuiltinModel::Cube, "Cube", makeCube(0.5f)},
                    {BuiltinModel::Sphere, "Sphere", makeUvSphere(0.5f, 48, 24)},
                    {BuiltinModel::Plane, "Plane", makePlane(0.5f, 1.0f)},
                    // 与默认角色控制器同尺寸：半径 0.35、高 1.8，底部在原点
                    {BuiltinModel::Capsule, "Capsule", makeCapsule(0.35f, 1.8f)}};
    for (B& b : builtins) {
        Slot s;
        s.guid = builtinGuid(b.which);
        s.name = std::string("内置/") + b.name;
        s.model = std::make_unique<Model>(makeSingleMeshModel(*m_device, b.name, b.mesh, def));
        s.status = AssetStatus::Ready;
        s.builtin = true;
        s.version = 1;
        const uint32_t index = uint32_t(m_slots.size());
        m_byGuid[s.guid] = index;
        m_slots.push_back(std::move(s));
        m_builtins[int(b.which)] = {index};
    }
}

void AssetManager::shutdown() {
    if (!m_device) return;
    for (Slot& s : m_slots)
        if (s.model) destroyModel(*m_device, *s.model);
    m_slots.clear();
    m_byGuid.clear();
    for (TextureSlot& t : m_textures)
        if (t.image) rhi::destroyImage(*m_device, *t.image);
    m_textures.clear();
    m_textureByGuid.clear();
    if (m_fallback) destroyModel(*m_device, *m_fallback);
    m_fallback.reset();
    m_device = nullptr;
}

ModelHandle AssetManager::builtin(BuiltinModel which) const {
    return m_builtins[int(which)];
}

ModelHandle AssetManager::loadModel(Guid guid) {
    if (auto it = m_byGuid.find(guid); it != m_byGuid.end()) return {it->second};
    Slot s;
    s.guid = guid;
    const auto rec = m_db->find(guid);
    s.name = rec ? rec->displayName : guid.toString();
    const uint32_t index = uint32_t(m_slots.size());
    m_byGuid[guid] = index;
    m_slots.push_back(std::move(s));
    if (!rec) {
        m_slots[index].status = AssetStatus::Failed;
        m_slots[index].error = "资产数据库中没有这个 GUID（文件被删除或 .meta 丢失？）";
        LOG_WARN("加载模型 {} 失败: {}", guid.toString(), m_slots[index].error);
        return {index};
    }
    startLoad(index);
    return {index};
}

void AssetManager::startLoad(uint32_t index) {
    Slot& slot = m_slots[index];
    if (slot.inFlight || slot.builtin) return;
    slot.inFlight = true;
    slot.stamp = m_db->sourceStamp(slot.guid);
    ++m_pending;

    const Guid guid = slot.guid;
    AssetDatabase* db = m_db;
    JobSystem* jobs = m_jobs;
    // 后台线程：烘焙（如需要）+ 读文件 + 解析。不碰 GPU、不碰 m_slots
    jobs->submit([this, index, guid, db, jobs] {
        auto result = std::make_shared<std::optional<ModelData>>();
        auto error = std::make_shared<std::string>();
        if (db->ensureCooked(guid, jobs, *error)) {
            if (auto bytes = fs::readBinary(db->cookedPath(guid))) *result = readCookedModel(*bytes);
            if (!*result && error->empty()) *error = "烘焙文件损坏或版本不符: " + db->cookedPath(guid).string();
        }
        // 回到主线程：上传 GPU、替换槽位内容
        jobs->runOnMainThread([this, index, result, error] {
            Slot& s = m_slots[index];
            s.inFlight = false;
            --m_pending;
            if (*result) {
                if (s.model) {
                    // 热重载：旧模型可能还在被在途的帧使用，延迟销毁
                    auto old = std::make_shared<Model>(std::move(*s.model));
                    m_device->deferDestroy([dev = m_device, old] { destroyModel(*dev, *old); });
                }
                s.model = std::make_unique<Model>(uploadModel(*m_device, **result));
                s.status = AssetStatus::Ready;
                s.error.clear();
                ++s.version;
                LOG_INFO("模型就绪: {}（{} 个网格，{} 张贴图）", s.name, s.model->meshes.size(), s.model->textures.size());
            } else {
                // 热重载失败时保留旧模型（如果有）：和 shader 热重载同样的容错思路
                s.status = s.model ? AssetStatus::Ready : AssetStatus::Failed;
                s.error = *error;
                LOG_ERROR("模型 {} 加载失败: {}", s.name, *error);
            }
        });
    });
}

const Model& AssetManager::model(ModelHandle h) const {
    if (h.index < m_slots.size() && m_slots[h.index].model) return *m_slots[h.index].model;
    return *m_fallback;
}

Model* AssetManager::mutableModel(ModelHandle h) {
    return h.index < m_slots.size() ? m_slots[h.index].model.get() : nullptr;
}

AssetStatus AssetManager::status(ModelHandle h) const {
    return h.index < m_slots.size() ? m_slots[h.index].status : AssetStatus::Failed;
}

const std::string& AssetManager::error(ModelHandle h) const {
    static const std::string none;
    return h.index < m_slots.size() ? m_slots[h.index].error : none;
}

Guid AssetManager::guid(ModelHandle h) const {
    return h.index < m_slots.size() ? m_slots[h.index].guid : Guid{};
}

std::string AssetManager::name(ModelHandle h) const {
    return h.index < m_slots.size() ? m_slots[h.index].name : std::string("?");
}

uint32_t AssetManager::version(ModelHandle h) const {
    return h.index < m_slots.size() ? m_slots[h.index].version : 0u;
}

void AssetManager::reload(ModelHandle h) {
    if (h.index < m_slots.size()) startLoad(h.index);
}

// ---- 独立贴图：与模型同样的「Job 线程烘焙/读取 → 主线程上传」流程 ----------------------------------

TextureHandle AssetManager::loadTexture(Guid guid) {
    if (auto it = m_textureByGuid.find(guid); it != m_textureByGuid.end()) return {it->second};
    TextureSlot s;
    s.guid = guid;
    const auto rec = m_db->find(guid);
    s.name = rec ? rec->displayName : guid.toString();
    const uint32_t index = uint32_t(m_textures.size());
    m_textureByGuid[guid] = index;
    m_textures.push_back(std::move(s));
    if (!rec || rec->type != AssetType::Texture) {
        m_textures[index].status = AssetStatus::Failed;
        m_textures[index].error = rec ? "这个资产不是贴图" : "资产数据库中没有这个 GUID";
        LOG_WARN("加载贴图 {} 失败: {}", guid.toString(), m_textures[index].error);
        return {index};
    }
    startTextureLoad(index);
    return {index};
}

void AssetManager::startTextureLoad(uint32_t index) {
    TextureSlot& slot = m_textures[index];
    if (slot.inFlight) return;
    slot.inFlight = true;
    slot.stamp = m_db->sourceStamp(slot.guid);
    ++m_pending;

    const Guid guid = slot.guid;
    AssetDatabase* db = m_db;
    JobSystem* jobs = m_jobs;
    jobs->submit([this, index, guid, db, jobs] {
        auto result = std::make_shared<std::optional<TextureData>>();
        auto error = std::make_shared<std::string>();
        if (db->ensureCooked(guid, jobs, *error)) {
            if (auto bytes = fs::readBinary(db->cookedPath(guid))) *result = readCookedTexture(*bytes);
            if (!*result && error->empty()) *error = "烘焙文件损坏或版本不符: " + db->cookedPath(guid).string();
        }
        jobs->runOnMainThread([this, index, result, error] {
            TextureSlot& s = m_textures[index];
            s.inFlight = false;
            --m_pending;
            if (*result) {
                const TextureData& td = **result;
                if (s.image) {
                    auto old = std::make_shared<rhi::Image>(*s.image);
                    m_device->deferDestroy([dev = m_device, old] { rhi::destroyImage(*dev, *old); });
                }
                // 未压缩的单级 RGBA8（烘焙设置 compress = false）由 GPU 生成 mip；其余直接上传烘焙好的 mip 链
                const bool raw = td.mips.size() == 1 && (td.format == VK_FORMAT_R8G8B8A8_UNORM || td.format == VK_FORMAT_R8G8B8A8_SRGB);
                s.image = std::make_unique<rhi::Image>(
                    raw ? rhi::createTexture2D(*m_device, td.mips[0].data(), td.width, td.height, td.format, true, s.name.c_str())
                        : rhi::createTextureFromMips(*m_device, td.format, td.width, td.height, td.mips, s.name.c_str()));
                s.status = AssetStatus::Ready;
                s.error.clear();
                LOG_INFO("贴图就绪: {}（{}×{}，{} 级 mip）", s.name, td.width, td.height, td.mips.size());
            } else {
                s.status = s.image ? AssetStatus::Ready : AssetStatus::Failed;
                s.error = *error;
                LOG_ERROR("贴图 {} 加载失败: {}", s.name, *error);
            }
        });
    });
}

const rhi::Image* AssetManager::texture(TextureHandle h) const {
    return h.index < m_textures.size() ? m_textures[h.index].image.get() : nullptr;
}

AssetStatus AssetManager::status(TextureHandle h) const {
    return h.index < m_textures.size() ? m_textures[h.index].status : AssetStatus::Failed;
}

void AssetManager::update(double nowSeconds) {
    // 每帧最多完成 4 个上传：大量资产同时加载完时，分摊到几帧，避免一帧卡顿
    m_jobs->pumpMainThread(4);

    // 热重载：每 0.5 秒检查一次已加载资产的源文件和 .meta
    if (m_lastPoll >= 0.0 && nowSeconds - m_lastPoll < 0.5) return;
    m_lastPoll = nowSeconds;
    for (uint32_t i = 0; i < m_slots.size(); ++i) {
        Slot& s = m_slots[i];
        if (s.builtin || s.inFlight || !m_db->find(s.guid)) continue;
        if (m_db->sourceStamp(s.guid) != s.stamp) {
            LOG_INFO("资产已修改，热重载: {}", s.name);
            startLoad(i);
        }
    }
    for (uint32_t i = 0; i < m_textures.size(); ++i) {
        TextureSlot& t = m_textures[i];
        if (t.inFlight || !m_db->find(t.guid)) continue;
        if (m_db->sourceStamp(t.guid) != t.stamp) {
            LOG_INFO("贴图已修改，热重载: {}", t.name);
            startTextureLoad(i);
        }
    }
}

std::vector<AssetManager::Info> AssetManager::loaded() const {
    std::vector<Info> out;
    for (uint32_t i = 0; i < m_slots.size(); ++i) {
        const Slot& s = m_slots[i];
        out.push_back({{i}, s.guid, s.name, s.status, s.error, s.builtin});
    }
    return out;
}

} // namespace eng::asset
