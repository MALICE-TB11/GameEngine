#include "world/Scene.h"

#include "core/Log.h"
#include "math/Geometry.h"

#include <algorithm>

namespace eng {

EntityId Scene::add(Entity e) {
    e.previous = e.current;
    m_entities.push_back(std::move(e));
    return static_cast<EntityId>(m_entities.size() - 1);
}

void Scene::remove(EntityId id) {
    if (id < m_entities.size()) m_entities.erase(m_entities.begin() + id);
}

std::optional<EntityId> Scene::findByName(const std::string& name) const {
    for (EntityId id = 0; id < m_entities.size(); ++id)
        if (m_entities[id].name == name) return id;
    return std::nullopt;
}

EntityId Scene::instantiate(ModelHandle model, const Transform& root, const std::string& prefix) {
    const Model& m = m_assets->model(model);
    // 模型还在加载时只知道兜底模型的实例数（1 个）；加载完成后实例更多的模型需要重新实例化
    // （场景文件里每个实体都记录了实例号，所以「加载场景」不受影响）。
    // EXTEND: 支持「模型加载完成后自动补齐实例」
    const size_t count = std::max<size_t>(1, m.instances.size());
    const EntityId first = static_cast<EntityId>(m_entities.size());
    for (uint32_t i = 0; i < count; ++i) {
        Entity e;
        const std::string node = i < m.instances.size() ? m.instances[i].name : "Node";
        e.name = count > 1 ? prefix + "/" + node : prefix;
        e.current = root;
        e.model = model;
        e.instance = i;
        add(std::move(e));
    }
    return first;
}

void Scene::beginFixedStep() {
    for (Entity& e : m_entities) e.previous = e.current;
}

const ModelInstance* Scene::resolveInstance(const Entity& e) const {
    const Model& m = m_assets->model(e.model);
    if (m.instances.empty()) return nullptr;
    // 实例号越界（模型被修改、实例变少）时退回第 0 个，而不是崩溃
    return &m.instances[e.instance < m.instances.size() ? e.instance : 0];
}

const GpuMesh* Scene::mesh(EntityId id) const {
    const Entity& e = m_entities[id];
    const ModelInstance* inst = resolveInstance(e);
    const Model& m = m_assets->model(e.model);
    return inst && inst->mesh < m.meshes.size() ? &m.meshes[inst->mesh] : nullptr;
}

const Material* Scene::material(EntityId id) const {
    const Entity& e = m_entities[id];
    const ModelInstance* inst = resolveInstance(e);
    const Model& m = m_assets->model(e.model);
    return inst && inst->material < m.materials.size() ? &m.materials[inst->material] : nullptr;
}

mat4 Scene::worldMatrix(EntityId id, float alpha) const {
    const Entity& e = m_entities[id];
    const ModelInstance* inst = resolveInstance(e);
    return Transform::lerp(e.previous, e.current, alpha).matrix() * (inst ? inst->transform : mat4(1.0f));
}

bool Scene::worldBounds(EntityId id, vec3& outMin, vec3& outMax) const {
    const GpuMesh* m = mesh(id);
    if (!m) return false;
    transformAabb(worldMatrix(id), m->bounds.min, m->bounds.max, outMin, outMax);
    return true;
}

void Scene::buildDrawList(DrawList& out, float alpha, const LodContext& lod, std::optional<EntityId> highlighted,
                          float highlightStrength) const {
    out.opaque.clear();
    uint64_t triangles = 0;
    for (EntityId id = 0; id < m_entities.size(); ++id) {
        const Entity& e = m_entities[id];
        const GpuMesh* m = e.visible ? mesh(id) : nullptr;
        if (!m) continue;
        const mat4 world = worldMatrix(id, alpha);

        // ---- LOD 选择：把每一级的几何误差投影到屏幕上，选误差仍小于阈值的最粗一级 ----
        // 屏幕误差（像素）≈ 世界误差 / 距离 × 每弧度像素数。距离用到包围球表面的距离，近处物体保守地用细的 LOD。
        uint32_t lodIndex = 0;
        if (m->lods.size() > 1) {
            vec3 mn, mx;
            transformAabb(world, m->bounds.min, m->bounds.max, mn, mx);
            const vec3 center = (mn + mx) * 0.5f;
            const float radius = glm::length(mx - mn) * 0.5f;
            const float dist = std::max(glm::length(center - lod.cameraPos) - radius, 0.1f);
            const float scale = std::max({glm::length(vec3(world[0])), glm::length(vec3(world[1])), glm::length(vec3(world[2]))});
            for (uint32_t i = uint32_t(m->lods.size()) - 1; i > 0; --i) {
                const float px = m->lods[i].error * scale / dist * lod.pixelsPerRadian;
                if (px <= lod.thresholdPx) {
                    lodIndex = i;
                    break;
                }
            }
        }
        triangles += m->lods[lodIndex].indexCount / 3;

        DrawItem item;
        item.mesh = m;
        item.model = world;
        item.material = material(id);
        item.paramsOverride = e.materialOverride ? &*e.materialOverride : nullptr;
        item.lod = lodIndex;
        item.highlight = (highlighted && *highlighted == id) ? highlightStrength : 0.0f;
        out.opaque.push_back(item);
    }
    m_lastTriangles = triangles;
}

SceneDesc Scene::toDesc() const {
    SceneDesc d;
    for (const Entity& e : m_entities) {
        EntityDesc ed;
        ed.name = e.name;
        ed.position = e.current.position;
        ed.rotation = e.current.rotation;
        ed.scale = e.current.scale;
        ed.model = m_assets->guid(e.model).toString();
        ed.instance = e.instance;
        ed.visible = e.visible;
        ed.material = e.materialOverride;
        d.entities.push_back(std::move(ed));
    }
    return d;
}

void Scene::fromDesc(const SceneDesc& desc) {
    m_entities.clear();
    for (const EntityDesc& ed : desc.entities) {
        const auto guid = Guid::parse(ed.model);
        if (!guid) {
            LOG_WARN("实体 {} 的模型 GUID 无效（{}），已跳过", ed.name, ed.model);
            continue;
        }
        Entity e;
        e.name = ed.name;
        e.current = {ed.position, ed.rotation, ed.scale};
        // 找不到的资产也会得到一个句柄（状态 Failed），实体显示兜底模型 —— 场景结构不丢
        e.model = m_assets->loadModel(*guid);
        e.instance = ed.instance;
        e.visible = ed.visible;
        e.materialOverride = ed.material;
        add(std::move(e));
    }
}

} // namespace eng
