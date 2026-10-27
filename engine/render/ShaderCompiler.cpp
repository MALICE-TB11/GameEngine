#include "render/ShaderCompiler.h"

#include "core/FileSystem.h"
#include "core/Log.h"
#include "core/Process.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <regex>
#include <set>

namespace eng {
namespace {

const char* stageSuffix(ShaderStage s) {
    switch (s) {
    case ShaderStage::Vertex: return "vs";
    case ShaderStage::Pixel: return "ps";
    default: return "cs";
    }
}
const char* stageEntry(ShaderStage s) {
    switch (s) {
    case ShaderStage::Vertex: return "VSMain";
    case ShaderStage::Pixel: return "PSMain";
    default: return "CSMain";
    }
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::optional<std::vector<uint32_t>> readSpirv(const std::filesystem::path& path) {
    auto bytes = fs::readBinary(path);
    if (!bytes || bytes->size() < 4 || bytes->size() % 4 != 0) return std::nullopt;
    std::vector<uint32_t> words(bytes->size() / 4);
    std::memcpy(words.data(), bytes->data(), bytes->size());
    // SPIR-V 魔数 0x07230203：文件被截断或不是 SPIR-V 时及早发现
    if (words[0] != 0x07230203u) return std::nullopt;
    return words;
}

void collectIncludes(const std::filesystem::path& file, std::set<std::filesystem::path>& out) {
    if (!out.insert(file).second) return; // 已经处理过（也防止循环 include）
    auto text = fs::readText(file);
    if (!text) return;
    static const std::regex kInclude(R"(#\s*include\s*"([^"]+)\")");
    for (std::sregex_iterator it(text->begin(), text->end(), kInclude), end; it != end; ++it) {
        const std::string inc = (*it)[1].str();
        // 与 dxc 的查找顺序一致：先相对当前文件，再相对 shaders/ 根目录（-I）
        std::filesystem::path candidate = file.parent_path() / inc;
        if (!std::filesystem::exists(candidate)) candidate = fs::resolve("shaders") / inc;
        collectIncludes(candidate.lexically_normal(), out);
    }
}

} // namespace

std::filesystem::path ShaderCompiler::sourcePath(const std::string& name) {
    return fs::resolve("shaders") / (name + ".hlsl");
}

std::filesystem::path ShaderCompiler::binaryPath(const std::string& name, ShaderStage stage) {
    return fs::shaderBinaryDir() / (name + "." + stageSuffix(stage) + ".spv");
}

std::vector<std::filesystem::path> ShaderCompiler::dependencies(const std::string& name) const {
    std::set<std::filesystem::path> files;
    collectIncludes(sourcePath(name).lexically_normal(), files);
    return {files.begin(), files.end()};
}

std::filesystem::file_time_type ShaderCompiler::newestDependencyTime(const std::string& name) const {
    auto newest = std::filesystem::file_time_type::min();
    for (const auto& f : dependencies(name)) newest = std::max(newest, fs::lastWriteTime(f));
    return newest;
}

std::optional<std::vector<uint32_t>> ShaderCompiler::load(const std::string& name, ShaderStage stage, std::string& error) {
    const auto bin = binaryPath(name, stage);
    const auto binTime = fs::lastWriteTime(bin);
    if (binTime != std::filesystem::file_time_type::min() && binTime >= newestDependencyTime(name)) {
        if (auto spv = readSpirv(bin)) return spv;
    }
    LOG_INFO("shader {} ({}) 的 .spv 缺失或过期，运行时编译", name, stageSuffix(stage));
    return compile(name, stage, error);
}

std::optional<std::vector<uint32_t>> ShaderCompiler::compile(const std::string& name, ShaderStage stage, std::string& error) {
    const auto src = sourcePath(name);
    if (!std::filesystem::exists(src)) {
        error = "找不到 shader 源文件: " + src.string();
        return std::nullopt;
    }
    // 输出到临时文件而不是直接覆盖 build/shaders：编译失败时不破坏已有的 .spv
    const auto out = fs::shaderBinaryDir() / "hot" / (name + "." + stageSuffix(stage) + ".spv");
    std::error_code ec;
    std::filesystem::create_directories(out.parent_path(), ec);
    std::filesystem::remove(out, ec);

    // 编译参数来自 CMake（ENGINE_DXC_FLAGS），保证与构建时完全一致
    std::string cmd = std::string("\"") + ENGINE_DXC_EXECUTABLE + "\" " + ENGINE_DXC_FLAGS + " -T " + stageSuffix(stage) + "_" +
                      ENGINE_SHADER_MODEL + " -E " + stageEntry(stage) + " -I \"" + fs::resolve("shaders").string() +
                      "\" -Fo \"" + out.string() + "\" \"" + src.string() + "\"";
    std::string output;
    const int code = runProcess(widen(cmd), output);
    if (code != 0) {
        error = output.empty() ? ("dxc 启动失败或返回 " + std::to_string(code)) : output;
        if (output.find("0x80070459") != std::string::npos)
            error += "\n提示：shader 含中文但不是「UTF-8 带 BOM」编码。请以 UTF-8 with BOM 保存，"
                     "或运行 scripts/fix_shader_encoding.ps1。";
        return std::nullopt;
    }
    auto spv = readSpirv(out);
    if (!spv) {
        error = "dxc 成功但读取输出失败: " + out.string();
        return std::nullopt;
    }
    return spv;
}

} // namespace eng
