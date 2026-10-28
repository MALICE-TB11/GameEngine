#include "render/Shadows.h"

#include <algorithm>
#include <cmath>

namespace eng {

mat4 orthoReverseZ(float l, float r, float b, float t, float n, float f) {
    // 视空间 z ∈ [-f, -n]（看向 -Z）映射到深度 [0, 1]，近平面为 1：depth = (z + f) / (f - n)
    mat4 m(1.0f);
    m[0][0] = 2.0f / (r - l);
    m[1][1] = 2.0f / (t - b);
    m[2][2] = 1.0f / (f - n);
    m[3][0] = -(r + l) / (r - l);
    m[3][1] = -(t + b) / (t - b);
    m[3][2] = f / (f - n);
    return m;
}

ShadowCascades computeCascades(const mat4& cameraView, float fovY, float aspect, float cameraNear, const vec3& sunDir,
                               const ShadowSettings& s) {
    ShadowCascades out;
    const mat4 invView = glm::inverse(cameraView);
    const float n = std::max(cameraNear, 0.01f), f = std::max(s.maxDistance, n + 1.0f);
    const float tanY = std::tan(fovY * 0.5f), tanX = tanY * aspect;
    const float cascadeRes = float(s.atlasSize / 2);

    float prevSplit = n;
    for (uint32_t i = 0; i < kShadowCascades; ++i) {
        // ---- 切分距离：对数与均匀切分的混合（「实用切分」，Zhang 2006）----
        const float p = float(i + 1) / float(kShadowCascades);
        const float logSplit = n * std::pow(f / n, p);
        const float uniSplit = n + (f - n) * p;
        const float split = s.splitLambda * logSplit + (1.0f - s.splitLambda) * uniSplit;
        out.splits[i] = split;

        // ---- 这一段视锥的 8 个角（世界空间） ----
        vec3 corners[8];
        int k = 0;
        for (float d : {prevSplit, split})
            for (float sy : {-1.0f, 1.0f})
                for (float sx : {-1.0f, 1.0f}) corners[k++] = vec3(invView * vec4(sx * tanX * d, sy * tanY * d, -d, 1.0f));

        // ---- 用包围球而不是包围盒：球不随相机旋转改变大小，阴影纹素的大小稳定，不会「游泳」 ----
        vec3 center(0.0f);
        for (const vec3& c : corners) center += c;
        center /= 8.0f;
        float radius = 0.0f;
        for (const vec3& c : corners) radius = std::max(radius, glm::length(c - center));
        radius = std::ceil(radius * 16.0f) / 16.0f; // 半径量化：避免浮点抖动导致投影范围每帧微变

        // ---- 光源视图：从太阳方向看向球心 ----
        const vec3 up = std::abs(sunDir.y) > 0.99f ? vec3(0, 0, 1) : vec3(0, 1, 0);
        const float back = radius + s.casterExtent; // 光源放在球心后方足够远，捕获范围外的遮挡物
        mat4 lightView = glm::lookAt(center + sunDir * back, center, up);

        // ---- 纹素对齐：把球心在光源空间里的位置吸附到纹素网格，相机平移时阴影边缘不闪 ----
        const float texel = (2.0f * radius) / cascadeRes;
        vec4 origin = lightView * vec4(center, 1.0f);
        const vec2 snapped = glm::floor(vec2(origin) / texel) * texel;
        lightView[3][0] += snapped.x - origin.x;
        lightView[3][1] += snapped.y - origin.y;

        const mat4 proj = orthoReverseZ(-radius, radius, -radius, radius, 0.0f, back + radius);
        out.viewProj[i] = proj * lightView;
        out.texelWorldSize[i] = texel;
        prevSplit = split;
    }
    return out;
}

} // namespace eng
