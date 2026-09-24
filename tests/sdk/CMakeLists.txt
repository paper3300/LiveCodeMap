# Behaviour tests for tools/bootstrap-clang-sdk.ps1 and cmake/ClangSdk.cmake.
# They use tiny local fixtures under the ignored build tree; no network, no
# 0.9 GB download, and nothing outside the workspace is touched.
if(NOT WIN32)
  return()
endif()

find_program(LCM_POWERSHELL powershell.exe)
if(NOT LCM_POWERSHELL)
  message(STATUS "LiveCodeMap: powershell.exe not found; SDK script tests are not registered")
  return()
endif()

set(_lcm_sdk_test_root "${CMAKE_BINARY_DIR}/sdk-tests")

add_test(NAME lcm_sdk_bootstrap_tests
  COMMAND ${LCM_POWERSHELL} -NoProfile -ExecutionPolicy Bypass
          -File ${CMAKE_CURRENT_SOURCE_DIR}/bootstrap.tests.ps1
          -WorkRoot ${_lcm_sdk_test_root}/bootstrap)

add_test(NAME lcm_sdk_discovery_tests
  COMMAND ${LCM_POWERSHELL} -NoProfile -ExecutionPolicy Bypass
          -File ${CMAKE_CURRENT_SOURCE_DIR}/discovery.tests.ps1
          -WorkRoot ${_lcm_sdk_test_root}/discovery
          -Generator "${CMAKE_GENERATOR}"
          -Platform "${CMAKE_GENERATOR_PLATFORM}"
          -SdkRoot "${LCM_CLANG_SDK_ROOT}"
          -Compiler "${CMAKE_CXX_COMPILER}")
