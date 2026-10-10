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

# SQLite 3.53.4 amalgamation, public domain. The coordinator-verified archive
# under build/deps/downloads is used when present; otherwise the official
# origin. Either way the SHA-256 below is enforced.
set(LCM_SQLITE_ARCHIVE "${CMAKE_SOURCE_DIR}/build/deps/downloads/sqlite-amalgamation-3530400.zip")
if(EXISTS "${LCM_SQLITE_ARCHIVE}")
  set(_lcm_sqlite_url "${LCM_SQLITE_ARCHIVE}")
else()
  set(_lcm_sqlite_url "https://www.sqlite.org/2026/sqlite-amalgamation-3530400.zip")
endif()
FetchContent_Declare(sqlite_amalgamation
  URL ${_lcm_sqlite_url}
  URL_HASH SHA256=1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d)
FetchContent_MakeAvailable(sqlite_amalgamation)

# Compiled as C. Threading stays serialized (the default); extension loading
# is omitted and double-quoted string literals are disabled. Foreign keys are
# enabled per connection by the store.
add_library(lcm_sqlite3 STATIC ${sqlite_amalgamation_SOURCE_DIR}/sqlite3.c)
set_target_properties(lcm_sqlite3 PROPERTIES LINKER_LANGUAGE C)
target_include_directories(lcm_sqlite3 SYSTEM PUBLIC ${sqlite_amalgamation_SOURCE_DIR})
target_compile_definitions(lcm_sqlite3 PRIVATE
  SQLITE_OMIT_LOAD_EXTENSION
  SQLITE_DQS=0)

# Lay the single headers out under conventional include paths.
set(LCM_THIRD_PARTY_INCLUDE ${CMAKE_BINARY_DIR}/third_party/include)
configure_file(${nlohmann_json_single_SOURCE_DIR}/json.hpp ${LCM_THIRD_PARTY_INCLUDE}/nlohmann/json.hpp COPYONLY)
configure_file(${doctest_single_SOURCE_DIR}/doctest.h ${LCM_THIRD_PARTY_INCLUDE}/doctest/doctest.h COPYONLY)

add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json SYSTEM INTERFACE ${LCM_THIRD_PARTY_INCLUDE})
target_compile_definitions(nlohmann_json INTERFACE JSON_USE_IMPLICIT_CONVERSIONS=0)

add_library(doctest INTERFACE)
target_include_directories(doctest SYSTEM INTERFACE ${LCM_THIRD_PARTY_INCLUDE})
