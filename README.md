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
| Python | 3.x（可选） | 只用于重新生成测试资源 |
| Git | 2.5x | |

检查环境：

```powershell
.\scripts\check_env.ps1
```

## 构建与运行

```powershell
.\scripts\build.ps1 -Config Debug            # 配置 + 构建（自动修正 shader 文件编码）
.\scripts\build.ps1 -Config Release -Test    # 构建 + 单元测试 + 冒烟测试
.\build\bin\Debug\sandbox.exe                # 运行演示程序
```

`sandbox.exe` 的命令行参数：

| 参数 | 作用 |
|---|---|
| `--frames N` | 冒烟测试：跑 N 帧（中途缩放窗口）后退出，返回码 0 = 无错误日志、无 validation 报错 |
| `--capture out.png` | 保存最后一帧截图 |
| `--validation` / `--no-validation` | 强制开/关 validation layer（Debug 默认开） |
| `--renderdoc` / `--no-renderdoc` | 强制加载/不加载 RenderDoc（F12 捕获） |
| `--model path.glb` | 加载指定 glTF 模型代替演示遗迹 |
| `--no-ui` | 以游戏模式启动：场景全屏、隐藏编辑器（运行中按 F1 切换） |
| `--select 名字` | 启动时选中某个实体，例如 `--select ruins/Pillar_R` |

界面是仿 UE / Unity 的停靠式编辑器（场景大纲、视口、检视器、环境光照、日志、性能……），说明见 [docs/编辑器界面.md](docs/编辑器界面.md)。
常用操作：视口内右键拖动 + WASD/QE 飞行，左键选择，F 聚焦，F1 编辑器/游戏模式，F5/F6 暂停/单步模拟，F9 截图，F12 RenderDoc 捕获，Ctrl+S 保存配置，Ctrl+R 重编 shader。「帮助 → 快捷键与操作」里有完整列表。

也可以直接用 VS 2026「打开本地文件夹」，它会读取 `CMakePresets.json`（需要先在 Developer PowerShell 中运行一次 `build.ps1`，或让 VS 自行配置）。

重新生成测试模型：`python tools/make_test_assets.py`（输出 `assets/models/ruins.glb`，已提交到仓库）。

## 模块地图

```
math ← core ← platform ← rhi ← render ← world / debug / 特性模块(terrain / ocean / medium / physics) ← editor ← sandbox
```

| 目录 | 职责 | 里程碑 |
|---|---|---|
| `engine/core` | 日志、断言、文件监视、JSON 配置、固定步长、图片读写、进程调用 | M1–M2 |
| `engine/platform` | 窗口、输入 | M1 |
| `engine/math` | GLM 之上的小工具：变换、反向 Z 投影 | M1 |
| `engine/rhi` | Vulkan 薄封装：设备、交换链、资源、上传、描述符、管线、GPU 计时 | M1–M2 |
| `engine/render` | Render Graph、Renderer、管线缓存与热重载、相机、网格、材质、glTF、各 pass | M2–M3 |
| `engine/world` | 场景/实体（固定步长 + 插值） | M3 |
| `engine/debug` | ImGui 初始化与字体、日志/性能/Render Graph/Shader 面板、RenderDoc | M1–M2 |
| `engine/editor` | 编辑器外壳：菜单、工具栏、状态栏、停靠布局、视口点选、大纲、检视器、环境光照 | M3 后 |
| `engine/terrain` | 高度图地形、地形材质 | M4 |
| `engine/physics` | PhysicsWorld（封装 Jolt） | M4 |
| `engine/ocean` | 海面模拟与渲染 | M5 |
| `engine/medium` | 介质系统 | M6 |
| `shaders/common` | 帧常量、网格输入、SurfaceData、光照输入、BRDF、着色模型、天空、介质接口、色调映射 | M2–M3 |
| `shaders/passes` | 全屏 pass（天空、色调映射） | M2–M3 |
| `shaders/materials` | 材质（PBR、错误材质） | M2–M3 |
| `config/` | 可热加载的参数（JSON） | M1 起 |
| `tools/` | 资源生成脚本、界面字形扫描（`update_ui_glyphs.py`） | M3 起 |
| `docs/` | 每个里程碑一篇中文学习笔记 | 全程 |

## 从哪里开始读代码

