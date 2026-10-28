add_executable(engine_tests
    test_main.cpp
    test_math.cpp
    test_geometry.cpp
    test_loop.cpp
    test_config.cpp
    test_rendergraph.cpp)
# 链接 engine_render 只是为了测试其中的纯逻辑（rg::compile），测试不会创建 Vulkan 设备
target_link_libraries(engine_tests PRIVATE engine_core engine_render doctest::doctest)
engine_set_warnings(engine_tests)
set_target_properties(engine_tests PROPERTIES FOLDER tests)
add_test(NAME engine_tests COMMAND engine_tests)
