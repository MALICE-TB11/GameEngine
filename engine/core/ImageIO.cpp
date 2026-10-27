#include "core/ImageIO.h"

#include "core/Log.h"

// stb 是单头文件库：在且仅在一个 .cpp 里定义 IMPLEMENTATION 宏，生成函数实现
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#pragma warning(push, 0)
#include <stb_image.h>
#include <stb_image_write.h>
#pragma warning(pop)

#include <cstring>

namespace eng {

std::optional<ImageData> loadImageRGBA8(const std::filesystem::path& path) {
    int w = 0, h = 0, n = 0;
    stbi_uc* pixels = stbi_load(path.string().c_str(), &w, &h, &n, 4);
    if (!pixels) {
        LOG_WARN("读取图片失败 {}: {}", path.string(), stbi_failure_reason());
        return std::nullopt;
    }
    ImageData img{w, h, std::vector<uint8_t>(pixels, pixels + size_t(w) * h * 4)};
    stbi_image_free(pixels);
    return img;
}

std::optional<ImageData> decodeImageRGBA8(const uint8_t* bytes, size_t size) {
    int w = 0, h = 0, n = 0;
    stbi_uc* pixels = stbi_load_from_memory(bytes, static_cast<int>(size), &w, &h, &n, 4);
    if (!pixels) return std::nullopt;
    ImageData img{w, h, std::vector<uint8_t>(pixels, pixels + size_t(w) * h * 4)};
    stbi_image_free(pixels);
    return img;
}

bool writePngRGBA8(const std::filesystem::path& path, int width, int height, const uint8_t* rgba8) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    return stbi_write_png(path.string().c_str(), width, height, 4, rgba8, width * 4) != 0;
}

} // namespace eng
