# 海洋魂类游戏 · 自研引擎

用 C++20 + Vulkan 1.3 搭建的学习型引擎，服务于一款以海洋为主场景的 3D 魂类动作冒险游戏。
亮点是 **水、光照、地形材质**，以及 **写实环境 + 二次元角色** 的混合渲染。

> 这个工程首先要「看得懂、改得动、改坏了不至于全崩」，其次才是功能多。设计理念与约束见 [CLAUDE.md](CLAUDE.md)。

## 环境

| 工具 | 版本（当前开发机） | 说明 |
|---|---|---|
| Visual Studio | 2026 Community（18.x，MSVC 14.50） | 需「使用 C++ 的桌面开发」工作负载；VS 2022 也可以 |
| CMake / Ninja | VS 自带 CMake 4.1 + Ninja | 无需单独安装、无需加 PATH |
| Vulkan SDK | 1.4.363 | 提供 validation layer 与 dxc |
| RenderDoc | 1.46（可选） | 缺失时 in-app 捕获自动禁用 |
| Git | 2.5x | |

检查环境：

```powershell
.\scripts\check_env.ps1
```

## 构建与运行

```powershell
.\scripts\build.ps1 -Config Debug            # 配置 + 构建
.\scripts\build.ps1 -Config Release -Test    # 构建 + 单元测试 + 冒烟测试
.\build\bin\Debug\sandbox.exe                # 运行演示程序
.\build\bin\Debug\sandbox.exe --frames 300   # 冒烟测试：跑 300 帧后退出，返回码 0 = 成功
```

也可以直接用 VS 2026「打开本地文件夹」，它会读取 `CMakePresets.json`。

## 模块地图

```
core ← platform ← rhi ← render ← 特性模块(terrain / ocean / medium / physics) ← sandbox
```

| 目录 | 职责 | 里程碑 |
|---|---|---|
| `engine/core` | 日志、断言、计时、文件、配置 | M1 |
| `engine/platform` | 窗口、输入 | M1 |
| `engine/math` | GLM 之上的小工具 | M1 |
| `engine/rhi` | Vulkan 薄封装 | M1–M2 |
| `engine/render` | Render Graph、Renderer、相机、材质、各 Pass | M2–M3 |
| `engine/debug` | ImGui 面板、GPU 计时、RenderDoc | M1–M2 |
| `engine/world` | 场景/实体 | M3 |
| `engine/terrain` | 高度图地形、地形材质 | M4 |
| `engine/physics` | PhysicsWorld（封装 Jolt） | M4 |
| `engine/ocean` | 海面模拟与渲染 | M5 |
| `engine/medium` | 介质系统 | M6 |
| `shaders/common` | 光照输入、BRDF、介质函数、色调映射 | M3 |
| `config/` | 可热加载的参数（JSON） | M1 |
| `docs/` | 每个里程碑一篇中文学习笔记 | 全程 |

## 从哪里开始读代码

随里程碑补充。

1. [CLAUDE.md](CLAUDE.md) —— 设计理念与约束
2. [docs/00-环境与工程约定.md](docs/00-环境与工程约定.md)
3. [sandbox/App.cpp](sandbox/App.cpp) —— 主循环：从这里看各模块如何串起来
4. [engine/core/Time.h](engine/core/Time.h) —— 固定步长 + 插值
5. [engine/rhi/Device.cpp](engine/rhi/Device.cpp) → [engine/rhi/Swapchain.cpp](engine/rhi/Swapchain.cpp) —— Vulkan 初始化
6. [engine/render/Renderer.cpp](engine/render/Renderer.cpp) —— 一帧的同步：fence、信号量、barrier
7. [docs/01-窗口与主循环.md](docs/01-窗口与主循环.md)
8. [tests/test_rendergraph.cpp](tests/test_rendergraph.cpp) —— 先看测试理解 barrier 规则
9. [engine/render/RenderGraph.cpp](engine/render/RenderGraph.cpp) —— `rg::compile()` 与执行
10. [engine/render/PipelineCache.cpp](engine/render/PipelineCache.cpp) —— 热重载与兜底
11. [engine/render/passes/](engine/render/passes/) + [shaders/](shaders/) —— 具体的 pass 与 shader
12. [docs/02-RHI与RenderGraph.md](docs/02-RHI与RenderGraph.md)

## 里程碑进度

- [x] **M0 环境检查**：工具链、git、README、CLAUDE.md
- [x] **M1 窗口与清屏**：GLFW + Vulkan 1.3 + 交换链、validation、缩放/最小化、ImGui 统计、固定步长主循环、config 热加载
- [x] **M2 RHI + Render Graph**：自动 barrier、瞬态贴图、pass 开关与默认资源、shader 热重载与错误材质、RenderDoc、GPU 计时、自由相机
- [ ] M3 网格与 PBR
- [ ] M4 地形与物理
- [ ] M5 海面
- [ ] M6 Medium + 水下 + 材质读取介质
- [ ] M7 风格化角色材质
