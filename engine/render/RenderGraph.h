#pragma once
// Render Graph：每帧声明「有哪些 pass、每个 pass 读写哪些贴图」，由图来负责：
//   1. 自动推导并插入 barrier（布局转换 + 执行/内存依赖）
//   2. 管理瞬态贴图（按需创建、跨帧复用、尺寸变化时重建）
//   3. pass 开关：被关掉或「没准备好」（例如 shader 编译失败）的 pass 不执行，
//      它的输出由默认贴图（黑/白/平坦法线）代替，下游 pass 照常运行
//
// 为什么要它：手写 barrier 是 Vulkan 最容易出错的部分——少一个就是偶发的画面闪烁，
// 多一个就是性能损失，而且 pass 一多、能开关之后，正确的 barrier 组合会随开关状态变化。
// 把「声明」和「推导」分开后，加一个 pass 只需要写它读写什么。
//
// 文件分两部分：
//   rg::compile() —— 纯逻辑，不碰 GPU，输入 pass 声明，输出 barrier 列表。单元测试直接测它。
//   RenderGraph   —— 每帧构建、分配真实贴图、录制命令。

#include "rhi/Device.h"
#include "rhi/Resources.h"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace eng {
struct FrameContext;
class DefaultResources;
namespace rhi { class GpuProfiler; }
} // namespace eng

namespace eng::rg {

enum class PassType { Graphics, Compute, Transfer };

// 一个 pass 对一个资源的用法。决定了 barrier 的目标状态（阶段、访问类型、布局）。
enum class Access : uint8_t {
    ColorWrite,   // 颜色附件（写，也可能读：混合 / LOAD）
    DepthWrite,   // 深度附件，测试 + 写入
    DepthRead,    // 深度附件，只测试不写（例如水面、粒子）
    Sampled,      // shader 里采样
    StorageRead,  // compute 读 storage image
    StorageWrite, // compute 写 storage image
    TransferSrc,  // 拷贝源（截图）
    TransferDst,  // 拷贝目标 / 清屏
};

enum class LoadOp { Load, Clear, DontCare };

// pass 被跳过时，读它输出的下游 pass 改为读哪张默认贴图
enum class Fallback : uint8_t { Black, White, FlatNormal, None };

bool isWrite(Access a);
bool isAttachment(Access a);
rhi::ImageState stateFor(Access a, PassType type);

// ======================= 纯逻辑部分（单元测试覆盖） =======================

struct AccessDecl {
    uint32_t resource = 0;
    Access access = Access::Sampled;
    LoadOp load = LoadOp::Load; // 只对附件写入有意义
};

struct PassDecl {
    std::string name;
    PassType type = PassType::Graphics;
    std::vector<AccessDecl> accesses;
    bool active = true; // false = 被关闭或没准备好
};

struct ResourceDecl {
    std::string name;
    rhi::ImageState initial;         // 本帧开始时的状态（瞬态贴图 = 上一帧结束时的状态）
    rhi::ImageState lastWrite;       // 最近一次写入的 stage/access（跨过中间的读取仍要记住，见 compile）
    bool hasContent = false;         // 本帧开始时是否已有有效内容（瞬态贴图总是 false）
    bool hasFinal = false;           // 是否要求帧末处于特定状态（交换链 → PRESENT）
    rhi::ImageState final;
    bool clearIfUnwritten = false;   // 帧末仍没被写过时先清屏（避免显示垃圾内存）
};

struct Barrier {
    uint32_t resource = 0;
    rhi::ImageState from, to;
};

struct CompiledPass {
    bool executed = false;
    std::vector<Barrier> barriers;       // 执行前要插入的 barrier
    std::vector<uint32_t> substituted;   // 这些资源没有内容，读时换成默认贴图
    std::vector<LoadOp> loadOps;         // 与 accesses 一一对应的「实际」load op
};

struct CompileResult {
    std::vector<CompiledPass> passes;
    std::vector<Barrier> clearBarriers;  // 帧末清屏前的 barrier（目标 TRANSFER_DST）
    std::vector<uint32_t> clears;        // 需要帧末清屏的资源
    std::vector<Barrier> finalBarriers;  // 帧末过渡到要求状态
    std::vector<rhi::ImageState> endStates;
    std::vector<rhi::ImageState> endWrites; // 每个资源帧末的「最近一次写入」，传给下一帧
};

CompileResult compile(const std::vector<PassDecl>& passes, const std::vector<ResourceDecl>& resources);

// ======================= GPU 部分 =======================

struct TextureHandle {
    uint32_t index = UINT32_MAX;
    bool valid() const { return index != UINT32_MAX; }
};

struct TextureDesc {
    std::string name;
    VkFormat format = VK_FORMAT_R16G16B16A16_SFLOAT;
    // 尺寸 = 交换链尺寸 × scale（例如半分辨率体积雾用 0.5）；width/height 非 0 时用绝对尺寸
    float scale = 1.0f;
    uint32_t width = 0;
    uint32_t height = 0;
    Fallback fallback = Fallback::Black;
    VkClearValue clear{};
};

class RenderGraph;

struct PassContext {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkExtent2D extent{};      // 第一个附件的尺寸（compute pass 为交换链尺寸）
    FrameContext* frame = nullptr;
    // 资源的 image view；没有内容的资源会返回默认贴图
    VkImageView view(TextureHandle h) const;
    const RenderGraph* graph = nullptr;
    uint32_t passIndex = 0;
};

class PassBuilder {
public:
    PassBuilder& color(TextureHandle t, LoadOp load = LoadOp::Clear);
    PassBuilder& depth(TextureHandle t, LoadOp load = LoadOp::Clear);
    PassBuilder& depthReadOnly(TextureHandle t);
    PassBuilder& sampled(TextureHandle t);
    PassBuilder& storageRead(TextureHandle t);
    PassBuilder& storageWrite(TextureHandle t);
    PassBuilder& transferSrc(TextureHandle t);
    PassBuilder& transferDst(TextureHandle t);
    // 执行条件（例如管线是否编译成功）；返回 false 时 pass 被跳过，按「关闭」处理
    PassBuilder& require(std::function<bool()> ready);
    PassBuilder& execute(std::function<void(PassContext&)> fn);

private:
    friend class RenderGraph;
    PassBuilder(RenderGraph& g, uint32_t index) : m_graph(g), m_index(index) {}
    RenderGraph& m_graph;
    uint32_t m_index;
};

struct PassReport {
    enum class Status { Ran, Disabled, NotReady };
    std::string name;
    Status status = Status::Ran;
    float gpuMs = 0.0f;
    std::vector<std::string> substituted;
};

class RenderGraph {
public:
    void init(rhi::Device& device, DefaultResources& defaults);
    void shutdown();

