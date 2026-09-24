# Ordinary-C++ Phase 1A analyzer. Built only when the pinned Clang SDK was
# accepted (see cmake/ClangSdk.cmake); links the common core under the
# project-wide static CRT policy.
add_library(lcm_analyzer STATIC
  analyzer.cpp
  pch_replay.cpp
  safety.cpp
  facts_builder.cpp)

get_filename_component(_lcm_src_root "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
target_include_directories(lcm_analyzer PUBLIC ${_lcm_src_root})
target_link_libraries(lcm_analyzer
  PUBLIC lcm_core clangTooling clangFrontend clangAnalysis clangAST clangBasic clangLex
  PRIVATE lcm_warnings)
lcm_configure_clang_target(lcm_analyzer)
