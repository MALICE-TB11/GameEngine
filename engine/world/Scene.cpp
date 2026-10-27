#include "world/Scene.h"

#include "core/Log.h"

namespace eng {

EntityId Scene::add(Entity e) {
    e.previous = e.current;
    m_entities.push_back(std::move(e));
    return static_cast<EntityId>(m_entities.size() - 1);
}

bool Scene::addModel(rhi::Device& device, const std::filesystem::path& path, const Transform& root) {
    auto model = loadGltf(device, path);
    if (!model) return false;
    auto owned = std::make_unique<Model>(std::move(*model));
    for (const Model::Instance& inst : owned->instances) {
        Entity e;
        e.name = owned->name;
        e.current = root;
        e.meshOffset = inst.transform;
        e.mesh = &owned->meshes[inst.mesh];
        e.material = inst.material < owned->materials.size() ? &owned->materials[inst.material] : nullptr;
        add(std::move(e));
    }
    m_models.push_back(std::move(owned));
    return true;
}

const GpuMesh* Scene::addMesh(rhi::Device& device, const MeshData& data, const std::string& name) {
    m_meshes.push_back(std::make_unique<GpuMesh>(uploadMesh(device, data, name)));
    return m_meshes.back().get();
}

Material* Scene::addMaterial(const Material& m) {
    m_materials.push_back(std::make_unique<Material>(m));
    return m_materials.back().get();
}

void Scene::beginFixedStep() {
    for (Entity& e : m_entities) e.previous = e.current;
}

void Scene::buildDrawList(DrawList& out, float alpha) const {
    out.opaque.clear();
    for (const Entity& e : m_entities) {
        if (!e.visible || !e.mesh) continue;
        const mat4 world = Transform::lerp(e.previous, e.current, alpha).matrix() * e.meshOffset;
        out.opaque.push_back({e.mesh, world, e.material});
    }
}

std::vector<Material*> Scene::materials() {
    std::vector<Material*> out;
    for (auto& m : m_materials) out.push_back(m.get());
    for (auto& model : m_models)
        for (Material& m : model->materials) out.push_back(&m);
    return out;
}

void Scene::destroy(rhi::Device& device) {
    for (auto& m : m_models) destroyModel(device, *m);
    for (auto& m : m_meshes) destroyMesh(device, *m);
    m_models.clear();
    m_meshes.clear();
    m_materials.clear();
    m_entities.clear();
}

} // namespace eng
