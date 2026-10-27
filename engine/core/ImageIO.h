#pragma once
// 图片读写（stb_image / stb_image_write 的薄封装）。

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace eng {

struct ImageData {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba8; // 每像素 4 字节
};

// 读取 PNG/JPG/TGA 等为 RGBA8
std::optional<ImageData> loadImageRGBA8(const std::filesystem::path& path);
// 从内存（例如 glb 内嵌贴图）解码为 RGBA8
std::optional<ImageData> decodeImageRGBA8(const uint8_t* bytes, size_t size);
bool writePngRGBA8(const std::filesystem::path& path, int width, int height, const uint8_t* rgba8);

} // namespace eng
