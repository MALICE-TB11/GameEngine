#include "render/PipelineCache.h"

#include "core/Log.h"
#include "rhi/Descriptors.h"
#include "rhi/Device.h"

namespace eng {

void PipelineCache::init(rhi::Device& device, VkDescriptorSetLayout globalLayout) {
    m_device = &device;
    m_globalLayout = globalLayout;
    // 不需要 set 1 的管线也占一个空的 set 1：所有管线布局形状一致，规则更简单
    m_emptyLayout = rhi::createSetLayout(device, {}, "EmptySetLayout");
}

void PipelineCache::shutdown() {
    VkDevice dev = m_device->device();
    for (Entry& e : m_entries) {
        if (e.pipeline) vkDestroyPipeline(dev, e.pipeline, nullptr);
        vkDestroyPipelineLayout(dev, e.layout, nullptr);
    }
    m_entries.clear();
    vkDestroyDescriptorSetLayout(dev, m_emptyLayout, nullptr);
}

VkPipelineLayout PipelineCache::makeLayout(VkDescriptorSetLayout set1, const std::string& name) {
    const VkDescriptorSetLayout sets[2] = {m_globalLayout, set1 ? set1 : m_emptyLayout};
    VkPushConstantRange push{VK_SHADER_STAGE_ALL, 0, kPushConstantSize};
    VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    ci.setLayoutCount = 2;
    ci.pSetLayouts = sets;
    ci.pushConstantRangeCount = 1;
    ci.pPushConstantRanges = &push;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    ENG_CHECK_VK(vkCreatePipelineLayout(m_device->device(), &ci, nullptr, &layout));
    rhi::setDebugName(m_device->device(), layout, (name + "Layout").c_str());
    return layout;
}

VkPipeline PipelineCache::build(Entry& e, bool forceCompile, std::string& error) {
    VkDevice dev = m_device->device();
    const std::string& shader = shaderOf(e);
    auto get = [&](ShaderStage stage) {
        return forceCompile ? m_compiler.compile(shader, stage, error) : m_compiler.load(shader, stage, error);
    };
    // 记录依赖文件的最新时间：之后它变了就触发热重载
    e.stamp = m_compiler.newestDependencyTime(shader);

    if (e.compute) {
        auto cs = get(ShaderStage::Compute);
        if (!cs) return VK_NULL_HANDLE;
        VkShaderModule m = rhi::createShaderModule(dev, *cs, (shader + ".cs").c_str());
        VkPipeline p = rhi::createComputePipeline(dev, e.layout, m, e.computeDesc.name.c_str());
        // shader module 只在创建管线时需要，之后就可以销毁
        vkDestroyShaderModule(dev, m, nullptr);
        if (!p) error = "vkCreateComputePipelines 失败（检查 validation 日志）";
        return p;
    }
    auto vs = get(ShaderStage::Vertex);
    if (!vs) return VK_NULL_HANDLE;
    auto ps = get(ShaderStage::Pixel);
    if (!ps) return VK_NULL_HANDLE;
    VkShaderModule vm = rhi::createShaderModule(dev, *vs, (shader + ".vs").c_str());
    VkShaderModule pm = rhi::createShaderModule(dev, *ps, (shader + ".ps").c_str());
    VkPipeline p = rhi::createGraphicsPipeline(dev, e.layout, vm, pm, e.graphics.state, e.graphics.name.c_str());
    vkDestroyShaderModule(dev, vm, nullptr);
    vkDestroyShaderModule(dev, pm, nullptr);
    if (!p) error = "vkCreateGraphicsPipelines 失败（检查 validation 日志）";
    return p;
}

PipelineHandle PipelineCache::createGraphics(const GraphicsPipelineDesc& desc) {
    Entry e;
    e.graphics = desc;
    e.layout = makeLayout(desc.set1, desc.name);
    e.pipeline = build(e, false, e.error);
    if (!e.pipeline) LOG_ERROR("管线 {} 创建失败（将使用兜底）:\n{}", desc.name, e.error);
    m_entries.push_back(std::move(e));
    return static_cast<PipelineHandle>(m_entries.size() - 1);
}

PipelineHandle PipelineCache::createCompute(const ComputePipelineDesc& desc) {
    Entry e;
    e.compute = true;
    e.computeDesc = desc;
    e.layout = makeLayout(desc.set1, desc.name);
    e.pipeline = build(e, false, e.error);
    if (!e.pipeline) LOG_ERROR("管线 {} 创建失败（将使用兜底）:\n{}", desc.name, e.error);
    m_entries.push_back(std::move(e));
    return static_cast<PipelineHandle>(m_entries.size() - 1);
}

bool PipelineCache::ready(PipelineHandle h) const {
    return h < m_entries.size() && m_entries[h].pipeline != VK_NULL_HANDLE;
}

VkPipeline PipelineCache::pipeline(PipelineHandle h) const {
    return m_entries[h].pipeline;
}

VkPipelineLayout PipelineCache::layout(PipelineHandle h) const {
    return m_entries[h].layout;
}

void PipelineCache::bind(VkCommandBuffer cmd, PipelineHandle h, VkDescriptorSet globalSet) const {
    const Entry& e = m_entries[h];
    const VkPipelineBindPoint bp = e.compute ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS;
    vkCmdBindPipeline(cmd, bp, e.pipeline);
    vkCmdBindDescriptorSets(cmd, bp, e.layout, 0, 1, &globalSet, 0, nullptr);
}

void PipelineCache::bindSet1(VkCommandBuffer cmd, PipelineHandle h, VkDescriptorSet set) const {
    const Entry& e = m_entries[h];
    const VkPipelineBindPoint bp = e.compute ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS;
    vkCmdBindDescriptorSets(cmd, bp, e.layout, 1, 1, &set, 0, nullptr);
}

void PipelineCache::pushConstants(VkCommandBuffer cmd, PipelineHandle h, const void* data, uint32_t size) const {
    vkCmdPushConstants(cmd, m_entries[h].layout, VK_SHADER_STAGE_ALL, 0, size, data);
}

void PipelineCache::reload(Entry& e) {
    std::string error;
    VkPipeline fresh = build(e, true, error);
    if (!fresh) {
        // 保留旧管线：画面继续，错误显示在面板上
        e.error = error;
        LOG_ERROR("shader 热重载失败，保留旧管线 [{}]:\n{}", nameOf(e), error);
        return;
    }
    if (e.pipeline) {
        VkPipeline old = e.pipeline;
        VkDevice dev = m_device->device();
        m_device->deferDestroy([dev, old] { vkDestroyPipeline(dev, old, nullptr); });
    }
    e.pipeline = fresh;
    e.error.clear();
    ++m_reloadCount;
    LOG_INFO("shader 热重载成功: {}", nameOf(e));
}

void PipelineCache::pollHotReload(double nowSeconds) {
    if (!autoReload) return;
    if (m_lastPoll >= 0.0 && nowSeconds - m_lastPoll < 0.25) return;
    m_lastPoll = nowSeconds;
    for (Entry& e : m_entries)
        if (m_compiler.newestDependencyTime(shaderOf(e)) != e.stamp) reload(e);
}

void PipelineCache::reloadAll() {
    for (Entry& e : m_entries) reload(e);
}

std::vector<PipelineCache::Status> PipelineCache::status() const {
    std::vector<Status> out;
    for (const Entry& e : m_entries) out.push_back({nameOf(e), shaderOf(e), e.pipeline != VK_NULL_HANDLE, e.error});
    return out;
}

uint32_t PipelineCache::errorCount() const {
    uint32_t n = 0;
    for (const Entry& e : m_entries) n += e.error.empty() ? 0u : 1u;
    return n;
}

} // namespace eng
