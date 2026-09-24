add_executable(lcm_core_tests
  test_main.cpp
  hash_tests.cpp
  source_tests.cpp
  identity_tests.cpp
  facts_tests.cpp
  status_tests.cpp
  compile_db_tests.cpp
  response_file_tests.cpp
  compile_context_tests.cpp)

target_link_libraries(lcm_core_tests PRIVATE lcm_core doctest lcm_warnings)
target_include_directories(lcm_core_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(lcm_core_tests PRIVATE
  LCM_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

add_test(NAME lcm_core_tests COMMAND lcm_core_tests)

if(LCM_CLANG_FOUND)
  add_subdirectory(clang)
  add_subdirectory(analyzer)
endif()

# SDK bootstrap/discovery scripts (PowerShell); independent of the SDK being present.
add_subdirectory(sdk)
