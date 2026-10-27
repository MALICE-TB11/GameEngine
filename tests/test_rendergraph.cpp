// Render Graph 的 barrier 推导测试：只测纯逻辑 rg::compile()，不需要 GPU。
#include "render/RenderGraph.h"

#include <doctest/doctest.h>

using namespace eng;
using namespace eng::rg;

namespace {

ResourceDecl transient(const char* name) {
    ResourceDecl r;
    r.name = name;
    return r; // 初始状态全空 + 无内容
}

ResourceDecl swapchain() {
    ResourceDecl r;
    r.name = "Backbuffer";
    r.initial = {VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_NONE, VK_IMAGE_LAYOUT_UNDEFINED};
    r.hasFinal = true;
    r.final = {VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR};
    r.clearIfUnwritten = true;
    return r;
}

PassDecl pass(const char* name, std::vector<AccessDecl> acc, bool active = true, PassType type = PassType::Graphics) {
    return {name, type, std::move(acc), active};
}

} // namespace

TEST_CASE("写后读：颜色附件 → 采样，插入布局转换和 RAW 依赖") {
    const std::vector<ResourceDecl> res = {transient("Color"), swapchain()};
    const std::vector<PassDecl> passes = {
        pass("Forward", {{0, Access::ColorWrite, LoadOp::Clear}}),
        pass("Tonemap", {{0, Access::Sampled}, {1, Access::ColorWrite, LoadOp::DontCare}}),
    };
    const CompileResult r = compile(passes, res);

    // Forward：Color 从 UNDEFINED（丢弃）→ COLOR_ATTACHMENT
    REQUIRE(r.passes[0].barriers.size() == 1);
    CHECK(r.passes[0].barriers[0].from.layout == VK_IMAGE_LAYOUT_UNDEFINED);
    CHECK(r.passes[0].barriers[0].to.layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    // Tonemap：Color → SHADER_READ_ONLY，src 必须包含颜色附件写入（RAW）
    REQUIRE(r.passes[1].barriers.size() == 2);
    const Barrier& b = r.passes[1].barriers[0];
    CHECK(b.resource == 0);
    CHECK(b.from.layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    CHECK(b.to.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    CHECK((b.from.stage & VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT) != 0);
    CHECK((b.from.access & VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT) != 0);
    // src 只保留写访问：读不需要 flush
    CHECK((b.from.access & VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT) == 0);
    CHECK((b.to.stage & VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT) != 0);

    // 交换链：帧末过渡到 PRESENT
    REQUIRE(r.finalBarriers.size() == 1);
    CHECK(r.finalBarriers[0].to.layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    CHECK(r.clears.empty());
}

TEST_CASE("读后读：同布局不插 barrier，之后的写会等待全部读者") {
    const std::vector<ResourceDecl> res = {transient("Tex")};
    const std::vector<PassDecl> passes = {
        pass("Produce", {{0, Access::StorageWrite}}, true, PassType::Compute),
        pass("ReadA", {{0, Access::Sampled}}, true, PassType::Graphics),
        pass("ReadB", {{0, Access::Sampled}}, true, PassType::Compute),
        pass("Overwrite", {{0, Access::StorageWrite}}, true, PassType::Compute),
    };
    const CompileResult r = compile(passes, res);
    CHECK(r.passes[1].barriers.size() == 1); // GENERAL → SHADER_READ_ONLY
    CHECK(r.passes[2].barriers.empty());     // 读后读，布局相同
    REQUIRE(r.passes[3].barriers.size() == 1);
    const Barrier& war = r.passes[3].barriers[0];
    // WAR：要同时等图形 pass 的片元着色器和 compute pass 的读取
    CHECK((war.from.stage & VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT) != 0);
    CHECK((war.from.stage & VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT) != 0);
    // 读不需要内存依赖；但隔着读者的「上一次写」（Produce）仍要包含进来（WAW）
    CHECK((war.from.access & VK_ACCESS_2_SHADER_SAMPLED_READ_BIT) == 0);
    CHECK(war.from.access == VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    CHECK(war.to.layout == VK_IMAGE_LAYOUT_GENERAL);
}

TEST_CASE("pass 被关闭：下游读取改用默认贴图，且不插 barrier") {
    const std::vector<ResourceDecl> res = {transient("Color"), swapchain()};
    const std::vector<PassDecl> passes = {
        pass("Forward", {{0, Access::ColorWrite, LoadOp::Clear}}, /*active=*/false),
        pass("Tonemap", {{0, Access::Sampled}, {1, Access::ColorWrite, LoadOp::DontCare}}),
    };
    const CompileResult r = compile(passes, res);
    CHECK_FALSE(r.passes[0].executed);
    CHECK(r.passes[0].barriers.empty());
    REQUIRE(r.passes[1].substituted.size() == 1);
    CHECK(r.passes[1].substituted[0] == 0);
    // 只剩交换链的那个 barrier
    REQUIRE(r.passes[1].barriers.size() == 1);
    CHECK(r.passes[1].barriers[0].resource == 1);
}

TEST_CASE("上游被关闭：下游的 LOAD 自动改为 CLEAR") {
    const std::vector<ResourceDecl> res = {swapchain()};
    const std::vector<PassDecl> passes = {
        pass("Tonemap", {{0, Access::ColorWrite, LoadOp::DontCare}}, false),
        pass("UI", {{0, Access::ColorWrite, LoadOp::Load}}),
    };
    const CompileResult r = compile(passes, res);
    CHECK(r.passes[1].loadOps[0] == LoadOp::Clear);
    // 没有内容可保留：旧布局写 UNDEFINED
    REQUIRE(r.passes[1].barriers.size() == 1);
    CHECK(r.passes[1].barriers[0].from.layout == VK_IMAGE_LAYOUT_UNDEFINED);
}

TEST_CASE("所有写交换链的 pass 都关闭：帧末先清屏再 PRESENT") {
    const std::vector<ResourceDecl> res = {swapchain()};
    const std::vector<PassDecl> passes = {pass("Tonemap", {{0, Access::ColorWrite, LoadOp::DontCare}}, false)};
    const CompileResult r = compile(passes, res);
    REQUIRE(r.clears.size() == 1);
    REQUIRE(r.clearBarriers.size() == 1);
    CHECK(r.clearBarriers[0].to.layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    REQUIRE(r.finalBarriers.size() == 1);
    CHECK(r.finalBarriers[0].from.layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    CHECK((r.finalBarriers[0].from.access & VK_ACCESS_2_TRANSFER_WRITE_BIT) != 0);
    CHECK(r.finalBarriers[0].to.layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
}

TEST_CASE("写后写（同一附件两个 pass 都 LOAD）也需要 barrier") {
    const std::vector<ResourceDecl> res = {transient("Color")};
    const std::vector<PassDecl> passes = {
        pass("Sky", {{0, Access::ColorWrite, LoadOp::Clear}}),
        pass("Forward", {{0, Access::ColorWrite, LoadOp::Load}}),
    };
    const CompileResult r = compile(passes, res);
    REQUIRE(r.passes[1].barriers.size() == 1);
    CHECK(r.passes[1].loadOps[0] == LoadOp::Load); // 有内容，保留 LOAD
    CHECK(r.passes[1].barriers[0].from.layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    CHECK((r.passes[1].barriers[0].from.access & VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT) != 0);
}

TEST_CASE("深度：写入 → 只读深度测试 → 采样") {
    const std::vector<ResourceDecl> res = {transient("Depth")};
    const std::vector<PassDecl> passes = {
        pass("Prepass", {{0, Access::DepthWrite, LoadOp::Clear}}),
        pass("Water", {{0, Access::DepthRead}}),
        pass("Fog", {{0, Access::Sampled}}),
    };
    const CompileResult r = compile(passes, res);
    REQUIRE(r.passes[1].barriers.size() == 1);
    CHECK(r.passes[1].barriers[0].to.layout == VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);
    REQUIRE(r.passes[2].barriers.size() == 1);
    CHECK(r.passes[2].barriers[0].to.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

TEST_CASE("只读深度但上游被关：改为清空并写入，不读未初始化的深度") {
    const std::vector<ResourceDecl> res = {transient("Depth")};
    const std::vector<PassDecl> passes = {
        pass("Prepass", {{0, Access::DepthWrite, LoadOp::Clear}}, false),
        pass("Water", {{0, Access::DepthRead}}),
    };
    const CompileResult r = compile(passes, res);
    CHECK(r.passes[1].substituted.empty());
    CHECK(r.passes[1].loadOps[0] == LoadOp::Clear);
    REQUIRE(r.passes[1].barriers.size() == 1);
    CHECK(r.passes[1].barriers[0].to.layout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
}

TEST_CASE("回归：写 → 只读 → 下一帧写（布局转换）要等最初那次写（WAW 跨过读者）") {
    // 实际遇到的同步验证报错：Forward 写深度 → Sky 只读深度 → 下一帧 Forward 清深度。
    // 第二个 barrier 只让写入对「读」可见，第三帧的布局转换（也是写）必须显式等待最初的写。
    const std::vector<PassDecl> passes = {
        pass("Forward", {{0, Access::DepthWrite, LoadOp::Clear}}),
        pass("Sky", {{0, Access::DepthRead}}),
    };
    const CompileResult frame1 = compile(passes, {transient("Depth")});

    ResourceDecl next = transient("Depth");
    next.initial = frame1.endStates[0];
    next.lastWrite = frame1.endWrites[0];
    const CompileResult frame2 = compile(passes, {next});
    REQUIRE(frame2.passes[0].barriers.size() == 1);
    const Barrier& b = frame2.passes[0].barriers[0];
    CHECK((b.from.access & VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT) != 0);
    CHECK((b.from.stage & VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT) != 0);
    CHECK(b.from.layout == VK_IMAGE_LAYOUT_UNDEFINED);
}

TEST_CASE("跨帧：瞬态贴图的初始阶段来自上一帧，第一次写入要等它（WAR）") {
    ResourceDecl color = transient("Color");
    color.initial = {VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const std::vector<PassDecl> passes = {pass("Forward", {{0, Access::ColorWrite, LoadOp::Clear}})};
    const CompileResult r = compile(passes, {color});
    REQUIRE(r.passes[0].barriers.size() == 1);
    CHECK(r.passes[0].barriers[0].from.stage == VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
    CHECK(r.passes[0].barriers[0].from.layout == VK_IMAGE_LAYOUT_UNDEFINED); // 内容丢弃
    CHECK(r.endStates[0].layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
}
