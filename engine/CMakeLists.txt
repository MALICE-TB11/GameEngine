# 引擎模块。依赖方向（只能向左依赖）：
#   math ← core ← platform ← rhi ← render ← debug / 特性模块 ← sandbox
# math 是 GLM 之上的薄层，任何模块都可以用。

# ---- math：header-only ---------------------------------------------------------------
add_library(engine_math INTERFACE)
target_include_directories(engine_math INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(engine_math INTERFACE glm::glm)
# 这些宏必须对所有翻译单元一致，所以放在 target 上而不是某个头文件里：
#   DEPTH_ZERO_TO_ONE：Vulkan 的 NDC 深度范围是 [0,1]，不是 OpenGL 的 [-1,1]
#   ENABLE_EXPERIMENTAL：启用 gtx 扩展（quat 插值、矩阵分解等）
target_compile_definitions(engine_math INTERFACE
    GLM_FORCE_DEPTH_ZERO_TO_ONE GLM_FORCE_RADIANS GLM_ENABLE_EXPERIMENTAL)

# ---- core ------------------------------------------------------------------------------
engine_module(core
    SOURCES
        core/Log.cpp
        core/Assert.cpp
        core/FileSystem.cpp
        core/Config.cpp
        core/ImageIO.cpp
    DEPS engine_math spdlog::spdlog nlohmann_json::nlohmann_json
    PRIVATE_DEPS stb)
# 让运行时能找到源码目录里的 config/、shaders/、assets/（热重载要改的是源码目录里的文件，
# 而不是 build 目录里的副本）。
# EXTEND: 做发布包时，把这些改成相对 exe 的路径。
target_compile_definitions(engine_core PUBLIC
    ENGINE_ROOT_DIR="${CMAKE_SOURCE_DIR}"
    ENGINE_SHADER_BINARY_DIR="${CMAKE_BINARY_DIR}/shaders"
    $<$<CONFIG:Debug>:ENGINE_DEBUG=1>)

# ---- platform --------------------------------------------------------------------------
engine_module(platform
    SOURCES
        platform/Window.cpp
        platform/Input.cpp
    DEPS engine_core glfw)

# ---- rhi：Vulkan 薄封装 ----------------------------------------------------------------
engine_module(rhi
    SOURCES
        rhi/VkCommon.cpp
        rhi/Device.cpp
        rhi/Swapchain.cpp
        rhi/Resources.cpp
        rhi/VmaImpl.cpp
    DEPS engine_platform volk GPUOpen::VulkanMemoryAllocator
    PRIVATE_DEPS vk-bootstrap::vk-bootstrap)
# volk 自己负责加载函数指针，所以所有代码都不应该看到 vulkan.h 里的函数原型声明
target_compile_definitions(engine_rhi PUBLIC VK_NO_PROTOTYPES
    VMA_STATIC_VULKAN_FUNCTIONS=0 VMA_DYNAMIC_VULKAN_FUNCTIONS=1)

# ---- render -----------------------------------------------------------------------------
engine_module(render
    SOURCES
        render/Renderer.cpp
    DEPS engine_rhi imgui)

# ---- debug：ImGui 面板 ----------------------------------------------------------------
engine_module(debug
    SOURCES
        debug/ImGuiLayer.cpp
        debug/StatsPanel.cpp
    DEPS engine_render imgui)
