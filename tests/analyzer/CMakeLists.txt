add_executable(lcm_analyzer_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/../test_main.cpp
  analyzer_tests.cpp)

get_filename_component(_lcm_fixture_root "${CMAKE_CURRENT_SOURCE_DIR}/../fixtures" ABSOLUTE)
target_link_libraries(lcm_analyzer_tests PRIVATE lcm_analyzer doctest lcm_warnings)
target_include_directories(lcm_analyzer_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_definitions(lcm_analyzer_tests PRIVATE
  LCM_TEST_FIXTURE_DIR="${_lcm_fixture_root}"
  LCM_CLANG_RESOURCE_DIR="${LCM_CLANG_RESOURCE_DIR}")
lcm_configure_clang_target(lcm_analyzer_tests)

add_test(NAME lcm_analyzer_tests COMMAND lcm_analyzer_tests)
