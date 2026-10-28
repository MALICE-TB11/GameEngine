# 自研游戏引擎

用 C++20 + Vulkan 1.3 搭建的学习型引擎，服务于一款以海洋为主场景的 3D 魂类动作冒险游戏。
亮点是 **水、大气系统、光照、地形材质**，以及 **写实环境 + 二次元** 的混合渲染。


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
| `--scene scenes/x.scene.json` | 打开指定场景（默认见 `config/engine.json` 的 `scene.file`，当前为 `scenes/island.scene.json`） |
| `--model path.glb` | 额外放入一个 glTF 模型（外部文件只做临时登记，不复制进工程） |
| `--no-ui` | 以游戏模式启动：第三人称操控角色、隐藏编辑器（运行中按 F1 切换） |
| `--select 名字` | 启动时选中某个实体，例如 `--select ruins/Pillar_R` |
| `--camera x,y,z,yaw,pitch` | 编辑器相机的初始位置与朝向（度），截图用，例如 `--camera 60,120,260,-110,-22` |
| `--terrain-lod` | 地形按四叉树级别着色 |
| `--set section.key=value` | 本次运行临时覆盖一个配置项，可多次使用，例如 `--set ocean.windSpeed=15 --set ocean.mode=0` |
| `--disable-pass A,B` | 启动时关闭这些 pass（故障演练，例如 `--disable-pass Tonemap,FXAA`），见 [docs/稳定性与故障演练.md](docs/稳定性与故障演练.md) |
| `--selftest ocean` | 海洋 GPU 自测：回读 GPU 结果与 CPU 参考逐点比较、逐个关 pass 检查退化、暂停与拖动参数，结束后退出（返回码 0 = 全部通过） |

常用操作：视口内右键拖动 + WASD/QE 飞行，左键选择，F 聚焦，F1 编辑器/游戏模式，F5/F6 暂停/单步模拟，F9 截图，F12 RenderDoc 捕获，Ctrl+S 保存配置，Ctrl+R 重编 shader。「帮助 → 快捷键与操作」里有完整列表。
游戏模式（F1）：WASD 移动、Shift 冲刺、空格跳、鼠标转视角、滚轮调远近、R 回到出生点。

也可以直接用 VS 2026「打开本地文件夹」，它会读取 `CMakePresets.json`（需要先在 Developer PowerShell 中运行一次 `build.ps1`，或让 VS 自行配置）。

重新生成测试模型：`python tools/make_test_assets.py`（输出 `assets/models/ruins.glb`，已提交到仓库）。
重新生成地形分层贴图：`python tools/make_terrain_textures.py`（需要 numpy；输出 `assets/terrain/*.png`，已提交到仓库）。
重新生成泡沫细节贴图：`python tools/make_ocean_textures.py`（输出 `assets/ocean/foam_data.png`）。

**资产**：把 `.glb / .gltf / .png` 放进 `assets/`（或直接拖进编辑器窗口），启动时自动生成 `.meta`（GUID + 导入设置，请一起提交），首次使用时在后台烘焙到 `build/cooked/`（网格优化 + LOD、贴图 BC7/BC5 压缩）。场景保存在 `scenes/*.scene.json`。详见 [docs/04-资源层.md](docs/04-资源层.md)。

## 模块地图

```
math ← core ← platform ← rhi ← render ← asset ← world / debug / 特性模块(terrain / ocean / medium / physics) ← editor ← sandbox
physics 只依赖 core（Jolt 是私有依赖）；world 用 physics 做场景同步；特性模块通过 RenderFeature 接口挂进渲染器
```

