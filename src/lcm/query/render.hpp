// Human-readable rendering of a query response (CLI output and the optional
// second MCP text block). The JSON response stays the authoritative form.
#pragma once

#include <string>

#include "lcm/query/query.hpp"

namespace lcm::query {

[[nodiscard]] std::string render_text(const Json& response);

}  // namespace lcm::query
