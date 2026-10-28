// Sandbox 入口：解析命令行，运行 App。
//
//   sandbox.exe                         正常运行
//   sandbox.exe --frames 300            冒烟测试：300 帧后退出，返回码 0 = 无错误
//   sandbox.exe --frames 60 --capture out.png   额外保存最后一帧截图
//   sandbox.exe --validation / --no-validation  强制开/关 validation layer
//   sandbox.exe --renderdoc / --no-renderdoc    强制加载/不加载 RenderDoc（冒烟测试默认不加载）
//   sandbox.exe --model path/to/model.glb       额外放入一个 glTF 模型（外部文件只做临时登记）
//   sandbox.exe --scene scenes/x.scene.json     打开指定场景（默认见 config: scene.file）
//   sandbox.exe --no-ui                         以游戏模式启动：场景全屏、隐藏编辑器（F1 切换）
//   sandbox.exe --select ruins/Pillar_R         启动时选中某个实体（检视器显示它）
//   sandbox.exe --camera 0,80,200,-90,-25       编辑器相机初始位置 x,y,z 与朝向 yaw,pitch（度），截图用
//   sandbox.exe --terrain-lod                   地形按四叉树级别着色（截图/调参用）
//   sandbox.exe --disable-pass FXAA,Tonemap     启动时关闭这些 pass（故障演练：下游应自动用默认资源）
//   sandbox.exe --set ocean.windSpeed=15        临时覆盖一个配置项（可多次使用；不会写回 config）
//   sandbox.exe --selftest ocean                海洋 GPU 自测：回读 GPU 结果与 CPU 参考逐点比较、逐个关 pass 检查退化

#include "App.h"

#include "core/FileSystem.h"
#include "core/Log.h"

#include <cstdio>
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
        } else if (arg == "--scene" && i + 1 < argc) {
            opts.scene = argv[++i];
        } else if (arg == "--model" && i + 1 < argc) {
            opts.model = argv[++i];
        } else if (arg == "--camera" && i + 1 < argc) {
            float v[5] = {0.0f, 0.0f, 0.0f, -90.0f, 0.0f};
            if (sscanf_s(argv[++i], "%f,%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3], &v[4]) >= 3)
                opts.camera = std::array<float, 5>{v[0], v[1], v[2], v[3], v[4]};
            else
                LOG_WARN("--camera 格式应为 x,y,z[,yaw,pitch]");
        } else if (arg == "--set" && i + 1 < argc) {
            // --set section.key=value：本次运行临时覆盖一个配置项（不写回文件），value 按 JSON 解析，失败就当字符串
            const std::string kv = argv[++i];
            const size_t dot = kv.find('.'), eq = kv.find('=');
            if (dot != std::string::npos && eq != std::string::npos && dot < eq)
                opts.overrides.push_back({kv.substr(0, dot), kv.substr(dot + 1, eq - dot - 1), kv.substr(eq + 1)});
            else
                LOG_WARN("--set 格式应为 section.key=value，例如 --set ocean.mode=0");
        } else if (arg == "--terrain-lod") {
            opts.terrainLod = true;
        } else if (arg == "--disable-pass" && i + 1 < argc) {
            // 逗号分隔的 pass 名，例如 --disable-pass FXAA,Tonemap：验证「pass 被关掉时下游不崩、不报 validation」
            std::string list = argv[++i];
            for (size_t start = 0; start <= list.size();) {
                const size_t comma = std::min(list.find(',', start), list.size());
                if (comma > start) opts.disabledPasses.push_back(list.substr(start, comma - start));
                start = comma + 1;
            }
        } else if (arg == "--selftest" && i + 1 < argc) {
            opts.selfTest = argv[++i];
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
