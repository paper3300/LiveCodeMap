# Minimal project used by discovery.tests.ps1 to exercise cmake/ClangSdk.cmake
# in isolation. It requires the SDK so that rejection is a configure error.
cmake_minimum_required(VERSION 3.30)
project(lcm_sdk_probe LANGUAGES CXX)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
set(LCM_REQUIRE_CLANG ON CACHE BOOL "" FORCE)
include(${LCM_PROJECT_ROOT}/cmake/ClangSdk.cmake)
message(STATUS "PROBE_RESULT: found=${LCM_CLANG_FOUND} verified=${LCM_CLANG_SDK_VERIFIED} root=${LCM_CLANG_SDK_ROOT}")
