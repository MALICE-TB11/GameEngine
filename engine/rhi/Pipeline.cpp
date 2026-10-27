#include "rhi/Pipeline.h"

#include "core/Log.h"

namespace eng::rhi {

VkShaderModule createShaderModule(VkDevice device, const std::vector<uint32_t>& spirv, const char* name) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = spirv.size() * sizeof(uint32_t);
    ci.pCode = spirv.data();
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &ci, nullptr, &module) != VK_SUCCESS) return VK_NULL_HANDLE;
    setDebugName(device, module, name);
    return module;
}

VkPipeline createGraphicsPipeline(VkDevice device, VkPipelineLayout layout, VkShaderModule vs, VkShaderModule ps,
                                  const GraphicsState& s, const char* name) {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "VSMain";
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = ps;
    stages[1].pName = "PSMain";

    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInput.vertexBindingDescriptionCount = static_cast<uint32_t>(s.vertexBindings.size());
    vertexInput.pVertexBindingDescriptions = s.vertexBindings.data();
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(s.vertexAttributes.size());
    vertexInput.pVertexAttributeDescriptions = s.vertexAttributes.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    inputAssembly.topology = s.topology;

    // 视口和裁剪矩形设为动态状态：窗口缩放后不用重建管线
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;

    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = s.polygonMode;
    raster.cullMode = s.cullMode;
    raster.frontFace = s.frontFace;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = s.depthTest;
    depth.depthWriteEnable = s.depthWrite;
    depth.depthCompareOp = s.depthCompare;

    std::vector<VkPipelineColorBlendAttachmentState> blendStates(s.colorFormats.size());
    for (auto& b : blendStates) {
        b.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        if (s.blend == BlendMode::Opaque) continue;
        b.blendEnable = VK_TRUE;
        b.srcColorBlendFactor = s.blend == BlendMode::Alpha ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_ONE;
        b.dstColorBlendFactor = s.blend == BlendMode::Alpha ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE;
        b.colorBlendOp = VK_BLEND_OP_ADD;
        b.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        b.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        b.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = static_cast<uint32_t>(blendStates.size());
    blend.pAttachments = blendStates.data();

    // dynamic rendering 的附件格式信息挂在 pNext 上，代替 renderPass/subpass
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = static_cast<uint32_t>(s.colorFormats.size());
    rendering.pColorAttachmentFormats = s.colorFormats.data();
    rendering.depthAttachmentFormat = s.depthFormat;

    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.pNext = &rendering;
    ci.stageCount = 2;
    ci.pStages = stages;
    ci.pVertexInputState = &vertexInput;
    ci.pInputAssemblyState = &inputAssembly;
    ci.pViewportState = &viewport;
    ci.pRasterizationState = &raster;
    ci.pMultisampleState = &multisample;
    ci.pDepthStencilState = &depth;
    ci.pColorBlendState = &blend;
    ci.pDynamicState = &dynamic;
    ci.layout = layout;

    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult r = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline);
    if (r != VK_SUCCESS) {
        LOG_ERROR("创建图形管线 {} 失败: {}", name, toString(r));
        return VK_NULL_HANDLE;
    }
    setDebugName(device, pipeline, name);
    return pipeline;
}

VkPipeline createComputePipeline(VkDevice device, VkPipelineLayout layout, VkShaderModule cs, const char* name) {
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = cs;
    ci.stage.pName = "CSMain";
    ci.layout = layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult r = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline);
    if (r != VK_SUCCESS) {
        LOG_ERROR("创建计算管线 {} 失败: {}", name, toString(r));
        return VK_NULL_HANDLE;
    }
    setDebugName(device, pipeline, name);
    return pipeline;
}

} // namespace eng::rhi
