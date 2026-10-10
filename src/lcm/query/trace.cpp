// trace_calls: call-graph traversal over public `calls` relations (contract
// R7, R8, D4, D8).
//
// Level-synchronous BFS from the selected symbol (hop 0). A record is keyed by
// (direction, other symbol) and is sealed when it is discovered: all of its
// public callsites into the previous level are collected at once, so no later
// page changes an emitted record and each record reports its shortest hop.
//
// Discovery order (deterministic on one snapshot): levels in hop order; inside
// a level the previous level's records in their discovery order; for each of
// them its slots, and for each slot its relations in relation-row order.
//   incoming: slots are the node itself, then its public `overrides` bases
//             (breadth-first, relation-row order). Callers of a base reach the
//             node only through virtual dispatch: those callsites are `possible`.
//   outgoing: the node's callees in relation-row order; after a callee whose
//             visible evidence includes a virtual-slot call, the public
//             overriders of that slot (breadth-first) as `possible` branches.
// A confirmed virtual-slot edge stays confirmed to its slot only; a record's
// path is confirmed only when it has a confirmed callsite into a node whose
// own path is confirmed. Unresolved sites of the selected body are listed in
// the outgoing result and never traversed.
//
// Continuation replays the traversal from the selected symbol (the cursor
// holds counters only); the visited-node budget counts records beyond the
// furthest point an earlier page discovered, so every continuation advances.

#include <algorithm>
#include <chrono>
#include <climits>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "lcm/query/query_internal.hpp"

namespace lcm::query::detail {

namespace {

struct SiteKey {
  std::int64_t caller = 0;
  std::int64_t slot = 0;
  std::int64_t file = -1;  // file row; -1 for no file
  SourceSpan span;
  std::optional<Sha256Digest> span_hash;
  DispatchKind dispatch = DispatchKind::static_target;
  friend auto operator<=>(const SiteKey&, const SiteKey&) = default;
};

struct Site {
  std::int64_t version_row = 0;  // located file state, resolved for output only
  Confidence relation_confidence = Confidence::confirmed;
  std::set<std::int64_t> unit_rows;
};

struct Record {
  std::int64_t other = 0;
  int hop = 0;
  bool path_confirmed = false;
  std::map<SiteKey, Site> confirmed;
  std::map<SiteKey, Site> possible;
};

class Traversal {
 public:
  Traversal(Call& call, const TraceArgs& args, std::int64_t start, std::uint64_t prior_discovered)
      : call_(call),
        incoming_(args.direction == Direction::incoming),
        max_depth_(args.all ? INT_MAX : args.depth),
        node_limit_(prior_discovered + args.node_budget),
        deadline_(std::chrono::steady_clock::now() + args.time_budget),
        start_(start) {}

  void run() {
    visited_.insert(start_);
    std::vector<std::int64_t> frontier{start_};
    path_confirmed_[start_] = true;
    for (int hop = 1; hop <= max_depth_ && !frontier.empty() && !stop_; ++hop) {
      frontier_set_ = std::unordered_set<std::int64_t>(frontier.begin(), frontier.end());
      frontier_sorted_ = std::set<std::int64_t>(frontier.begin(), frontier.end());
      base_slots_.clear();
      if (incoming_) {
        for (const std::int64_t n : frontier) {
          for (const std::int64_t b : bases_of(n)) base_slots_.insert(b);
        }
      }
      slot_targets_ = frontier_sorted_;
      slot_targets_.insert(base_slots_.begin(), base_slots_.end());
      rejected_.clear();
      level_.clear();
      hop_ = hop;
      for (const std::int64_t n : frontier) {
        expand(n);
        if (stop_) break;
      }
      frontier = level_;
    }
  }

  [[nodiscard]] bool complete() const { return !stop_; }
  [[nodiscard]] const std::optional<std::string>& stop_reason() const { return stop_reason_; }
  [[nodiscard]] const std::vector<Record>& records() const { return records_; }

 private:
  bool is_public(std::int64_t row) { return call_.view().presence(row).has_value(); }

  const std::vector<store::SiteRow>& visible_evidence(std::int64_t relation_row) {
    auto it = evidence_.find(relation_row);
    if (it == evidence_.end()) {
      std::vector<store::SiteRow> rows;
      for (auto& e : call_.snap().evidence_sites(relation_row)) {
        if (!call_.view().file_withheld(e.file_row)) rows.push_back(std::move(e));
      }
      it = evidence_.emplace(relation_row, std::move(rows)).first;
    }
    return it->second;
  }

