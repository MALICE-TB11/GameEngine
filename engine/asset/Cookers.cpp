#include "asset/Cookers.h"

#include "core/JobSystem.h"
#include "core/Log.h"

#include <bc7enc.h>
#include <meshoptimizer.h>
#include <rgbcx.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

namespace eng::asset {
namespace {

// ---- 颜色空间：sRGB 贴图必须在线性空间里求平均，否则 mip 会整体偏暗 ----
float srgbToLinear(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}
float linearToSrgb(float c) {
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

struct FloatImage {
    uint32_t w = 0, h = 0;
    std::vector<vec4> px;
    vec4& at(uint32_t x, uint32_t y) { return px[size_t(y) * w + x]; }
    const vec4& at(uint32_t x, uint32_t y) const { return px[size_t(y) * w + x]; }
};

FloatImage toFloat(const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h, TextureUsage usage) {
    FloatImage img{w, h, std::vector<vec4>(size_t(w) * h)};
    for (size_t i = 0; i < img.px.size(); ++i) {
        vec4 c(rgba[i * 4] / 255.0f, rgba[i * 4 + 1] / 255.0f, rgba[i * 4 + 2] / 255.0f, rgba[i * 4 + 3] / 255.0f);
        if (usage == TextureUsage::Color) c = vec4(srgbToLinear(c.r), srgbToLinear(c.g), srgbToLinear(c.b), c.a);
        if (usage == TextureUsage::Normal) c = vec4(vec3(c) * 2.0f - 1.0f, c.a); // 法线在 [-1,1] 空间求平均
        img.px[i] = c;
    }
    return img;
}

std::vector<uint8_t> toBytes(const FloatImage& img, TextureUsage usage) {
    std::vector<uint8_t> out(img.px.size() * 4);
    for (size_t i = 0; i < img.px.size(); ++i) {
        vec4 c = img.px[i];
        if (usage == TextureUsage::Color) c = vec4(linearToSrgb(c.r), linearToSrgb(c.g), linearToSrgb(c.b), c.a);
        if (usage == TextureUsage::Normal) {
            // 平均后的法线变短了（越粗糙的表面越短），重新归一化再编码
            const vec3 n = glm::length(vec3(c)) > 1e-6f ? glm::normalize(vec3(c)) : vec3(0, 0, 1);
            c = vec4(n * 0.5f + 0.5f, c.a);
        }
        for (int k = 0; k < 4; ++k) out[i * 4 + k] = uint8_t(std::clamp(c[k], 0.0f, 1.0f) * 255.0f + 0.5f);
    }
    return out;
}

// 2×2 盒式滤波缩小一半；奇数尺寸时边缘像素重复使用
FloatImage downsample(const FloatImage& src) {
    FloatImage dst{std::max(1u, src.w / 2), std::max(1u, src.h / 2), {}};
    dst.px.resize(size_t(dst.w) * dst.h);
    for (uint32_t y = 0; y < dst.h; ++y)
        for (uint32_t x = 0; x < dst.w; ++x) {
            const uint32_t x0 = std::min(x * 2, src.w - 1), x1 = std::min(x * 2 + 1, src.w - 1);
            const uint32_t y0 = std::min(y * 2, src.h - 1), y1 = std::min(y * 2 + 1, src.h - 1);
            dst.at(x, y) = (src.at(x0, y0) + src.at(x1, y0) + src.at(x0, y1) + src.at(x1, y1)) * 0.25f;
        }
    return dst;
}

void initEncodersOnce() {
    static std::once_flag once;
    std::call_once(once, [] {
        bc7enc_compress_block_init();
        rgbcx::init();
    });
}

// 把一级 RGBA8 图像压成 BC 块。按块行并行（每行块互不依赖）
std::vector<uint8_t> compressLevel(const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h, VkFormat format, JobSystem* jobs) {
    const uint32_t bw = std::max(1u, (w + 3) / 4), bh = std::max(1u, (h + 3) / 4);
    const uint32_t blockBytes = bcBlockBytes(format);
    std::vector<uint8_t> out(size_t(bw) * bh * blockBytes);

    bc7enc_compress_block_params params;
    bc7enc_compress_block_params_init(&params);
    params.m_uber_level = 0;       // 速度优先：0 已经明显优于 BC1/BC3，最高 4 会慢一个数量级
    params.m_max_partitions = 16;
    if (format == VK_FORMAT_BC7_UNORM_BLOCK) bc7enc_compress_block_params_init_linear_weights(&params); // 数据贴图按 RGB 等权
    else bc7enc_compress_block_params_init_perceptual_weights(&params);                                // 颜色按人眼敏感度

    auto doRows = [&](uint32_t rowBegin, uint32_t rowEnd) {
        uint8_t block[16 * 4];
        for (uint32_t by = rowBegin; by < rowEnd; ++by)
            for (uint32_t bx = 0; bx < bw; ++bx) {
                // 取 4×4 像素；超出图像的部分复制边缘（小于 4×4 的 mip 也能编码）
                for (uint32_t y = 0; y < 4; ++y)
                    for (uint32_t x = 0; x < 4; ++x) {
                        const uint32_t sx = std::min(bx * 4 + x, w - 1), sy = std::min(by * 4 + y, h - 1);
                        std::memcpy(&block[(y * 4 + x) * 4], &rgba[(size_t(sy) * w + sx) * 4], 4);
                    }
                uint8_t* dst = &out[(size_t(by) * bw + bx) * blockBytes];
                if (format == VK_FORMAT_BC5_UNORM_BLOCK)
                    rgbcx::encode_bc5(dst, block, 0, 1, 4); // 只编码 R、G 两个通道（法线 XY）
                else
                    bc7enc_compress_block(dst, block, &params);
            }
    };
    if (jobs && bh > 4)
        jobs->parallelFor(bh, 4, doRows);
    else
        doRows(0, bh);
    return out;
}

} // namespace

uint32_t bcBlockBytes(VkFormat f) {
    switch (f) {
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
    case VK_FORMAT_BC4_UNORM_BLOCK: return 8;
    case VK_FORMAT_BC5_UNORM_BLOCK:
    case VK_FORMAT_BC7_UNORM_BLOCK:
    case VK_FORMAT_BC7_SRGB_BLOCK: return 16;
    default: return 0;
    }
}

bool cookTexture(TextureData& tex, const TextureCookSettings& s, JobSystem* jobs, std::string& error) {
    const bool rgba8 = tex.format == VK_FORMAT_R8G8B8A8_UNORM || tex.format == VK_FORMAT_R8G8B8A8_SRGB;
    if (!rgba8 || tex.mips.size() != 1 || tex.mips[0].size() != size_t(tex.width) * tex.height * 4) {
        error = "贴图 " + tex.name + " 不是单级 RGBA8，无法烘焙";
        return false;
    }
    initEncodersOnce();

    // 1. 生成 mip 链（在浮点线性空间里做）
    std::vector<std::vector<uint8_t>> levels;
    std::vector<std::pair<uint32_t, uint32_t>> sizes;
    FloatImage cur = toFloat(tex.mips[0], tex.width, tex.height, tex.usage);
    levels.push_back(std::move(tex.mips[0]));
    sizes.push_back({tex.width, tex.height});
    while (s.generateMips && (cur.w > 1 || cur.h > 1)) {
        cur = downsample(cur);
        levels.push_back(toBytes(cur, tex.usage));
        sizes.push_back({cur.w, cur.h});
    }

    // 2. 压缩
    if (s.compress) {
        const VkFormat bc = tex.usage == TextureUsage::Color    ? VK_FORMAT_BC7_SRGB_BLOCK
                          : tex.usage == TextureUsage::Normal ? VK_FORMAT_BC5_UNORM_BLOCK
                                                              : VK_FORMAT_BC7_UNORM_BLOCK;
        for (size_t i = 0; i < levels.size(); ++i) levels[i] = compressLevel(levels[i], sizes[i].first, sizes[i].second, bc, jobs);
        tex.format = bc;
    }
    tex.mips = std::move(levels);
    return true;
}

void optimizeMesh(MeshData& mesh, const ModelCookSettings& s) {
    if (mesh.indices.empty() || mesh.vertices.empty()) return;
    const size_t vsize = sizeof(Vertex);

    // 1. 去重：完全相同的顶点合并（glTF 导出器经常产生重复顶点）
    std::vector<uint32_t> remap(mesh.vertices.size());
    const size_t unique = meshopt_generateVertexRemap(remap.data(), mesh.indices.data(), mesh.indices.size(), mesh.vertices.data(),
                                                      mesh.vertices.size(), vsize);
    std::vector<Vertex> vertices(unique);
    meshopt_remapVertexBuffer(vertices.data(), mesh.vertices.data(), mesh.vertices.size(), vsize, remap.data());
    std::vector<uint32_t> lod0(mesh.indices.size());
    meshopt_remapIndexBuffer(lod0.data(), mesh.indices.data(), mesh.indices.size(), remap.data());

    // 2. 顶点缓存：让相邻三角形尽量复用刚变换过的顶点，减少顶点着色器调用
    meshopt_optimizeVertexCache(lod0.data(), lod0.data(), lod0.size(), vertices.size());
    // 3. 过绘制：在不明显破坏缓存命中的前提下（阈值 1.05），让外侧三角形先画，深度测试更早剔除
    meshopt_optimizeOverdraw(lod0.data(), lod0.data(), lod0.size(), &vertices[0].position.x, vertices.size(), vsize, 1.05f);

    // 4. LOD 链：每一级都从 LOD0 简化（比逐级简化误差更小）
    const float scale = meshopt_simplifyScale(&vertices[0].position.x, vertices.size(), vsize);
    std::vector<uint32_t> all = lod0;
    std::vector<MeshLod> lods{{0, uint32_t(lod0.size()), 0.0f}};
    size_t target = lod0.size();
    for (uint32_t level = 1; level < s.maxLods; ++level) {
        target = size_t(double(target) * s.lodReduction) / 3 * 3;
        if (target < 36) break; // 不到 12 个三角形就没必要再简化了
        std::vector<uint32_t> lod(lod0.size());
        float resultError = 0.0f;
        const size_t count = meshopt_simplify(lod.data(), lod0.data(), lod0.size(), &vertices[0].position.x, vertices.size(), vsize,
                                              target, s.lodMaxError, 0, &resultError);
        // 简化不下去（受误差上限约束）：再生成一级也只是重复，停止
        if (count == 0 || count >= size_t(lods.back().indexCount * 0.95)) break;
        lod.resize(count);
        meshopt_optimizeVertexCache(lod.data(), lod.data(), lod.size(), vertices.size());
        lods.push_back({uint32_t(all.size()), uint32_t(count), resultError * scale});
        all.insert(all.end(), lod.begin(), lod.end());
    }

    // 5. 按「第一次被使用」的顺序重排顶点：GPU 取顶点时访存更连续
    const size_t used = meshopt_optimizeVertexFetch(vertices.data(), all.data(), all.size(), vertices.data(), vertices.size(), vsize);
    vertices.resize(used);

    mesh.vertices = std::move(vertices);
    mesh.indices = std::move(all);
    mesh.lods = std::move(lods);
}

bool cookModel(ModelData& model, const ModelCookSettings& s, JobSystem* jobs, std::string& error) {
    if (s.optimize) {
        if (jobs) {
            jobs->parallelFor(uint32_t(model.meshes.size()), 1, [&](uint32_t b, uint32_t e) {
                for (uint32_t i = b; i < e; ++i) optimizeMesh(model.meshes[i], s);
            });
        } else {
            for (MeshData& m : model.meshes) optimizeMesh(m, s);
        }
    }
    TextureCookSettings ts;
    ts.compress = s.compressTextures;
    for (TextureData& t : model.textures)
        if (!cookTexture(t, ts, jobs, error)) return false;
    return true;
}

} // namespace eng::asset