| 目录 | 职责 | 里程碑 |
|---|---|---|
| `engine/core` | 日志、断言、文件监视、JSON 配置、固定步长、图片读写、进程调用、Job 系统、GUID、二进制读写 | M1–M3.5 |
| `engine/asset` | 资源层：资产数据库（GUID/.meta）、烘焙（LOD、BC 压缩）、句柄与后台加载、热重载 | M3.5 |
| `engine/platform` | 窗口、输入 | M1 |
| `engine/math` | GLM 之上的小工具：变换、反向 Z 投影 | M1 |
| `engine/rhi` | Vulkan 薄封装：设备、交换链、资源、上传、描述符、管线、GPU 计时 | M1–M2 |
| `engine/render` | Render Graph、Renderer、管线缓存与热重载、相机、网格、材质、glTF、级联阴影、各 pass（阴影/前向/天空/色调映射/FXAA）、RenderFeature 接口 | M2–M4 |
| `engine/world` | 场景/实体（固定步长 + 插值）、LOD 选择、场景文件读写、场景 ↔ 物理同步 | M3–M4 |
| `engine/debug` | ImGui 初始化与字体、日志/性能/Render Graph/Shader 面板、RenderDoc | M1–M2 |
| `engine/editor` | 编辑器外壳：菜单、工具栏、状态栏、停靠布局、视口点选、大纲、检视器、环境光照、地形与物理面板 | M3 后 |
| `engine/terrain` | 程序化岛屿高度场、四叉树 LOD + 裙边、分层材质（高度混合 + 三平面） | M4 |
| `engine/physics` | PhysicsWorld（封装 Jolt）：形状、刚体、射线、角色控制器 | M4 |
| `engine/ocean` | 海浪频谱（JONSWAP/TMA，CPU）、FFT 三级联 / Gerstner 参考实现、泡沫累积、水面着色、局部交互（波纹）的 CPU 参考 | M5 |
| `engine/medium` | 介质系统 | M6 |
| `shaders/common` | 帧常量、网格输入、SurfaceData、光照输入、BRDF、着色模型、天空、介质接口、色调映射 | M2–M3 |
| `shaders/passes` | 全屏 pass（天空、色调映射、FXAA）、阴影深度 | M2–M4 |
| `shaders/materials` | 材质（PBR、错误材质） | M2–M3 |
| `shaders/terrain` | 地形顶点生成（SV_VertexID）、分层材质、地形阴影 | M4 |
| `shaders/ocean` | 频谱演化、groupshared 逆 FFT、泡沫累积、波纹（波动方程 / 浅水方程）、水面着色 | M5 |
| `sandbox/` | 演示程序：主循环胶水、玩家与第三人称相机 | M1 起 |
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
16. [docs/04-资源层.md](docs/04-资源层.md) → [engine/asset/AssetManager.cpp](engine/asset/AssetManager.cpp) → [engine/asset/Cookers.cpp](engine/asset/Cookers.cpp) —— 资产从磁盘到 GPU
17. [docs/05-地形与物理.md](docs/05-地形与物理.md) → [engine/render/Shadows.cpp](engine/render/Shadows.cpp) → [shaders/common/shadow.hlsli](shaders/common/shadow.hlsli) —— 级联阴影
18. [engine/terrain/TerrainQuadtree.cpp](engine/terrain/TerrainQuadtree.cpp) → [shaders/terrain/terrain_common.hlsli](shaders/terrain/terrain_common.hlsli) → [shaders/terrain/terrain.hlsl](shaders/terrain/terrain.hlsl) —— 地形 LOD 与分层材质
19. [engine/physics/PhysicsWorld.h](engine/physics/PhysicsWorld.h) → [engine/world/ScenePhysics.cpp](engine/world/ScenePhysics.cpp) → [sandbox/Player.cpp](sandbox/Player.cpp) —— 物理封装、场景同步、角色
20. [docs/稳定性与故障演练.md](docs/稳定性与故障演练.md) → [tests/test_robustness.cpp](tests/test_robustness.cpp) —— 失败时该怎样表现，以及怎样验证
21. [docs/06-海洋.md](docs/06-海洋.md) → [engine/ocean/OceanSpectrum.cpp](engine/ocean/OceanSpectrum.cpp) → [shaders/ocean/ocean_evolve.hlsl](shaders/ocean/ocean_evolve.hlsl) → [ocean_fft.hlsl](shaders/ocean/ocean_fft.hlsl) → [ocean_surface.hlsl](shaders/ocean/ocean_surface.hlsl) —— 从频谱到水面；再读 [OceanRipples.h](engine/ocean/OceanRipples.h) → [ocean_ripples.hlsl](shaders/ocean/ocean_ripples.hlsl) —— 玩家激起的波纹

## 里程碑进度

