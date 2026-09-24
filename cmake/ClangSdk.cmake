# Optional discovery of the pinned Clang/LLVM LibTooling SDK.
#
# The SDK is the official prebuilt Windows release described in
# cmake/clang-sdk.json and bootstrapped project-locally by
# tools/bootstrap-clang-sdk.ps1. Nothing here downloads or installs anything.
#
# Trust model:
#   * Default location (build/deps/<archive_root>): accepted only with the
#     bootstrap provenance marker (<deps>/<name>-<version>.ok containing the
#     pinned SHA-256) AND every required path present. The marker is written
#     only after size/SHA-256 (and, when available, signature) verification.
#   * LCM_CLANG_SDK_DIR override: provenance is NOT verified and configuration
#     says so loudly. The override must be complete (required paths); its
#     declarations are checked when present (pinned version, RTTI OFF, Release,
#     and an explicitly declared CRT must be MultiThreaded); and a configure-time
#     compile/link/run probe against clangBasic with /MT is decisive for the
#     actual ABI. An undeclared CRT is allowed when the probe passes. Version
#     equality alone never proves integrity.
#
# Result variables:
#   LCM_CLANG_FOUND        TRUE when LLVM+Clang CMake packages were imported
#   LCM_CLANG_SDK_ROOT     the SDK root that was used
#   LCM_CLANG_SDK_VERIFIED TRUE for the marker-backed default location, FALSE for an override
#   LCM_CLANG_RESOURCE_DIR <root>/lib/clang/<major>, the pinned resource directory
# Helper:
#   lcm_configure_clang_target(<target>)  applies include paths, definitions
#   and the RTTI constraint of the prebuilt libraries.

option(LCM_WITH_CLANG "Build Clang LibTooling based targets when the pinned SDK is found" ON)
option(LCM_REQUIRE_CLANG "Fail configuration when LCM_WITH_CLANG is ON and the SDK is not found" OFF)
option(LCM_CLANG_SDK_SEARCH_DEFAULT "Also look for the SDK at the bootstrap location build/deps/<archive_root>" ON)
set(LCM_CLANG_SDK_DIR "" CACHE PATH "Root of a Clang/LLVM SDK to use instead of the bootstrapped one (provenance unverified)")

set(LCM_CLANG_FOUND FALSE)
set(LCM_CLANG_SDK_ROOT "")
set(LCM_CLANG_SDK_VERIFIED FALSE)
set(LCM_CLANG_RESOURCE_DIR "")

