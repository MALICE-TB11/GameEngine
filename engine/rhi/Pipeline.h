#pragma once
// 管线（pipeline）创建辅助。管线 = shader + 固定功能状态（光栅化、深度、混合……）的不可变组合。
// Vulkan 要求提前把这些状态「烘焙」进管线对象，所以改 shader 就要重建管线——热重载就是在做这件事。

#include "rhi/VkCommon.h"

#include <cstdint>
#include <vector>

namespace eng::rhi {

enum class BlendMode { Opaque, Alpha, Additive };

struct GraphicsState {
    // dynamic rendering：管线只需要知道附件格式，不需要 VkRenderPass
    std::vector<VkFormat> colorFormats;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;

    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPolygonMode polygonMode = VK_POLYGON_MODE_FILL;
    VkCullModeFlags cullMode = VK_CULL_MODE_NONE;
    // 逆时针为正面（与 glTF / OpenGL 一致）。配合负高度视口，屏幕上看到的绕序不变。
    VkFrontFace frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    bool depthTest = false;
    bool depthWrite = false;
    // reverse-Z：近处深度大，所以「更近」= GREATER
    VkCompareOp depthCompare = VK_COMPARE_OP_GREATER_OR_EQUAL;

    BlendMode blend = BlendMode::Opaque;

    // 顶点输入；为空表示 shader 用 SV_VertexID 自己生成顶点（全屏三角形等）
    std::vector<VkVertexInputBindingDescription> vertexBindings;
    std::vector<VkVertexInputAttributeDescription> vertexAttributes;
};

VkShaderModule createShaderModule(VkDevice device, const std::vector<uint32_t>& spirv, const char* name);
// 失败返回 VK_NULL_HANDLE（而不是终止）：热重载时新 shader 可能有问题，要能退回旧管线
VkPipeline createGraphicsPipeline(VkDevice device, VkPipelineLayout layout, VkShaderModule vs, VkShaderModule ps,
                                  const GraphicsState& state, const char* name);
VkPipeline createComputePipeline(VkDevice device, VkPipelineLayout layout, VkShaderModule cs, const char* name);

} // namespace eng::rhi