1. [CLAUDE.md](CLAUDE.md) —— 设计理念与约束
2. [docs/00-环境与工程约定.md](docs/00-环境与工程约定.md)
3. [sandbox/App.cpp](sandbox/App.cpp) —— 主循环：从这里看各模块如何串起来
4. [engine/core/Time.h](engine/core/Time.h) —— 固定步长 + 插值
5. [engine/rhi/Device.cpp](engine/rhi/Device.cpp) → [engine/rhi/Swapchain.cpp](engine/rhi/Swapchain.cpp) —— Vulkan 初始化
6. [engine/render/Renderer.cpp](engine/render/Renderer.cpp) —— 一帧的同步：fence、信号量；Render Graph 的组装
7. [docs/01-窗口与主循环.md](docs/01-窗口与主循环.md)
8. [tests/test_rendergraph.cpp](tests/test_rendergraph.cpp) —— 先看测试理解 barrier 规则
9. [engine/render/RenderGraph.cpp](engine/render/RenderGraph.cpp) —— `rg::compile()` 与执行
10. [engine/render/PipelineCache.cpp](engine/render/PipelineCache.cpp) —— 热重载与兜底
11. [docs/02-RHI与RenderGraph.md](docs/02-RHI与RenderGraph.md)
12. [shaders/materials/pbr_standard.hlsl](shaders/materials/pbr_standard.hlsl) → [shaders/common/shading.hlsli](shaders/common/shading.hlsli) —— 材质如何填 SurfaceData、光照在哪里计算
13. [engine/render/GltfLoader.cpp](engine/render/GltfLoader.cpp) —— 模型加载
14. [docs/03-网格与PBR.md](docs/03-网格与PBR.md)
15. [docs/编辑器界面.md](docs/编辑器界面.md) → [engine/editor/Editor.cpp](engine/editor/Editor.cpp) —— 编辑器外壳与视口

## 里程碑进度

- [x] **M0 环境检查**：工具链、git、README、CLAUDE.md
- [x] **M1 窗口与清屏**：GLFW + Vulkan 1.3 + 交换链、validation、缩放/最小化、ImGui 统计、固定步长主循环、config 热加载
- [x] **M2 RHI + Render Graph**：自动 barrier、瞬态贴图、pass 开关与默认资源、shader 热重载与错误材质、RenderDoc、GPU 计时、自由相机
- [x] **M3 网格与 PBR**：glTF、shader 公共库与 `SurfaceData → ShadingModel`、Cook-Torrance、解析天空、ACES / AgX、调试视图、材质编辑
- [x] **编辑器界面**：仿 UE/Unity 停靠布局、离屏视口、点选与聚焦、大纲、检视器、环境光照（太阳方位盘、时间预设）、日志/性能/Render Graph/Shader 面板、状态栏
- [ ] M3.5 资源层（见 [架构与路线图](docs/架构与路线图.md)）
- [ ] M4 地形与物理
- [ ] M5 海面
- [ ] M6 Medium + 水下 + 材质读取介质
- [ ] M7 风格化角色材质

## 架构与路线图

五层架构、资源层设计、**海洋系统**（FFT 频谱 + 波动方程/浅水方程交互 + 泡沫累积）、**大气与天气系统**（物理大气、体积云、雨、闪电、天气渐变）以及后续里程碑顺序，见 [docs/架构与路线图.md](docs/架构与路线图.md)。

## 后续里程碑：主要文件与风险

| 里程碑 | 主要文件（计划） | 风险点 |
|---|---|---|
| M4 地形与物理 | `engine/terrain/{Heightfield,TerrainLod,TerrainPass}`、`shaders/materials/terrain.hlsl`、`engine/physics/PhysicsWorld`、`world` 实体加刚体句柄 | Jolt 默认 `/MT` 与 `/GR-`，需关闭 `USE_STATIC_MSVC_RUNTIME_LIBRARY`；四叉树 LOD 接缝（裙边或索引缝合）；heightfield 碰撞与渲染高度的采样一致性；CharacterVirtual 在台阶/斜坡的参数调节 |
| M5 海面 | `engine/ocean/{OceanSim,Gerstner,FftOcean,OceanPass}`、`shaders/passes/ocean_*.hlsl`、compute 管线 | Render Graph 目前只跟踪图像，FFT 中间缓冲需要加入缓冲跟踪；Gerstner 与 FFT 两套接口一致；海面与地形相交处的泡沫需要深度/水深数据；远处海面的 LOD 与闪烁 |
| M6 Medium + 水下 | `engine/medium/Medium`、`shaders/common/medium.hlsli`（已预留接口）、水下后处理 pass、浮力 | CPU 端水面高度：FFT 结果在 GPU 上，需要低精度 CPU 近似或异步回读（延迟 2–3 帧），两者取舍需要确认 |
| M7 toon 角色 | `shaders/common/shading.hlsli` 的 `ShadeToon`、描边 pass、角色材质 | 描边方案（背面外扩 vs 屏幕空间）；toon 与 PBR 在同一光照下的亮度一致性；角色模型资源来源 |
