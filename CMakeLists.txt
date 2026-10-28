cmake_minimum_required(VERSION 3.25)

# CMake 4.x 不再接受 cmake_minimum_required(VERSION < 3.5) 的老工程（doctest 等依赖仍这么写）。
# 这个变量让这些依赖按 3.5 的策略配置，而不用去改它们的源码。
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)

project(OceanEngine LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 注意（增量构建的头文件依赖）：Ninja 靠解析 cl /showIncludes 输出的前缀（中文版是「注意: 包含文件:」）得知
# 每个 .cpp 包含了哪些头文件。这个前缀的编码跟着控制台代码页走，配置时探测一次就缓存下来；
# 之后在另一种代码页的控制台里构建（例如 UTF-8 的终端），前缀对不上，Ninja 不报错，只是悄悄记下「没有依赖」——
# 改了头文件也不会重新编译包含它的 .cpp，结构体布局在不同 .obj 里不一致，运行时内存错乱。
# scripts/build.ps1 因此固定控制台代码页，并在构建后检查依赖是否真的记录下来了。

# 所有 exe/dll 放到 build/bin/<Config>/，运行时依赖（如 dll）与 sandbox 在同一目录，省去 PATH 配置。
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/$<CONFIG>)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/$<CONFIG>)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

option(ENGINE_BUILD_TESTS "Build doctest unit tests" ON)

# Vulkan SDK：只用它的头文件、dxc 和 validation layer。函数指针由 volk 在运行时加载，
# 所以不链接 vulkan-1.lib（见 engine/rhi/VkCommon.h 的注释）。
find_package(Vulkan REQUIRED COMPONENTS dxc)

include(cmake/Dependencies.cmake)
include(cmake/Shaders.cmake)
include(cmake/EngineModule.cmake)

add_subdirectory(engine)
add_subdirectory(shaders)
add_subdirectory(sandbox)

if(ENGINE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