    // ---- 每帧构建 ----
    void beginFrame(VkExtent2D backbuffer, uint64_t frameNumber);
    TextureHandle createTexture(const TextureDesc& desc);
    // 外部资源（交换链图像）：给出初始状态与帧末要求的状态
    TextureHandle importTexture(const std::string& name, VkImage image, VkImageView view, VkFormat format, VkExtent2D extent,
                                const rhi::ImageState& initial, const rhi::ImageState& final);
    PassBuilder addPass(const std::string& name, PassType type);
    void execute(VkCommandBuffer cmd, FrameContext& frame, rhi::GpuProfiler* profiler);

    // ---- 持久的 pass 开关（ImGui 改它） ----
    bool& passEnabled(const std::string& name);
    const std::vector<PassReport>& lastReport() const { return m_report; }
    // 调试：当前帧各资源及其物理尺寸
    struct ResourceInfo {
        std::string name;
        VkFormat format;
        VkExtent2D extent;
        bool imported;
    };
    std::vector<ResourceInfo> resourceInfos() const;

private:
    friend class PassBuilder;
    friend struct PassContext;

    struct Resource {
        TextureDesc desc;
        bool imported = false;
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkExtent2D extent{};
        VkImageUsageFlags usage = 0;
        rhi::ImageState importInitial, importFinal;
    };
    struct Pass {
        std::string name;
        PassType type;
        std::vector<AccessDecl> accesses;
        std::function<bool()> ready;
        std::function<void(PassContext&)> fn;
    };
    // 跨帧复用的瞬态贴图
    struct Pooled {
        rhi::Image image;
        VkImageUsageFlags usage = 0;
        rhi::ImageState state;
        rhi::ImageState lastWrite;
        uint64_t lastUsedFrame = 0;
    };

    void addAccess(uint32_t pass, TextureHandle t, Access a, LoadOp load);
    void realizeTransients();
    VkImageView fallbackView(Fallback f) const;

    rhi::Device* m_device = nullptr;
    DefaultResources* m_defaults = nullptr;
    VkExtent2D m_backbuffer{};
    uint64_t m_frameNumber = 0;
    std::vector<Resource> m_resources;
    std::vector<Pass> m_passes;
    std::unordered_map<std::string, Pooled> m_pool;
    std::unordered_map<std::string, bool> m_enabled;
    std::vector<PassReport> m_report;
    // execute 期间：每个 pass 被替换为默认贴图的资源
    std::vector<std::vector<uint32_t>> m_substituted;
};

} // namespace eng::rg
