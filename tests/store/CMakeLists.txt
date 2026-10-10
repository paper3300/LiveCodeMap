# Store tests are independent of the Clang SDK.
add_executable(lcm_store_child store_child.cpp)
target_link_libraries(lcm_store_child PRIVATE lcm_store lcm_warnings)

add_executable(lcm_store_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/../test_main.cpp
  store_tests.cpp)
target_link_libraries(lcm_store_tests PRIVATE lcm_store lcm_sqlite3 doctest lcm_warnings)
target_compile_definitions(lcm_store_tests PRIVATE
  LCM_STORE_CHILD_EXE="$<TARGET_FILE:lcm_store_child>"
  LCM_TEST_SCRATCH_DIR="${CMAKE_CURRENT_BINARY_DIR}/scratch")
add_dependencies(lcm_store_tests lcm_store_child)

add_test(NAME lcm_store_tests COMMAND lcm_store_tests)