  // Public symbols reachable over public `overrides` relations (up: bases,
  // down: overriders), breadth-first. A relation is public, as everywhere
  // else, only with both endpoints public and at least one visible evidence
  // site; an override known only from a withheld file expands nothing.
  const std::vector<std::int64_t>& override_closure(std::int64_t node, bool up) {
    auto& memo = up ? bases_ : overriders_;
    auto it = memo.find(node);
    if (it != memo.end()) return it->second;
    std::vector<std::int64_t> out;
    std::set<std::int64_t> seen{node};
    for (std::size_t i = 0, n = 1; i < n; ++i) {
      const std::int64_t x = i == 0 ? node : out[i - 1];
      for (const auto& r : up ? call_.snap().relations_from(x, RelationKind::overrides)
                              : call_.snap().relations_to(x, RelationKind::overrides)) {
        const std::int64_t y = up ? r.target_row : r.source_row;
        if (seen.count(y) || !is_public(y) || visible_evidence(r.row).empty()) continue;
        seen.insert(y);
        out.push_back(y);
        ++n;
      }
    }
    return memo.emplace(node, std::move(out)).first->second;
  }
  const std::vector<std::int64_t>& bases_of(std::int64_t node) { return override_closure(node, true); }
  const std::vector<std::int64_t>& overriders_of(std::int64_t node) { return override_closure(node, false); }

  void add_site(Record& r, bool possible, std::int64_t caller, std::int64_t slot, const store::SiteRow& e,
                Confidence relation_confidence) {
    SiteKey key{caller, slot, e.file_row.value_or(-1), e.span, e.span_hash, e.dispatch};
    Site& s = (possible ? r.possible : r.confirmed)[key];
    s.version_row = e.version_row;
    s.relation_confidence = relation_confidence;
    s.unit_rows.insert(e.unit_row);
  }

  // Relations of one kind from `source` into any of `targets` (incoming
  // sealing) or into `target` from any of `sources` (outgoing sealing). A
  // small set is probed pairwise through UNIQUE(profile, source, target, ...);
  // otherwise the symbol's own relations are scanned and filtered.
  static constexpr std::size_t kPairProbeLimit = 16;

  std::vector<store::RelationRow> calls_from_into(std::int64_t source, const std::set<std::int64_t>& targets) {
    if (targets.size() > kPairProbeLimit) {
      std::vector<store::RelationRow> out;
      for (auto& r : call_.snap().relations_from(source, RelationKind::calls)) {
        if (targets.count(r.target_row)) out.push_back(r);
      }
      return out;
    }
    std::vector<store::RelationRow> out;
    for (const std::int64_t t : targets) {
      for (auto& r : call_.snap().relations_between(call_.profile().row, source, t, RelationKind::calls)) out.push_back(r);
    }
    return out;
  }

  std::vector<store::RelationRow> calls_into_from(std::int64_t target, const std::set<std::int64_t>& sources) {
    if (sources.size() > kPairProbeLimit) {
      std::vector<store::RelationRow> out;
      for (auto& r : call_.snap().relations_to(target, RelationKind::calls)) {
        if (sources.count(r.source_row)) out.push_back(r);
      }
      return out;
    }
    std::vector<store::RelationRow> out;
    for (const std::int64_t s : sources) {
      for (auto& r : call_.snap().relations_between(call_.profile().row, s, target, RelationKind::calls)) out.push_back(r);
    }
    return out;
  }

  // All public callsites of `other` into the previous level.
  Record seal(std::int64_t other) {
    Record r;
    r.other = other;
    r.hop = hop_;
    if (incoming_) {
      for (const auto& rel : calls_from_into(other, slot_targets_)) {
        const bool own = frontier_set_.count(rel.target_row) != 0;
        const bool base = base_slots_.count(rel.target_row) != 0;
        for (const auto& e : visible_evidence(rel.row)) {
          if (own) {
            const bool possible = rel.confidence == Confidence::possible;
            add_site(r, possible, other, rel.target_row, e, rel.confidence);
            if (!possible && path_confirmed_[rel.target_row]) r.path_confirmed = true;
          }
          if (base && e.dispatch == DispatchKind::virtual_slot) {
            add_site(r, true, other, rel.target_row, e, rel.confidence);
          }
        }
      }
    } else {
      for (const auto& rel : calls_into_from(other, frontier_sorted_)) {
        for (const auto& e : visible_evidence(rel.row)) {
          const bool possible = rel.confidence == Confidence::possible;
          add_site(r, possible, rel.source_row, other, e, rel.confidence);
          if (!possible && path_confirmed_[rel.source_row]) r.path_confirmed = true;
        }
      }
      for (const std::int64_t base : bases_of(other)) {
        for (const auto& rel : calls_into_from(base, frontier_sorted_)) {
          for (const auto& e : visible_evidence(rel.row)) {
            if (e.dispatch == DispatchKind::virtual_slot) add_site(r, true, rel.source_row, base, e, rel.confidence);
          }
        }
      }
    }
    return r;
  }

