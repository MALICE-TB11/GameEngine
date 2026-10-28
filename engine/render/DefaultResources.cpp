#include "render/DefaultResources.h"

#include "rhi/Device.h"
#include "rhi/Upload.h"

#include <algorithm>
#include <array>

namespace eng {
namespace {

rhi::Image solid(rhi::Device& device, uint8_t r, uint8_t g, uint8_t b, uint8_t a, const char* name) {
    std::array<uint8_t, 4 * 4 * 4> px{};
    for (size_t i = 0; i < 16; ++i) {
        px[i * 4 + 0] = r;
        px[i * 4 + 1] = g;
        px[i * 4 + 2] = b;
        px[i * 4 + 3] = a;
    }
    return rhi::createTexture2D(device, px.data(), 4, 4, VK_FORMAT_R8G8B8A8_UNORM, false, name);
}

VkSampler makeSampler(rhi::Device& device, VkFilter filter, VkSamplerAddressMode mode, float anisotropy, const char* name) {
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.magFilter = filter;
    ci.minFilter = filter;
    ci.mipmapMode = filter == VK_FILTER_LINEAR ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = ci.addressModeV = ci.addressModeW = mode;
    ci.maxLod = VK_LOD_CLAMP_NONE;
    if (anisotropy > 1.0f) {
        ci.anisotropyEnable = VK_TRUE;
        ci.maxAnisotropy = std::min(anisotropy, device.properties().limits.maxSamplerAnisotropy);
    }
    VkSampler s = VK_NULL_HANDLE;
    ENG_CHECK_VK(vkCreateSampler(device.device(), &ci, nullptr, &s));
    rhi::setDebugName(device.device(), s, name);
    return s;
}

} // namespace

void DefaultResources::init(rhi::Device& device) {
    m_device = &device;
    m_black = solid(device, 0, 0, 0, 255, "DefaultBlack");
    m_white = solid(device, 255, 255, 255, 255, "DefaultWhite");
    m_flatNormal = solid(device, 128, 128, 255, 255, "DefaultFlatNormal");

    // 品红/黑 棋盘，8x8 格
    constexpr uint32_t kSize = 64;
    std::array<uint8_t, kSize * kSize * 4> px{};
    for (uint32_t y = 0; y < kSize; ++y)
        for (uint32_t x = 0; x < kSize; ++x) {
            const bool odd = ((x / 8) + (y / 8)) & 1;
            uint8_t* p = &px[(y * kSize + x) * 4];
            p[0] = odd ? 255 : 0;
            p[1] = 0;
            p[2] = odd ? 255 : 0;
            p[3] = 255;
        }
    m_error = rhi::createTexture2D(device, px.data(), kSize, kSize, VK_FORMAT_R8G8B8A8_UNORM, true, "DefaultError");

    m_linearWrap = makeSampler(device, VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT, 16.0f, "LinearWrapAniso");
    m_linearClamp = makeSampler(device, VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 1.0f, "LinearClamp");
    m_pointClamp = makeSampler(device, VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 1.0f, "PointClamp");

    // 比较采样器：线性过滤 + 比较 = 硬件对 2×2 个深度分别比较后再插值（免费的 2×2 PCF）
    VkSamplerCreateInfo sc{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sc.magFilter = sc.minFilter = VK_FILTER_LINEAR;
    sc.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sc.addressModeU = sc.addressModeV = sc.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sc.compareEnable = VK_TRUE;
    sc.compareOp = VK_COMPARE_OP_GREATER_OR_EQUAL; // 参考值（片元深度）≥ 图中深度 → 照亮
    sc.maxLod = 0.0f;
    ENG_CHECK_VK(vkCreateSampler(device.device(), &sc, nullptr, &m_shadowCompare));
    rhi::setDebugName(device.device(), m_shadowCompare, "ShadowCompare");

    // 深度 = 0 的 1×1 深度图（清除后转成着色器只读）
    rhi::ImageDesc dd;
    dd.format = VK_FORMAT_D32_SFLOAT;
    dd.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    dd.name = "DefaultDepthFar";
    m_depthFar = rhi::createImage(device, dd);
    device.immediateSubmit([&](VkCommandBuffer cmd) {
        const rhi::ImageState undefined{VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, VK_IMAGE_LAYOUT_UNDEFINED};
        const rhi::ImageState dst{VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL};
        const rhi::ImageState read{VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        rhi::imageBarrier(cmd, m_depthFar.image, VK_IMAGE_ASPECT_DEPTH_BIT, undefined, dst);
        const VkClearDepthStencilValue zero{0.0f, 0};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        vkCmdClearDepthStencilImage(cmd, m_depthFar.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
        rhi::imageBarrier(cmd, m_depthFar.image, VK_IMAGE_ASPECT_DEPTH_BIT, dst, read);
    });
}

void DefaultResources::shutdown() {
    if (!m_device) return;
    VkDevice dev = m_device->device();
    vkDestroySampler(dev, m_linearWrap, nullptr);
    vkDestroySampler(dev, m_linearClamp, nullptr);
    vkDestroySampler(dev, m_pointClamp, nullptr);
    vkDestroySampler(dev, m_shadowCompare, nullptr);
    rhi::destroyImage(*m_device, m_depthFar);
    rhi::destroyImage(*m_device, m_black);
    rhi::destroyImage(*m_device, m_white);
    rhi::destroyImage(*m_device, m_flatNormal);
    rhi::destroyImage(*m_device, m_error);
    m_device = nullptr;
}

} // namespace eng
