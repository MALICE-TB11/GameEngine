// glTF 2.0 加载（fastgltf）与模型上传。
// 支持：网格（位置/法线/UV/切线/索引）、金属-粗糙度 PBR 材质及其 5 种贴图、节点层级、.gltf 与 .glb。
// 不支持（EXTEND）：蒙皮、动画、morph target、KHR_texture_transform、多 UV 集。

#include "render/Model.h"

#include "core/ImageIO.h"
#include "core/Log.h"
#include "rhi/Device.h"
#include "rhi/Upload.h"

#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>

#include <cstring>
#include <variant>

namespace eng {
namespace {

mat4 toGlm(const fastgltf::math::fmat4x4& m) {
    mat4 r;
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) r[c][row] = m[c][row]; // 两者都是 [列][行]
    return r;
}

// 取出图片的原始字节（PNG/JPG），三种来源：glb 内嵌（bufferView）、外部文件、data URI
std::optional<ImageData> decodeGltfImage(const fastgltf::Asset& asset, const fastgltf::Image& image, const std::filesystem::path& dir) {
    std::optional<ImageData> out;
    std::visit(fastgltf::visitor{
                   [&](const fastgltf::sources::BufferView& view) {
                       const auto& bv = asset.bufferViews[view.bufferViewIndex];
                       const auto& buffer = asset.buffers[bv.bufferIndex];
                       std::visit(fastgltf::visitor{
                                      [&](const fastgltf::sources::Array& arr) {
                                          out = decodeImageRGBA8(reinterpret_cast<const uint8_t*>(arr.bytes.data()) + bv.byteOffset, bv.byteLength);
                                      },
                                      [&](const fastgltf::sources::Vector& vec) {
                                          out = decodeImageRGBA8(reinterpret_cast<const uint8_t*>(vec.bytes.data()) + bv.byteOffset, bv.byteLength);
                                      },
                                      [](auto&) {},
                                  },
                                  buffer.data);
                   },
                   [&](const fastgltf::sources::URI& uri) {
                       out = loadImageRGBA8(dir / std::filesystem::path(std::string(uri.uri.path())));
                   },
                   [&](const fastgltf::sources::Array& arr) {
                       out = decodeImageRGBA8(reinterpret_cast<const uint8_t*>(arr.bytes.data()), arr.bytes.size());
                   },
                   [&](const fastgltf::sources::Vector& vec) {
                       out = decodeImageRGBA8(reinterpret_cast<const uint8_t*>(vec.bytes.data()), vec.bytes.size());
                   },
                   [](auto&) {},
               },
               image.data);
    return out;
}

} // namespace

