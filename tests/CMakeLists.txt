add_executable(engine_tests
    test_main.cpp
    test_math.cpp
    test_geometry.cpp
    test_loop.cpp
    test_config.cpp
    test_rendergraph.cpp
    test_core.cpp
    test_asset.cpp
    test_scene.cpp)
# 链接引擎模块只是为了测试其中的纯逻辑（barrier 推导、烘焙、场景描述），测试不会创建 Vulkan 设备。
# bc7enc 直接链接：测试要用它的解码器检查压缩质量。
target_link_libraries(engine_tests PRIVATE engine_core engine_render engine_asset engine_world bc7enc doctest::doctest)
engine_set_warnings(engine_tests)
set_target_properties(engine_tests PROPERTIES FOLDER tests)
add_test(NAME engine_tests COMMAND engine_tests)
