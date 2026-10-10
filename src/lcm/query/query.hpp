// Read-only query layer over a LiveCodeMap store: find_code, trace_calls,
// file_api and check_freshness with the common response model (PRD FR-API-003,
// FR-API-004, FR-RET-003, FR-IDX-004/005; contract R1-R11, D1-D8).
//
// Every call takes its own read snapshot of one long-lived read-only
// connection (ReadOnlyStore) and returns the response object that the CLI
// prints with --json and MCP returns as structured content. Failures are
// responses with status "error", never exceptions. See docs/query.md.
#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace lcm::query {

using Json = nlohmann::ordered_json;

// Limits a caller may request (R9 diagnostics outside these bounds).
inline constexpr std::size_t kMaxLimit = 1000;
inline constexpr int kMaxDepth = 5;
inline constexpr std::size_t kMaxNodeBudget = 1'000'000;
inline constexpr std::chrono::milliseconds kMaxTimeBudget{60'000};

enum class Direction { incoming, outgoing };

struct FindCodeArgs {
  std::string query;
  std::size_t limit = 5;  // MCP default; the CLI passes 8
  std::optional<std::string> cursor;
};

struct TraceArgs {
  std::string selector;  // stable ID, qualified name or simple name
  Direction direction = Direction::incoming;
  int depth = 1;
  bool all = false;  // lifts the depth limit only; budgets still apply
  std::size_t limit = 20;
  std::size_t node_budget = 20'000;
  std::chrono::milliseconds time_budget{2000};
  std::optional<std::string> cursor;
};

struct FileApiArgs {
  std::string path;  // repository-relative or absolute under the workspace root
  std::size_t limit = 100;
  std::optional<std::string> cursor;
};

struct CheckArgs {
  std::chrono::milliseconds budget{2000};
};

struct EngineOptions {
  std::optional<std::filesystem::path> db;
  std::optional<std::string> profile;            // required when the store has several profiles
  std::optional<std::filesystem::path> root;     // when given, must be the store's workspace root
  std::chrono::milliseconds search_budget{2000};  // find_code time budget, checked between tiers
  // Test hook only; the CLI and MCP never set it. When set, check_freshness
  // passes it to the inspection as InspectRequest::before_probe (called before
  // each input file of pass 1 and pass 2). Empty by default: no effect.
  std::function<void(int pass, const std::string& path_utf8)> freshness_probe_hook;
};

class QueryEngine {
 public:
  explicit QueryEngine(EngineOptions options);
  ~QueryEngine();
  QueryEngine(const QueryEngine&) = delete;
  QueryEngine& operator=(const QueryEngine&) = delete;

  [[nodiscard]] Json find_code(const FindCodeArgs& args);
  [[nodiscard]] Json trace_calls(const TraceArgs& args);
  [[nodiscard]] Json file_api(const FileApiArgs& args);
  [[nodiscard]] Json check_freshness(const CheckArgs& args);

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

// "error" responses make the CLI exit non-zero and MCP set isError.
[[nodiscard]] bool is_error(const Json& response);

}  // namespace lcm::query
