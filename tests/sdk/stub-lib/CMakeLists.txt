# Builds a stub clangBasic.lib that implements exactly the symbol probed by
# cmake/ClangSdk.cmake (clang::getClangFullVersion) with a selectable CRT, so
# the discovery tests can show the ABI probe failing on a real CRT mismatch
# (LNK2038 RuntimeLibrary) and passing on a compatible stub.
cmake_minimum_required(VERSION 3.30)
project(lcm_sdk_stub_lib LANGUAGES CXX)
set(STUB_RUNTIME "MultiThreaded" CACHE STRING "MultiThreaded or MultiThreadedDLL")
set(STUB_VERSION "0.0.0" CACHE STRING "version string the stub reports")
set(CMAKE_MSVC_RUNTIME_LIBRARY "${STUB_RUNTIME}")
file(WRITE ${CMAKE_BINARY_DIR}/stub.cpp
  "#include <string>\n"
  "namespace clang { std::string getClangFullVersion() { return \"clang version ${STUB_VERSION} (stub)\"; } }\n")
add_library(clangBasic STATIC ${CMAKE_BINARY_DIR}/stub.cpp)
target_compile_features(clangBasic PRIVATE cxx_std_17)
set_target_properties(clangBasic PROPERTIES OUTPUT_NAME clangBasic
  ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/out
  ARCHIVE_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/out)
