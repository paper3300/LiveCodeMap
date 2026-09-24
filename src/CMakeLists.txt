add_library(lcm_core STATIC
  lcm/hash.cpp
  lcm/source.cpp
  lcm/identity.cpp
  lcm/facts.cpp
  lcm/status.cpp
  lcm/compile_db.cpp
  lcm/response_file.cpp
  lcm/compile_context.cpp)

target_include_directories(lcm_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lcm_core
  PUBLIC lcm_warnings
  PRIVATE nlohmann_json)

if(LCM_CLANG_FOUND)
  add_subdirectory(lcm/clang)
  add_subdirectory(lcm/analyzer)
endif()
