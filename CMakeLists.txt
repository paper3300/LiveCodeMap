cmake_minimum_required(VERSION 3.30)

project(LiveCodeMap
  VERSION 0.0.1
  DESCRIPTION "C++/Unreal code exploration and context layer"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(LCM_BUILD_TESTS "Build LiveCodeMap unit tests" ON)

# Runtime policy: every target uses the static release CRT (/MT) in every
# configuration. This matches the prebuilt LLVM libraries (their LLVMConfig
# declares the same value) and keeps core, analyzer and tests linkable with
# each other regardless of whether the SDK is present. Consequence: the Debug
# configuration builds own code unoptimised on the release CRT (no _DEBUG,
# _ITERATOR_DEBUG_LEVEL=0).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

include(cmake/Dependencies.cmake)
# Semantic analysis is compiler-evidence based and needs the pinned Clang SDK.
# Without it the Clang-based targets are absent and configuration says so; no
# substitute is built (see cmake/ClangSdk.cmake, tools/bootstrap-clang-sdk.ps1).
include(cmake/ClangSdk.cmake)

add_library(lcm_warnings INTERFACE)
if(MSVC)
  target_compile_options(lcm_warnings INTERFACE /W4 /WX /permissive- /utf-8 /EHsc /Zc:__cplusplus /Zc:preprocessor)
  target_compile_definitions(lcm_warnings INTERFACE _CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
  target_compile_options(lcm_warnings INTERFACE -Wall -Wextra -Wpedantic -Werror)
endif()

add_subdirectory(src)

# Single-TU replay tool; needs the Clang-based analyzer.
if(LCM_CLANG_FOUND)
  add_subdirectory(tools/lcm_replay)
endif()

if(LCM_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
