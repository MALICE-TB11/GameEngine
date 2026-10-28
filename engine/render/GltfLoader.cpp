// glTF 2.0 加载（fastgltf）。
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

std::optional<Model> loadGltf(rhi::Device& device, const std::filesystem::path& path) {
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
    Model model;
    model.name = path.stem().string();

    // ---- 1. 贴图：先看每张图被哪种材质槽位使用，决定 sRGB 还是线性 ----
    // 颜色贴图（baseColor、emissive）存的是 sRGB 编码的值，采样时要让硬件转回线性；
    // 数据贴图（法线、粗糙度、AO）本身就是线性数值，不能做 sRGB 转换。
    std::vector<bool> isColor(gltf.textures.size(), false);
    for (const auto& m : gltf.materials) {
        if (m.pbrData.baseColorTexture) isColor[m.pbrData.baseColorTexture->textureIndex] = true;
        if (m.emissiveTexture) isColor[m.emissiveTexture->textureIndex] = true;
    }
    model.textures.resize(gltf.textures.size());
    std::vector<bool> textureOk(gltf.textures.size(), false);
    for (size_t t = 0; t < gltf.textures.size(); ++t) {
        const auto& tex = gltf.textures[t];
        if (!tex.imageIndex) continue;
        auto img = decodeGltfImage(gltf, gltf.images[*tex.imageIndex], path.parent_path());
        if (!img) {
            LOG_WARN("{}: 贴图 {} 解码失败，使用默认贴图", model.name, t);
            continue;
        }
        const std::string name = model.name + "_tex" + std::to_string(t);
        const VkFormat fmt = isColor[t] ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        model.textures[t] = rhi::createTexture2D(device, img->rgba8.data(), uint32_t(img->width), uint32_t(img->height), fmt, true, name.c_str());
        textureOk[t] = true;
    }
    auto tex = [&](size_t index) -> const rhi::Image* { return textureOk[index] ? &model.textures[index] : nullptr; };

    // ---- 2. 材质 ----
    for (const auto& m : gltf.materials) {
        Material mat;
        mat.name = std::string(m.name);
        const auto& pbr = m.pbrData;
        mat.params.baseColor = {pbr.baseColorFactor[0], pbr.baseColorFactor[1], pbr.baseColorFactor[2], pbr.baseColorFactor[3]};
        mat.params.pbr = {pbr.metallicFactor, pbr.roughnessFactor, 1.0f, m.alphaCutoff};
        mat.params.emissive = {m.emissiveFactor[0], m.emissiveFactor[1], m.emissiveFactor[2], 1.0f};
        // flags.z：alpha 裁剪开关。EXTEND: alphaMode = BLEND 目前按不透明绘制，需要透明 pass
        const float alphaMask = m.alphaMode == fastgltf::AlphaMode::Mask ? 1.0f : 0.0f;
        mat.params.flags = {float(ShadingModel::PBR), m.emissiveStrength, alphaMask, 0.0f};
        if (pbr.baseColorTexture) mat.baseColorTex = tex(pbr.baseColorTexture->textureIndex);
        if (pbr.metallicRoughnessTexture) mat.metalRoughTex = tex(pbr.metallicRoughnessTexture->textureIndex);
        if (m.normalTexture) {
            mat.normalTex = tex(m.normalTexture->textureIndex);
            mat.params.emissive.w = m.normalTexture->scale;
        }
        if (m.occlusionTexture) {
            mat.occlusionTex = tex(m.occlusionTexture->textureIndex);
            mat.params.pbr.z = m.occlusionTexture->strength;
        }
        if (m.emissiveTexture) mat.emissiveTex = tex(m.emissiveTexture->textureIndex);
        mat.doubleSided = m.doubleSided;
        model.materials.push_back(std::move(mat));
    }

    // ---- 3. 网格：每个 primitive 转成引擎的统一顶点格式 ----
    std::vector<std::vector<std::pair<uint32_t, uint32_t>>> meshPrims(gltf.meshes.size()); // (GpuMesh 下标, 材质)
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

            const std::string name = model.name + "_" + std::string(mesh.name) + "_" + std::to_string(pi);
            model.meshes.push_back(uploadMesh(device, md, name));
            const uint32_t material = prim.materialIndex ? uint32_t(*prim.materialIndex) : UINT32_MAX;
            meshPrims[mi].push_back({uint32_t(model.meshes.size() - 1), material});
        }
    }

    // ---- 4. 节点：展开成实例 ----
    const size_t sceneIndex = gltf.defaultScene ? *gltf.defaultScene : 0;
    if (sceneIndex < gltf.scenes.size()) {
        fastgltf::iterateSceneNodes(gltf, sceneIndex, fastgltf::math::fmat4x4(), [&](fastgltf::Node& node, const fastgltf::math::fmat4x4& m) {
            if (!node.meshIndex) return;
            const mat4 world = toGlm(m);
            const std::string nodeName = node.name.empty() ? "Node" : std::string(node.name);
            for (const auto& [meshIdx, material] : meshPrims[*node.meshIndex]) {
                model.instances.push_back({meshIdx, material, world, nodeName});
                const Aabb& b = model.meshes[meshIdx].bounds;
                for (int c = 0; c < 8; ++c) {
                    const vec3 corner{(c & 1) ? b.max.x : b.min.x, (c & 2) ? b.max.y : b.min.y, (c & 4) ? b.max.z : b.min.z};
                    model.bounds.expand(vec3(world * vec4(corner, 1.0f)));
                }
            }
        });
    }
    LOG_INFO("已加载模型 {}：{} 个网格，{} 个材质，{} 张贴图，{} 个实例", model.name, model.meshes.size(), model.materials.size(),
             model.textures.size(), model.instances.size());
    return model;
}

void destroyModel(rhi::Device& device, Model& model) {
    for (GpuMesh& m : model.meshes) destroyMesh(device, m);
    for (rhi::Image& t : model.textures) rhi::destroyImage(device, t);
    model = {};
}

} // namespace eng
