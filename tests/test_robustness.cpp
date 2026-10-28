// 故障路径测试：输入出错、任务抛异常、pass 被关掉时，系统应当「报错并继续」，而不是崩溃或静默出错。
#include "asset/AssetDatabase.h"
#include "core/Config.h"
#include "core/FileSystem.h"
#include "core/ImageIO.h"
#include "core/JobSystem.h"
#include "core/Time.h"
#include "render/RenderGraph.h"

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace eng;

TEST_CASE("Job 系统：任务抛异常不会终止进程，计数照常归零") {
    JobSystem jobs;
    jobs.init(2);
    auto counter = std::make_shared<JobCounter>();
    int ok = 0;
    jobs.submit([] { throw std::runtime_error("模拟的烘焙错误"); }, counter);
    jobs.submit([&ok] { ok = 1; }, counter);
    jobs.wait(*counter);
    CHECK(counter->done());
    CHECK(ok == 1);
    // 主线程队列同样有保护
    jobs.runOnMainThread([] { throw std::runtime_error("模拟的上传错误"); });
    CHECK(jobs.pumpMainThread(4) == 1);
    jobs.shutdown();
}

TEST_CASE("固定步长：非法设置被修正，不会出现未定义行为或死循环") {
    FixedStepSettings s;
    s.fixedHz = 0.0;
    s.maxStepsPerFrame = -3;
    s.maxFrameTime = -1.0; // std::clamp(x, 0, -1) 是未定义行为
    FixedStepLoop loop(s);
    CHECK(loop.settings().fixedHz >= 1.0);
    CHECK(loop.settings().maxStepsPerFrame >= 1);
    CHECK(loop.settings().maxFrameTime >= loop.fixedDt());
    CHECK(loop.advance(10.0) >= 1);
}

TEST_CASE("固定步长：补步上限丢弃的时间被统计") {
    FixedStepSettings s;
    s.fixedHz = 60.0;
    s.maxStepsPerFrame = 2;
    s.maxFrameTime = 1.0;
    FixedStepLoop loop(s);
    CHECK(loop.advance(0.5) == 2);
    CHECK(loop.droppedFrames() == 1);
    CHECK(loop.droppedSeconds() > 0.4);
}

TEST_CASE("配置：超出范围和 NaN 都被夹到合法值") {
    float fov = 0.0f;
    CHECK(clampSetting(fov, 10.0f, 150.0f, "fov"));
    CHECK(fov == 10.0f);
    float n = std::nanf("");
    CHECK(clampSetting(n, 0.1f, 1.0f, "nan"));
    CHECK(n == 0.1f);
    int steps = 5;
    CHECK_FALSE(clampSetting(steps, 1, 32, "steps"));
    CHECK(steps == 5);
}

TEST_CASE("Render Graph：先被图形 pass 采样、再被 compute 采样，compute 读者也要和写入同步") {
    using namespace rg;
    ResourceDecl tex;
    tex.name = "Tex";
    const std::vector<PassDecl> passes = {
        {"Write", PassType::Graphics, {{0, Access::ColorWrite, LoadOp::Clear}}, true},
        {"ReadGfx", PassType::Graphics, {{0, Access::Sampled}}, true},
        {"ReadCompute", PassType::Compute, {{0, Access::Sampled}}, true},
    };
    const CompileResult r = compile(passes, {tex});
    // 图形读者：正常的 RAW barrier（含布局转换）
    REQUIRE(r.passes[1].barriers.size() == 1);
    // compute 读者：布局不变，但要补一个「颜色写入 → compute 着色器读取」的依赖
    REQUIRE(r.passes[2].barriers.size() == 1);
    const Barrier& b = r.passes[2].barriers[0];
    CHECK((b.from.stage & VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT) != 0);
    CHECK((b.from.access & VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT) != 0);
    CHECK((b.to.stage & VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT) != 0);
    CHECK(b.from.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    CHECK(b.to.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // 同阶段的第二个读者不需要额外 barrier
    const std::vector<PassDecl> twice = {
        {"Write", PassType::Graphics, {{0, Access::ColorWrite, LoadOp::Clear}}, true},
        {"ReadA", PassType::Graphics, {{0, Access::Sampled}}, true},
        {"ReadB", PassType::Graphics, {{0, Access::Sampled}}, true},
    };
    CHECK(compile(twice, {tex}).passes[2].barriers.empty());
}

TEST_CASE("Render Graph：外部也会采样的贴图在写入者全被关掉时被真正清空，而不是只换 view") {
    using namespace rg;
    ResourceDecl viewport;
    viewport.name = "ViewportColor";
    viewport.materializeIfUnwritten = true;
    const std::vector<PassDecl> passes = {
        {"Tonemap", PassType::Graphics, {{0, Access::ColorWrite, LoadOp::DontCare}}, false}, // 被关掉
        {"UI", PassType::Graphics, {{0, Access::Sampled}}, true},
    };
    const CompileResult r = compile(passes, {viewport});
    const CompiledPass& ui = r.passes[1];
    CHECK(ui.substituted.empty()); // 不能只换 view：ImGui 用的是真实图像
    REQUIRE(ui.clears.size() == 1);
    REQUIRE(ui.clearBarriers.size() == 1);
    CHECK(ui.clearBarriers[0].to.layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    REQUIRE(ui.barriers.size() == 1); // 清屏之后转成采样布局
    CHECK(ui.barriers[0].from.layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    CHECK(ui.barriers[0].to.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // 普通瞬态贴图保持原来的行为：换成默认贴图
    ResourceDecl plain;
    plain.name = "Plain";
    CHECK(compile(passes, {plain}).passes[1].substituted.size() == 1);
}

TEST_CASE("资产：.meta 里类型写错用默认值，.meta 写坏时沿用之前的设置") {
    namespace stdfs = std::filesystem;
    const stdfs::path root = stdfs::temp_directory_path() / "eng_test_assets";
    stdfs::remove_all(root);
    stdfs::create_directories(root / "assets");
    const stdfs::path png = root / "assets" / "tile.png";
    std::vector<uint8_t> pixels(16 * 16 * 4, 128);
    REQUIRE(writePngRGBA8(png, 16, 16, pixels.data()));

    asset::AssetDatabase db;
    db.init(root / "assets", root / "cooked");
    db.scan();
    const auto guid = db.findByPath(png);
    REQUIRE(guid.has_value());

    // 1. compress 写成字符串：警告 + 用默认值，烘焙照常成功（以前会抛异常并终止后台线程）
    const stdfs::path meta = png.string() + ".meta";
    fs::writeText(meta, R"({"guid":")" + guid->toString() + R"(","type":"texture","settings":{"compress":"yes","generateMips":true,"usage":"color"}})");
    std::string error;
    CHECK(db.ensureCooked(*guid, nullptr, error));
    CHECK(error.empty());

    // 2. .meta 改成不合法的 JSON，并让源文件变新以触发重烘：沿用上次的设置，不抛异常
    fs::writeText(meta, "{ this is not json");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(writePngRGBA8(png, 16, 16, pixels.data()));
    CHECK(db.ensureCooked(*guid, nullptr, error));
    stdfs::remove_all(root);
}
