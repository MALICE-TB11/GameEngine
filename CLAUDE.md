# CLAUDE.md — 海洋魂类游戏自研引擎

本文件每次会话自动读取。修改代码前先读这里；与本文件冲突的改动需要先问用户。

## 项目定位
- 用户是有 C++ 基础的本科生，工程既是后续海洋题材 3D 魂类游戏的基础，也是学习参考。
- 用户会自己改各层代码（尤其 shader）：**可读性、可修改性、容错性 > 功能数量**。
- 引擎亮点：水、光照、地形材质；写实环境 + 二次元角色的混合渲染。

## 核心设计理念
1. **介质是一等公民**：`Medium::Query(worldPos)` 返回介质类型（Air/Water/Solid）、水面高度与法线、水深、到固体表面距离、吸收系数、流速。Medium 不复制数据，只组合数据源（ocean 提供水，terrain + physics 提供固体）。渲染、物理、音效、AI 都通过它获取环境信息。
2. **光即叙事，深度即难度**：少量强光源 + 体积散射；水下按 RGB 分别做 Beer–Lambert 吸收。
3. **材质读取介质**：PBR 与 toon 材质都通过 `shaders/common/` 的共享函数拿介质信息（水线湿润、水下焦散、按深度的附着层）。
4. **风格化 = 物理光照之上的映射**：所有材质拿同一份光照输入（`LightingInputs`）；环境走 PBR，角色走 ramp/toon 重映射。**角色不允许使用独立光照。**

## 技术栈
- C++20，CMake ≥ 3.25（当前用 VS 自带 CMake 4.1 + Ninja Multi-Config），MSVC（VS 2026 / 18.x 工具集 14.50）
- 依赖全部 CMake FetchContent，不用 vcpkg：GLFW、volk、vk-bootstrap、VMA、GLM、Dear ImGui(docking)、spdlog、fastgltf、stb_image、Jolt Physics、doctest
- Vulkan 1.3：dynamic rendering + synchronization2，不用 VkRenderPass/VkFramebuffer
- Shader：HLSL → dxc（Vulkan SDK 自带）→ SPIR-V；CMake 内置编译步骤，运行时支持热重载
- 物理：Jolt，经 `PhysicsWorld` 薄封装，玩法代码不接触 Jolt 类型

## 模块依赖（单向，禁止反向 include）
```
core ← platform ← rhi ← render ← asset ← 特性模块(terrain / ocean / medium / physics …) / world / debug ← editor ← sandbox
math 只依赖 GLM，任何层都可用；debug 可依赖 render；editor 在最上层，引擎其他模块都不依赖它
```
- 特性模块之间**不互相 include**，通过 `medium/`、`world/` 的共享接口交流。
- `physics` 只依赖 `core`，Jolt 是 PRIVATE 依赖：**公开头文件里不出现 Jolt 类型**（PIMPL）。`world` 依赖 `physics` 做场景同步（`ScenePhysics`）。
- 特性模块（地形、海洋……）实现 `RenderFeature`，由 App 注册给 Renderer；Renderer 不 include 任何特性模块。
- 新文件放到对应模块目录，CMake target 名为 `engine_<module>`。

