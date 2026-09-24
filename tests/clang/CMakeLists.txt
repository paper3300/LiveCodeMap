add_executable(lcm_clang_smoke_tests
  ${CMAKE_SOURCE_DIR}/tests/test_main.cpp
  smoke_tests.cpp)

target_link_libraries(lcm_clang_smoke_tests PRIVATE lcm_clang_smoke doctest lcm_warnings)
target_compile_definitions(lcm_clang_smoke_tests PRIVATE
  LCM_TEST_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures")
lcm_configure_clang_target(lcm_clang_smoke_tests)

# Runs in every configuration: the target always uses the static release CRT
# of the prebuilt SDK (see lcm_configure_clang_target), so Debug builds link
# consistently too, with only the project's own code unoptimized.
add_test(NAME lcm_clang_smoke_tests COMMAND lcm_clang_smoke_tests)
