#pragma once
// 默认资源：容错的基础。
//   - 贴图缺失 / pass 被关掉 → 用 黑 / 白 / 平坦法线 代替，下游照常运行
//   - 材质出错 → 用醒目的品红棋盘「错误贴图」，一眼就能看出哪里坏了
//   - 全局采样器：所有 shader 在 set 0 共用，不需要每个材质各建一份

#include "rhi/Resources.h"

namespace eng {

namespace rhi { class Device; }

class DefaultResources {
public:
    void init(rhi::Device& device);
    void shutdown();

    const rhi::Image& black() const { return m_black; }
    const rhi::Image& white() const { return m_white; }
    const rhi::Image& flatNormal() const { return m_flatNormal; } // (0.5, 0.5, 1) = 切线空间 +Z
    const rhi::Image& error() const { return m_error; }

    VkSampler linearWrap() const { return m_linearWrap; }   // 带各向异性，用于材质贴图
    VkSampler linearClamp() const { return m_linearClamp; } // 全屏后处理
    VkSampler pointClamp() const { return m_pointClamp; }   // 精确取像素（深度、调试）

private:
    rhi::Device* m_device = nullptr;
    rhi::Image m_black, m_white, m_flatNormal, m_error;
    VkSampler m_linearWrap = VK_NULL_HANDLE;
    VkSampler m_linearClamp = VK_NULL_HANDLE;
    VkSampler m_pointClamp = VK_NULL_HANDLE;
};

} // namespace eng