## 容错约定
- 每个特性模块：小接口 + **参考实现**（简单、正确）+ **优化实现**，ImGui 可切换对比。
- Render Graph 中每个 pass 可在 ImGui 开关；关闭/失败的 pass 输出由默认资源（黑/白/平坦法线）代替，下游不崩溃。
- Shader 热重载失败时保留旧 pipeline，错误显示在 ImGui；缺失/出错材质用醒目错误材质（品红棋盘）。
- Shader 分层：`common/` 公共函数；材质遵循 `SurfaceData → ShadingModel` 固定接口。
- 可调参数放 `config/*.json`，ImGui 修改后可保存、可热加载；**不要硬编码魔法数字**（物理常数与公式里的系数除外，需注释出处）。
- 物理固定步长，渲染插值。
- **可恢复的失败：报错、保留旧的（或用默认的）、继续运行；绝不覆盖用户数据。** 读不出来的文件（场景、`.meta`、配置）不要拿默认内容写回原路径。
- 跨帧只存句柄（+ 下标 + 版本），不存指向资产内部的指针（`Material*`、`GpuMesh*`、`rhi::Image*`）；指针只在本帧内用。
- 后台任务在自己的边界把异常转成失败结果，并保证回到主线程收尾（`JobSystem` 的 `runGuarded` 只是最后一道防线）。
- 配置数值进入引擎前统一做范围检查（`App::sanitizeSettings` / `clampSetting`）；核心类（如 `FixedStepLoop`）自己也要保证不变量。
- 图之外的代码会直接采样的 Render Graph 贴图（例如编辑器视口）设 `TextureDesc::externalRead`。
- 地形的渲染网格、`Heightfield::heightAt`、Jolt 高度场必须是同一个三角形表面（每格沿 (x,z)→(x+1,z+1) 对角线切开）。
- 海洋的两种实现（Gerstner / FFT）共用 `OceanSpectrum` 的同一份频谱与位移约定（y = a cos θ，水平 = −Q a d̂ sin θ，θ = k·x − ωt，FFT 演化用 e^{−iωt}），改一边要同步另一边，并跑 `sandbox --selftest ocean`（GPU 结果与 CPU 参考 `evaluateCascade` 逐点比较）。
- 波纹（局部交互）的 GPU 版与 CPU 参考一行对一行（`ocean_ripples.hlsl` ↔ `rippleStep`，`ocean_shallow_flux/depth.hlsl` ↔ `shallowStep`），改一边同步另一边；每个物理固定步一步（`OceanRenderer::queueRippleStep`）；扰动源是体积流量（m/s），走动用净体积为零的偶极子；吸收层只阻尼速度 / 流量。
- 世界里随时间变化的东西（海浪、泡沫）用 `RenderView::simTime/simDt`（受暂停/时间缩放控制），不用墙钟 `time`；传给 GPU 的海洋时间先用 `oceanPhaseTime` 取模。
- 需要读「上一帧结果」的模拟贴图用 `TextureDesc::persistent`（A/B 交替命名），而且只放历史状态，不放「本帧输出」（否则 pass 被跳过时下游读到旧数据）；多层数据用 `TextureDesc::layers`（shader 里是 Texture2DArray）。
- storage 读取、拷贝源是**必需输入**：没有内容时 Render Graph 自动跳过该 pass、输出不标记，不需要在 execute 里检查。计算链的每一步用不同的逻辑贴图，不要让后一步覆盖前一步的贴图。
- shader 读「采样声明」的输入时可能拿到 1×1 默认贴图：不能用 `Load`/整数下标，用 `SampleLevel` + clamp，或 `ctx.hasContent()` 判断后用 push constant 传标志。
- 「validation 无报错」不等于输出正确：有 CPU 参考的 GPU 计算要做数值校验（回读 + 逐点比较）。
- 程序生成的网格要检查绕序（正面朝向），否则 `SV_IsFrontFace` 会让法线翻转。
- 参考仓库（HPWater MPL 2.0、HPVolumeCloud MIT）只读思路，不复制代码。

## 调试设施
validation layer（Debug 默认开）、所有 Vulkan 对象 debug name、RenderDoc in-app 捕获（F12）、每个 pass 的 GPU timestamp。

