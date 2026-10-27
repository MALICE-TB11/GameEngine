cmake_minimum_required(VERSION 3.25)

# CMake 4.x 不再接受 cmake_minimum_required(VERSION < 3.5) 的老工程（doctest 等依赖仍这么写）。
# 这个变量让这些依赖按 3.5 的策略配置，而不用去改它们的源码。
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)

project(OceanEngine LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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