  void discover(std::int64_t other) {
    if (stop_ || visited_.count(other) || rejected_.count(other)) return;
    if (records_.size() >= node_limit_) {
      stop("node_budget");
      return;
    }
    if (std::chrono::steady_clock::now() >= deadline_) {
      stop("time_budget");
      return;
    }
    if (!is_public(other)) {
      rejected_.insert(other);
      return;
    }
    Record r = seal(other);
    if (r.confirmed.empty() && r.possible.empty()) {
      rejected_.insert(other);
      return;
    }
    path_confirmed_[other] = r.path_confirmed;
    visited_.insert(other);
    level_.push_back(other);
    records_.push_back(std::move(r));
  }

  void expand(std::int64_t node) {
    if (incoming_) {
      std::vector<std::int64_t> slots{node};
      const auto& bases = bases_of(node);
      slots.insert(slots.end(), bases.begin(), bases.end());
      for (const std::int64_t slot : slots) {
        for (const auto& rel : call_.snap().relations_to(slot, RelationKind::calls)) {
          discover(rel.source_row);
          if (stop_) return;
        }
      }
    } else {
      for (const auto& rel : call_.snap().relations_from(node, RelationKind::calls)) {
        discover(rel.target_row);
        if (stop_) return;
        bool dynamic = false;
        for (const auto& e : visible_evidence(rel.row)) dynamic = dynamic || e.dispatch == DispatchKind::virtual_slot;
        if (!dynamic) continue;
        for (const std::int64_t o : overriders_of(rel.target_row)) {
          discover(o);
          if (stop_) return;
        }
      }
    }
  }

  void stop(const char* reason) {
    stop_ = true;
    stop_reason_ = reason;
  }

