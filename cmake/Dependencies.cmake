# 第三方依赖：全部用 FetchContent 拉取并固定版本。
# 为什么固定 tag：依赖升级可能改 API（ImGui、fastgltf 都有过），固定版本才能保证「今天能编、明年也能编」。
# EXTEND: 升级某个依赖时只改这里的 GIT_TAG，然后全量重新构建并跑测试。

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)
# 依赖都是第三方代码，不需要它们的警告和测试
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)

# ---- GLFW：窗口与输入 ---------------------------------------------------------
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw GIT_REPOSITORY https://github.com/glfw/glfw.git GIT_TAG 3.4 GIT_SHALLOW TRUE)

# ---- volk：Vulkan 函数加载器 -----------------------------------------------------
# 版本与已安装的 Vulkan SDK 一致，保证它认识 SDK 头文件里的所有函数。
set(VOLK_PULL_IN_VULKAN ON CACHE BOOL "" FORCE)
FetchContent_Declare(volk GIT_REPOSITORY https://github.com/zeux/volk.git GIT_TAG vulkan-sdk-1.4.363.0 GIT_SHALLOW TRUE)

# ---- vk-bootstrap：实例/设备/交换链创建的样板代码 ----------------------------------
FetchContent_Declare(vk_bootstrap GIT_REPOSITORY https://github.com/charles-lunarg/vk-bootstrap.git GIT_TAG v1.4.363 GIT_SHALLOW TRUE)

# ---- VMA：显存分配 -----------------------------------------------------------------
FetchContent_Declare(vma GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git GIT_TAG v3.3.0 GIT_SHALLOW TRUE)

# ---- GLM：数学 ---------------------------------------------------------------------
set(GLM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glm GIT_REPOSITORY https://github.com/g-truc/glm.git GIT_TAG 1.0.1 GIT_SHALLOW TRUE)

# ---- spdlog：日志 -------------------------------------------------------------------
FetchContent_Declare(spdlog GIT_REPOSITORY https://github.com/gabime/spdlog.git GIT_TAG v1.15.3 GIT_SHALLOW TRUE)

# ---- nlohmann/json：config/ 下参数文件的读写 ------------------------------------------
# 需求列表里没有 JSON 库，但「参数数据化 + 可保存 + 热加载」需要一个。选它是因为单头文件、API 直白。
# 用官方发布的精简 tarball，比 clone 整个仓库小两个数量级。
FetchContent_Declare(json URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)

# ---- Dear ImGui（docking 分支）：没有 CMakeLists，下面自己建 target -----------------------
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.91.9b-docking GIT_SHALLOW TRUE)

# ---- stb：只用 stb_image / stb_image_write ------------------------------------------------
FetchContent_Declare(stb GIT_REPOSITORY https://github.com/nothings/stb.git GIT_TAG 003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5)

# ---- doctest：单元测试 ----------------------------------------------------------------------
set(DOCTEST_WITH_TESTS OFF CACHE BOOL "" FORCE)
set(DOCTEST_NO_INSTALL ON CACHE BOOL "" FORCE)
FetchContent_Declare(doctest GIT_REPOSITORY https://github.com/doctest/doctest.git GIT_TAG v2.4.12 GIT_SHALLOW TRUE)

FetchContent_MakeAvailable(glfw volk vk_bootstrap vma glm spdlog json imgui stb doctest)

# volk 需要知道目标平台，才会加载 vkCreateWin32SurfaceKHR 等平台函数
target_compile_definitions(volk PUBLIC VK_USE_PLATFORM_WIN32_KHR)

# ---- ImGui target --------------------------------------------------------------------------
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
# 让 ImGui 的 Vulkan 后端也通过 volk 取函数指针，而不是链接 vulkan-1.lib
target_compile_definitions(imgui PUBLIC IMGUI_IMPL_VULKAN_USE_VOLK)
target_link_libraries(imgui PUBLIC glfw volk)

# ---- stb target（header-only，实现放在 engine/core/StbImpl.cpp） -------------------------------
add_library(stb INTERFACE)
target_include_directories(stb INTERFACE ${stb_SOURCE_DIR})

# 把第三方 target 收进 IDE 的 "external" 文件夹；
# /utf-8：部分依赖的源码含非 ASCII 字符，中文 Windows 默认按 GBK 读会报 C4819
foreach(t glfw volk vk-bootstrap imgui spdlog)
    if(TARGET ${t})
        set_target_properties(${t} PROPERTIES FOLDER external)
        target_compile_options(${t} PRIVATE /utf-8)
    endif()
endforeach()