- [x] **M0 环境检查**：工具链、git、README、CLAUDE.md
- [x] **M1 窗口与清屏**：GLFW + Vulkan 1.3 + 交换链、validation、缩放/最小化、ImGui 统计、固定步长主循环、config 热加载
- [x] **M2 RHI + Render Graph**：自动 barrier、瞬态贴图、pass 开关与默认资源、shader 热重载与错误材质、RenderDoc、GPU 计时、自由相机
- [x] **M3 网格与 PBR**：glTF、shader 公共库与 `SurfaceData → ShadingModel`、Cook-Torrance、解析天空、ACES / AgX、调试视图、材质编辑
- [x] **编辑器界面**：仿 UE/Unity 停靠布局、离屏视口、点选与聚焦、大纲、检视器、环境光照（太阳方位盘、时间预设）、日志/性能/Render Graph/Shader 面板、状态栏
- [x] **M3.5 资源层**：Job 系统、GUID 与 .meta、烘焙（meshoptimizer LOD、BC7/BC5 压缩）、句柄 + 后台加载 + 兜底 + 热重载、场景保存/加载、拖放导入、按实体材质覆盖
- [x] **M4 地形与物理**：级联阴影（4 级联图集、纹素对齐、法线偏移、PCF）、FXAA、RenderFeature 接口；程序化岛屿高度场、四叉树 LOD + 裙边、分层材质（高度混合、三平面岩石、远近双尺度）；Jolt 物理封装、场景实体自动生成碰撞体（静态网格/动态凸包）、角色控制器 + 第三人称相机
- [x] **稳定性修整**：场景文件损坏不再被覆盖、材质检视器不再持有悬空指针、后台任务异常不再终止进程、上传与读后读同步、视口默认贴图、地形三表面统一、配置范围检查、故障演练清单与帧时间 P95/P99
- [x] **M5 海面**：JONSWAP/TMA 频谱、三级联 256² GPU 逆 FFT（优化）与 Gerstner（参考）可切换、雅可比泡沫累积 + 岸边泡沫、菲涅尔反射 + RGB 吸收折射 + 太阳高光 + 浪尖透光；Render Graph 数组/持久贴图
- [x] M5 第二轮审查：Render Graph「必需输入」规则（失败沿计算链传播）、FFT 传播方向与 Gerstner 统一、浅水波数求解、Nyquist 串扰、海浪走模拟时间、频谱后台生成；GPU 结果校验 `--selftest ocean`
- [x] M5 后续 · 局部交互（参考）：以玩家为中心的二维波动方程（物理步推进、地形作反射边界、边缘吸收层、偶极子扰动源、尾迹泡沫），GPU 与 CPU 逐格校验
- [x] M5 后续 · 局部交互的第二种求解器：浅水方程（交错网格有符号流量、水量守恒、冲上沙滩、流速），与波动方程在面板切换；两者的 GPU 结果都与 CPU 逐格校验；扰动源统一为体积流量
- [x] 构建：`build.ps1` 固定控制台代码页并检查 Ninja 的头文件依赖（UTF-8 终端里会悄悄丢失，导致增量构建漏编译）
- [ ] M6 Medium + 水下 + 材质读取介质
- [ ] M7 风格化角色材质

## 架构与路线图

五层架构、资源层设计、**海洋系统**（FFT 频谱 + 波动方程/浅水方程交互 + 泡沫累积）、**大气与天气系统**（物理大气、体积云、雨、闪电、天气渐变）以及后续里程碑顺序，见 [docs/架构与路线图.md](docs/架构与路线图.md)。

## 后续里程碑：主要文件与风险

| 里程碑 | 主要文件（计划） | 风险点 |
|---|---|---|
| M6 Medium + 水下 | `engine/medium/Medium`、`shaders/common/medium.hlsli`（已预留接口）、水下后处理 pass、浮力 | CPU 端水面高度：FFT 结果在 GPU 上，需要低精度 CPU 近似或异步回读（延迟 2–3 帧），两者取舍需要确认 |
| M7 toon 角色 | `shaders/common/shading.hlsli` 的 `ShadeToon`、描边 pass、角色材质 | 描边方案（背面外扩 vs 屏幕空间）；toon 与 PBR 在同一光照下的亮度一致性；角色模型资源来源 |
