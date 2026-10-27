// 颜色空间工具。
// 约定：shader 里所有光照计算都在「线性」空间；只在最后输出到交换链时编码为 sRGB。
#pragma once

// 线性 → sRGB（IEC 61966-2-1 分段函数）。交换链是 UNORM 格式，需要手动编码。
float3 LinearToSRGB(float3 c)
{
    c = saturate(c);
    float3 lo = c * 12.92;
    float3 hi = 1.055 * pow(c, 1.0 / 2.4) - 0.055;
    return select(c <= 0.0031308, lo, hi);
}

// sRGB → 线性。贴图用 *_SRGB 格式时由硬件自动完成，这里给手动场景（例如颜色参数）用。
float3 SRGBToLinear(float3 c)
{
    float3 lo = c / 12.92;
    float3 hi = pow((c + 0.055) / 1.055, 2.4);
    return select(c <= 0.04045, lo, hi);
}

// 相对亮度（Rec.709 系数）
float Luminance(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}
