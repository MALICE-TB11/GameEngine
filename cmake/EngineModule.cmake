# engine_module(<name> SOURCES ... DEPS ...)
# 每个引擎模块是一个静态库 engine_<name>。用 target_link_libraries 表达模块依赖：
# 依赖方向写错（比如 rhi 去 include render 的头文件）时，头文件路径根本不可见，编译直接报错，
# 这比靠自觉维护「单向依赖」可靠得多。
function(engine_module name)
    cmake_parse_arguments(ARG "" "" "SOURCES;DEPS;PRIVATE_DEPS" ${ARGN})
    set(target engine_${name})
    add_library(${target} STATIC ${ARG_SOURCES})
    # 头文件按 "<模块>/<文件>.h" 引用，例如 #include "core/Log.h"
    target_include_directories(${target} PUBLIC ${CMAKE_SOURCE_DIR}/engine)
    target_link_libraries(${target} PUBLIC ${ARG_DEPS} PRIVATE ${ARG_PRIVATE_DEPS})
    engine_set_warnings(${target})
    set_target_properties(${target} PROPERTIES FOLDER engine)
endfunction()

function(engine_set_warnings target)
    target_compile_options(${target} PRIVATE
        /W4            # 高警告级别
        /permissive-   # 标准一致性模式
        /utf-8         # 源码是 UTF-8（含中文注释），不加会被按 GBK 解析并产生乱码警告
        /wd4100        # 未使用参数：回调签名里很常见
        /wd4201)       # 匿名结构体：GLM 内部使用
    target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endfunction()
