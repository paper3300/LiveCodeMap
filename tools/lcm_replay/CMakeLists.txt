# Minimal single-TU replay tool. Calls the public analysis API only; it repairs
# nothing and substitutes nothing, so a rejection here is the product contract
# speaking, not the tool.
add_executable(lcm_replay main.cpp)
target_link_libraries(lcm_replay PRIVATE lcm_analyzer nlohmann_json lcm_warnings)
lcm_configure_clang_target(lcm_replay)
