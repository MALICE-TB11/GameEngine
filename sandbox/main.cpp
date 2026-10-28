// Sandbox 入口：解析命令行，运行 App。
//
//   sandbox.exe                         正常运行
//   sandbox.exe --frames 300            冒烟测试：300 帧后退出，返回码 0 = 无错误
//   sandbox.exe --frames 60 --capture out.png   额外保存最后一帧截图
//   sandbox.exe --validation / --no-validation  强制开/关 validation layer
//   sandbox.exe --renderdoc / --no-renderdoc    强制加载/不加载 RenderDoc（冒烟测试默认不加载）
//   sandbox.exe --model path/to/model.glb       加载指定 glTF 模型代替演示遗迹
//   sandbox.exe --no-ui                         以游戏模式启动：场景全屏、隐藏编辑器（F1 切换）
//   sandbox.exe --select ruins/Pillar_R         启动时选中某个实体（检视器显示它）

#include "App.h"

#include "core/FileSystem.h"
#include "core/Log.h"

#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
    eng::log::init((eng::fs::projectRoot() / "logs" / "engine.log").string());

    eng::LaunchOptions opts;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--frames" && i + 1 < argc) {
            opts.frames = std::atoi(argv[++i]);
        } else if (arg == "--capture" && i + 1 < argc) {
            opts.capturePath = std::filesystem::absolute(argv[++i]);
        } else if (arg == "--validation") {
            opts.validation = true;
        } else if (arg == "--no-validation") {
            opts.validation = false;
        } else if (arg == "--no-ui") {
            opts.hideUI = true;
        } else if (arg == "--select" && i + 1 < argc) {
            opts.select = argv[++i];
        } else if (arg == "--model" && i + 1 < argc) {
            opts.model = argv[++i];
        } else if (arg == "--renderdoc") {
            opts.renderdoc = true;
        } else if (arg == "--no-renderdoc") {
            opts.renderdoc = false;
        } else {
            LOG_WARN("未知参数: {}", arg);
        }
    }
    if (!opts.capturePath.empty() && opts.frames <= 0) opts.frames = 60;

    eng::App app;
    const int code = app.run(opts);
    LOG_INFO("退出，返回码 {}", code);
    eng::log::shutdown();
    return code;
}
