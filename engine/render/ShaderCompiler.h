#pragma once
// Shader 加载与运行时编译。
//
// 命名约定（与 cmake/Shaders.cmake 一致）：
//   shader 名 "passes/composite" → 源码 shaders/passes/composite.hlsl
//   入口 VSMain/PSMain/CSMain    → 构建产物 build/shaders/passes/composite.{vs,ps,cs}.spv
//
// 加载策略：
//   1. 源码（及其 include 的文件）比 .spv 新 → 运行时调用 dxc 重新编译（你改了 shader 但没重新构建）
//   2. 否则直接读构建好的 .spv
// 编译失败返回 nullopt + 错误信息，调用方决定是保留旧管线还是用错误材质。

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace eng {

enum class ShaderStage { Vertex, Pixel, Compute };

class ShaderCompiler {
public:
    std::optional<std::vector<uint32_t>> load(const std::string& name, ShaderStage stage, std::string& error);
    // 强制用 dxc 从源码编译（热重载时调用）
    std::optional<std::vector<uint32_t>> compile(const std::string& name, ShaderStage stage, std::string& error);

    // 源文件及其递归 include 的所有文件（热重载据此判断「是否需要重编」）
    std::vector<std::filesystem::path> dependencies(const std::string& name) const;
    std::filesystem::file_time_type newestDependencyTime(const std::string& name) const;

    static std::filesystem::path sourcePath(const std::string& name);
    static std::filesystem::path binaryPath(const std::string& name, ShaderStage stage);
};

} // namespace eng
