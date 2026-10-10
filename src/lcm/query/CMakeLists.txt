# Read-only query layer, CLI and MCP session; independent of the Clang SDK.
add_library(lcm_query STATIC
  identifier.cpp
  cursor.cpp
  query.cpp
  trace.cpp
  file_api.cpp
  render.cpp
  mcp_session.cpp
  cli.cpp)
target_include_directories(lcm_query PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(lcm_query
  PUBLIC lcm_store lcm_core nlohmann_json
  PRIVATE lcm_warnings)
target_compile_definitions(lcm_query PRIVATE LCM_QUERY_VERSION="${PROJECT_VERSION}")