std::optional<ModelData> loadGltfData(const std::filesystem::path& path) {
    auto data = fastgltf::GltfDataBuffer::FromPath(path);
    if (data.error() != fastgltf::Error::None) {
        LOG_ERROR("读取 glTF 失败 {}: {}", path.string(), fastgltf::getErrorMessage(data.error()));
        return std::nullopt;
    }
    fastgltf::Parser parser;
    // LoadExternalBuffers：把 .bin 读进内存；GenerateMeshIndices：没有索引的网格自动生成 0..N-1
    const auto options = fastgltf::Options::LoadExternalBuffers | fastgltf::Options::GenerateMeshIndices;
    auto asset = parser.loadGltf(data.get(), path.parent_path(), options);
    if (asset.error() != fastgltf::Error::None) {
        LOG_ERROR("解析 glTF 失败 {}: {}", path.string(), fastgltf::getErrorMessage(asset.error()));
        return std::nullopt;
    }
    fastgltf::Asset& gltf = asset.get();
    ModelData model;
    model.name = path.stem().string();

    // ---- 1. 贴图用途：由引用它的材质槽位决定（决定 sRGB/线性，以及烘焙时用哪种压缩） ----
    // 颜色贴图（baseColor、emissive）存的是 sRGB 编码的值，采样时要让硬件转回线性；
    // 数据贴图（法线、粗糙度、AO）本身就是线性数值，不能做 sRGB 转换。
    std::vector<TextureUsage> usage(gltf.textures.size(), TextureUsage::Data);
    for (const auto& m : gltf.materials) {
        if (m.pbrData.baseColorTexture) usage[m.pbrData.baseColorTexture->textureIndex] = TextureUsage::Color;
        if (m.emissiveTexture) usage[m.emissiveTexture->textureIndex] = TextureUsage::Color;
        if (m.normalTexture) usage[m.normalTexture->textureIndex] = TextureUsage::Normal;
    }
    std::vector<int32_t> textureIndex(gltf.textures.size(), -1); // glTF 贴图 → ModelData::textures
    for (size_t t = 0; t < gltf.textures.size(); ++t) {
        const auto& tex = gltf.textures[t];
        if (!tex.imageIndex) continue;
        auto img = decodeGltfImage(gltf, gltf.images[*tex.imageIndex], path.parent_path());
        if (!img) {
            LOG_WARN("{}: 贴图 {} 解码失败，使用默认贴图", model.name, t);
            continue;
        }
        TextureData td;
        td.name = model.name + "_tex" + std::to_string(t);
        td.usage = usage[t];
        td.format = usage[t] == TextureUsage::Color ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        td.width = uint32_t(img->width);
        td.height = uint32_t(img->height);
        td.mips.push_back(std::move(img->rgba8));
        textureIndex[t] = int32_t(model.textures.size());
        model.textures.push_back(std::move(td));
    }

    // ---- 2. 材质 ----
    for (const auto& m : gltf.materials) {
        MaterialDesc mat;
        mat.name = std::string(m.name);
        const auto& pbr = m.pbrData;
        mat.params.baseColor = {pbr.baseColorFactor[0], pbr.baseColorFactor[1], pbr.baseColorFactor[2], pbr.baseColorFactor[3]};
        mat.params.pbr = {pbr.metallicFactor, pbr.roughnessFactor, 1.0f, m.alphaCutoff};
        mat.params.emissive = {m.emissiveFactor[0], m.emissiveFactor[1], m.emissiveFactor[2], 1.0f};
        // flags.z：alpha 裁剪开关。EXTEND: alphaMode = BLEND 目前按不透明绘制，需要透明 pass
        const float alphaMask = m.alphaMode == fastgltf::AlphaMode::Mask ? 1.0f : 0.0f;
        mat.params.flags = {float(ShadingModel::PBR), m.emissiveStrength, alphaMask, 0.0f};
        if (pbr.baseColorTexture) mat.textures[kSlotBaseColor] = textureIndex[pbr.baseColorTexture->textureIndex];
        if (pbr.metallicRoughnessTexture) mat.textures[kSlotMetalRough] = textureIndex[pbr.metallicRoughnessTexture->textureIndex];
        if (m.normalTexture) {
            mat.textures[kSlotNormal] = textureIndex[m.normalTexture->textureIndex];
            mat.params.emissive.w = m.normalTexture->scale;
        }
        if (m.occlusionTexture) {
            mat.textures[kSlotOcclusion] = textureIndex[m.occlusionTexture->textureIndex];
            mat.params.pbr.z = m.occlusionTexture->strength;
        }
        if (m.emissiveTexture) mat.textures[kSlotEmissive] = textureIndex[m.emissiveTexture->textureIndex];
        mat.doubleSided = m.doubleSided;
        model.materials.push_back(std::move(mat));
    }

    // ---- 3. 网格：每个 primitive 转成引擎的统一顶点格式 ----
    std::vector<std::vector<std::pair<uint32_t, uint32_t>>> meshPrims(gltf.meshes.size()); // (网格下标, 材质)
    for (size_t mi = 0; mi < gltf.meshes.size(); ++mi) {
        const auto& mesh = gltf.meshes[mi];
        for (size_t pi = 0; pi < mesh.primitives.size(); ++pi) {
            const auto& prim = mesh.primitives[pi];
            if (prim.type != fastgltf::PrimitiveType::Triangles) {
                LOG_WARN("{}: 跳过非三角形 primitive（{}#{}）", model.name, mesh.name, pi);
                continue;
            }
            auto posIt = prim.findAttribute("POSITION");
            if (posIt == prim.attributes.end()) continue;
            MeshData md;
            const auto& posAcc = gltf.accessors[posIt->accessorIndex];
            md.vertices.resize(posAcc.count);
            fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(gltf, posAcc, [&](fastgltf::math::fvec3 p, size_t i) {
                md.vertices[i].position = {p.x(), p.y(), p.z()};
                md.vertices[i].normal = {0, 1, 0};
                md.vertices[i].uv = {0, 0};
                md.vertices[i].tangent = {1, 0, 0, 1};
            });
            if (auto it = prim.findAttribute("NORMAL"); it != prim.attributes.end())
                fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(gltf, gltf.accessors[it->accessorIndex], [&](fastgltf::math::fvec3 n, size_t i) {
                    md.vertices[i].normal = {n.x(), n.y(), n.z()};
                });
            if (auto it = prim.findAttribute("TEXCOORD_0"); it != prim.attributes.end())
                fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec2>(gltf, gltf.accessors[it->accessorIndex], [&](fastgltf::math::fvec2 uv, size_t i) {
                    md.vertices[i].uv = {uv.x(), uv.y()};
                });
            md.indices.resize(gltf.accessors[*prim.indicesAccessor].count);
            fastgltf::copyFromAccessor<uint32_t>(gltf, gltf.accessors[*prim.indicesAccessor], md.indices.data());

            if (auto it = prim.findAttribute("TANGENT"); it != prim.attributes.end()) {
                fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec4>(gltf, gltf.accessors[it->accessorIndex], [&](fastgltf::math::fvec4 t, size_t i) {
                    md.vertices[i].tangent = {t.x(), t.y(), t.z(), t.w()};
                });
            } else {
                computeTangents(md); // 法线贴图需要切线；文件里没有就自己算
            }

            model.meshNames.push_back(model.name + "_" + std::string(mesh.name) + "_" + std::to_string(pi));
            model.meshes.push_back(std::move(md));
            const uint32_t material = prim.materialIndex ? uint32_t(*prim.materialIndex) : UINT32_MAX;
            meshPrims[mi].push_back({uint32_t(model.meshes.size() - 1), material});
        }
    }

    // ---- 4. 节点：展开成实例 ----
    const size_t sceneIndex = gltf.defaultScene ? *gltf.defaultScene : 0;
    if (sceneIndex < gltf.scenes.size()) {
        fastgltf::iterateSceneNodes(gltf, sceneIndex, fastgltf::math::fmat4x4(), [&](fastgltf::Node& node, const fastgltf::math::fmat4x4& m) {
            if (!node.meshIndex) return;
            const std::string nodeName = node.name.empty() ? "Node" : std::string(node.name);
            for (const auto& [meshIdx, material] : meshPrims[*node.meshIndex])
                model.instances.push_back({meshIdx, material, toGlm(m), nodeName});
        });
    }
    return model;
}

