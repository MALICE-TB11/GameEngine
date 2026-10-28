#pragma once
// 烘焙产物的二进制格式（引擎私有，放在 build/cooked/，可随时删掉重新生成）。
//
//   .emodel —— 一个模型的全部运行时数据：优化后的网格（含 LOD 链）、材质、压缩好的贴图、实例
//   .etex   —— 单张贴图（地形、天气等独立贴图用，M4 起）
//
// 为什么不用 glTF / PNG 直接运行：glTF 里的网格没有优化、没有 LOD，PNG 要在加载时解码、生成 mip，
// 而且不能是 BC 压缩格式。烘焙一次后，运行时只需要「读文件 → 上传」。
// 为什么不用 KTX2：KTX2 是更好的通用交换格式，但需要额外的库和格式描述块；
// 引擎私有缓存用一个简单格式更容易读懂和调试。EXTEND: 需要和外部工具交换贴图时再加 KTX2。
//
// 文件头带「烘焙器版本」：改了烘焙算法就把版本号加一，旧缓存自动失效重新烘焙。

#include "render/Model.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace eng::asset {

// 烘焙算法或格式有任何改动都要加一（所有缓存会自动重新生成）
inline constexpr uint32_t kCookerVersion = 1;

std::vector<uint8_t> writeCookedModel(const ModelData& model);
std::optional<ModelData> readCookedModel(const std::vector<uint8_t>& bytes);

std::vector<uint8_t> writeCookedTexture(const TextureData& texture);
std::optional<TextureData> readCookedTexture(const std::vector<uint8_t>& bytes);

// 只读文件头，判断缓存是否由当前版本的烘焙器生成（不用读整个文件）
bool cookedHeaderIsCurrent(const std::vector<uint8_t>& firstBytes);

} // namespace eng::asset
