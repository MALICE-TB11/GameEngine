#include "render/RenderGraph.h"

#include "core/Assert.h"
#include "core/Log.h"
#include "render/DefaultResources.h"
#include "render/FrameContext.h"
#include "rhi/GpuProfiler.h"

#include <algorithm>
#include <cmath>

namespace eng::rg {

namespace {
// 「写」类访问。barrier 的 srcAccess 只需要包含写：读操作不产生需要「刷出」的数据，
// 读之后再写（WAR）只需要执行依赖（等读完），不需要内存依赖。
constexpr VkAccessFlags2 kWriteMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_WRITE_BIT |
                                      VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_HOST_WRITE_BIT;
} // namespace

bool isWrite(Access a) {
    return a == Access::ColorWrite || a == Access::DepthWrite || a == Access::StorageWrite || a == Access::TransferDst;
}

bool isAttachment(Access a) {
    return a == Access::ColorWrite || a == Access::DepthWrite || a == Access::DepthRead;
}

bool requiresContent(Access a) {
    // 默认贴图只能「被采样」：没有 STORAGE / TRANSFER_SRC 用途，尺寸、格式、层数也对不上
    return a == Access::StorageRead || a == Access::TransferSrc;
}

rhi::ImageState stateFor(Access a, PassType type) {
    // 着色器阶段取决于 pass 类型：图形 pass 里顶点和片元着色器都可能采样（例如海面位移贴图在 VS 里采样）
    const VkPipelineStageFlags2 shaderStage = type == PassType::Compute
        ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
        : (VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
    const VkPipelineStageFlags2 depthStages = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    switch (a) {
    case Access::ColorWrite:
        return {VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    case Access::DepthWrite:
        return {depthStages, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL};
    case Access::DepthRead:
        return {depthStages, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL};
    case Access::Sampled:
        return {shaderStage, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    case Access::StorageRead:
        return {shaderStage, VK_ACCESS_2_SHADER_STORAGE_READ_BIT, VK_IMAGE_LAYOUT_GENERAL};
    case Access::StorageWrite:
        return {shaderStage, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT, VK_IMAGE_LAYOUT_GENERAL};
    case Access::TransferSrc:
        return {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
    case Access::TransferDst:
        return {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL};
    }
    return {};
}

// =============================== 纯逻辑：barrier 推导 ===============================

CompileResult compile(const std::vector<PassDecl>& passes, const std::vector<ResourceDecl>& resources) {
    CompileResult out;
    out.passes.resize(passes.size());
    std::vector<rhi::ImageState> state(resources.size());
    // 「最近一次写入」单独记录：写 → 读 → 写/布局转换 时，中间那个 barrier 只让写入对「读」可见；
    // 后面的写（布局转换本身也是写）仍需要显式等待最初那次写入（WAW）。
    // 只跟踪 state 会在读取合并后丢失这个信息 —— 同步验证层正是为此报过错。
    std::vector<rhi::ImageState> lastWrite(resources.size());
    std::vector<bool> content(resources.size());
    for (size_t r = 0; r < resources.size(); ++r) {
        state[r] = resources[r].initial;
        lastWrite[r] = resources[r].lastWrite;
        content[r] = resources[r].hasContent;
    }
    // 生成从 cur 到 target 的 barrier 源状态
    auto sourceOf = [&](uint32_t res, bool writesOrTransitions) {
        rhi::ImageState from = state[res];
        from.access &= kWriteMask;
        if (writesOrTransitions) {
            from.stage |= lastWrite[res].stage;
            from.access |= lastWrite[res].access;
        }
        return from;
    };

    for (size_t p = 0; p < passes.size(); ++p) {
        const PassDecl& pass = passes[p];
        CompiledPass& cp = out.passes[p];
        for (const AccessDecl& a : pass.accesses) cp.loadOps.push_back(a.load);
        if (!pass.active) continue; // 被跳过的 pass：不改变任何资源的状态和「是否有内容」

        // ---- 容错 0：必需输入没有内容 → 整个 pass 跳过 ----
        // 以海洋的计算链 频谱 → 行 FFT → 列 FFT → 组装 为例：关掉「频谱」后，「行 FFT」读不到输入。
        // 如果它照常被标记为「执行过」，它的输出就会被当作有效，下游会拿未初始化的显存继续算。
        // 跳过它、并且不标记它的输出，下游按同一规则继续跳过 —— 失败一路传到第一个「可选」读取
        // （采样），在那里换成默认贴图（例如海面拿到零位移，退化成平静水面）。
        for (const AccessDecl& a : pass.accesses)
            if (requiresContent(a.access) && !content[a.resource]) {
                cp.missingInput = a.resource;
                break;
            }
        if (cp.missingInput != kNoResource) continue;
        cp.executed = true;

        for (size_t k = 0; k < pass.accesses.size(); ++k) {
            const uint32_t res = pass.accesses[k].resource;
            Access access = pass.accesses[k].access;
            LoadOp load = pass.accesses[k].load;

            // ---- 容错 1：读一个本帧没人写过的资源 ----
            if (!content[res] && !isWrite(access)) {
                if (access == Access::DepthRead) {
                    // 只读深度附件没法换成默认贴图：改为「写并清空」，深度测试全部通过
                    access = Access::DepthWrite;
                    load = LoadOp::Clear;
                } else if (resources[res].materializeIfUnwritten) {
                    // 图之外的代码也直接采样这张图（编辑器视口经 ImGui 显示）：只在图内部换 view 不够，
                    // 外部看到的还是真实图像 —— 可能是旧画面，尺寸刚变时甚至是未初始化、布局不对的显存。
                    // 所以先把真实图像清成默认值，再按正常流程转成采样布局。
                    rhi::ImageState from = sourceOf(res, true);
                    from.layout = VK_IMAGE_LAYOUT_UNDEFINED;
                    const rhi::ImageState to = stateFor(Access::TransferDst, PassType::Transfer);
                    cp.clearBarriers.push_back({res, from, to});
                    cp.clears.push_back(res);
                    state[res] = to;
                    lastWrite[res] = {to.stage, to.access, VK_IMAGE_LAYOUT_UNDEFINED};
                    content[res] = true;
                } else {
                    cp.substituted.push_back(res); // 执行时绑定默认贴图，不需要 barrier
                    continue;
                }
            }
            // ---- 容错 2：上游写入者被跳过，本 pass 想 LOAD 旧内容 → 改为 CLEAR ----
            if (isAttachment(access) && load == LoadOp::Load && !content[res]) load = LoadOp::Clear;
            cp.loadOps[k] = load;

            const rhi::ImageState target = stateFor(access, pass.type);
            rhi::ImageState& cur = state[res];
            const bool write = isWrite(access);
            // 「丢弃旧内容」：没有内容可保留，或附件不 LOAD。此时 oldLayout 可以写 UNDEFINED，
            // 驱动不用为保留数据做布局转换，开销更低。
            const bool discard = write && (!content[res] || (isAttachment(access) && load != LoadOp::Load));
            const bool prevWrite = (cur.access & kWriteMask) != 0;

            const bool layoutChange = cur.layout != target.layout;
            if (layoutChange || write || prevWrite) {
                // 需要 barrier 的三种情况：
                //   布局变化（必须显式转换）；本次是写（要等之前的读/写完成：WAR / WAW）；
                //   之前是写（要让写入结果对本次访问可见：RAW）
                rhi::ImageState from = sourceOf(res, write || layoutChange);
                if (discard) from.layout = VK_IMAGE_LAYOUT_UNDEFINED;
                cp.barriers.push_back({res, from, target});
                cur = target;
            } else {
                // 读之后再读、布局相同：读与读之间不需要同步。但要注意「上一次写入」：
                // 之前那个 barrier 的目标阶段只覆盖了当时的读者。新读者如果在别的阶段
                // （例如先被片元着色器采样，再被计算着色器采样），它和那次写入之间还没有任何依赖，
                // 必须补一个「最近写入 → 新阶段」的 barrier（布局不变）。
                const VkPipelineStageFlags2 uncovered = target.stage & ~cur.stage;
                if (uncovered && lastWrite[res].stage != VK_PIPELINE_STAGE_2_NONE) {
                    const rhi::ImageState from{lastWrite[res].stage, lastWrite[res].access, cur.layout};
                    cp.barriers.push_back({res, from, {uncovered, target.access, target.layout}});
                }
                // 阶段合并起来：之后若有写入，barrier 会同时等待所有读取
                cur.stage |= target.stage;
                cur.access |= target.access;
            }
            if (write) {
                content[res] = true;
                lastWrite[res] = {target.stage, target.access & kWriteMask, VK_IMAGE_LAYOUT_UNDEFINED};
            }
        }
    }

    // ---- 帧末：外部资源过渡到要求的状态（交换链 → PRESENT） ----
    for (uint32_t r = 0; r < resources.size(); ++r) {
        if (!resources[r].hasFinal) continue;
        if (!content[r] && resources[r].clearIfUnwritten) {
            // 所有写它的 pass 都被关了：清成黑色，避免把未初始化的显存显示出来
            rhi::ImageState from = sourceOf(r, true);
            from.layout = VK_IMAGE_LAYOUT_UNDEFINED;
            const rhi::ImageState to = stateFor(Access::TransferDst, PassType::Transfer);
            out.clearBarriers.push_back({r, from, to});
            out.clears.push_back(r);
            state[r] = to;
            lastWrite[r] = {to.stage, to.access, VK_IMAGE_LAYOUT_UNDEFINED};
        }
        // 帧末过渡（例如 → PRESENT）是一次布局转换，同样要等最近一次写入
        out.finalBarriers.push_back({r, sourceOf(r, true), resources[r].final});
        state[r] = resources[r].final;
        lastWrite[r] = {};
    }
    out.endContent = content;
    out.endStates = std::move(state);
    out.endWrites = std::move(lastWrite);
    return out;
}

// =============================== GPU 部分 ===============================

VkImageView PassContext::view(TextureHandle h) const {
    ENG_ASSERT(h.valid() && h.index < graph->m_resources.size(), "无效的 TextureHandle");
    const auto& subs = graph->m_substituted[passIndex];
    if (std::find(subs.begin(), subs.end(), h.index) != subs.end())
        return graph->fallbackView(graph->m_resources[h.index].desc.fallback, graph->m_resources[h.index].layers);
    return graph->m_resources[h.index].view;
}

VkImage PassContext::image(TextureHandle h) const {
    ENG_ASSERT(h.valid() && h.index < graph->m_resources.size(), "无效的 TextureHandle");
    ENG_ASSERT(hasContent(h), "默认贴图不能作为拷贝的源或目标");
    return graph->m_resources[h.index].image;
}

bool PassContext::hasContent(TextureHandle h) const {
    const auto& subs = graph->m_substituted[passIndex];
    return std::find(subs.begin(), subs.end(), h.index) == subs.end();
}

PassBuilder& PassBuilder::color(TextureHandle t, LoadOp load) {
    m_graph.addAccess(m_index, t, Access::ColorWrite, load);
    return *this;
}
PassBuilder& PassBuilder::depth(TextureHandle t, LoadOp load) {
    m_graph.addAccess(m_index, t, Access::DepthWrite, load);
    return *this;
}
PassBuilder& PassBuilder::depthReadOnly(TextureHandle t) {
    m_graph.addAccess(m_index, t, Access::DepthRead, LoadOp::Load);
    return *this;
}
PassBuilder& PassBuilder::sampled(TextureHandle t) {
    m_graph.addAccess(m_index, t, Access::Sampled, LoadOp::Load);
    return *this;
}
PassBuilder& PassBuilder::storageRead(TextureHandle t) {
    m_graph.addAccess(m_index, t, Access::StorageRead, LoadOp::Load);
    return *this;
}
PassBuilder& PassBuilder::storageWrite(TextureHandle t) {
    m_graph.addAccess(m_index, t, Access::StorageWrite, LoadOp::Load);
    return *this;
}
PassBuilder& PassBuilder::transferSrc(TextureHandle t) {
    m_graph.addAccess(m_index, t, Access::TransferSrc, LoadOp::Load);
    return *this;
}
PassBuilder& PassBuilder::transferDst(TextureHandle t) {
    m_graph.addAccess(m_index, t, Access::TransferDst, LoadOp::Load);
    return *this;
}
PassBuilder& PassBuilder::require(std::function<bool()> ready) {
    m_graph.m_passes[m_index].ready = std::move(ready);
    return *this;
}
PassBuilder& PassBuilder::execute(std::function<void(PassContext&)> fn) {
    m_graph.m_passes[m_index].fn = std::move(fn);
    return *this;
}

void RenderGraph::init(rhi::Device& device, DefaultResources& defaults) {
    m_device = &device;
    m_defaults = &defaults;
}

void RenderGraph::shutdown() {
    if (!m_device) return; // init 失败（或从没 init）时也能安全调用
    for (auto& [name, p] : m_pool) rhi::destroyImage(*m_device, p.image);
    m_pool.clear();
}

void RenderGraph::beginFrame(VkExtent2D backbuffer, uint64_t frameNumber) {
    m_backbuffer = backbuffer;
    m_frameNumber = frameNumber;
    m_resources.clear();
    m_passes.clear();
}

TextureHandle RenderGraph::createTexture(const TextureDesc& desc) {
    Resource r;
    r.desc = desc;
    r.format = desc.format;
    r.layers = std::max(1u, desc.layers);
    if (desc.externalRead) r.usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT; // 没人写时要能清屏
    if (desc.width && desc.height) {
        r.extent = {desc.width, desc.height};
    } else {
        r.extent = {std::max(1u, uint32_t(std::lround(m_backbuffer.width * desc.scale))),
                    std::max(1u, uint32_t(std::lround(m_backbuffer.height * desc.scale)))};
    }
    m_resources.push_back(std::move(r));
    return {static_cast<uint32_t>(m_resources.size() - 1)};
}

TextureHandle RenderGraph::importTexture(const std::string& name, VkImage image, VkImageView view, VkFormat format,
                                         VkExtent2D extent, const rhi::ImageState& initial, const rhi::ImageState& final) {
    Resource r;
    r.desc.name = name;
    r.desc.fallback = Fallback::Black;
    r.imported = true;
    r.image = image;
    r.view = view;
    r.format = format;
    r.extent = extent;
    r.importInitial = initial;
    r.importFinal = final;
    m_resources.push_back(std::move(r));
    return {static_cast<uint32_t>(m_resources.size() - 1)};
}

PassBuilder RenderGraph::addPass(const std::string& name, PassType type) {
    m_passes.push_back({name, type, {}, {}, {}});
    return PassBuilder(*this, static_cast<uint32_t>(m_passes.size() - 1));
}

void RenderGraph::addAccess(uint32_t pass, TextureHandle t, Access a, LoadOp load) {
    ENG_ASSERT(t.valid() && t.index < m_resources.size(), "pass 声明了无效的资源");
    auto& accesses = m_passes[pass].accesses;
    // 一个 pass 对同一资源只能声明一种用法：否则需要的布局互相矛盾
    for (const auto& existing : accesses) ENG_ASSERT(existing.resource != t.index, "同一 pass 重复声明了同一资源");
    accesses.push_back({t.index, a, load});

    Resource& r = m_resources[t.index];
    switch (a) {
    case Access::ColorWrite: r.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; break;
    case Access::DepthWrite:
    case Access::DepthRead: r.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT; break;
    case Access::Sampled: r.usage |= VK_IMAGE_USAGE_SAMPLED_BIT; break;
    case Access::StorageRead:
    case Access::StorageWrite: r.usage |= VK_IMAGE_USAGE_STORAGE_BIT; break;
    case Access::TransferSrc: r.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT; break;
    case Access::TransferDst: r.usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT; break;
    }
}

bool& RenderGraph::passEnabled(const std::string& name) {
    return m_enabled.try_emplace(name, true).first->second;
}

VkImageView RenderGraph::fallbackView(Fallback f, uint32_t layers) const {
    // 数组贴图的读者在 shader 里声明的是 Texture2DArray：默认贴图也必须是数组 view，否则 view 类型不匹配
    if (layers > 1) return m_defaults->blackArray().view;
    switch (f) {
    case Fallback::White: return m_defaults->white().view;
    case Fallback::FlatNormal: return m_defaults->flatNormal().view;
    case Fallback::DepthFar: return m_defaults->depthFar().view;
    default: return m_defaults->black().view;
    }
}

RenderGraph::Pooled& RenderGraph::acquirePooled(const std::string& name, VkFormat format, VkExtent2D extent, VkImageUsageFlags usage,
                                                uint32_t layers) {
    // 总是带上 SAMPLED 与 TRANSFER_SRC：方便调试面板查看、截图，开销可以忽略
    usage |= VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    Pooled& p = m_pool[name];
    const bool mismatch = !p.image || p.image.format != format || p.image.extent.width != extent.width ||
                          p.image.extent.height != extent.height || p.image.layers != layers || (p.usage & usage) != usage;
    if (mismatch) {
        if (p.image) {
            // 旧贴图可能还被前一两帧使用：延迟到 GPU 用完再销毁
            rhi::Image old = p.image;
            m_device->deferDestroy([dev = m_device, old]() mutable { rhi::destroyImage(*dev, old); });
        }
        rhi::ImageDesc d;
        d.width = extent.width;
        d.height = extent.height;
        d.format = format;
        d.layers = layers;
        d.usage = usage | p.usage; // 只增不减：避免两种用法交替出现时反复重建
        d.name = name.c_str();
        p.image = rhi::createImage(*m_device, d);
        p.usage = d.usage;
        p.state = {};
        p.lastWrite = {};
        p.hasContent = false; // 新建的图里是未初始化的显存
    }
    p.lastUsedFrame = m_frameNumber;
    return p;
}

VkImageView RenderGraph::preparePooledTexture(const TextureDesc& desc, VkImageUsageFlags usage) {
    ENG_ASSERT(desc.width > 0 && desc.height > 0, "preparePooledTexture 需要绝对尺寸");
    return acquirePooled(desc.name, desc.format, {desc.width, desc.height}, usage).image.view;
}

void RenderGraph::realizeTransients() {
    for (Resource& r : m_resources) {
        if (r.imported) continue;
        Pooled& p = acquirePooled(r.desc.name, r.format, r.extent, r.usage, r.layers);
        r.image = p.image.image;
        r.view = p.image.view;
    }
    // EXTEND: 很久没用的池化贴图（例如关掉的特效）可以在这里按 lastUsedFrame 回收，
    //         或做「生命周期不重叠的瞬态贴图共用同一块显存」（aliasing）来省显存。
}

void RenderGraph::execute(VkCommandBuffer cmd, FrameContext& frame, rhi::GpuProfiler* profiler) {
    realizeTransients();

    // ---- 1. 组装纯逻辑输入 ----
    std::vector<PassDecl> decls(m_passes.size());
    m_report.assign(m_passes.size(), {});
    for (size_t i = 0; i < m_passes.size(); ++i) {
        const Pass& p = m_passes[i];
        decls[i].name = p.name;
        decls[i].type = p.type;
        decls[i].accesses = p.accesses;
        const bool enabled = passEnabled(p.name);
        const bool ready = p.fn && (!p.ready || p.ready());
        decls[i].active = enabled && ready;
        m_report[i].name = p.name;
        m_report[i].status = !enabled ? PassReport::Status::Disabled
                           : !ready   ? PassReport::Status::NotReady
                                      : PassReport::Status::Ran;
    }
    std::vector<ResourceDecl> res(m_resources.size());
    for (size_t i = 0; i < m_resources.size(); ++i) {
        const Resource& r = m_resources[i];
        res[i].name = r.desc.name;
        if (r.imported) {
            res[i].initial = r.importInitial;
            res[i].hasFinal = true;
            res[i].final = r.importFinal;
            res[i].clearIfUnwritten = true;
        } else {
            // 瞬态贴图的「初始状态」= 上一帧结束时的状态。布局会被当作 UNDEFINED 丢弃，
            // 但阶段要保留：这样本帧第一次写入会等上一帧对它的读取结束（跨帧的 WAR 冒险）。
            res[i].initial = m_pool[r.desc.name].state;
            res[i].lastWrite = m_pool[r.desc.name].lastWrite;
            res[i].materializeIfUnwritten = r.desc.externalRead;
            // 持久贴图：上一帧写过的内容仍然有效（读它不会被换成默认贴图，布局也不会被当成 UNDEFINED 丢弃）
            res[i].hasContent = r.desc.persistent && m_pool[r.desc.name].hasContent;
        }
    }

    // ---- 2. 推导 barrier ----
    const CompileResult compiled = compile(decls, res);
    m_substituted.assign(m_passes.size(), {});

    auto emitBarriers = [&](const std::vector<Barrier>& barriers) {
        if (barriers.empty()) return;
        std::vector<VkImageMemoryBarrier2> vk;
        vk.reserve(barriers.size());
        for (const Barrier& b : barriers) {
            const Resource& r = m_resources[b.resource];
            vk.push_back(rhi::makeImageBarrier(r.image, rhi::aspectOf(r.format), b.from, b.to));
        }
        // 一个 pass 的所有 barrier 合并成一次调用：驱动可以一起处理，比逐个提交高效
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = static_cast<uint32_t>(vk.size());
        dep.pImageMemoryBarriers = vk.data();
        vkCmdPipelineBarrier2(cmd, &dep);
    };

    // ---- 3. 录制每个 pass ----
    for (size_t i = 0; i < m_passes.size(); ++i) {
        const Pass& pass = m_passes[i];
        const CompiledPass& cp = compiled.passes[i];
        m_substituted[i] = cp.substituted;
        for (uint32_t s : cp.substituted) m_report[i].substituted.push_back(m_resources[s].desc.name);
        if (cp.missingInput != kNoResource) {
            m_report[i].status = PassReport::Status::MissingInput;
            m_report[i].missingInput = m_resources[cp.missingInput].desc.name;
        }
        if (!cp.executed) continue;

        // 被外部直接采样、但本帧没人写的贴图：先清成默认值（见 compile 的「容错 1」）
        emitBarriers(cp.clearBarriers);
        for (uint32_t r : cp.clears) {
            const float v = m_resources[r].desc.fallback == Fallback::White ? 1.0f : 0.0f;
            const VkClearColorValue color{{v, v, v, 1.0f}};
            const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
            vkCmdClearColorImage(cmd, m_resources[r].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
        }
        emitBarriers(cp.barriers);
        rhi::beginLabel(cmd, pass.name.c_str());
        const uint32_t scope = profiler ? profiler->begin(cmd, pass.name.c_str()) : UINT32_MAX;

        PassContext ctx;
        ctx.cmd = cmd;
        ctx.frame = &frame;
        ctx.graph = this;
        ctx.passIndex = static_cast<uint32_t>(i);
        ctx.extent = m_backbuffer;

        if (pass.type == PassType::Graphics) {
            // 根据声明自动组装 dynamic rendering 的附件
            std::vector<VkRenderingAttachmentInfo> colors;
            VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            bool hasDepth = false;
            bool extentSet = false;
            for (size_t k = 0; k < pass.accesses.size(); ++k) {
                const AccessDecl& a = pass.accesses[k];
                if (!isAttachment(a.access)) continue;
                const Resource& r = m_resources[a.resource];
                if (!extentSet) {
                    ctx.extent = r.extent;
                    extentSet = true;
                }
                // compile 可能把「只读深度」改成「写并清空」：按改后的状态决定布局
                const bool depthAccess = a.access != Access::ColorWrite;
                const LoadOp load = cp.loadOps[k];
                VkRenderingAttachmentInfo info{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                info.imageView = r.view;
                info.loadOp = load == LoadOp::Clear ? VK_ATTACHMENT_LOAD_OP_CLEAR
                            : load == LoadOp::Load  ? VK_ATTACHMENT_LOAD_OP_LOAD
                                                    : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                info.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                info.clearValue = r.desc.clear;
                if (depthAccess) {
                    const bool readOnly = a.access == Access::DepthRead && load == LoadOp::Load;
                    info.imageLayout = readOnly ? VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
                    depth = info;
                    hasDepth = true;
                } else {
                    info.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    colors.push_back(info);
                }
            }
            VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
            ri.renderArea = {{0, 0}, ctx.extent};
            ri.layerCount = 1;
            ri.colorAttachmentCount = static_cast<uint32_t>(colors.size());
            ri.pColorAttachments = colors.data();
            ri.pDepthAttachment = hasDepth ? &depth : nullptr;
            vkCmdBeginRendering(cmd, &ri);

            // 负高度视口：把 Vulkan 的「Y 向下」翻转成「Y 向上」，
            // 这样 GLM 的投影矩阵和 glTF 模型都不用改。需要 Vulkan 1.1（已是核心功能）。
            VkViewport vp{0.0f, float(ctx.extent.height), float(ctx.extent.width), -float(ctx.extent.height), 0.0f, 1.0f};
            VkRect2D scissor{{0, 0}, ctx.extent};
            vkCmdSetViewport(cmd, 0, 1, &vp);
            vkCmdSetScissor(cmd, 0, 1, &scissor);

            pass.fn(ctx);
            vkCmdEndRendering(cmd);
        } else {
            pass.fn(ctx);
        }

        if (profiler) profiler->end(cmd, scope);
        rhi::endLabel(cmd);
    }

    // ---- 4. 帧末：清空没人写的外部资源，并过渡到要求的状态 ----
    emitBarriers(compiled.clearBarriers);
    for (uint32_t r : compiled.clears) {
        const VkClearColorValue black{{0.0f, 0.0f, 0.0f, 1.0f}};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        vkCmdClearColorImage(cmd, m_resources[r].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    }
    emitBarriers(compiled.finalBarriers);

    // ---- 5. 记下瞬态贴图的结束状态，供下一帧推导使用 ----
    for (size_t i = 0; i < m_resources.size(); ++i) {
        if (m_resources[i].imported) continue;
        Pooled& p = m_pool[m_resources[i].desc.name];
        p.state = compiled.endStates[i];
        p.lastWrite = compiled.endWrites[i];
        p.hasContent = m_resources[i].desc.persistent && compiled.endContent[i];
    }

    // GPU 耗时来自 profiler 读回的「两帧前」结果，按名字对应
    if (profiler)
        for (auto& rep : m_report)
            for (const auto& r : profiler->results())
                if (r.name == rep.name) rep.gpuMs = r.ms;
}

std::vector<RenderGraph::ResourceInfo> RenderGraph::resourceInfos() const {
    std::vector<ResourceInfo> out;
    for (const Resource& r : m_resources) out.push_back({r.desc.name, r.format, r.extent, r.imported});
    return out;
}

} // namespace eng::rg
