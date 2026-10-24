add_executable(core_tests core_tests.cpp)
target_include_directories(core_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_options(core_tests PRIVATE -Wall -Wextra -UNDEBUG)
add_test(NAME core_tests COMMAND core_tests)
