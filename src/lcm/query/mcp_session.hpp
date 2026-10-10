// MCP server session over newline-delimited JSON-RPC 2.0 (contract R6).
//
// Protocol revision: `initialize` answers with the client's requested revision
// when it is one of 2025-11-25, 2025-06-18 or 2025-03-26, otherwise with
// 2025-11-25. Supported requests: initialize, ping, tools/list, tools/call.
// Notifications (any message without an id) never get a response. Unknown
// methods are -32601; invalid JSON -32700; an invalid envelope (including a
// batch array) -32600; malformed tools/call parameters or an unknown tool
// -32602. A tool-level error is a tool result with isError true.
#pragma once

#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>

#include "lcm/query/query.hpp"

namespace lcm::query {

inline constexpr std::string_view kLatestProtocolVersion = "2025-11-25";

class McpSession {
 public:
  McpSession(QueryEngine& engine, std::string server_version);

  // One input line; the response line (no trailing newline), or nullopt.
  [[nodiscard]] std::optional<std::string> handle(std::string_view line);

  // Serves until end of input; one JSON object per output line.
  void serve(std::istream& in, std::ostream& out);

  // The revision agreed by the last initialize (empty before it).
  [[nodiscard]] const std::string& protocol_version() const { return protocol_version_; }

 private:
  QueryEngine& engine_;
  std::string server_version_;
  std::string protocol_version_;
};

// The tools/list result: exactly the four tools with their input schemas.
[[nodiscard]] Json mcp_tool_list();

}  // namespace lcm::query
