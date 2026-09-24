# Exact pinned third-party dependencies. Everything is fetched into the
# project-local build tree; nothing is installed machine-wide.
include(FetchContent)

set(FETCHCONTENT_QUIET OFF)

# nlohmann/json 3.12.0, single header, MIT.
FetchContent_Declare(nlohmann_json_single
  URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.hpp
  URL_HASH SHA256=aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63
  DOWNLOAD_NO_EXTRACT TRUE)
FetchContent_MakeAvailable(nlohmann_json_single)

# doctest 2.4.12, single header, MIT.
FetchContent_Declare(doctest_single
  URL https://raw.githubusercontent.com/doctest/doctest/v2.4.12/doctest/doctest.h
  URL_HASH SHA256=94029a7d32da24a56249658147dbd2b33ff0b9ed665295cbbaf19aafff5b0ced
  DOWNLOAD_NO_EXTRACT TRUE)
FetchContent_MakeAvailable(doctest_single)

# Lay the single headers out under conventional include paths.
set(LCM_THIRD_PARTY_INCLUDE ${CMAKE_BINARY_DIR}/third_party/include)
configure_file(${nlohmann_json_single_SOURCE_DIR}/json.hpp ${LCM_THIRD_PARTY_INCLUDE}/nlohmann/json.hpp COPYONLY)
configure_file(${doctest_single_SOURCE_DIR}/doctest.h ${LCM_THIRD_PARTY_INCLUDE}/doctest/doctest.h COPYONLY)

add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json SYSTEM INTERFACE ${LCM_THIRD_PARTY_INCLUDE})
target_compile_definitions(nlohmann_json INTERFACE JSON_USE_IMPLICIT_CONVERSIONS=0)

add_library(doctest INTERFACE)
target_include_directories(doctest SYSTEM INTERFACE ${LCM_THIRD_PARTY_INCLUDE})
