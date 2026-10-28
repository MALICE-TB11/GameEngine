// 资源烘焙测试：纯 CPU，不需要 GPU。
#include "asset/CookedFormats.h"
#include "asset/Cookers.h"

#include <bc7decomp.h>
#include <doctest/doctest.h>

#include <cmath>

using namespace eng;
using namespace eng::asset;

namespace {

TextureData gradientTexture(uint32_t size, TextureUsage usage) {
    TextureData t;
    t.name = "test";
    t.width = t.height = size;
    t.usage = usage;
    t.format = usage == TextureUsage::Color ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    std::vector<uint8_t> px(size_t(size) * size * 4);
    for (uint32_t y = 0; y < size; ++y)
        for (uint32_t x = 0; x < size; ++x) {
            uint8_t* p = &px[(size_t(y) * size + x) * 4];
            p[0] = uint8_t(x * 255 / (size - 1));
            p[1] = uint8_t(y * 255 / (size - 1));
            p[2] = uint8_t((x + y) * 127 / (size - 1));
            p[3] = 255;
        }
    t.mips.push_back(std::move(px));
    return t;
}

} // namespace

TEST_CASE("贴图烘焙：生成完整 mip 链，BC7 压缩后质量足够（PSNR > 35 dB）") {
    TextureData t = gradientTexture(64, TextureUsage::Data);
    const std::vector<uint8_t> original = t.mips[0];
    std::string error;
    REQUIRE(cookTexture(t, {}, nullptr, error));
    CHECK(t.format == VK_FORMAT_BC7_UNORM_BLOCK);
    CHECK(t.mips.size() == 7); // 64,32,16,8,4,2,1
    CHECK(t.mips[0].size() == 16 * 16 * 16); // 16×16 个块，每块 16 字节
    CHECK(t.mips[6].size() == 16);           // 1×1 也占一个完整块

    // 解压第 0 级，与原图比较
    double mse = 0.0;
    for (uint32_t by = 0; by < 16; ++by)
        for (uint32_t bx = 0; bx < 16; ++bx) {
            bc7decomp::color_rgba block[16];
            REQUIRE(bc7decomp::unpack_bc7(&t.mips[0][(by * 16 + bx) * 16], block));
            for (uint32_t y = 0; y < 4; ++y)
                for (uint32_t x = 0; x < 4; ++x)
                    for (int c = 0; c < 3; ++c) {
                        const double d = double(block[y * 4 + x].m_comps[c]) - double(original[((by * 4 + y) * 64 + bx * 4 + x) * 4 + c]);
                        mse += d * d;
                    }
        }
    mse /= 64.0 * 64.0 * 3.0;
    const double psnr = 10.0 * std::log10(255.0 * 255.0 / std::max(mse, 1e-9));
    CHECK(psnr > 35.0);
}

TEST_CASE("贴图烘焙：颜色贴图用 BC7 sRGB，法线贴图用 BC5，不压缩时保留 RGBA8 mip") {
    std::string error;
    TextureData color = gradientTexture(16, TextureUsage::Color);
    REQUIRE(cookTexture(color, {}, nullptr, error));
    CHECK(color.format == VK_FORMAT_BC7_SRGB_BLOCK);

    TextureData normal = gradientTexture(16, TextureUsage::Normal);
    REQUIRE(cookTexture(normal, {}, nullptr, error));
    CHECK(normal.format == VK_FORMAT_BC5_UNORM_BLOCK);

    TextureData raw = gradientTexture(16, TextureUsage::Data);
    TextureCookSettings s;
    s.compress = false;
    REQUIRE(cookTexture(raw, s, nullptr, error));
    CHECK(raw.format == VK_FORMAT_R8G8B8A8_UNORM);
    CHECK(raw.mips.size() == 5);
    CHECK(raw.mips[4].size() == 4);
}

TEST_CASE("网格烘焙：LOD 链三角形数递减、误差递增，所有索引都有效") {
    MeshData sphere = makeUvSphere(1.0f, 64, 32);
    const size_t originalTris = sphere.indices.size() / 3;
    optimizeMesh(sphere, {});
    REQUIRE(sphere.lods.size() >= 2);
    CHECK(sphere.lods[0].indexCount / 3 == originalTris);
    CHECK(sphere.lods[0].error == 0.0f);
    for (size_t i = 1; i < sphere.lods.size(); ++i) {
        CHECK(sphere.lods[i].indexCount < sphere.lods[i - 1].indexCount);
        CHECK(sphere.lods[i].error >= sphere.lods[i - 1].error);
    }
    bool indicesValid = true;
    for (uint32_t idx : sphere.indices) indicesValid &= idx < sphere.vertices.size();
    CHECK(indicesValid);
}

TEST_CASE("网格烘焙：三角形很少的网格只保留 LOD0") {
    MeshData cube = makeCube(0.5f);
    optimizeMesh(cube, {});
    CHECK(cube.lods.size() == 1);
    CHECK(cube.lods[0].indexCount == 36);
}

TEST_CASE("烘焙格式：模型往返一致，损坏的数据返回空") {
    ModelData m;
    m.name = "test";
    m.meshNames.push_back("cube");
    m.meshes.push_back(makeCube(0.5f));
    optimizeMesh(m.meshes[0], {});
    MaterialDesc mat;
    mat.name = "mat";
    mat.params.baseColor = {0.1f, 0.2f, 0.3f, 1.0f};
    mat.textures[kSlotNormal] = 0;
    m.materials.push_back(mat);
    m.textures.push_back(gradientTexture(8, TextureUsage::Normal));
    m.instances.push_back({0, 0, glm::translate(mat4(1.0f), vec3(1, 2, 3)), "node"});

    const auto bytes = writeCookedModel(m);
    const auto back = readCookedModel(bytes);
    REQUIRE(back.has_value());
    CHECK(back->name == "test");
    CHECK(back->meshes[0].vertices.size() == m.meshes[0].vertices.size());
    CHECK(back->meshes[0].lods.size() == m.meshes[0].lods.size());
    CHECK(back->materials[0].params.baseColor.z == doctest::Approx(0.3f));
    CHECK(back->materials[0].textures[kSlotNormal] == 0);
    CHECK(back->textures[0].mips[0] == m.textures[0].mips[0]);
    CHECK(back->instances[0].transform[3].y == doctest::Approx(2.0f));
    CHECK(back->instances[0].name == "node");

    std::vector<uint8_t> broken(bytes.begin(), bytes.begin() + bytes.size() / 2);
    CHECK_FALSE(readCookedModel(broken).has_value());
    CHECK(cookedHeaderIsCurrent(bytes));
}
