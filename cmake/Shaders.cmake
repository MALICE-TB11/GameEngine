# HLSL → SPIR-V 构建步骤。
#
# 约定（运行时的 ShaderLibrary 用同一套约定，两边必须一致）：
#   shaders/<dir>/<name>.hlsl 里出现 VSMain / PSMain / CSMain 入口，
#   就分别编译成 build/shaders/<dir>/<name>.vs.spv / .ps.spv / .cs.spv。
#   shaders/common/*.hlsli 是被 include 的公共库，不单独编译。
#
# 为什么构建时也编译一遍：Release 发布时直接加载 .spv，不依赖 dxc；
# 构建时编译还能让 shader 语法错误在 CI/命令行里第一时间暴露。
# 运行时热重载用的 dxc 参数通过 ENGINE_DXC_FLAGS 宏传给 C++，保证两边一致。

set(ENGINE_SHADER_SOURCE_DIR ${CMAKE_SOURCE_DIR}/shaders)
set(ENGINE_SHADER_BINARY_DIR ${CMAKE_BINARY_DIR}/shaders)

# -spirv：输出 SPIR-V；-fspv-target-env：允许使用 Vulkan 1.3 的 SPIR-V 1.6 特性
# -HV 2021：HLSL 2021（模板、运算符重载、逻辑运算短路）
# -Zi -Qembed_debug：把源码嵌进 SPIR-V，RenderDoc 里可以按 HLSL 源码单步调试
# -fspv-preserve-bindings 等不需要；绑定号全部在 HLSL 里用 [[vk::binding]] 显式写出
set(ENGINE_DXC_FLAGS -spirv -fspv-target-env=vulkan1.3 -HV 2021 -Zi -Qembed_debug -O3 -WX)
set(ENGINE_SHADER_MODEL 6_6)

function(engine_compile_shaders target_name)
    file(GLOB_RECURSE shader_sources CONFIGURE_DEPENDS
        ${ENGINE_SHADER_SOURCE_DIR}/*.hlsl)
    file(GLOB_RECURSE shader_includes CONFIGURE_DEPENDS
        ${ENGINE_SHADER_SOURCE_DIR}/*.hlsli)

    set(outputs)
    foreach(src ${shader_sources})
        file(RELATIVE_PATH rel ${ENGINE_SHADER_SOURCE_DIR} ${src})
        string(REGEX REPLACE "\\.hlsl$" "" rel_noext ${rel})
        file(READ ${src} content)
        foreach(stage vs ps cs)
            if(stage STREQUAL "vs")
                set(entry VSMain)
            elseif(stage STREQUAL "ps")
                set(entry PSMain)
            else()
                set(entry CSMain)
            endif()
            string(FIND "${content}" "${entry}" found)
            if(found EQUAL -1)
                continue()
            endif()
            set(out ${ENGINE_SHADER_BINARY_DIR}/${rel_noext}.${stage}.spv)
            get_filename_component(out_dir ${out} DIRECTORY)
            add_custom_command(
                OUTPUT ${out}
                COMMAND ${CMAKE_COMMAND} -E make_directory ${out_dir}
                COMMAND Vulkan::dxc_exe ${ENGINE_DXC_FLAGS}
                        -T ${stage}_${ENGINE_SHADER_MODEL} -E ${entry}
                        -I ${ENGINE_SHADER_SOURCE_DIR}
                        -Fo ${out} ${src}
                # 粗粒度依赖：任何 .hlsli 变了都重编所有 shader。shader 数量少，换来的是规则简单可靠。
                DEPENDS ${src} ${shader_includes}
                COMMENT "dxc ${rel} (${entry})"
                VERBATIM)
            list(APPEND outputs ${out})
        endforeach()
    endforeach()

    add_custom_target(${target_name} ALL DEPENDS ${outputs} SOURCES ${shader_sources} ${shader_includes})
    set_target_properties(${target_name} PROPERTIES FOLDER engine)
endfunction()
