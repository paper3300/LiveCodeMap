# Query layer, CLI and MCP tests; independent of the Clang SDK. Fixture stores
# are written through the store's writer API into scratch directories.
add_executable(lcm_query_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/../test_main.cpp
  query_tests.cpp
  mcp_tests.cpp)
target_link_libraries(lcm_query_tests PRIVATE lcm_query lcm_store lcm_sqlite3 doctest lcm_warnings)
target_include_directories(lcm_query_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../store)
target_compile_definitions(lcm_query_tests PRIVATE
  LCM_LIVECODEMAP_EXE="$<TARGET_FILE:livecodemap>"
  LCM_TEST_SCRATCH_DIR="${CMAKE_CURRENT_BINARY_DIR}/scratch")
add_dependencies(lcm_query_tests livecodemap)

add_test(NAME lcm_query_tests COMMAND lcm_query_tests)