## 代码风格
- 注释写「为什么」，Vulkan 同步/barrier/描述符等难点要讲清楚；注释用中文。
- 不要过度抽象：每个抽象要说明它服务于后面哪个功能，能直白就直白。
- 扩展点用 `// EXTEND:` 标注，并在对应 docs 笔记的「扩展点」一节汇总。
- shader 文件（.hlsl/.hlsli）必须是 **UTF-8 带 BOM**：dxc 用 `-Zi` 嵌入含中文的源码时，无 BOM 会报 `0x80070459`。`build.ps1` 会自动修正。
- PowerShell 脚本保持纯 ASCII（Windows PowerShell 5.1 按 ANSI 读无 BOM 文件）。
- 界面文字新增汉字后运行 `python tools/update_ui_glyphs.py`（否则不在常用字表里的字显示为「?」）。
- 编辑器面板窗口名用「图标 标题###固定ID」，新面板在 `Editor::buildDefaultLayout` 里指定停靠位置，开关放进「视图」菜单。
- 编辑器只通过 `EditorContext` 访问引擎状态；引擎模块不得 include `editor/`。
- 资产：源文件放 `assets/`，`.meta`（GUID）**提交 git**；烘焙缓存在 `build/cooked/`（不提交）。改了烘焙算法或格式就把 `asset::kCookerVersion` 加一。
- 场景/玩法代码通过 `ModelHandle` + 实例号引用模型，**不要保存 `GpuMesh*` / `Material*`**（热重载后会悬空）。
- 后台线程（Job）不碰 GPU 和场景；需要上传或修改场景时用 `JobSystem::runOnMainThread`。
- 新增渲染 pass：在 `engine/render/passes/` 建文件（init 建管线，add 声明读写），在 `Renderer::buildGraph` 中按顺序调用；不要手写 barrier。
- 新增材质：只写「填 SurfaceData」的部分，光照统一调用 `ShadeSurface`，阴影统一调用 `SampleSunShadow`，介质统一调用 `medium.hlsli`。
- 场景实体的物理只设 `Entity::physics`（none/static/dynamic），刚体由 `ScenePhysics` 自动创建与同步；不要在玩法代码里手动给场景实体建刚体。
- 地形高度只有一份（`terrain::Heightfield`）：渲染、物理、玩法查询都从它来，不要另建一份「近似高度」。
- 贴图用途按文件名后缀推断（`_n` 法线、`_data/_orm/_mask` 数据，其余颜色），猜错了改 `.meta`。
- 命名：类型 `PascalCase`，函数 `camelCase`，成员 `m_` 前缀，常量 `kName`，命名空间 `eng`。
- 错误处理：可恢复错误返回 `Result`/`bool` 并记日志；不可恢复用 `ENG_ASSERT`/`ENG_CHECK_VK`。

## 构建与测试
- 用 `scripts/build.ps1` 构建：它固定控制台代码页并检查 Ninja 是否记录了头文件依赖（中文版 cl 的 `/showIncludes` 前缀随代码页变，UTF-8 终端里依赖会悄悄丢失，改头文件不再触发重新编译）。
```powershell
.\scripts\check_env.ps1             # 环境检查
.\scripts\build.ps1 -Config Debug   # 进入 VS 开发环境 → configure → build
.\scripts\build.ps1 -Config Release -Test   # 构建并运行单元测试 + 冒烟测试
```
- 冒烟测试：`sandbox --frames N` 运行 N 帧后退出，返回码 0 = 成功。
- 单元测试（doctest）覆盖 math、Medium 查询、Render Graph barrier 推导。

## 里程碑流程
- 每个里程碑：Debug + Release 构建通过、单元测试 + 冒烟测试通过、validation 无报错、实际确认画面后**单独 git 提交**。
- 验收还包括 [docs/稳定性与故障演练.md](docs/稳定性与故障演练.md) 里的故障演练（关 pass 后缩放、损坏场景、错误 `.meta`、`--selftest ocean`……），并记录冒烟测试输出的帧时间 P95/P99 与最长帧。
- 每个里程碑在 `docs/` 写一篇中文笔记：解决的问题、mermaid 数据流图、关键公式（`$...$`）、扩展点汇总、动手练习。
- README 维护模块地图、阅读顺序、里程碑进度。
- 分三段推进，每段结束停下等用户确认：M0–M3 → M4–M5 → M6–M7。

## 约束
- 性能预算：1080p 60 FPS，显存 ≤ 2 GB（RTX 5060 Laptop 8 GB，笔记本双显卡，必须优先选独显）。
- 不要删除目录中已有的文件；不可逆改动先问用户。
- 需要用户决定的设计取舍：列出选项 + 推荐，简短提问。