Model uploadModel(rhi::Device& device, const ModelData& data) {
    Model model;
    model.name = data.name;
    // 先上传贴图并把 vector 尺寸定死：Material 里存的是指向元素的指针
    model.textures.resize(data.textures.size());
    for (size_t t = 0; t < data.textures.size(); ++t) {
        const TextureData& td = data.textures[t];
        const bool rawRgba8 = td.mips.size() == 1 && (td.format == VK_FORMAT_R8G8B8A8_UNORM || td.format == VK_FORMAT_R8G8B8A8_SRGB);
        model.textures[t] = rawRgba8 ? rhi::createTexture2D(device, td.mips[0].data(), td.width, td.height, td.format, true, td.name.c_str())
                                     : rhi::createTextureFromMips(device, td.format, td.width, td.height, td.mips, td.name.c_str());
    }
    for (const MaterialDesc& md : data.materials) {
        Material m;
        m.name = md.name;
        m.params = md.params;
        m.doubleSided = md.doubleSided;
        const rhi::Image** slots[kMaterialSlotCount] = {&m.baseColorTex, &m.normalTex, &m.metalRoughTex, &m.occlusionTex, &m.emissiveTex};
        for (uint32_t s = 0; s < kMaterialSlotCount; ++s)
            if (md.textures[s] >= 0 && size_t(md.textures[s]) < model.textures.size()) *slots[s] = &model.textures[md.textures[s]];
        model.materials.push_back(std::move(m));
    }
    for (size_t i = 0; i < data.meshes.size(); ++i) {
        const MeshData& md = data.meshes[i];
        model.meshes.push_back(uploadMesh(device, md, i < data.meshNames.size() ? data.meshNames[i] : data.name));
        // 碰撞几何：只留位置和 LOD0 的索引（物理不需要法线/UV）
        CollisionMesh& c = model.collision.emplace_back();
        c.positions.reserve(md.vertices.size());
        for (const Vertex& v : md.vertices) c.positions.push_back(v.position);
        const uint32_t count = md.lods.empty() ? uint32_t(md.indices.size()) : md.lods[0].indexCount;
        const uint32_t first = md.lods.empty() ? 0u : md.lods[0].firstIndex;
        c.indices.assign(md.indices.begin() + first, md.indices.begin() + first + count);
    }
    model.instances = data.instances;
    for (const ModelInstance& inst : model.instances) {
        if (inst.mesh >= model.meshes.size()) continue;
        const Aabb& b = model.meshes[inst.mesh].bounds;
        for (int c = 0; c < 8; ++c) {
            const vec3 corner{(c & 1) ? b.max.x : b.min.x, (c & 2) ? b.max.y : b.min.y, (c & 4) ? b.max.z : b.min.z};
            model.bounds.expand(vec3(inst.transform * vec4(corner, 1.0f)));
        }
    }
    return model;
}

std::optional<Model> loadGltf(rhi::Device& device, const std::filesystem::path& path) {
    auto data = loadGltfData(path);
    if (!data) return std::nullopt;
    Model m = uploadModel(device, *data);
    LOG_INFO("已加载模型 {}：{} 个网格，{} 个材质，{} 张贴图，{} 个实例", m.name, m.meshes.size(), m.materials.size(),
             m.textures.size(), m.instances.size());
    return m;
}

void destroyModel(rhi::Device& device, Model& model) {
    for (GpuMesh& m : model.meshes) destroyMesh(device, m);
    for (rhi::Image& t : model.textures) rhi::destroyImage(device, t);
    model = {};
}

} // namespace eng
