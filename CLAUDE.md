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
core ← platform ← rhi ← render ← 特性模块(terrain / ocean / medium / physics …) ← sandbox
math 只依赖 GLM，任何层都可用；debug 可依赖 render
```
- 特性模块之间**不互相 include**，通过 `medium/`、`world/` 的共享接口交流。
- 新文件放到对应模块目录，CMake target 名为 `engine_<module>`。

## 容错约定
- 每个特性模块：小接口 + **参考实现**（简单、正确）+ **优化实现**，ImGui 可切换对比。
- Render Graph 中每个 pass 可在 ImGui 开关；关闭/失败的 pass 输出由默认资源（黑/白/平坦法线）代替，下游不崩溃。
- Shader 热重载失败时保留旧 pipeline，错误显示在 ImGui；缺失/出错材质用醒目错误材质（品红棋盘）。
- Shader 分层：`common/` 公共函数；材质遵循 `SurfaceData → ShadingModel` 固定接口。
- 可调参数放 `config/*.json`，ImGui 修改后可保存、可热加载；**不要硬编码魔法数字**（物理常数与公式里的系数除外，需注释出处）。
- 物理固定步长，渲染插值。

## 调试设施
validation layer（Debug 默认开）、所有 Vulkan 对象 debug name、RenderDoc in-app 捕获（F12）、每个 pass 的 GPU timestamp。

## 代码风格
- 注释写「为什么」，Vulkan 同步/barrier/描述符等难点要讲清楚；注释用中文。
- 不要过度抽象：每个抽象要说明它服务于后面哪个功能，能直白就直白。
- 扩展点用 `// EXTEND:` 标注，并在对应 docs 笔记的「扩展点」一节汇总。
- 命名：类型 `PascalCase`，函数 `camelCase`，成员 `m_` 前缀，常量 `kName`，命名空间 `eng`。
- 错误处理：可恢复错误返回 `Result`/`bool` 并记日志；不可恢复用 `ENG_ASSERT`/`ENG_CHECK_VK`。

## 构建与测试
```powershell
.\scripts\check_env.ps1             # 环境检查
.\scripts\build.ps1 -Config Debug   # 进入 VS 开发环境 → configure → build
.\scripts\build.ps1 -Config Release -Test   # 构建并运行单元测试 + 冒烟测试
```
- 冒烟测试：`sandbox --frames N` 运行 N 帧后退出，返回码 0 = 成功。
- 单元测试（doctest）覆盖 math、Medium 查询、Render Graph barrier 推导。

## 里程碑流程
- 每个里程碑：Debug + Release 构建通过、单元测试 + 冒烟测试通过、validation 无报错、实际确认画面后**单独 git 提交**。
- 每个里程碑在 `docs/` 写一篇中文笔记：解决的问题、mermaid 数据流图、关键公式（`$...$`）、扩展点汇总、动手练习。
- README 维护模块地图、阅读顺序、里程碑进度。
- 分三段推进，每段结束停下等用户确认：M0–M3 → M4–M5 → M6–M7。

## 约束
- 性能预算：1080p 60 FPS，显存 ≤ 2 GB（RTX 5060 Laptop 8 GB，笔记本双显卡，必须优先选独显）。
- 不要删除目录中已有的文件；不可逆改动先问用户。
- 需要用户决定的设计取舍：列出选项 + 推荐，简短提问。
