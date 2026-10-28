#include "asset/CookedFormats.h"

#include "core/BinaryIO.h"

namespace eng::asset {
namespace {

constexpr uint32_t kMagicModel = 0x4C444D45;   // "EMDL"
constexpr uint32_t kMagicTexture = 0x58455445; // "ETEX"

void writeHeader(BinaryWriter& w, uint32_t magic) {
    w.write(magic);
    w.write(kCookerVersion);
}

bool readHeader(BinaryReader& r, uint32_t magic) {
    return r.read<uint32_t>() == magic && r.read<uint32_t>() == kCookerVersion && r.ok();
}

void writeTexture(BinaryWriter& w, const TextureData& t) {
    w.writeString(t.name);
    w.write(static_cast<uint32_t>(t.format));
    w.write(t.width);
    w.write(t.height);
    w.write(static_cast<uint8_t>(t.usage));
    w.write(static_cast<uint32_t>(t.mips.size()));
    for (const auto& m : t.mips) w.writeArray(m);
}

TextureData readTexture(BinaryReader& r) {
    TextureData t;
    t.name = r.readString();
    t.format = static_cast<VkFormat>(r.read<uint32_t>());
    t.width = r.read<uint32_t>();
    t.height = r.read<uint32_t>();
    t.usage = static_cast<TextureUsage>(r.read<uint8_t>());
    const uint32_t mips = r.read<uint32_t>();
    for (uint32_t i = 0; i < mips && r.ok(); ++i) t.mips.push_back(r.readArray<uint8_t>());
    return t;
}

} // namespace

std::vector<uint8_t> writeCookedModel(const ModelData& m) {
    BinaryWriter w;
    writeHeader(w, kMagicModel);
    w.writeString(m.name);

    w.write(static_cast<uint32_t>(m.meshes.size()));
    for (size_t i = 0; i < m.meshes.size(); ++i) {
        w.writeString(i < m.meshNames.size() ? m.meshNames[i] : m.name);
        w.writeArray(m.meshes[i].vertices);
        w.writeArray(m.meshes[i].indices);
        w.writeArray(m.meshes[i].lods);
    }

    w.write(static_cast<uint32_t>(m.materials.size()));
    for (const MaterialDesc& mat : m.materials) {
        w.writeString(mat.name);
        w.write(mat.params);
        w.write(static_cast<uint8_t>(mat.doubleSided));
        w.write(mat.textures);
    }

    w.write(static_cast<uint32_t>(m.textures.size()));
    for (const TextureData& t : m.textures) writeTexture(w, t);

    w.write(static_cast<uint32_t>(m.instances.size()));
    for (const ModelInstance& inst : m.instances) {
        w.write(inst.mesh);
        w.write(inst.material);
        w.write(inst.transform);
        w.writeString(inst.name);
    }
    return std::move(w.data());
}

std::optional<ModelData> readCookedModel(const std::vector<uint8_t>& bytes) {
    BinaryReader r(bytes);
    if (!readHeader(r, kMagicModel)) return std::nullopt;
    ModelData m;
    m.name = r.readString();

    const uint32_t meshCount = r.read<uint32_t>();
    for (uint32_t i = 0; i < meshCount && r.ok(); ++i) {
        m.meshNames.push_back(r.readString());
        MeshData md;
        md.vertices = r.readArray<Vertex>();
        md.indices = r.readArray<uint32_t>();
        md.lods = r.readArray<MeshLod>();
        m.meshes.push_back(std::move(md));
    }

    const uint32_t matCount = r.read<uint32_t>();
    for (uint32_t i = 0; i < matCount && r.ok(); ++i) {
        MaterialDesc mat;
        mat.name = r.readString();
        mat.params = r.read<MaterialParams>();
        mat.doubleSided = r.read<uint8_t>() != 0;
        mat.textures = r.read<std::array<int32_t, kMaterialSlotCount>>();
        m.materials.push_back(std::move(mat));
    }

    const uint32_t texCount = r.read<uint32_t>();
    for (uint32_t i = 0; i < texCount && r.ok(); ++i) m.textures.push_back(readTexture(r));

    const uint32_t instCount = r.read<uint32_t>();
    for (uint32_t i = 0; i < instCount && r.ok(); ++i) {
        ModelInstance inst;
        inst.mesh = r.read<uint32_t>();
        inst.material = r.read<uint32_t>();
        inst.transform = r.read<mat4>();
        inst.name = r.readString();
        m.instances.push_back(std::move(inst));
    }
    // 读到一半出错或有多余字节，都说明文件坏了或格式不匹配
    if (!r.ok() || !r.atEnd()) return std::nullopt;
    return m;
}

std::vector<uint8_t> writeCookedTexture(const TextureData& t) {
    BinaryWriter w;
    writeHeader(w, kMagicTexture);
    writeTexture(w, t);
    return std::move(w.data());
}

std::optional<TextureData> readCookedTexture(const std::vector<uint8_t>& bytes) {
    BinaryReader r(bytes);
    if (!readHeader(r, kMagicTexture)) return std::nullopt;
    TextureData t = readTexture(r);
    if (!r.ok() || !r.atEnd()) return std::nullopt;
    return t;
}

bool cookedHeaderIsCurrent(const std::vector<uint8_t>& firstBytes) {
    if (firstBytes.size() < 8) return false;
    BinaryReader r(firstBytes.data(), 8);
    const uint32_t magic = r.read<uint32_t>();
    return (magic == kMagicModel || magic == kMagicTexture) && r.read<uint32_t>() == kCookerVersion;
}

} // namespace eng::asset
