add_executable(sandbox
    main.cpp
    App.cpp
    App.h
    Player.cpp
    Player.h)
target_link_libraries(sandbox PRIVATE engine_editor engine_asset engine_terrain engine_world engine_physics glfw)
engine_set_warnings(sandbox)
# shader 是运行时数据：构建 sandbox 时确保它们也编译好了
add_dependencies(sandbox engine_shaders)
set_target_properties(sandbox PROPERTIES
    FOLDER sandbox
    VS_DEBUGGER_WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
