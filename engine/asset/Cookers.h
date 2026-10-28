#pragma once
// 资源烘焙器：把「导入的原始数据」变成「运行时最省事的数据」。都是纯 CPU 计算，可以在 Job 线程里跑。
//
// 贴图：生成完整 mip 链 → BC 压缩（颜色 BC7 sRGB、法线 BC5、数据 BC7 线性）。
//       显存约为 RGBA8 的 1/4（BC7/BC5 每像素 1 字节），采样带宽同样降低。
// 网格：去重顶点 → 优化顶点缓存命中 → 减少过绘制 → 生成 LOD 链 → 按使用顺序重排顶点。

#include "render/Model.h"

#include <string>

namespace eng {
class JobSystem;
}

namespace eng::asset {

struct TextureCookSettings {
    bool generateMips = true;
    bool compress = true;
};

struct ModelCookSettings {
    bool optimize = true;
    uint32_t maxLods = 4;          // 包括 LOD0
    float lodReduction = 0.5f;     // 每一级三角形数约为上一级的一半
    float lodMaxError = 0.02f;     // 简化允许的最大误差（相对网格尺寸的比例）
    bool compressTextures = true;
};

// 把单级 RGBA8 贴图原地转换为 mip 链（+ BC 压缩）。jobs 可为空（单线程执行）
bool cookTexture(TextureData& texture, const TextureCookSettings& settings, JobSystem* jobs, std::string& error);

// 网格优化 + LOD 生成（原地修改）。单独公开是为了单元测试
void optimizeMesh(MeshData& mesh, const ModelCookSettings& settings);

// 整个模型：所有网格 + 所有贴图
bool cookModel(ModelData& model, const ModelCookSettings& settings, JobSystem* jobs, std::string& error);

// BC 格式每 4×4 块的字节数；不是块压缩格式返回 0
uint32_t bcBlockBytes(VkFormat format);

} // namespace eng::asset
