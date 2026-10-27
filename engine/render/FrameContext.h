#pragma once
// 一帧内各 pass 共享的东西：帧描述符分配器、全局描述符集（set 0）、管线缓存。
// 由 Renderer 每帧填好，通过 PassContext::frame 传给每个 pass 的执行函数。

#include "rhi/Descriptors.h"

namespace eng {

class PipelineCache;
class DefaultResources;

struct FrameContext {
    uint32_t slot = 0;           // 0..kFramesInFlight-1
    uint64_t frameNumber = 0;
    rhi::DescriptorAllocator* descriptors = nullptr;
    VkDescriptorSet globalSet = VK_NULL_HANDLE; // set 0：FrameConstants + 采样器
    PipelineCache* pipelines = nullptr;
    DefaultResources* defaults = nullptr;
    VkDevice device = VK_NULL_HANDLE;
};

} // namespace eng
