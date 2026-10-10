// Shared machinery of the query tools: one call bound to one snapshot, the
// common response envelope, pagination (R3), cursor binding (R2) and status
// selection (R1). Internal to lcm_query.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "lcm/query/cursor.hpp"
#include "lcm/query/query.hpp"
#include "lcm/store/store.hpp"

namespace lcm::query {

struct QueryEngine::Impl {
  EngineOptions options;
  std::optional<store::ReadOnlyStore> store;
  std::string db_identity;

  // Distinct symbol names with their components, per (profile, fact generation).
  std::string names_key;
  std::vector<std::pair<std::string, std::vector<std::string>>> names;
};

namespace detail {

struct Page {
  bool complete = true;
  std::uint64_t discovered = 0;
  std::uint64_t returned_before = 0;  // records emitted by earlier pages of the chain
  std::uint64_t shown = 0;
  std::optional<std::string> budget_reason;  // time_budget | node_budget when incomplete
  // Builds the continuation cursor for the given cumulative returned count;
  // empty for a result that has no continuation (check_freshness).
  std::function<std::string(std::uint64_t returned)> cursor;
};

class Call {
 public:
  Call(QueryEngine::Impl& engine, std::string tool);

  // Opens the store if needed, takes the snapshot, selects the profile and
  // captures the public view. On failure returns the error response.
  [[nodiscard]] std::optional<Json> begin();
  // Ends the snapshot (check_freshness runs its own snapshots afterwards).
  void end_snapshot();

  [[nodiscard]] const store::ReadSnapshot& snap() const { return *snap_; }
  [[nodiscard]] const store::PublicView& view() const { return *view_; }
  [[nodiscard]] const store::ProfileInfo& profile() const { return info_; }
  [[nodiscard]] QueryEngine::Impl& engine() { return engine_; }
  [[nodiscard]] const std::string& tool() const { return tool_; }

  // Error response with the envelope fields known so far.
  [[nodiscard]] Json error(const std::string& code, const std::string& message) const;
  void diagnostic(const std::string& code, const std::string& message);

  // Cursor binding. `binding_args` are the normalized arguments that bind
  // (never limit or budgets). `accept` returns an error response for an
  // invalid or expired cursor and fills `position` otherwise.
  [[nodiscard]] std::string make_cursor(const nlohmann::json& binding_args, const nlohmann::json& position) const;
  [[nodiscard]] std::optional<Json> accept(const std::string& cursor, const nlohmann::json& binding_args,
                                           nlohmann::json& position) const;

  // R1: a returned fact owned by a unit whose persisted semantic state is stale.
  void note_unit(const std::string& fact_id, std::int64_t unit_row);
  [[nodiscard]] bool unit_stale(std::int64_t unit_row) const;
  [[nodiscard]] const std::string& unit_label(std::int64_t unit_row) const;

  // The complete response: status (R1), pagination (R3), result.
  [[nodiscard]] Json finish(Json result, const Page& page);

 private:
  [[nodiscard]] Json envelope() const;

  QueryEngine::Impl& engine_;
  std::string tool_;
  std::optional<store::ReadSnapshot> snap_;
  std::optional<store::PublicView> view_;
  store::ProfileInfo info_;
  std::optional<store::ProfileStatus> status_;
  std::unordered_map<std::int64_t, store::UnitSummaryRow> units_;  // unit row -> label, persisted semantic state
  std::vector<std::pair<std::string, std::string>> diagnostics_;
  std::set<std::string> stale_ids_;
};

// JSON of facts. Locations carry the repository-relative path, span, span
// hash and file content hash; never source text.
[[nodiscard]] Json location_json(const SourceLocation& location);
[[nodiscard]] std::string qualified_name(const CanonicalKey& key);

// Deterministic primary location: visible non-implicit locations, definition
// before declaration, then (path, begin offset, end offset, unit label); an
// implicit declaration only when nothing else is visible; none for a
// placeholder.
[[nodiscard]] std::optional<store::LocationRow> primary_location(Call& call, std::int64_t symbol_row);

// A public symbol with its primary location (null when there is none).
[[nodiscard]] Json symbol_json(Call& call, const store::SymbolRow& symbol, SymbolPresence presence);

struct ResolvedSelector {
  std::optional<store::SymbolRow> symbol;  // the unique public match
  std::optional<Json> error;               // invalid/unknown selector, or the ambiguity response
};

// trace_calls selector: a well-formed stable ID, or a name resolved with the
// exact (tier 1) or qualified (tier 2) rule over the complete public match
// set. Ambiguity returns the paginated candidate response.
[[nodiscard]] ResolvedSelector resolve_selector(Call& call, const std::string& selector, std::size_t limit,
                                                const std::optional<std::string>& cursor);

// Public exact or qualified matches of a name (tier 1 / tier 2 rules),
// implicit_hidden excluded, in the R9 tie order.
struct NameMatch {
  store::SymbolRow symbol;
  SymbolPresence presence = SymbolPresence::indexed;
  std::string qualified;
};
// R9 tie order: normalized qualified name, kind, normalized signature, stable ID.
[[nodiscard]] bool tie_less(const NameMatch& a, const NameMatch& b);
[[nodiscard]] std::optional<std::vector<NameMatch>> name_matches(Call& call, const std::string& name,
                                                                std::string* invalid_reason);

// check_freshness pagination (C1): complete only for a complete inspection or
// when every unit is already detected stale; otherwise limit_reason is
// time_budget or generation_changed. It never has a continuation cursor.
struct FreshnessPage {
  bool complete = true;
  std::optional<std::string> limit_reason;
};
[[nodiscard]] FreshnessPage freshness_page(store::InspectionOutcome outcome, std::size_t units, std::size_t stale_units);
// The diagnostic (code, message) of an incomplete inspection, consistent with
// its page: an exact all-stale count is not called inexact.
[[nodiscard]] std::pair<std::string, std::string> freshness_diagnostic(store::InspectionOutcome outcome,
                                                                       const FreshnessPage& page,
                                                                       std::size_t unchecked_inputs);

[[nodiscard]] Json trace_calls(Call& call, const TraceArgs& args);
[[nodiscard]] Json file_api(Call& call, const FileApiArgs& args);

}  // namespace detail
}  // namespace lcm::query
