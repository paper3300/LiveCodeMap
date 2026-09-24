# Built only when the pinned Clang SDK was found (see cmake/ClangSdk.cmake).
add_library(lcm_clang_smoke STATIC smoke.cpp)
target_include_directories(lcm_clang_smoke PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(lcm_clang_smoke
  PUBLIC clangTooling clangFrontend clangAST clangBasic
  PRIVATE lcm_warnings)
lcm_configure_clang_target(lcm_clang_smoke)
