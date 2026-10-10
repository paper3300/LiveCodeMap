#include "lcm/query/mcp_session.hpp"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <exception>
#include <istream>
#include <ostream>
#include <set>
#include <vector>

#include "lcm/query/render.hpp"

namespace lcm::query {

namespace {

constexpr std::array<std::string_view, 3> kSupportedVersions = {"2025-11-25", "2025-06-18", "2025-03-26"};

std::string line_of(const Json& j) { return j.dump(-1, ' ', false, Json::error_handler_t::replace); }

Json integer(const char* description, std::int64_t default_value, std::int64_t minimum, std::int64_t maximum) {
  return Json{{"type", "integer"},
              {"description", description},
              {"default", default_value},
              {"minimum", minimum},
              {"maximum", maximum}};
}

Json cursor_property() {
  return Json{{"type", "string"},
              {"description", "Opaque next_cursor from a previous response of the same call. Expired cursors return "
                              "status error with diagnostic cursor_expired: re-run without a cursor."}};
}

Json schema(Json properties, std::vector<std::string> required) {
  return Json{{"type", "object"},
              {"properties", std::move(properties)},
              {"required", std::move(required)},
              {"additionalProperties", false}};
}

struct ParamError {
  std::string message;
};

// Typed argument readers; a wrong type is a malformed call (-32602).
void allow_only(const Json& a, std::initializer_list<const char*> names) {
  std::set<std::string> allowed(names.begin(), names.end());
  for (const auto& [k, v] : a.items()) {
    if (!allowed.count(k)) throw ParamError{"unknown argument '" + k + "'"};
  }
}

std::optional<std::string> text_arg(const Json& a, const char* name, bool required) {
  if (!a.contains(name)) {
    if (required) throw ParamError{std::string("missing required argument '") + name + "'"};
    return std::nullopt;
  }
  if (!a[name].is_string()) throw ParamError{std::string("argument '") + name + "' must be a string"};
  return a[name].get<std::string>();
}

std::optional<std::int64_t> int_arg(const Json& a, const char* name) {
  if (!a.contains(name)) return std::nullopt;
  if (!a[name].is_number_integer()) throw ParamError{std::string("argument '") + name + "' must be an integer"};
  return a[name].get<std::int64_t>();
}

std::size_t count_of(std::int64_t v) { return v < 0 ? 0 : static_cast<std::size_t>(v); }

Json call_tool(QueryEngine& engine, const std::string& name, const Json& a) {
  if (name == "find_code") {
    allow_only(a, {"query", "limit", "cursor"});
    FindCodeArgs args;
    args.query = *text_arg(a, "query", true);
    if (auto v = int_arg(a, "limit")) args.limit = count_of(*v);
    args.cursor = text_arg(a, "cursor", false);
    return engine.find_code(args);
  }
  if (name == "trace_calls") {
    allow_only(a, {"selector", "direction", "depth", "all", "limit", "node_budget", "time_budget_ms", "cursor"});
    TraceArgs args;
    args.selector = *text_arg(a, "selector", true);
    if (auto d = text_arg(a, "direction", false)) {
      if (*d == "incoming") {
        args.direction = Direction::incoming;
      } else if (*d == "outgoing") {
        args.direction = Direction::outgoing;
      } else {
        throw ParamError{"argument 'direction' must be \"incoming\" or \"outgoing\""};
      }
    }
    if (auto v = int_arg(a, "depth")) args.depth = static_cast<int>(std::clamp<std::int64_t>(*v, -1, 1'000'000));
    if (a.contains("all")) {
      if (!a["all"].is_boolean()) throw ParamError{"argument 'all' must be a boolean"};
      args.all = a["all"].get<bool>();
    }
    if (auto v = int_arg(a, "limit")) args.limit = count_of(*v);
    if (auto v = int_arg(a, "node_budget")) args.node_budget = count_of(*v);
    if (auto v = int_arg(a, "time_budget_ms")) args.time_budget = std::chrono::milliseconds(*v);
    args.cursor = text_arg(a, "cursor", false);
    return engine.trace_calls(args);
  }
  if (name == "file_api") {
    allow_only(a, {"path", "limit", "cursor"});
    FileApiArgs args;
    args.path = *text_arg(a, "path", true);
    if (auto v = int_arg(a, "limit")) args.limit = count_of(*v);
    args.cursor = text_arg(a, "cursor", false);
    return engine.file_api(args);
  }
  if (name == "check_freshness") {
    allow_only(a, {"budget_ms"});
    CheckArgs args;
    if (auto v = int_arg(a, "budget_ms")) args.budget = std::chrono::milliseconds(*v);
    return engine.check_freshness(args);
  }
  throw ParamError{"unknown tool '" + name + "'"};
}

}  // namespace

Json mcp_tool_list() {
  Json tools = Json::array();
  tools.push_back(Json{
      {"name", "find_code"},
      {"description",
       "Find C++ symbols and files by identifier. Matching tiers, best first: exact identifier (case-sensitive), "
       "qualified name or ::suffix (e.g. 'Foo::Bar'; a leading '::' means the exact full name), identifier "
       "components (camelCase/snake_case parts, e.g. 'Connect' matches TryConnect), then file path. Returns stable "
       "IDs (cm1:...) and locations (path, span, hashes; no source text). Use a returned ID as the trace_calls "
       "selector. Not a natural-language search."},
      {"inputSchema",
       schema(Json{{"query", Json{{"type", "string"}, {"description", "Identifier, qualified name or path fragment."}}},
                   {"limit", integer("Results per page.", 5, 1, 1000)},
                   {"cursor", cursor_property()}},
              {"query"})}});
  tools.push_back(Json{
      {"name", "trace_calls"},
      {"description",
       "Callers (default) or callees of one symbol over the compiler-confirmed call graph. The selector is a stable "
       "ID, a qualified name or a name; an ambiguous name returns status error with candidates (pick an ID). One "
       "record per caller/callee with its grouped callsites; confirmed calls are separated from possible virtual "
       "dispatch through base or override slots. Default 1 hop and 20 records; deeper traversals are bounded by "
       "visited-node and time budgets and return status partial with next_cursor when a budget ends."},
      {"inputSchema",
       schema(Json{{"selector", Json{{"type", "string"}, {"description", "cm1:... stable ID, Qualified::Name or Name."}}},
                   {"direction", Json{{"type", "string"},
                                      {"enum", {"incoming", "outgoing"}},
                                      {"default", "incoming"},
                                      {"description", "incoming = callers, outgoing = callees."}}},
                   {"depth", integer("Hops to traverse.", 1, 1, 5)},
                   {"all", Json{{"type", "boolean"},
                                {"default", false},
                                {"description", "Lift the depth limit (budgets still apply)."}}},
                   {"limit", integer("Records per page.", 20, 1, 1000)},
                   {"node_budget", integer("Maximum newly visited nodes per call.", 20000, 1, 1000000)},
                   {"time_budget_ms", integer("Wall-clock budget per call in milliseconds.", 2000, 1, 60000)},
                   {"cursor", cursor_property()}},
              {"selector"})}});
  tools.push_back(Json{
      {"name", "file_api"},
      {"description",
       "The public API of one physical source file: its direct includes and the symbols declared or defined in it "
       "(with their locations in this file and their other declaration/definition locations), ordered by position. "
       "The path is repository-relative or absolute inside the workspace. Module/target metadata is null (not "
       "recorded)."},
      {"inputSchema",
       schema(Json{{"path", Json{{"type", "string"}, {"description", "File path, e.g. Source/Game/Foo.h."}}},
                   {"limit", integer("Entries per page.", 100, 1, 1000)},
                   {"cursor", cursor_property()}},
              {"path"})}});
  tools.push_back(Json{
      {"name", "check_freshness"},
      {"description",
       "Read-only check of whether the index matches the files on disk: hashes every recorded input twice within "
       "the budget and reports changed or deleted inputs and the affected units. Never builds, never writes; a unit "
       "is fresh only when fully verified (build-action currency included), otherwise unknown or stale with "
       "`livecodemap build` guidance. Budget exhaustion returns status partial."},
      {"inputSchema", schema(Json{{"budget_ms", integer("Inspection budget in milliseconds.", 2000, 1, 60000)}}, {})}});
  return Json{{"tools", std::move(tools)}};
}

McpSession::McpSession(QueryEngine& engine, std::string server_version)
    : engine_(engine), server_version_(std::move(server_version)) {}

std::optional<std::string> McpSession::handle(std::string_view line) {
  const auto failure = [](const Json& id, int code, const std::string& message) {
    return line_of(Json{{"jsonrpc", "2.0"}, {"id", id}, {"error", Json{{"code", code}, {"message", message}}}});
  };
  const Json request = Json::parse(line, nullptr, false);
  if (request.is_discarded()) return failure(nullptr, -32700, "Parse error");
  if (!request.is_object()) return failure(nullptr, -32600, "Invalid Request: expected one JSON-RPC request object");
  const bool has_id = request.contains("id");
  const Json id = has_id ? request["id"] : Json(nullptr);
  if (has_id && !(id.is_string() || id.is_number_integer() || id.is_null())) {
    return failure(nullptr, -32600, "Invalid Request: id must be a string or an integer");
  }
  if (!request.contains("jsonrpc") || request["jsonrpc"] != "2.0" || !request.contains("method") ||
      !request["method"].is_string() || (request.contains("params") && !request["params"].is_object() &&
                                         !request["params"].is_array())) {
    return failure(id, -32600, "Invalid Request");
  }
  if (!has_id) return std::nullopt;  // notification: never answered
  const std::string method = request["method"].get<std::string>();
  const Json params = request.contains("params") ? request["params"] : Json::object();
  const auto success = [&](Json result) {
    return line_of(Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}});
  };
  try {
    if (method == "initialize") {
      std::string requested;
      if (params.is_object() && params.contains("protocolVersion") && params["protocolVersion"].is_string()) {
        requested = params["protocolVersion"].get<std::string>();
      }
      protocol_version_ = std::string(kLatestProtocolVersion);
      for (const auto v : kSupportedVersions) {
        if (requested == v) protocol_version_ = requested;
      }
      return success(Json{{"protocolVersion", protocol_version_},
                          {"capabilities", Json{{"tools", Json::object()}}},
                          {"serverInfo", Json{{"name", "livecodemap"}, {"version", server_version_}}}});
    }
    if (method == "ping") return success(Json::object());
    if (method == "tools/list") return success(mcp_tool_list());
    if (method == "tools/call") {
      if (!params.is_object() || !params.contains("name") || !params["name"].is_string() ||
          (params.contains("arguments") && !params["arguments"].is_object())) {
        return failure(id, -32602, "Invalid params: tools/call needs a tool name and an arguments object");
      }
      const Json arguments = params.contains("arguments") ? params["arguments"] : Json::object();
      Json response;
      try {
        response = call_tool(engine_, params["name"].get<std::string>(), arguments);
      } catch (const ParamError& e) {
        return failure(id, -32602, "Invalid params: " + e.message);
      }
      const bool error = is_error(response);
      Json content = Json::array();
      content.push_back(Json{{"type", "text"}, {"text", line_of(response)}});
      content.push_back(Json{{"type", "text"}, {"text", render_text(response)}});
      return success(Json{{"content", std::move(content)}, {"structuredContent", std::move(response)}, {"isError", error}});
    }
    return failure(id, -32601, "Method not found: " + method);
  } catch (const std::exception& e) {
    return failure(id, -32603, std::string("Internal error: ") + e.what());
  }
}

void McpSession::serve(std::istream& in, std::ostream& out) {
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.find_first_not_of(" \t") == std::string::npos) continue;
    if (const auto reply = handle(line)) {
      out << *reply << '\n';
      out.flush();
    }
  }
}

}  // namespace lcm::query
