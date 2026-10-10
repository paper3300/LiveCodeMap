#include "lcm/query/query.hpp"

#include <algorithm>
#include <cstdio>
#include <exception>
#include <map>
#include <tuple>

#include "lcm/identity.hpp"
#include "lcm/query/identifier.hpp"
#include "lcm/query/query_internal.hpp"

namespace lcm::query {

namespace fs = std::filesystem;

bool is_error(const Json& response) {
  return response.is_object() && response.contains("status") && response["status"] == "error";
}

namespace detail {

namespace {

std::string iso8601(Timestamp t) {
  const auto days = std::chrono::floor<std::chrono::days>(t);
  const std::chrono::year_month_day ymd{days};
  const std::chrono::hh_mm_ss<std::chrono::seconds> hms{t - days};
  char buffer[32];
  std::snprintf(buffer, sizeof buffer, "%04d-%02u-%02uT%02d:%02d:%02dZ", static_cast<int>(ymd.year()),
                static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()),
                static_cast<int>(hms.hours().count()), static_cast<int>(hms.minutes().count()),
                static_cast<int>(hms.seconds().count()));
  return buffer;
}

Json optional_time(const std::optional<Timestamp>& t) { return t ? Json(iso8601(*t)) : Json(nullptr); }

Json layer_json(const LayerState& s) {
  Json j;
  j["layer"] = std::string(to_string(s.layer));
  j["applicable"] = s.applicable;
  j["availability"] = std::string(to_string(s.availability));
  j["freshness"] = std::string(to_string(s.freshness));
  j["units"] = Json{{"total", s.units.total},
                    {"succeeded", s.units.succeeded},
                    {"failed", s.units.failed},
                    {"stale", s.units.stale},
                    {"ignored", s.units.ignored}};
  j["last_success"] = optional_time(s.last_success);
  j["last_attempt"] = optional_time(s.last_attempt);
  j["reason"] = s.reason;
  return j;
}

std::string open_failure_code(store::ReadOnlyFailure f) {
  switch (f) {
    case store::ReadOnlyFailure::missing:
      return "db_missing";
    case store::ReadOnlyFailure::foreign:
      return "db_foreign";
    case store::ReadOnlyFailure::schema_version:
      return "db_schema_version";
    case store::ReadOnlyFailure::root_mismatch:
      return "db_root_mismatch";
  }
  return "db_error";
}

std::string join(const std::vector<std::string>& items, const char* separator) {
  std::string out;
  for (const auto& i : items) {
    if (!out.empty()) out += separator;
    out += i;
  }
  return out;
}

}  // namespace

Call::Call(QueryEngine::Impl& engine, std::string tool) : engine_(engine), tool_(std::move(tool)) {}

std::optional<Json> Call::begin() {
  if (!engine_.store) {
    if (!engine_.options.db) {
      return error("db_missing", "no database given; pass --db <path> or set LIVECODEMAP_DB");
    }
    try {
      engine_.store.emplace(store::ReadOnlyStore::open(*engine_.options.db, engine_.options.root));
    } catch (const store::ReadOnlyStoreError& e) {
      return error(open_failure_code(e.failure()), e.what());
    } catch (const std::exception& e) {
      return error("db_error", e.what());
    }
    const store::ReadOnlyStore& s = *engine_.store;
    engine_.db_identity = short_digest(path_key(s.path()) + "\n" + path_key(s.workspace_root()));
  }
  try {
    snap_.emplace(engine_.store->read());
    const std::vector<std::string> profiles = snap_->profiles();
    std::string profile;
    if (engine_.options.profile) {
      profile = *engine_.options.profile;
      if (std::find(profiles.begin(), profiles.end(), profile) == profiles.end()) {
        Json e = error("unknown_profile", "profile '" + profile + "' is not in the store; profiles: " + join(profiles, ", "));
        e["result"] = Json{{"profiles", profiles}};
        return e;
      }
    } else if (profiles.size() == 1) {
      profile = profiles.front();
    } else {
      Json e = error("profile_required", profiles.empty() ? std::string("the store has no profile")
                                                          : "the store has several profiles; pass --profile <key>: " +
                                                                join(profiles, ", "));
      e["result"] = Json{{"profiles", profiles}};
      return e;
    }
    info_ = *snap_->profile_info(profile);
    status_ = snap_->status(profile);
    view_.emplace(snap_->public_view(profile));
    for (auto& u : snap_->unit_summaries(profile)) units_.emplace(u.row, std::move(u));
  } catch (const std::exception& e) {
    return error("db_error", e.what());
  }
  return std::nullopt;
}

void Call::end_snapshot() {
  view_.reset();
  snap_.reset();
}

Json Call::envelope() const {
  Json j;
  j["tool"] = tool_;
  j["status"] = "success";
  if (status_) {
    j["project_kind"] = info_.uht_applicable ? "unreal" : "cpp";
    j["profile"] = info_.key;
    j["fact_generation"] = info_.fact_generation;
    j["ranking_generation"] = info_.ranking_generation;
    Json layers = Json::array();
    for (const auto& l : status_->layers) layers.push_back(layer_json(l));
    j["layers"] = std::move(layers);
    j["failed_slots"] = status_->failed_slots;
  } else {
    j["project_kind"] = nullptr;
    j["profile"] = nullptr;
    j["fact_generation"] = nullptr;
    j["ranking_generation"] = nullptr;
    j["layers"] = Json::array();
    j["failed_slots"] = Json::array();
  }
  Json diags = Json::array();
  for (const auto& [code, message] : diagnostics_) diags.push_back(Json{{"code", code}, {"message", message}});
  j["diagnostics"] = std::move(diags);
  return j;
}

Json Call::error(const std::string& code, const std::string& message) const {
  Json j = envelope();
  j["status"] = "error";
  j["diagnostics"].push_back(Json{{"code", code}, {"message", message}});
  j["pagination"] = Json{{"complete", false},  {"total", nullptr},      {"shown", 0},
                         {"returned_count", 0}, {"discovered", 0},      {"omitted", nullptr},
                         {"next_cursor", nullptr}, {"limit_reason", nullptr}};
  j["result"] = nullptr;
  return j;
}

void Call::diagnostic(const std::string& code, const std::string& message) { diagnostics_.emplace_back(code, message); }

std::string Call::make_cursor(const nlohmann::json& binding_args, const nlohmann::json& position) const {
  CursorBinding b{tool_,
                  engine_.db_identity,
                  info_.key,
                  info_.fact_generation,
                  info_.ranking_generation,
                  short_digest(binding_args.dump()),
                  view_->digest().hex()};
  return encode_cursor(b, position);
}

std::optional<Json> Call::accept(const std::string& cursor, const nlohmann::json& binding_args,
                                 nlohmann::json& position) const {
  const auto decoded = decode_cursor(cursor);
  if (!decoded || decoded->binding.tool != tool_) {
    return error("invalid_cursor", "the cursor is malformed, altered or belongs to another tool; re-run the query without a cursor");
  }
  const CursorBinding expected{tool_,
                               engine_.db_identity,
                               info_.key,
                               info_.fact_generation,
                               info_.ranking_generation,
                               short_digest(binding_args.dump()),
                               view_->digest().hex()};
  if (!(decoded->binding == expected)) {
    std::string why;
    if (decoded->binding.db != expected.db || decoded->binding.profile != expected.profile) {
      why = "it belongs to another database or profile";
    } else if (decoded->binding.fact_generation != expected.fact_generation ||
               decoded->binding.ranking_generation != expected.ranking_generation) {
      why = "the index changed (fact or ranking generation)";
    } else if (decoded->binding.withheld != expected.withheld) {
      why = "the set of excluded or missing files changed";
    } else {
      why = "it was issued for other arguments";
    }
    return error("cursor_expired", "the cursor no longer applies because " + why +
                                       "; re-run the same query without a cursor to start again");
  }
  position = decoded->position;
  return std::nullopt;
}

void Call::note_unit(const std::string& fact_id, std::int64_t unit_row) {
  if (unit_stale(unit_row)) stale_ids_.insert(fact_id);
}

bool Call::unit_stale(std::int64_t unit_row) const {
  const auto it = units_.find(unit_row);
  return it != units_.end() && it->second.semantic == Freshness::stale;
}

const std::string& Call::unit_label(std::int64_t unit_row) const { return units_.at(unit_row).fact_label; }

Json Call::finish(Json result, const Page& page) {
  const std::uint64_t returned = page.returned_before + page.shown;
  Json pagination;
  pagination["complete"] = page.complete;
  pagination["total"] = nullptr;
  pagination["shown"] = page.shown;
  pagination["returned_count"] = returned;
  pagination["discovered"] = std::max(page.discovered, returned);
  pagination["omitted"] = nullptr;
  pagination["next_cursor"] = nullptr;
  pagination["limit_reason"] = nullptr;
  if (page.complete) {
    const std::uint64_t total = std::max(page.discovered, returned);
    pagination["total"] = total;
    pagination["omitted"] = total - returned;
    if (total > returned) {
      pagination["next_cursor"] = page.cursor(returned);
      pagination["limit_reason"] = "page_limit";
    }
  } else {
    // A result without continuation (check_freshness) keeps a null cursor.
    if (page.cursor) pagination["next_cursor"] = page.cursor(returned);
    pagination["limit_reason"] = page.budget_reason ? Json(*page.budget_reason) : Json("time_budget");
  }
  const bool partial = !page.complete || (status_ && !status_->failed_slots.empty());
  if (!stale_ids_.empty()) {
    diagnostic("stale_result", "facts owned by units whose persisted semantic state is stale: " +
                                   join(std::vector<std::string>(stale_ids_.begin(), stale_ids_.end()), ", "));
  }
  if (status_ && !status_->failed_slots.empty()) {
    diagnostic("failed_slots", std::to_string(status_->failed_slots.size()) +
                                   " slot(s) of the profile failed in their latest attempt; results may be incomplete");
  }
  Json j = envelope();
  j["status"] = partial ? "partial" : (!stale_ids_.empty() ? "stale" : "success");
  j["pagination"] = std::move(pagination);
  j["result"] = std::move(result);
  return j;
}

// ---------------------------------------------------------------------------
// Fact JSON.

Json location_json(const SourceLocation& l) {
  Json j;
  j["path"] = l.file.generic;
  j["span"] = Json{{"begin_line", l.span.begin_line},     {"begin_column", l.span.begin_column},
                   {"end_line", l.span.end_line},         {"end_column", l.span.end_column},
                   {"begin_offset", l.span.begin_offset}, {"end_offset", l.span.end_offset}};
  j["span_hash"] = l.span_hash ? Json(l.span_hash->hex()) : Json(nullptr);
  j["content_hash"] = l.content_hash.digest.hex();
  j["size"] = l.content_hash.size;
  return j;
}

std::string qualified_name(const CanonicalKey& key) {
  std::string out;
  for (const auto& owner : key.owner_chain) {
    out += owner.name.empty() ? "(anonymous)" : owner.name;
    out += "::";
  }
  out += key.canonical_name.empty() ? "(anonymous)" : key.canonical_name;
  return out;
}

std::optional<store::LocationRow> primary_location(Call& call, std::int64_t symbol_row) {
  std::optional<store::LocationRow> best;
  const auto rank = [](LocationRole r) { return r == LocationRole::definition ? 0 : r == LocationRole::declaration ? 1 : 2; };
  const auto order = [&](const store::LocationRow& l) {
    return std::make_tuple(rank(l.location.role), l.location.location.file.generic, l.location.location.span.begin_offset,
                           l.location.location.span.end_offset, l.location.analysis_unit);
  };
  for (auto& l : call.snap().symbol_locations(symbol_row)) {
    if (call.view().file_withheld(l.file_row)) continue;
    if (!best || order(l) < order(*best)) best = std::move(l);
  }
  return best;
}

Json symbol_json(Call& call, const store::SymbolRow& symbol, SymbolPresence presence) {
  Json j;
  j["id"] = symbol.id.value;
  j["name"] = symbol.key.canonical_name;
  j["qualified_name"] = qualified_name(symbol.key);
  j["kind"] = std::string(to_string(symbol.key.kind));
  j["signature"] = symbol.key.normalized_signature;
  j["presence"] = std::string(to_string(presence));
  j["primary_location"] = nullptr;
  if (presence != SymbolPresence::external_placeholder) {
    if (const auto l = primary_location(call, symbol.row)) {
      Json loc = location_json(l->location.location);
      loc["role"] = std::string(to_string(l->location.role));
      j["primary_location"] = std::move(loc);
      call.note_unit(symbol.id.value, l->unit_row);
    }
  }
  return j;
}

// ---------------------------------------------------------------------------
// Name resolution shared by find_code tiers 1/2 and trace_calls selectors.

namespace {

// A query is qualified when it contains "::"; a leading "::" asks for the
// exact full qualified name, otherwise it is a "::"-aligned suffix.
struct QualifiedQuery {
  bool anchored = false;
  std::string text;  // without the leading "::"
  std::string last;  // last segment
};

std::optional<QualifiedQuery> parse_qualified(const std::string& query, std::string* invalid_reason) {
  QualifiedQuery q;
  q.text = query;
  if (q.text.rfind("::", 0) == 0) {
    q.anchored = true;
    q.text = q.text.substr(2);
  }
  const auto pos = q.text.rfind("::");
  q.last = pos == std::string::npos ? q.text : q.text.substr(pos + 2);
  if (q.last.empty() || q.text.find(":::") != std::string::npos || q.text.rfind("::", 0) == 0) {
    if (invalid_reason) *invalid_reason = "malformed qualified name '" + query + "'";
    return std::nullopt;
  }
  return q;
}

bool qualified_matches(const QualifiedQuery& q, const std::string& qualified) {
  if (qualified == q.text) return true;
  if (q.anchored || qualified.size() <= q.text.size() + 2) return false;
  return qualified.compare(qualified.size() - q.text.size(), q.text.size(), q.text) == 0 &&
         qualified.compare(qualified.size() - q.text.size() - 2, 2, "::") == 0;
}

}  // namespace

bool tie_less(const NameMatch& a, const NameMatch& b) {
  return std::make_tuple(std::string_view(a.qualified), to_string(a.symbol.key.kind),
                         std::string_view(a.symbol.key.normalized_signature), std::string_view(a.symbol.id.value)) <
         std::make_tuple(std::string_view(b.qualified), to_string(b.symbol.key.kind),
                         std::string_view(b.symbol.key.normalized_signature), std::string_view(b.symbol.id.value));
}

std::optional<std::vector<NameMatch>> name_matches(Call& call, const std::string& name, std::string* invalid_reason) {
  std::vector<NameMatch> out;
  const bool qualified = name.find("::") != std::string::npos;
  std::optional<QualifiedQuery> q;
  if (qualified) {
    q = parse_qualified(name, invalid_reason);
    if (!q) return std::nullopt;
  }
  for (auto& row : call.snap().symbols_named(call.profile().key, qualified ? q->last : name)) {
    const auto presence = call.view().presence(row.row);
    if (!presence || *presence == SymbolPresence::implicit_hidden) continue;
    std::string qn = qualified_name(row.key);
    if (qualified && !qualified_matches(*q, qn)) continue;
    out.push_back(NameMatch{std::move(row), *presence, std::move(qn)});
  }
  std::sort(out.begin(), out.end(), tie_less);
  return out;
}

ResolvedSelector resolve_selector(Call& call, const std::string& raw, std::size_t limit,
                                  const std::optional<std::string>& cursor) {
  ResolvedSelector out;
  const std::string selector = trim(raw);
  if (selector.empty()) {
    out.error = call.error("empty_selector", "the selector is empty; pass a stable ID (cm1:...), a qualified name or a name");
    return out;
  }
  if (selector.rfind("cm1:", 0) == 0) {
    if (!is_well_formed_stable_id(selector)) {
      out.error = call.error("malformed_stable_id", "'" + selector + "' is not a well-formed stable ID (cm1: + 32 hex digits)");
      return out;
    }
    auto row = call.snap().symbol_by_stable_id(call.profile().key, selector);
    if (!row || !call.view().presence(row->row)) {
      out.error = call.error("unknown_selector", "no public symbol has the stable ID " + selector);
      return out;
    }
    out.symbol = std::move(row);
    return out;
  }
  std::string invalid;
  auto matches = name_matches(call, selector, &invalid);
  if (!matches) {
    out.error = call.error("invalid_selector", invalid);
    return out;
  }
  if (matches->empty()) {
    out.error = call.error("unknown_selector", "no public symbol is named '" + selector + "'");
    return out;
  }
  if (matches->size() == 1) {  // a cursor here continues the trace; trace_calls checks it
    out.symbol = std::move(matches->front().symbol);
    return out;
  }
  // Ambiguous: candidates paginated
  // like any result, error status on every page, cursor bound to the selector.
  const nlohmann::json binding{{"mode", "ambiguous"}, {"selector", selector}};
  std::uint64_t returned = 0;
  if (cursor) {
    nlohmann::json pos;
    if (auto e = call.accept(*cursor, binding, pos)) {
      out.error = std::move(e);
      return out;
    }
    if (!pos.contains("returned") || !pos["returned"].is_number_unsigned()) {
      out.error = call.error("invalid_cursor", "the cursor position is malformed; re-run the query without a cursor");
      return out;
    }
    returned = pos["returned"].get<std::uint64_t>();
    if (returned > matches->size()) {
      out.error = call.error("invalid_cursor", "the cursor position is outside the result; re-run the query without a cursor");
      return out;
    }
  }
  Json candidates = Json::array();
  const std::size_t end = std::min<std::size_t>(matches->size(), returned + limit);
  for (std::size_t i = returned; i < end; ++i) {
    candidates.push_back(symbol_json(call, (*matches)[i].symbol, (*matches)[i].presence));
  }
  Page page;
  page.complete = true;
  page.discovered = matches->size();
  page.returned_before = returned;
  page.shown = end - returned;
  page.cursor = [&](std::uint64_t r) { return call.make_cursor(binding, nlohmann::json{{"returned", r}}); };
  call.diagnostic("ambiguous_selector", "'" + selector + "' matches " + std::to_string(matches->size()) +
                                            " public symbols; pass one candidate's stable ID as the selector");
  Json response = call.finish(Json{{"selector", selector}, {"candidates", std::move(candidates)}}, page);
  response["status"] = "error";
  out.error = std::move(response);
  return out;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Engine.

using detail::Call;
using detail::Page;

QueryEngine::QueryEngine(EngineOptions options) : impl_(std::make_unique<Impl>()) { impl_->options = std::move(options); }
QueryEngine::~QueryEngine() = default;

namespace {

std::optional<std::string> limit_problem(std::size_t limit) {
  if (limit == 0 || limit > kMaxLimit) {
    return "limit must be between 1 and " + std::to_string(kMaxLimit) + " (got " + std::to_string(limit) + ")";
  }
  return std::nullopt;
}

// Run `body` and turn any unexpected failure into an error response.
template <typename F>
Json guarded(Call& call, F&& body) {
  try {
    return body();
  } catch (const std::exception& e) {
    return call.error("internal_error", e.what());
  }
}

struct Hit {
  int tier = 0;
  bool file = false;
  store::SymbolRow symbol;
  SymbolPresence presence = SymbolPresence::indexed;
  std::string qualified;
  std::string path;
};

const char* tier_name(int tier) {
  switch (tier) {
    case 1:
      return "exact_identifier";
    case 2:
      return "qualified_name";
    case 3:
      return "identifier_component";
    default:
      return "path";
  }
}

}  // namespace

Json QueryEngine::find_code(const FindCodeArgs& args) {
  Call call(*impl_, "find_code");
  if (auto e = call.begin()) return *e;
  return guarded(call, [&]() -> Json {
    const std::string query = trim(args.query);
    if (query.empty()) return call.error("empty_query", "the query is empty");
    if (auto p = limit_problem(args.limit)) return call.error("invalid_limit", *p);
    const nlohmann::json binding{{"query", query}};
    std::uint64_t returned = 0;
    int tiers_done = 0;
    if (args.cursor) {
      nlohmann::json pos;
      if (auto e = call.accept(*args.cursor, binding, pos)) return *e;
      if (!pos.contains("returned") || !pos["returned"].is_number_unsigned() || !pos.contains("tiers") ||
          !pos["tiers"].is_number_unsigned() || pos["tiers"].get<std::uint64_t>() > 4) {
        return call.error("invalid_cursor", "the cursor position is malformed; re-run the query without a cursor");
      }
      returned = pos["returned"].get<std::uint64_t>();
      tiers_done = pos["tiers"].get<int>();
    }

    const bool qualified = query.find("::") != std::string::npos;
    std::string invalid;
    if (qualified && !detail::name_matches(call, query, &invalid)) return call.error("invalid_query", invalid);

    // Tiers are computed whole; the time budget is checked between tiers and
    // never before a tier beyond the ones an earlier page completed.
    const auto start = std::chrono::steady_clock::now();
    std::vector<Hit> hits;
    std::set<std::int64_t> seen;
    int tiers_complete = 0;
    bool budget_stop = false;
    const auto tier_done = [&](int tier) {
      tiers_complete = tier;
      if (tier >= 4 || tier < tiers_done + 1) return;
      if (std::chrono::steady_clock::now() - start >= impl_->options.search_budget) budget_stop = true;
    };
    const auto add_matches = [&](int tier, std::vector<detail::NameMatch> matches) {
      for (auto& m : matches) {
        if (!seen.insert(m.symbol.row).second) continue;
        hits.push_back(Hit{tier, false, std::move(m.symbol), m.presence, std::move(m.qualified), {}});
      }
    };
    // Tier 1: exact identifier (case-sensitive canonical name).
    if (!qualified) add_matches(1, *detail::name_matches(call, query, nullptr));
    tier_done(1);
    // Tier 2: qualified name or "::"-aligned suffix.
    if (!budget_stop) {
      if (qualified) add_matches(2, *detail::name_matches(call, query, nullptr));
      tier_done(2);
    }
    // Tier 3: contiguous identifier components.
    const std::vector<std::string> query_components = identifier_components(query);
    if (!budget_stop) {
      const std::string key = call.profile().key + "\n" + std::to_string(call.profile().fact_generation);
      if (impl_->names_key != key) {
        impl_->names.clear();
        for (auto& n : call.snap().distinct_symbol_names(call.profile().key)) {
          auto c = identifier_components(n);
          impl_->names.emplace_back(std::move(n), std::move(c));
        }
        impl_->names_key = key;
      }
      const std::vector<std::string> last_components =
          qualified ? identifier_components(query.substr(query.rfind("::") + 2)) : query_components;
      std::vector<detail::NameMatch> matches;
      for (const auto& [name, components] : impl_->names) {
        if (!contains_run(components, last_components)) continue;
        for (auto& row : call.snap().symbols_named(call.profile().key, name)) {
          if (seen.count(row.row)) continue;
          const auto presence = call.view().presence(row.row);
          if (!presence || *presence == SymbolPresence::implicit_hidden) continue;
          std::string qn = detail::qualified_name(row.key);
          if (qualified && !contains_run(identifier_components(qn), query_components)) continue;
          matches.push_back(detail::NameMatch{std::move(row), *presence, std::move(qn)});
        }
      }
      std::sort(matches.begin(), matches.end(), detail::tie_less);
      add_matches(3, std::move(matches));
      tier_done(3);
    }
    // Tier 4: file paths, as file records ordered by repository-relative path.
    if (!budget_stop) {
      for (const auto& f : call.snap().files(call.profile().key)) {
        if (call.view().file_withheld(f.row)) continue;
        if (contains_run(identifier_components(f.path), query_components)) {
          Hit h;
          h.tier = 4;
          h.file = true;
          h.path = f.path;
          hits.push_back(std::move(h));
        }
      }
      tier_done(4);
    }

    if (returned > hits.size()) {
      return call.error("invalid_cursor", "the cursor position is outside the result; re-run the query without a cursor");
    }
    Json results = Json::array();
    const std::size_t end = std::min<std::size_t>(hits.size(), returned + args.limit);
    for (std::size_t i = returned; i < end; ++i) {
      const Hit& h = hits[i];
      Json r;
      r["tier"] = h.tier;
      r["match"] = tier_name(h.tier);
      if (h.file) {
        r["kind"] = "file";
        r["file"] = Json{{"path", h.path}};
      } else {
        r["kind"] = "symbol";
        r["symbol"] = detail::symbol_json(call, h.symbol, h.presence);
      }
      results.push_back(std::move(r));
    }
    Page page;
    page.complete = tiers_complete == 4;
    page.discovered = hits.size();
    page.returned_before = returned;
    page.shown = end - returned;
    page.budget_reason = "time_budget";
    page.cursor = [&](std::uint64_t r) {
      return call.make_cursor(binding, nlohmann::json{{"returned", r}, {"tiers", tiers_complete}});
    };
    return call.finish(Json{{"query", query}, {"results", std::move(results)}}, page);
  });
}

Json QueryEngine::trace_calls(const TraceArgs& args) {
  Call call(*impl_, "trace_calls");
  if (auto e = call.begin()) return *e;
  return guarded(call, [&] { return detail::trace_calls(call, args); });
}

Json QueryEngine::file_api(const FileApiArgs& args) {
  Call call(*impl_, "file_api");
  if (auto e = call.begin()) return *e;
  return guarded(call, [&] { return detail::file_api(call, args); });
}

namespace detail {

FreshnessPage freshness_page(store::InspectionOutcome outcome, std::size_t units, std::size_t stale_units) {
  if (outcome == store::InspectionOutcome::complete) return {true, std::nullopt};
  // Unchecked inputs could still make more units stale; the count is exact
  // only when every unit of the profile is already detected stale.
  if (units != 0 && stale_units == units) return {true, std::nullopt};
  return {false, outcome == store::InspectionOutcome::generation_changed ? "generation_changed" : "time_budget"};
}

std::pair<std::string, std::string> freshness_diagnostic(store::InspectionOutcome outcome, const FreshnessPage& page,
                                                         std::size_t unchecked_inputs) {
  if (outcome == store::InspectionOutcome::generation_changed) {
    return {"generation_changed",
            "a publication changed the index during the check, so this result is not current; re-run check (with a "
            "larger --budget / budget_ms if it does not finish)"};
  }
  const std::string ended = "the budget ended before every input was hashed twice; " + std::to_string(unchecked_inputs) +
                            " input(s) unchecked in the last pass; no unit is reported fresh";
  if (page.complete) {
    return {"budget_exhausted", ended + "; every unit is already detected stale, so the stale count is exact"};
  }
  return {"budget_exhausted", ended + " and the stale count is not exact; re-run with a larger --budget (budget_ms)"};
}

}  // namespace detail

Json QueryEngine::check_freshness(const CheckArgs& args) {
  Call call(*impl_, "check_freshness");
  if (auto e = call.begin()) return *e;
  return guarded(call, [&]() -> Json {
    if (args.budget <= std::chrono::milliseconds(0) || args.budget > kMaxTimeBudget) {
      return call.error("invalid_budget", "budget must be between 1 and " + std::to_string(kMaxTimeBudget.count()) + " ms");
    }
    const std::string profile = call.profile().key;
    call.end_snapshot();  // the inspection takes its own snapshots on the same connection
    store::InspectRequest request;
    request.profile = profile;
    request.budget = args.budget;
    request.before_probe = impl_->options.freshness_probe_hook;  // test hook; empty outside tests
    const store::FreshnessReport report = impl_->store->inspect_freshness(request);

    std::size_t fresh = 0;
    std::size_t stale = 0;
    std::size_t unknown = 0;
    Json stale_units = Json::array();
    const auto combine = [&](auto field) -> std::string {
      if (report.units.empty()) return "unknown";
      bool all_fresh = true;
      for (const auto& u : report.units) {
        const std::optional<Freshness> f = field(u);
        if (!f) return "not_applicable";
        if (*f == Freshness::stale) return "stale";
        all_fresh = all_fresh && *f == Freshness::fresh;
      }
      return all_fresh ? "fresh" : "unknown";
    };
    for (const auto& u : report.units) {
      if (u.semantic == Freshness::fresh) ++fresh;
      if (u.semantic == Freshness::stale) ++stale;
      if (u.semantic == Freshness::unknown) ++unknown;
      if (u.semantic == Freshness::stale || u.compile_context == Freshness::stale) {
        stale_units.push_back(Json{{"slot", u.slot},
                                   {"semantic", std::string(to_string(u.semantic))},
                                   {"compile_context", std::string(to_string(u.compile_context))},
                                   {"uht", u.uht ? Json(std::string(to_string(*u.uht))) : Json(nullptr)},
                                   {"reasons", u.reasons}});
      }
    }
    Json changes = Json::array();
    for (const auto& c : report.changes) {
      changes.push_back(Json{{"path", c.repo_path ? c.repo_path->generic : c.path_utf8},
                             {"in_workspace", c.repo_path.has_value()},
                             {"role", std::string(to_string(c.role))},
                             {"change", std::string(to_string(c.kind))},
                             {"units", c.slots.size()}});
    }
    Json result;
    result["outcome"] = std::string(to_string(report.outcome));
    result["persisted"] = false;
    result["budget_ms"] = args.budget.count();
    result["elapsed_ms"] = report.elapsed.count();
    result["inputs_total"] = report.inputs_total;
    result["inputs_checked"] = report.inputs_checked;
    result["inputs_unchecked"] = report.inputs_total - std::min(report.inputs_total, report.inputs_checked);
    result["fact_generation_checked"] = report.fact_generation;
    // Live overlay of this inspection; the persisted state is in `layers`.
    result["live"] = Json{
        {"semantic_graph", combine([](const store::UnitFreshness& u) { return std::optional<Freshness>(u.semantic); })},
        {"compile_context",
         combine([](const store::UnitFreshness& u) { return std::optional<Freshness>(u.compile_context); })},
        {"uht_context", call.profile().uht_applicable
                            ? Json(combine([](const store::UnitFreshness& u) { return u.uht; }))
                            : Json("not_applicable")},
        {"build_currency", "caller evidence; unverified units are reported unknown, never fresh"}};
    result["units"] = Json{{"total", report.units.size()}, {"fresh", fresh}, {"stale", stale}, {"unknown", unknown}};
    result["changes"] = std::move(changes);
    result["stale_units"] = stale_units;
    result["guidance"] = report.guidance;

    const detail::FreshnessPage counted = detail::freshness_page(report.outcome, report.units.size(), stale);
    Page page;
    page.complete = counted.complete;
    page.budget_reason = counted.limit_reason;
    page.discovered = stale_units.size();
    page.shown = stale_units.size();  // no cursor: a check is re-run, not continued
    Json response = call.finish(std::move(result), page);
    if (report.outcome == store::InspectionOutcome::complete) {
      response["status"] = stale != 0 ? "stale" : "success";
    } else {
      response["status"] = "partial";
      const auto [code, message] = detail::freshness_diagnostic(
          report.outcome, counted, report.inputs_total - std::min(report.inputs_total, report.inputs_checked));
      response["diagnostics"].push_back(Json{{"code", code}, {"message", message}});
    }
    return response;
  });
}

}  // namespace lcm::query
