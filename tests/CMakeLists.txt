add_executable(engine_tests
    test_main.cpp
    test_math.cpp
    test_loop.cpp
    test_config.cpp)
target_link_libraries(engine_tests PRIVATE engine_core doctest::doctest)
engine_set_warnings(engine_tests)
set_target_properties(engine_tests PROPERTIES FOLDER tests)
add_test(NAME engine_tests COMMAND engine_tests)