  Call& call_;
  bool incoming_;
  int max_depth_;
  std::uint64_t node_limit_;
  std::chrono::steady_clock::time_point deadline_;
  std::int64_t start_;
  int hop_ = 0;
  bool stop_ = false;
  std::optional<std::string> stop_reason_;
  std::vector<Record> records_;
  std::unordered_set<std::int64_t> visited_;
  std::unordered_set<std::int64_t> rejected_;  // checked in this level: not public or no public callsite
  std::vector<std::int64_t> level_;
  std::unordered_set<std::int64_t> frontier_set_;
  std::set<std::int64_t> frontier_sorted_;  // the previous level
  std::set<std::int64_t> slot_targets_;     // incoming: the previous level and its base slots
  std::unordered_set<std::int64_t> base_slots_;
  std::unordered_map<std::int64_t, bool> path_confirmed_;
  std::unordered_map<std::int64_t, std::vector<store::SiteRow>> evidence_;
  std::unordered_map<std::int64_t, std::vector<std::int64_t>> bases_;
  std::unordered_map<std::int64_t, std::vector<std::int64_t>> overriders_;
};

// Named diagnostics for out-of-range arguments (R9): code and message.
std::optional<std::pair<std::string, std::string>> trace_args_problem(const TraceArgs& a) {
  if (a.limit == 0 || a.limit > kMaxLimit) {
    return std::pair{std::string("invalid_limit"), "limit must be between 1 and " + std::to_string(kMaxLimit)};
  }
  if (!a.all && (a.depth < 1 || a.depth > kMaxDepth)) {
    return std::pair{std::string("invalid_depth"), "depth must be between 1 and " + std::to_string(kMaxDepth)};
  }
  if (a.node_budget == 0 || a.node_budget > kMaxNodeBudget) {
    return std::pair{std::string("invalid_budget"), "node_budget must be between 1 and " + std::to_string(kMaxNodeBudget)};
  }
  if (a.time_budget <= std::chrono::milliseconds(0) || a.time_budget > kMaxTimeBudget) {
    return std::pair{std::string("invalid_budget"),
                     "time_budget must be between 1 and " + std::to_string(kMaxTimeBudget.count()) + " ms"};
  }
  return std::nullopt;
}

}  // namespace

Json trace_calls(Call& call, const TraceArgs& args) {
  if (auto p = trace_args_problem(args)) return call.error(p->first, p->second);
  ResolvedSelector selected = resolve_selector(call, args.selector, args.limit, args.cursor);
  if (selected.error) return std::move(*selected.error);
  const store::SymbolRow& start = *selected.symbol;
  const bool incoming = args.direction == Direction::incoming;
  const nlohmann::json binding{{"mode", "trace"},
                               {"selected", start.id.value},
                               {"direction", incoming ? "incoming" : "outgoing"},
                               {"depth", args.all ? nlohmann::json("all") : nlohmann::json(args.depth)}};
  std::uint64_t returned = 0;
  std::uint64_t prior = 0;
  if (args.cursor) {
    nlohmann::json pos;
    if (auto e = call.accept(*args.cursor, binding, pos)) return *e;
    if (!pos.contains("returned") || !pos["returned"].is_number_unsigned() || !pos.contains("discovered") ||
        !pos["discovered"].is_number_unsigned() || pos["returned"].get<std::uint64_t>() > pos["discovered"].get<std::uint64_t>()) {
      return call.error("invalid_cursor", "the cursor position is malformed; re-run the query without a cursor");
    }
    returned = pos["returned"].get<std::uint64_t>();
    prior = pos["discovered"].get<std::uint64_t>();
  }

  Traversal t(call, args, start.row, prior);
  t.run();
  const auto& records = t.records();

  std::unordered_map<std::int64_t, std::string> ids;
  const auto id_of = [&](std::int64_t row) -> const std::string& {
    auto it = ids.find(row);
    if (it == ids.end()) it = ids.emplace(row, call.snap().symbol(row).id.value).first;
    return it->second;
  };
  std::unordered_map<std::int64_t, store::VersionRow> versions;
  const auto sites_json = [&](const std::string& record_id, const std::map<SiteKey, Site>& sites) {
    Json out = Json::array();
    for (const auto& [key, site] : sites) {
      auto v = versions.find(site.version_row);
      if (v == versions.end()) v = versions.emplace(site.version_row, call.snap().file_version(site.version_row)).first;
      const SourceLocation location{RepoRelativePath{v->second.path, v->second.path_as_written}, v->second.content_hash,
                                    key.span, key.span_hash};
      std::set<std::string> units;
      for (const std::int64_t u : site.unit_rows) {
        call.note_unit(record_id, u);
        units.insert(call.unit_label(u));
      }
      out.push_back(Json{{"caller", id_of(key.caller)},
                         {"slot", id_of(key.slot)},
                         {"location", location_json(location)},
                         {"dispatch", std::string(to_string(key.dispatch))},
                         {"relation_confidence", std::string(to_string(site.relation_confidence))},
                         {"units", std::vector<std::string>(units.begin(), units.end())}});
    }
    return out;
  };

  Json page_records = Json::array();
  const std::uint64_t end = std::min<std::uint64_t>(records.size(), returned + args.limit);
  for (std::uint64_t i = returned; i < end; ++i) {
    const Record& r = records[i];
    const store::SymbolRow other = call.snap().symbol(r.other);
    Json rec;
    rec["direction"] = incoming ? "incoming" : "outgoing";
    rec["hop"] = r.hop;
    rec["symbol"] = symbol_json(call, other, *call.view().presence(r.other));
    rec["path_confidence"] = r.path_confirmed ? "confirmed" : "possible";
    rec["confirmed"] = sites_json(other.id.value, r.confirmed);
    rec["possible"] = sites_json(other.id.value, r.possible);
    page_records.push_back(std::move(rec));
  }

  std::uint64_t confirmed_sites = 0;
  std::uint64_t possible_sites = 0;
  for (const auto& r : records) {
    confirmed_sites += r.confirmed.size();
    possible_sites += r.possible.size();
  }
  Json result;
  result["selected"] = symbol_json(call, start, *call.view().presence(start.row));
  result["direction"] = incoming ? "incoming" : "outgoing";
  result["depth"] = args.all ? Json("all") : Json(args.depth);
  result["budgets"] = Json{{"node_budget", args.node_budget}, {"time_budget_ms", args.time_budget.count()}};
  result["counts"] = Json{{"exact", t.complete()},
                          {incoming ? "unique_callers" : "unique_callees", records.size()},
                          {"callsites", confirmed_sites + possible_sites},
                          {"confirmed_callsites", confirmed_sites},
                          {"possible_callsites", possible_sites}};
  result["records"] = std::move(page_records);
  if (!incoming) {
    // Unresolved sites of the selected body only; never traversed (R7). First page only.
    Json unresolved = Json::array();
    if (returned == 0) {
      for (const auto& u : call.snap().unresolved_in(start.row)) {
        if (call.view().file_withheld(u.file_row)) continue;
        call.note_unit(start.id.value, u.unit_row);
        unresolved.push_back(Json{{"expression", u.site.expression},
                                  {"reason", std::string(to_string(u.site.reason))},
                                  {"location", location_json(u.site.location)},
                                  {"unit", u.site.analysis_unit}});
      }
    }
    result["unresolved"] = std::move(unresolved);
  }

  Page page;
  page.complete = t.complete();
  page.discovered = std::max<std::uint64_t>(records.size(), prior);
  page.returned_before = returned;
  page.shown = end > returned ? end - returned : 0;
  page.budget_reason = t.stop_reason();
  page.cursor = [&](std::uint64_t r) {
    return call.make_cursor(binding, nlohmann::json{{"returned", r}, {"discovered", std::max<std::uint64_t>(records.size(), prior)}});
  };
  return call.finish(std::move(result), page);
}

}  // namespace lcm::query::detail