get_filename_component(_lcm_project_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
file(READ ${CMAKE_CURRENT_LIST_DIR}/clang-sdk.json _lcm_clang_sdk_json)
string(JSON LCM_CLANG_SDK_PINNED_VERSION GET "${_lcm_clang_sdk_json}" version)
string(JSON _lcm_sdk_name GET "${_lcm_clang_sdk_json}" name)
string(JSON _lcm_sdk_sha GET "${_lcm_clang_sdk_json}" sha256)
string(JSON _lcm_sdk_default_rel GET "${_lcm_clang_sdk_json}" install_relative_dir)
string(JSON _lcm_required_count LENGTH "${_lcm_clang_sdk_json}" required_paths)
set(_lcm_required_paths "")
math(EXPR _lcm_required_last "${_lcm_required_count} - 1")
foreach(_i RANGE 0 ${_lcm_required_last})
  string(JSON _p GET "${_lcm_clang_sdk_json}" required_paths ${_i})
  list(APPEND _lcm_required_paths "${_p}")
endforeach()
get_filename_component(LCM_CLANG_SDK_DEFAULT_DIR "${_lcm_project_root}/${_lcm_sdk_default_rel}" ABSOLUTE)
get_filename_component(_lcm_default_deps_dir "${LCM_CLANG_SDK_DEFAULT_DIR}/.." ABSOLUTE)
set(LCM_CLANG_SDK_MARKER "${_lcm_default_deps_dir}/${_lcm_sdk_name}-${LCM_CLANG_SDK_PINNED_VERSION}.ok")

# Lists the required paths missing under <root> in <out_var> (empty when complete).
function(_lcm_missing_required_paths root out_var)
  set(_missing "")
  foreach(_rel IN LISTS _lcm_required_paths)
    if(NOT EXISTS "${root}/${_rel}")
      list(APPEND _missing "${_rel}")
    endif()
  endforeach()
  set(${out_var} "${_missing}" PARENT_SCOPE)
endfunction()

# Imports the packages from <root> and checks what the SDK declares about
# itself. Declarations are checked when present (an explicit incompatible
# declaration is rejected); for overrides the compile/link/run probe below is
# decisive, because declared metadata cannot prove the libraries' ABI. The
# project's own CMAKE_MSVC_RUNTIME_LIBRARY is cleared around the import so a
# missing declaration is seen as missing, not as inherited.
macro(_lcm_import_and_check_sdk root origin)
  set(_lcm_saved_crt "${CMAKE_MSVC_RUNTIME_LIBRARY}")
  unset(CMAKE_MSVC_RUNTIME_LIBRARY)
  find_package(LLVM CONFIG REQUIRED PATHS ${root}/lib/cmake/llvm NO_DEFAULT_PATH)
  find_package(Clang CONFIG REQUIRED PATHS ${root}/lib/cmake/clang NO_DEFAULT_PATH)
  set(LCM_CLANG_SDK_DECLARED_CRT "${CMAKE_MSVC_RUNTIME_LIBRARY}")
  set(CMAKE_MSVC_RUNTIME_LIBRARY "${_lcm_saved_crt}")
  set(_lcm_abi_problems "")
  if(NOT LLVM_PACKAGE_VERSION VERSION_EQUAL LCM_CLANG_SDK_PINNED_VERSION)
    list(APPEND _lcm_abi_problems "version ${LLVM_PACKAGE_VERSION} != pinned ${LCM_CLANG_SDK_PINNED_VERSION}")
  endif()
  if(LCM_CLANG_SDK_DECLARED_CRT AND NOT LCM_CLANG_SDK_DECLARED_CRT STREQUAL "MultiThreaded")
    list(APPEND _lcm_abi_problems "SDK declares CMAKE_MSVC_RUNTIME_LIBRARY='${LCM_CLANG_SDK_DECLARED_CRT}', incompatible with the /MT prebuilt pin (MultiThreaded)")
  endif()
  if(NOT LCM_CLANG_SDK_DECLARED_CRT)
    message(STATUS "LiveCodeMap: SDK at ${root} does not declare a CRT; the /MT link probe is decisive")
  endif()
  if(LLVM_ENABLE_RTTI)
    list(APPEND _lcm_abi_problems "SDK declares LLVM_ENABLE_RTTI=ON, prebuilt pin is RTTI OFF")
  endif()
  if(NOT LLVM_BUILD_TYPE STREQUAL "Release")
    list(APPEND _lcm_abi_problems "SDK declares LLVM_BUILD_TYPE=${LLVM_BUILD_TYPE}, pin is Release")
  endif()
  if(_lcm_abi_problems)
    message(FATAL_ERROR "LiveCodeMap: Clang SDK at ${root} (${origin}) rejected: ${_lcm_abi_problems}. "
                        "Use the bootstrapped pinned SDK (tools/bootstrap-clang-sdk.ps1).")
  endif()
  _lcm_remap_dia_paths()
endmacro()

# The prebuilt export file bakes the LLVM build machine's DIA SDK path
# (VS Enterprise) into LLVMDebugInfoPDB. Remap it to this machine's VS
# instance right after import so every later link (including the ABI probe)
# sees the corrected dependency; report clearly when no local DIA SDK exists.
function(_lcm_remap_dia_paths)
  set(_lcm_dia_candidates "")
  if(CMAKE_GENERATOR_INSTANCE)
    list(APPEND _lcm_dia_candidates "${CMAKE_GENERATOR_INSTANCE}/DIA SDK/lib/amd64/diaguids.lib")
  endif()
  if(DEFINED ENV{VSINSTALLDIR})
    list(APPEND _lcm_dia_candidates "$ENV{VSINSTALLDIR}/DIA SDK/lib/amd64/diaguids.lib")
  endif()
  set(_lcm_dia "")
  foreach(_candidate IN LISTS _lcm_dia_candidates)
    if(EXISTS "${_candidate}")
      set(_lcm_dia "${_candidate}")
      break()
    endif()
  endforeach()
  foreach(_lib IN LISTS LLVM_AVAILABLE_LIBS)
    if(NOT TARGET ${_lib})
      continue()
    endif()
    get_target_property(_deps ${_lib} INTERFACE_LINK_LIBRARIES)
    if(NOT _deps OR NOT _deps MATCHES "diaguids\\.lib")
      continue()
    endif()
    set(_fixed "")
    foreach(_dep IN LISTS _deps)
      if(_dep MATCHES "DIA SDK.*diaguids\\.lib$")
        if(_lcm_dia)
          list(APPEND _fixed "${_lcm_dia}")
        else()
          message(WARNING "LiveCodeMap: ${_lib} references '${_dep}' which does not exist here and no local "
                          "DIA SDK was found; linking targets that pull in ${_lib} will fail")
          list(APPEND _fixed "${_dep}")
        endif()
      else()
        list(APPEND _fixed "${_dep}")
      endif()
    endforeach()
    set_target_properties(${_lib} PROPERTIES INTERFACE_LINK_LIBRARIES "${_fixed}")
    if(_lcm_dia)
      message(STATUS "LiveCodeMap: remapped DIA SDK dependency of ${_lib} to ${_lcm_dia}")
    endif()
  endforeach()
endfunction()

# Declared metadata cannot prove the libraries' ABI (an override may not even
# declare a CRT and would inherit ours). For overrides, compile, link and run
# a tiny program against the imported clangBasic target with /MT and check the
# version it reports. A CRT/RTTI/ABI mismatch or a wrong library fails here.
function(_lcm_probe_sdk_abi root)
  set(_probe_dir "${CMAKE_BINARY_DIR}/lcm_sdk_abi_probe")
  file(MAKE_DIRECTORY "${_probe_dir}")
  file(WRITE "${_probe_dir}/probe.cpp"
    "#include <clang/Basic/Version.h>\n#include <cstdio>\n"
    "int main() { std::printf(\"%s\", clang::getClangFullVersion().c_str()); return 0; }\n")
  separate_arguments(_llvm_defs NATIVE_COMMAND "${LLVM_DEFINITIONS}")
  set(_extra_flags "")
  if(MSVC)
    set(_extra_flags /EHsc /GR-)
  endif()
  set(CMAKE_TRY_COMPILE_CONFIGURATION Release)
  try_run(_run_result _compile_result
    "${_probe_dir}/bin" "${_probe_dir}/probe.cpp"
    CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${LLVM_INCLUDE_DIRS};${CLANG_INCLUDE_DIRS}"
                "-DCMAKE_POLICY_DEFAULT_CMP0091=NEW"
                "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded"
    COMPILE_DEFINITIONS ${_llvm_defs} ${_extra_flags}
    LINK_LIBRARIES clangBasic
    CXX_STANDARD 17
    COMPILE_OUTPUT_VARIABLE _compile_output
    RUN_OUTPUT_VARIABLE _run_output)
  if(NOT _compile_result)
    string(LENGTH "${_compile_output}" _len)
    if(_len GREATER 4000)
      math(EXPR _start "${_len} - 4000")
      string(SUBSTRING "${_compile_output}" ${_start} 4000 _compile_output)
    endif()
    message(FATAL_ERROR "LiveCodeMap: Clang SDK override at ${root} rejected: a /MT compile/link probe against "
                        "clangBasic failed (CRT/RTTI/ABI mismatch or unusable libraries). Tail of the build output:\n"
                        "${_compile_output}")
  endif()
  if(NOT _run_result EQUAL 0 OR NOT _run_output MATCHES "${LCM_CLANG_SDK_PINNED_VERSION}")
    message(FATAL_ERROR "LiveCodeMap: Clang SDK override at ${root} rejected: probe ran with exit ${_run_result} and "
                        "reported '${_run_output}', expected version ${LCM_CLANG_SDK_PINNED_VERSION}")
  endif()
  message(STATUS "LiveCodeMap: override ABI probe passed (${_run_output})")
endfunction()

if(LCM_WITH_CLANG)
  # 1. Explicit override: complete + compatible, but provenance unverified.
  if(LCM_CLANG_SDK_DIR)
    _lcm_missing_required_paths("${LCM_CLANG_SDK_DIR}" _lcm_missing)
    if(_lcm_missing)
      message(FATAL_ERROR "LiveCodeMap: LCM_CLANG_SDK_DIR='${LCM_CLANG_SDK_DIR}' rejected: incomplete SDK, missing ${_lcm_missing}")
    endif()
    _lcm_import_and_check_sdk("${LCM_CLANG_SDK_DIR}" "LCM_CLANG_SDK_DIR override")
    _lcm_probe_sdk_abi("${LCM_CLANG_SDK_DIR}")
    set(LCM_CLANG_SDK_ROOT "${LCM_CLANG_SDK_DIR}")
    set(LCM_CLANG_SDK_VERIFIED FALSE)
    message(WARNING "LiveCodeMap: using LCM_CLANG_SDK_DIR override ${LCM_CLANG_SDK_DIR}. Its provenance "
                    "(archive hash/signature) is NOT verified. Checked: completeness, declarations where present "
                    "(version ${LLVM_PACKAGE_VERSION}, CRT '${LCM_CLANG_SDK_DECLARED_CRT}', RTTI ${LLVM_ENABLE_RTTI}, "
                    "${LLVM_BUILD_TYPE}) and the decisive /MT compile/link/run probe against clangBasic.")
  # 2. Bootstrapped default: marker + required paths.
  elseif(LCM_CLANG_SDK_SEARCH_DEFAULT AND EXISTS "${LCM_CLANG_SDK_DEFAULT_DIR}")
    _lcm_missing_required_paths("${LCM_CLANG_SDK_DEFAULT_DIR}" _lcm_missing)
    set(_lcm_marker_ok FALSE)
    if(EXISTS "${LCM_CLANG_SDK_MARKER}")
      file(READ "${LCM_CLANG_SDK_MARKER}" _lcm_marker_content)
      string(STRIP "${_lcm_marker_content}" _lcm_marker_content)
      string(TOLOWER "${_lcm_marker_content}" _lcm_marker_content)
      string(TOLOWER "${_lcm_sdk_sha}" _lcm_pinned_sha_lower)
      if(_lcm_marker_content STREQUAL _lcm_pinned_sha_lower)
        set(_lcm_marker_ok TRUE)
      endif()
    endif()
    if(_lcm_marker_ok AND NOT _lcm_missing)
      _lcm_import_and_check_sdk("${LCM_CLANG_SDK_DEFAULT_DIR}" "bootstrapped default")
      set(LCM_CLANG_SDK_ROOT "${LCM_CLANG_SDK_DEFAULT_DIR}")
      set(LCM_CLANG_SDK_VERIFIED TRUE)
    else()
      set(_lcm_why "")
      if(NOT _lcm_marker_ok)
        string(APPEND _lcm_why "provenance marker ${LCM_CLANG_SDK_MARKER} missing or not matching the pinned SHA-256; ")
      endif()
      if(_lcm_missing)
        string(APPEND _lcm_why "required paths missing: ${_lcm_missing}; ")
      endif()
      message(STATUS "LiveCodeMap: SDK tree at ${LCM_CLANG_SDK_DEFAULT_DIR} is NOT accepted (${_lcm_why}run tools/bootstrap-clang-sdk.ps1)")
    endif()
  endif()

  if(LCM_CLANG_SDK_ROOT)
    set(LCM_CLANG_FOUND TRUE)
    set(LCM_CLANG_RESOURCE_DIR "${LCM_CLANG_SDK_ROOT}/lib/clang/${LLVM_VERSION_MAJOR}")
    if(NOT EXISTS "${LCM_CLANG_RESOURCE_DIR}/include/stddef.h")
      message(FATAL_ERROR "LiveCodeMap: Clang resource directory ${LCM_CLANG_RESOURCE_DIR} lacks builtin headers")
    endif()

    set(_lcm_verified_text "provenance marker verified")
    if(NOT LCM_CLANG_SDK_VERIFIED)
      set(_lcm_verified_text "PROVENANCE UNVERIFIED override")
    endif()
    set(_lcm_crt_text "${LCM_CLANG_SDK_DECLARED_CRT}")
    if(NOT _lcm_crt_text)
      set(_lcm_crt_text "undeclared")
    endif()
    message(STATUS "LiveCodeMap: Clang SDK ${LLVM_PACKAGE_VERSION} at ${LCM_CLANG_SDK_ROOT} (${_lcm_verified_text}; "
                   "declared CRT=${_lcm_crt_text}, project CRT=${CMAKE_MSVC_RUNTIME_LIBRARY}, RTTI=${LLVM_ENABLE_RTTI}, "
                   "assertions=${LLVM_ENABLE_ASSERTIONS}, LLVM build type=${LLVM_BUILD_TYPE}); "
                   "resource dir ${LCM_CLANG_RESOURCE_DIR}")
  else()
    set(_lcm_candidates "")
    if(LCM_CLANG_SDK_SEARCH_DEFAULT)
      list(APPEND _lcm_candidates "${LCM_CLANG_SDK_DEFAULT_DIR}")
    endif()
    string(CONCAT _lcm_msg
      "LiveCodeMap: Clang SDK not found. Searched: ${_lcm_candidates}. "
      "Clang-based targets and their tests are NOT built and semantic-graph behaviour is "
      "NOT verified by this build. Bootstrap with tools/bootstrap-clang-sdk.ps1 or pass "
      "-DLCM_CLANG_SDK_DIR=<root> (provenance-unverified override).")
    if(LCM_REQUIRE_CLANG)
      message(FATAL_ERROR "${_lcm_msg}")
    else()
      message(STATUS "${_lcm_msg}")
    endif()
  endif()
else()
  message(STATUS "LiveCodeMap: LCM_WITH_CLANG=OFF; Clang-based targets are not built and "
                 "semantic-graph behaviour is not verified by this build")
endif()

function(lcm_configure_clang_target target)
  if(NOT LCM_CLANG_FOUND)
    message(FATAL_ERROR "lcm_configure_clang_target(${target}) called without a Clang SDK")
  endif()
  target_include_directories(${target} SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
  separate_arguments(_llvm_defs NATIVE_COMMAND "${LLVM_DEFINITIONS}")
  target_compile_options(${target} PRIVATE ${_llvm_defs})
  if(MSVC)
    # The CRT is a project-wide policy (root CMakeLists.txt sets
    # CMAKE_MSVC_RUNTIME_LIBRARY to MultiThreaded, which is also what the SDK
    # declares), so no per-target runtime override is needed here.
    # /external:W0 silences parse-time warnings from SDK headers; C4702
    # (unreachable code) is emitted during code generation for LLVM/Clang
    # header templates and is not covered, so it is disabled here.
    target_compile_options(${target} PRIVATE /external:W0 /bigobj /wd4702)
    if(NOT LLVM_ENABLE_RTTI)
      target_compile_options(${target} PRIVATE /GR-)
    endif()
  endif()
endfunction()
