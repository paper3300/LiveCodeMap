#include <doctest/doctest.h>

#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "lcm/query/cli.hpp"
#include "lcm/query/identifier.hpp"
#include "lcm/query/query.hpp"
#include "lcm/query/query_internal.hpp"
#include "lcm/query/render.hpp"
#include "query_fixture.hpp"

using namespace lcm;
using namespace lcm::query;
using namespace lcm::query::test;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

fs::path scratch(const std::string& name) {
  static std::atomic<int> counter{0};
  const fs::path p = path_from_utf8(LCM_TEST_SCRATCH_DIR) /
                     path_from_utf8(name + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                                    "-" + std::to_string(counter++));
  fs::create_directories(p);
  return p;
}

std::string file_digest(const fs::path& p) {
  std::error_code ec;
  if (!fs::exists(p, ec)) return "absent";
  std::ifstream in(p, std::ios::binary);
  const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return sha256(bytes).hex();
}

fs::path sidecar(const fs::path& db, const char* suffix) {
  fs::path p = db;
  p += suffix;
  return p;
}

std::set<std::string> directory_listing(const fs::path& dir) {
  std::set<std::string> out;
  for (const auto& e : fs::directory_iterator(dir)) out.insert(path_to_utf8_generic(e.path().filename()));
  return out;
}

EngineOptions options_for(const QueryFx& fx) {
  EngineOptions o;
  o.db = fx.db;
  return o;
}

Json find(QueryEngine& e, const std::string& q, std::size_t limit = 1000, std::optional<std::string> cursor = {}) {
  FindCodeArgs a;
  a.query = q;
  a.limit = limit;
  a.cursor = std::move(cursor);
  return e.find_code(a);
}

Json trace(QueryEngine& e, const std::string& selector, Direction d = Direction::incoming, int depth = 1,
           std::size_t limit = 1000, std::optional<std::string> cursor = {}) {
  TraceArgs a;
  a.selector = selector;
  a.direction = d;
  a.depth = depth;
  a.limit = limit;
  a.cursor = std::move(cursor);
  return e.trace_calls(a);
}

Json api(QueryEngine& e, const std::string& path, std::size_t limit = 1000, std::optional<std::string> cursor = {}) {
  FileApiArgs a;
  a.path = path;
  a.limit = limit;
  a.cursor = std::move(cursor);
  return e.file_api(a);
}

bool has_diagnostic(const Json& r, const std::string& code) {
  for (const auto& d : r["diagnostics"]) {
    if (d["code"] == code) return true;
  }
  return false;
}

// FR-API-003 fields and the R3 pagination invariants of any response.
void check_response(const Json& r, const std::string& tool) {
  for (const char* k : {"tool", "status", "project_kind", "profile", "fact_generation", "ranking_generation", "layers",
                        "failed_slots", "diagnostics", "pagination", "result"}) {
    REQUIRE_MESSAGE(r.contains(k), k);
  }
  CHECK(r["tool"] == tool);
  const std::set<std::string> statuses{"success", "partial", "stale", "error"};
  CHECK(statuses.count(r["status"].get<std::string>()) == 1);
  if (!r["profile"].is_null()) {
    CHECK((r["project_kind"] == "cpp" || r["project_kind"] == "unreal"));
    REQUIRE(r["layers"].size() == 6);
    for (const auto& l : r["layers"]) {
      for (const char* k : {"layer", "applicable", "availability", "freshness", "units", "last_success", "last_attempt", "reason"}) {
        CHECK_MESSAGE(l.contains(k), k);
      }
      CHECK(l["freshness"] != "fresh");  // nothing in these fixtures is verified fresh
    }
  }
  for (const auto& d : r["diagnostics"]) {
    CHECK(d["code"].is_string());
    CHECK(d["message"].is_string());
  }
  const Json& p = r["pagination"];
  for (const char* k : {"complete", "total", "shown", "returned_count", "discovered", "omitted", "next_cursor", "limit_reason"}) {
    REQUIRE_MESSAGE(p.contains(k), k);
  }
  const auto shown = p["shown"].get<std::uint64_t>();
  const auto returned = p["returned_count"].get<std::uint64_t>();
  const auto discovered = p["discovered"].get<std::uint64_t>();
  CHECK(shown <= returned);
  CHECK(returned <= discovered);
  const Json& reason = p["limit_reason"];
  CHECK((reason.is_null() || reason == "page_limit" || reason == "time_budget" || reason == "node_budget" ||
         reason == "generation_changed"));
  if (!p["complete"].get<bool>()) {
    CHECK(p["total"].is_null());
    CHECK(p["omitted"].is_null());
    if (r["status"] != "error") {
      CHECK(reason.is_string());
      // A freshness check has no continuation (documented exception to R3): re-run with a larger budget.
      CHECK(p["next_cursor"].is_string() == (tool != "check_freshness"));
    }
  } else {
    CHECK(p["total"].get<std::uint64_t>() == discovered);
    CHECK(p["omitted"].get<std::uint64_t>() == discovered - returned);
    CHECK(p["next_cursor"].is_string() == (p["omitted"].get<std::uint64_t>() > 0));
  }
}

std::vector<std::string> symbol_ids(const Json& find_response) {
  std::vector<std::string> out;
  for (const auto& r : find_response["result"]["results"]) {
    if (r["kind"] == "symbol") out.push_back(r["symbol"]["id"].get<std::string>());
  }
  return out;
}

std::vector<std::string> record_ids(const Json& trace_response) {
  std::vector<std::string> out;
  for (const auto& r : trace_response["result"]["records"]) out.push_back(r["symbol"]["id"].get<std::string>());
  return out;
}

const Json* record_for(const Json& trace_response, const StableId& other) {
  for (const auto& r : trace_response["result"]["records"]) {
    if (r["symbol"]["id"] == other.value) return &r;
  }
  return nullptr;
}

// Follows next_cursor until the chain ends; returns every page.
template <typename Call>
std::vector<Json> chain(Call&& call) {
  std::vector<Json> pages;
  std::optional<std::string> cursor;
  for (int guard = 0; guard < 1000; ++guard) {
    Json page = call(cursor);
    pages.push_back(page);
    if (page["pagination"]["next_cursor"].is_null()) break;
    cursor = page["pagination"]["next_cursor"].get<std::string>();
  }
  return pages;
}

std::string location_key(const SourceLocation& l) {
  return l.file.generic + "@" + std::to_string(l.span.begin_offset) + "-" + std::to_string(l.span.end_offset) + "#" +
         (l.span_hash ? l.span_hash->hex() : "-") + "#" + l.content_hash.digest.hex();
}

std::string location_key(const Json& l) {
  return l["path"].get<std::string>() + "@" + std::to_string(l["span"]["begin_offset"].get<std::uint64_t>()) + "-" +
         std::to_string(l["span"]["end_offset"].get<std::uint64_t>()) + "#" +
         (l["span_hash"].is_null() ? std::string("-") : l["span_hash"].get<std::string>()) + "#" +
         l["content_hash"].get<std::string>();
}

std::int64_t raw_count(const fs::path& db, const std::string& sql) {
  sqlite3* h = nullptr;
  REQUIRE(sqlite3_open_v2(path_to_utf8_generic(db).c_str(), &h, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
  sqlite3_stmt* st = nullptr;
  REQUIRE(sqlite3_prepare_v2(h, sql.c_str(), -1, &st, nullptr) == SQLITE_OK);
  std::int64_t n = -1;
  if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int64(st, 0);
  sqlite3_finalize(st);
  sqlite3_close(h);
  return n;
}

void raw_create(const fs::path& db, const std::string& sql) {
  sqlite3* h = nullptr;
  REQUIRE(sqlite3_open_v2(path_to_utf8_generic(db).c_str(), &h, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) ==
          SQLITE_OK);
  char* err = nullptr;
  const int rc = sqlite3_exec(h, sql.c_str(), nullptr, nullptr, &err);
  INFO((err ? err : ""));
  sqlite3_free(err);
  sqlite3_close(h);
  REQUIRE(rc == SQLITE_OK);
}

}  // namespace

TEST_CASE("identifier components follow the documented splitter") {
  CHECK(identifier_components("TryConnect") == std::vector<std::string>{"try", "connect"});
  CHECK(identifier_components("FXMLLoadTask_OpenSocket") ==
        std::vector<std::string>{"fxml", "load", "task", "open", "socket"});
  CHECK(identifier_components("Vec3d") == std::vector<std::string>{"vec", "3", "d"});
  CHECK(identifier_components("HTTP2Server") == std::vector<std::string>{"http", "2", "server"});
  CHECK(identifier_components("snake_case-kebab::Qual/Path.h") ==
        std::vector<std::string>{"snake", "case", "kebab", "qual", "path", "h"});
  CHECK(identifier_components("조회_함수") == std::vector<std::string>{"조회", "함수"});
  CHECK(identifier_components("  ::  ").empty());
  CHECK(contains_run({"try", "connect"}, {"connect"}));
  CHECK_FALSE(contains_run({"try", "connect"}, {"connect", "try"}));
  CHECK_FALSE(contains_run({"a"}, {}));
  CHECK(trim("  x y \t") == "x y");
}

TEST_CASE("find_code: identifier tiers, tie order and the common response fields") {
  const QueryFx fx = QueryFx::create(scratch("find-tiers"));
  QueryEngine e(options_for(fx));
  const Keys& k = fx.k;

  Json r = find(e, "run_A");
  check_response(r, "find_code");
  CHECK(r["status"] == "success");
  CHECK(r["project_kind"] == "cpp");
  CHECK(r["profile"] == "default");
  CHECK(r["fact_generation"] == 1);
  CHECK(r["ranking_generation"] == 0);
  CHECK(r["layers"][2]["layer"] == "uht_context");
  CHECK(r["layers"][2]["applicable"] == false);
  REQUIRE(r["result"]["results"].size() >= 1);
  const Json& first = r["result"]["results"][0];
  CHECK(first["tier"] == 1);
  CHECK(first["match"] == "exact_identifier");
  CHECK(first["symbol"]["id"] == id(k.run_a).value);
  CHECK(first["symbol"]["qualified_name"] == "run_A");
  CHECK(first["symbol"]["presence"] == "indexed");
  CHECK(first["symbol"]["primary_location"]["path"] == "src/a.cpp");
  CHECK(first["symbol"]["primary_location"]["role"] == "definition");
  CHECK(first["symbol"]["primary_location"]["content_hash"].get<std::string>().size() == 64);
  CHECK(first["symbol"]["primary_location"]["span_hash"].is_string());
  CHECK(first["symbol"]["primary_location"]["span"]["begin_line"].is_number());
  CHECK(r.dump().find("\"text\"") == std::string::npos);  // locations only, no source text

  // Qualified name, "::"-aligned suffix and a leading "::" (exact full name).
  CHECK(symbol_ids(find(e, "geo::Shape::area")).front() == id(k.area).value);
  CHECK(find(e, "geo::Shape::area")["result"]["results"][0]["tier"] == 2);
  CHECK(symbol_ids(find(e, "Shape::area")).front() == id(k.area).value);
  CHECK(symbol_ids(find(e, "::geo::Shape")).front() == id(k.shape).value);
  CHECK(symbol_ids(find(e, "::Shape::area")).empty() == false);  // tier 3 components still match
  for (const auto& hit : find(e, "::Shape::area")["result"]["results"]) CHECK(hit["tier"] != 2);
  for (const auto& hit : find(e, "eo::Shape::area")["result"]["results"]) CHECK(hit["tier"] != 2);

  // Tie order inside a tier: qualified name, kind, signature, stable ID.
  const std::vector<std::string> draws = symbol_ids(find(e, "draw"));
  REQUIRE(draws.size() == 3);
  CHECK(draws == std::vector<std::string>{id(k.circle_draw).value, id(k.draw).value, id(k.square_draw).value});
  CHECK(symbol_ids(find(e, "dup")) == std::vector<std::string>{id(k.dup1).value, id(k.dup2).value});

  // Component tier, then the path tier as file records; every symbol once.
  const Json detail = find(e, "detail");
  REQUIRE(detail["result"]["results"].size() == 2);
  CHECK(detail["result"]["results"][0]["tier"] == 3);
  CHECK(detail["result"]["results"][0]["symbol"]["id"] == id(k.detail).value);
  CHECK(detail["result"]["results"][1]["tier"] == 4);
  CHECK(detail["result"]["results"][1]["kind"] == "file");
  CHECK(detail["result"]["results"][1]["file"]["path"] == "include/detail.inl");
  CHECK(symbol_ids(find(e, "run")) == std::vector<std::string>{id(k.run_a).value, id(k.run_b).value});
  const Json shape = find(e, "Shape");
  std::set<std::string> unique;
  for (const auto& s : symbol_ids(shape)) CHECK(unique.insert(s).second);
  CHECK(unique.count(id(k.shape).value) == 1);
  CHECK(unique.count(id(k.dtor).value) == 0);  // implicit_hidden is not searchable

  // Pure external placeholders are results without a location.
  const Json ext = find(e, "ext::api");
  REQUIRE(symbol_ids(ext).size() == 1);
  CHECK(ext["result"]["results"][0]["symbol"]["presence"] == "external_placeholder");
  CHECK(ext["result"]["results"][0]["symbol"]["primary_location"].is_null());

  // UTF-8 identifiers and paths are preserved.
  const Json korean = find(e, "조회_함수");
  REQUIRE(symbol_ids(korean).size() == 1);
  CHECK(korean["result"]["results"][0]["symbol"]["primary_location"]["path"] == "src/한글.cpp");

  // No hit: success, complete, total 0, no cursor.
  const Json none = find(e, "NoSuchIdentifier");
  check_response(none, "find_code");
  CHECK(none["status"] == "success");
  CHECK(none["pagination"]["total"] == 0);
  CHECK(none["pagination"]["next_cursor"].is_null());

  // Named diagnostics.
  CHECK(has_diagnostic(find(e, "   "), "empty_query"));
  CHECK(find(e, "   ")["status"] == "error");
  CHECK(has_diagnostic(find(e, "run_A", 0), "invalid_limit"));
  CHECK(has_diagnostic(find(e, "run_A", kMaxLimit + 1), "invalid_limit"));
  CHECK(has_diagnostic(find(e, "geo::"), "invalid_query"));
  check_response(find(e, "geo::"), "find_code");
}

TEST_CASE("pagination: cursor chains are duplicate- and gap-free and end with a null cursor") {
  const QueryFx fx = QueryFx::create(scratch("pages"));
  QueryEngine e(options_for(fx));

  const Json all = find(e, "f");
  REQUIRE(all["pagination"]["total"] == 12);  // the twelve fields f0..f11
  for (const std::size_t limit : {std::size_t{5}, std::size_t{6}, std::size_t{12}, std::size_t{1}}) {
    CAPTURE(limit);
    const auto pages = chain([&](const std::optional<std::string>& c) { return find(e, "f", limit, c); });
    std::vector<std::string> ids;
    std::uint64_t returned = 0;
    for (const auto& p : pages) {
      check_response(p, "find_code");
      CHECK(p["status"] == "success");
      CHECK(p["pagination"]["complete"] == true);
      returned += p["pagination"]["shown"].get<std::uint64_t>();
      CHECK(p["pagination"]["returned_count"] == returned);
      for (const auto& id : symbol_ids(p)) ids.push_back(id);
    }
    CHECK(ids == symbol_ids(all));
    CHECK(pages.size() == (12 + limit - 1) / limit);
    CHECK(pages.back()["pagination"]["omitted"] == 0);
    if (pages.size() > 1) CHECK(pages.front()["pagination"]["limit_reason"] == "page_limit");
  }

  // trace_calls and file_api pages follow the same rules.
  const std::string area = id(fx.k.area).value;
  const Json callers = trace(e, area);
  REQUIRE(callers["result"]["records"].size() == 3);
  std::vector<std::string> paged;
  for (const auto& p : chain([&](const std::optional<std::string>& c) { return trace(e, area, Direction::incoming, 1, 1, c); })) {
    check_response(p, "trace_calls");
    for (const auto& id : record_ids(p)) paged.push_back(id);
  }
  CHECK(paged == record_ids(callers));
  const Json header = api(e, "include/shapes.h");
  std::vector<std::string> entries;
  for (const auto& p : chain([&](const std::optional<std::string>& c) { return api(e, "include/shapes.h", 4, c); })) {
    check_response(p, "file_api");
    for (const auto& en : p["result"]["entries"]) entries.push_back(en["symbol"]["id"].get<std::string>());
  }
  std::vector<std::string> expected;
  for (const auto& en : header["result"]["entries"]) expected.push_back(en["symbol"]["id"].get<std::string>());
  CHECK(entries == expected);
}

TEST_CASE("cursors: generation, exclusion and deletion changes expire them; altered cursors are invalid") {
  const QueryFx fx = QueryFx::create(scratch("cursor-expiry"));
  QueryEngine e(options_for(fx));
  const auto cursor_of = [&] {
    const Json page = find(e, "f", 5);
    REQUIRE(page["pagination"]["next_cursor"].is_string());
    return page["pagination"]["next_cursor"].get<std::string>();
  };
  const auto expect_code = [&](const Json& r, const std::string& code) {
    check_response(r, r["tool"].get<std::string>());
    CHECK(r["status"] == "error");
    CHECK(has_diagnostic(r, code));
    CHECK(r["result"].is_null());  // no records
  };
  std::string cursor = cursor_of();
  CHECK(find(e, "f", 5, cursor)["status"] == "success");

  // Malformed, altered or foreign cursors.
  expect_code(find(e, "f", 5, std::string("garbage")), "invalid_cursor");
  std::string altered = cursor;
  altered[10] = altered[10] == 'A' ? 'B' : 'A';
  expect_code(find(e, "f", 5, altered), "invalid_cursor");
  expect_code(api(e, "include/shapes.h", 5, cursor), "invalid_cursor");
  expect_code(find(e, "f", 5, cursor + ".x"), "invalid_cursor");
  // Other arguments: expired with restart guidance.
  expect_code(find(e, "f0", 5, cursor), "cursor_expired");

  // Exclusion-set change.
  fx.exclude({"include/hidden.h"});
  expect_code(find(e, "f", 5, cursor), "cursor_expired");
  cursor = cursor_of();
  // Physical deletion of a repository file.
  fs::remove(fx.at("include/gone.h"));
  expect_code(find(e, "f", 5, cursor), "cursor_expired");
  cursor = cursor_of();
  // Fact generation change.
  fx.publish({unit_c(fx.k)});
  const Json expired = find(e, "f", 5, cursor);
  expect_code(expired, "cursor_expired");
  CHECK(expired["fact_generation"] == 2);
  CHECK(find(e, "f", 5, cursor_of())["status"] == "success");
}

TEST_CASE("selectors: ambiguity returns paginated candidates; invalid and unknown selectors are named") {
  const QueryFx fx = QueryFx::create(scratch("selectors"));
  QueryEngine e(options_for(fx));
  const Keys& k = fx.k;

  const Json dup = trace(e, "dup");
  check_response(dup, "trace_calls");
  CHECK(dup["status"] == "error");
  CHECK(has_diagnostic(dup, "ambiguous_selector"));
  REQUIRE(dup["result"]["candidates"].size() == 2);
  CHECK(dup["result"]["candidates"][0]["id"] == id(k.dup1).value);
  CHECK(dup["result"]["candidates"][0]["qualified_name"] == "one::dup");
  CHECK(dup["result"]["candidates"][0]["kind"] == "function");
  CHECK(dup["result"]["candidates"][0]["signature"] == "()");
  CHECK(dup["result"]["candidates"][0]["primary_location"]["path"] == "src/a.cpp");
  CHECK(dup["pagination"]["total"] == 2);

  std::vector<std::string> paged;
  const auto pages = chain([&](const std::optional<std::string>& c) { return trace(e, "dup", Direction::incoming, 1, 1, c); });
  REQUIRE(pages.size() == 2);
  for (const auto& p : pages) {
    check_response(p, "trace_calls");
    CHECK(p["status"] == "error");
    CHECK(has_diagnostic(p, "ambiguous_selector"));
    for (const auto& c : p["result"]["candidates"]) paged.push_back(c["id"].get<std::string>());
  }
  CHECK(paged == std::vector<std::string>{id(k.dup1).value, id(k.dup2).value});
  CHECK(trace(e, "draw")["result"]["candidates"].size() == 3);

  // Unique names, qualified names and stable IDs select.
  CHECK(trace(e, "geo::Shape::draw")["result"]["selected"]["id"] == id(k.draw).value);
  CHECK(trace(e, "run_A")["result"]["selected"]["id"] == id(k.run_a).value);
  CHECK(trace(e, id(k.dtor).value)["status"] == "success");  // implicit members are graph endpoints

  CHECK(has_diagnostic(trace(e, "  "), "empty_selector"));
  CHECK(has_diagnostic(trace(e, "cm1:zz"), "malformed_stable_id"));
  CHECK(has_diagnostic(trace(e, "cm1:" + std::string(32, '0')), "unknown_selector"));
  CHECK(has_diagnostic(trace(e, "nosuch"), "unknown_selector"));
  CHECK(has_diagnostic(trace(e, "geo::"), "invalid_selector"));
  CHECK(has_diagnostic(trace(e, "run_A", Direction::incoming, 0), "invalid_depth"));
  CHECK(has_diagnostic(trace(e, "run_A", Direction::incoming, kMaxDepth + 1), "invalid_depth"));
  CHECK(has_diagnostic(trace(e, "run_A", Direction::incoming, 1, 0), "invalid_limit"));
  TraceArgs bad;
  bad.selector = "run_A";
  bad.node_budget = 0;
  CHECK(has_diagnostic(e.trace_calls(bad), "invalid_budget"));
  bad.node_budget = 10;
  bad.time_budget = 0ms;
  CHECK(has_diagnostic(e.trace_calls(bad), "invalid_budget"));

  // A withheld symbol is unknown by name and by stable ID (no leak).
  fx.exclude({"include/hidden.h"});
  CHECK(has_diagnostic(trace(e, "hidden_fn"), "unknown_selector"));
  const Json by_id = trace(e, id(k.hidden).value);
  CHECK(has_diagnostic(by_id, "unknown_selector"));
}

namespace {

// Every fact a tool returns is a subset of load_facts(public_) with equal
// fields, and every public fact of the selected scope is returned.
void check_equivalence(const QueryFx& fx, QueryEngine& e) {
  const FactSet pub = fx.writer().read().load_facts("default", Projection::public_);

  // find_code: public, searchable symbols by exact name; nothing else.
  std::set<std::string> names;
  for (const auto& [sid, s] : pub.symbols()) {
    if (!s.key.canonical_name.empty()) names.insert(s.key.canonical_name);
  }
  for (const auto& name : names) {
    CAPTURE(name);
    const Json r = find(e, name);
    check_response(r, "find_code");
    std::set<std::string> tier1;
    for (const auto& hit : r["result"]["results"]) {
      if (hit["kind"] != "symbol") continue;
      const Symbol* s = pub.find_symbol(StableId{hit["symbol"]["id"].get<std::string>()});
      REQUIRE_MESSAGE(s != nullptr, hit["symbol"]["id"].get<std::string>());
      CHECK(hit["symbol"]["presence"] == std::string(to_string(s->presence)));
      CHECK(s->presence != SymbolPresence::implicit_hidden);
      if (hit["tier"] == 1) tier1.insert(hit["symbol"]["id"].get<std::string>());
      const Json& primary = hit["symbol"]["primary_location"];
      if (s->presence == SymbolPresence::external_placeholder) {
        CHECK(primary.is_null());
      } else {
        REQUIRE(primary.is_object());
        bool found = false;
        for (const auto& l : s->locations) found = found || location_key(l.location) == location_key(primary);
        CHECK(found);
      }
    }
    std::set<std::string> expected;
    for (const auto& [sid, s] : pub.symbols()) {
      if (s.key.canonical_name == name && s.presence != SymbolPresence::implicit_hidden) expected.insert(sid.value);
    }
    CHECK(tier1 == expected);
  }

  // trace_calls: own-slot callsites equal the public `calls` relations and evidence.
  for (const auto& [sid, s] : pub.symbols()) {
    CAPTURE(sid.value);
    for (const Direction d : {Direction::incoming, Direction::outgoing}) {
      const Json r = trace(e, sid.value, d);
      check_response(r, "trace_calls");
      REQUIRE(r["status"] == "success");
      std::map<std::string, std::set<std::string>> got;  // other id -> callsite keys into the selected slot
      for (const auto& rec : r["result"]["records"]) {
        for (const char* group : {"confirmed", "possible"}) {
          for (const auto& site : rec[group]) {
            const std::string slot = site["slot"].get<std::string>();
            const bool own = d == Direction::incoming ? slot == sid.value : slot == rec["symbol"]["id"].get<std::string>();
            if (!own) continue;
            got[rec["symbol"]["id"].get<std::string>()].insert(std::string(group) + "|" + location_key(site["location"]));
          }
        }
        CHECK(pub.find_symbol(StableId{rec["symbol"]["id"].get<std::string>()}) != nullptr);
      }
      std::map<std::string, std::set<std::string>> want;
      for (const auto& [rk, rel] : pub.relations()) {
        if (rk.kind != RelationKind::calls) continue;
        if (d == Direction::incoming ? rk.target != sid : rk.source != sid) continue;
        const std::string other = (d == Direction::incoming ? rk.source : rk.target).value;
        if (other == sid.value) continue;
        const char* group = rk.confidence == Confidence::possible ? "possible" : "confirmed";
        for (const auto& ev : rel.evidence) want[other].insert(std::string(group) + "|" + location_key(ev.location));
      }
      CHECK(got == want);
      if (d == Direction::outgoing) {
        std::set<std::string> unresolved;
        for (const auto& u : r["result"]["unresolved"]) unresolved.insert(u["expression"].get<std::string>() + location_key(u["location"]));
        std::set<std::string> expected;
        for (const auto& u : pub.unresolved_sites()) {
          if (u.enclosing == sid) expected.insert(u.expression + location_key(u.location));
        }
        CHECK(unresolved == expected);
      }
    }
  }

  // file_api: entries, locations here and elsewhere, and includes per public file.
  std::set<std::string> files;
  for (const auto& [sid, s] : pub.symbols()) {
    for (const auto& l : s.locations) files.insert(l.location.file.generic);
  }
  for (const auto& inc : pub.direct_includes()) files.insert(inc.includer.generic);
  for (const auto& file : files) {
    if (file.empty()) continue;
    CAPTURE(file);
    const Json r = api(e, file);
    check_response(r, "file_api");
    REQUIRE(r["status"] == "success");
    std::map<std::string, std::pair<std::set<std::string>, std::set<std::string>>> got;
    for (const auto& en : r["result"]["entries"]) {
      auto& g = got[en["symbol"]["id"].get<std::string>()];
      for (const auto& h : en["here"]) g.first.insert(h["role"].get<std::string>() + location_key(h));
      for (const auto& o : en["elsewhere"]) g.second.insert(o["role"].get<std::string>() + location_key(o));
    }
    std::map<std::string, std::pair<std::set<std::string>, std::set<std::string>>> want;
    for (const auto& [sid, s] : pub.symbols()) {
      bool here = false;
      for (const auto& l : s.locations) here = here || (l.location.file.generic == file && l.role != LocationRole::implicit_declaration);
      if (!here) continue;
      auto& w = want[sid.value];
      for (const auto& l : s.locations) {
        if (l.role == LocationRole::implicit_declaration) continue;
        (l.location.file.generic == file ? w.first : w.second)
            .insert(std::string(to_string(l.role)) + location_key(l.location));
      }
    }
    CHECK(got == want);
    // Every rendered include field takes part (F2: distinct observations are never merged).
    std::set<std::string> includes;
    for (const auto& inc : r["result"]["includes"]) {
      includes.insert(inc["directive"].get<std::string>() + "|" + inc["operand_kind"].get<std::string>() + "|" +
                      inc["operand"].get<std::string>() + "|" +
                      (inc["expanded"].is_null() ? std::string("-") : inc["expanded"].get<std::string>()) + "|" +
                      inc["activity"].get<std::string>() + "|" + inc["resolution"].get<std::string>() + "|" +
                      location_key(inc["location"]) + "->" +
                      (inc["target"].is_null() ? std::string("-") : inc["target"].get<std::string>()));
    }
    std::set<std::string> expected;
    for (const auto& inc : pub.direct_includes()) {
      if (inc.includer.generic != file) continue;
      expected.insert(std::string(to_string(inc.directive_kind)) + "|" + std::string(to_string(inc.operand_kind)) + "|" +
                      inc.operand_as_written + "|" + inc.expanded_spelling.value_or("-") + "|" +
                      std::string(to_string(inc.activity)) + "|" + std::string(to_string(inc.resolution)) + "|" +
                      location_key(inc.directive_location) + "->" +
                      (inc.resolved_repo_target ? inc.resolved_repo_target->generic : std::string("-")));
    }
    CHECK(includes == expected);
  }
}

// Nothing withheld appears anywhere in any response.
void check_no_leak(QueryEngine& e, const std::vector<std::string>& needles) {
  std::string all;
  for (const auto& q : {"hidden", "hidden_fn", "scoped_helper", "gone", "gone_fn", "c_only", "include", "f", "run_A", "draw"}) {
    all += find(e, q)["result"]["results"].dump();  // not the echoed query text
  }
  for (const auto& s : {"run_A", "run_B", "geo::Shape::area"}) {
    all += trace(e, s, Direction::incoming, 5).dump();
    all += trace(e, s, Direction::outgoing, 5).dump();
  }
  for (const auto& f : {"src/a.cpp", "src/b.cpp", "include/shapes.h"}) all += api(e, f).dump();
  for (const auto& n : needles) CHECK_MESSAGE(all.find(n) == std::string::npos, n);
}

}  // namespace

TEST_CASE("public projection: query results equal load_facts(public_) for excluded, deleted and endpoint-only facts") {
  const QueryFx fx = QueryFx::create(scratch("projection"));
  QueryEngine e(options_for(fx));
  SUBCASE("baseline") { check_equivalence(fx, e); }
  SUBCASE("excluded header") {
    fx.exclude({"include/hidden.h"});
    check_equivalence(fx, e);
    // The internal symbol scoped to hidden.h and the lambda local to hidden_fn
    // are withheld although their locations are visible.
    check_no_leak(e, {"hidden_fn", "scoped_helper", "include/hidden.h", "hidden_cb", id(fx.k.hidden).value,
                      id(fx.k.scoped).value, id(fx.k.lambda).value});
  }
  SUBCASE("deleted header") {
    fs::remove(fx.at("include/gone.h"));
    check_equivalence(fx, e);
    check_no_leak(e, {"gone_fn", "include/gone.h", id(fx.k.gone).value});
    // shared_fn keeps only its visible definition.
    const Json r = api(e, "src/b.cpp");
    bool shared = false;
    for (const auto& en : r["result"]["entries"]) {
      if (en["symbol"]["id"] != id(fx.k.shared).value) continue;
      shared = true;
      CHECK(en["elsewhere"].empty());
    }
    CHECK(shared);
  }
  SUBCASE("retired owner leaves an endpoint-only symbol") {
    fx.publish({}, {"C"});
    check_equivalence(fx, e);
    check_no_leak(e, {"c_only", id(fx.k.c_only).value});
  }
}

TEST_CASE("trace_calls: virtual dispatch, possible branches, path confidence and cycles (R7)") {
  const QueryFx fx = QueryFx::create(scratch("dispatch"));
  QueryEngine e(options_for(fx));
  const Keys& k = fx.k;

  // Incoming on an override: confirmed direct calls plus possible calls through the base slot.
  const Json circle = trace(e, id(k.circle_draw).value);
  check_response(circle, "trace_calls");
  REQUIRE(circle["result"]["records"].size() == 2);
  const Json* b = record_for(circle, id(k.run_b));
  REQUIRE(b);
  CHECK((*b)["confirmed"].size() == 1);
  CHECK((*b)["confirmed"][0]["dispatch"] == "qualified");
  CHECK((*b)["confirmed"][0]["slot"] == id(k.circle_draw).value);
  CHECK((*b)["path_confidence"] == "confirmed");
  const Json* a = record_for(circle, id(k.run_a));
  REQUIRE(a);
  CHECK((*a)["confirmed"].empty());
  REQUIRE((*a)["possible"].size() == 1);
  CHECK((*a)["possible"][0]["slot"] == id(k.draw).value);
  CHECK((*a)["possible"][0]["dispatch"] == "virtual_slot");
  CHECK((*a)["path_confidence"] == "possible");
  CHECK(circle["result"]["counts"]["unique_callers"] == 2);
  CHECK(circle["result"]["counts"]["confirmed_callsites"] == 1);
  CHECK(circle["result"]["counts"]["possible_callsites"] == 1);
  CHECK(circle["result"]["counts"]["exact"] == true);

  // A confirmed virtual-slot edge stays confirmed to its slot only.
  const Json base = trace(e, id(k.draw).value);
  REQUIRE(base["result"]["records"].size() == 1);
  CHECK(base["result"]["records"][0]["confirmed"][0]["dispatch"] == "virtual_slot");
  CHECK(base["result"]["records"][0]["possible"].empty());

  // Square::draw is reachable through two override paths: one record.
  const Json square = trace(e, id(k.square_draw).value);
  REQUIRE(square["result"]["records"].size() == 1);
  CHECK(square["result"]["records"][0]["symbol"]["id"] == id(k.run_a).value);
  CHECK(square["result"]["records"][0]["possible"].size() == 1);

  // Stored possible relations stay in the possible section.
  const Json area = trace(e, id(k.area).value);
  const Json* possible_b = record_for(area, id(k.run_b));
  REQUIRE(possible_b);
  CHECK((*possible_b)["confirmed"].empty());
  CHECK((*possible_b)["possible"][0]["relation_confidence"] == "possible");

  // Outgoing: override candidates are possible branches carrying the slot;
  // placeholders and implicit members are endpoints; unresolved sites are listed.
  const Json out = trace(e, id(k.run_a).value, Direction::outgoing);
  check_response(out, "trace_calls");
  std::map<std::string, int> seen;
  for (const auto& id : record_ids(out)) ++seen[id];
  for (const auto& [rid, n] : seen) CHECK(n == 1);
  const Json* sq = record_for(out, id(k.square_draw));
  REQUIRE(sq);
  CHECK((*sq)["confirmed"].empty());
  REQUIRE((*sq)["possible"].size() == 1);
  CHECK((*sq)["possible"][0]["slot"] == id(k.draw).value);
  CHECK((*sq)["possible"][0]["caller"] == id(k.run_a).value);
  const Json* cd = record_for(out, id(k.circle_draw));
  REQUIRE(cd);
  CHECK((*cd)["path_confidence"] == "possible");
  REQUIRE(record_for(out, id(k.draw)));
  CHECK((*record_for(out, id(k.draw)))["confirmed"].size() == 1);
  REQUIRE(record_for(out, id(k.ext)));
  CHECK((*record_for(out, id(k.ext)))["symbol"]["presence"] == "external_placeholder");
  REQUIRE(record_for(out, id(k.dtor)));
  CHECK((*record_for(out, id(k.dtor)))["symbol"]["presence"] == "implicit_hidden");
  REQUIRE(out["result"]["unresolved"].size() == 1);
  CHECK(out["result"]["unresolved"][0]["expression"] == "callback()");
  CHECK(out["result"]["counts"]["unique_callees"] == seen.size());

  // Two hops: a caller of a possible caller has a possible path.
  const Json two = trace(e, id(k.circle_draw).value, Direction::incoming, 2);
  const Json* korean = record_for(two, id(k.korean));
  REQUIRE(korean);
  CHECK((*korean)["hop"] == 2);
  CHECK((*korean)["path_confidence"] == "possible");
  CHECK((*korean)["confirmed"].size() == 1);

  // Cycles (run_A <-> run_B) terminate with --all.
  TraceArgs all;
  all.selector = id(k.run_a).value;
  all.all = true;
  const Json cycle = e.trace_calls(all);
  check_response(cycle, "trace_calls");
  CHECK(cycle["pagination"]["complete"] == true);
  CHECK(cycle["result"]["depth"] == "all");
  std::set<std::string> ids;
  for (const auto& id : record_ids(cycle)) CHECK(ids.insert(id).second);
  CHECK(ids.count(id(k.run_a).value) == 0);
  CHECK(ids.count(id(k.run_b).value) == 1);
  CHECK(ids.count(id(k.korean).value) == 1);
}

TEST_CASE("trace_calls: a visited-node budget returns partial with a cursor that resumes without gaps") {
  const QueryFx fx = QueryFx::create(scratch("budget"));
  QueryEngine e(options_for(fx));
  TraceArgs full;
  full.selector = id(fx.k.area).value;
  full.all = true;
  const Json complete = e.trace_calls(full);
  REQUIRE(complete["pagination"]["complete"] == true);
  const std::vector<std::string> expected = record_ids(complete);
  REQUIRE(expected.size() >= 4);

  std::vector<std::string> got;
  std::optional<std::string> cursor;
  int partial_pages = 0;
  for (int guard = 0; guard < 50; ++guard) {
    TraceArgs a = full;
    a.node_budget = 1;
    a.cursor = cursor;
    const Json page = e.trace_calls(a);
    check_response(page, "trace_calls");
    for (const auto& id : record_ids(page)) got.push_back(id);
    if (page["pagination"]["complete"] == false) {
      ++partial_pages;
      CHECK(page["status"] == "partial");
      CHECK(page["pagination"]["limit_reason"] == "node_budget");
      CHECK(page["result"]["counts"]["exact"] == false);
    }
    if (page["pagination"]["next_cursor"].is_null()) break;
    cursor = page["pagination"]["next_cursor"].get<std::string>();
  }
  CHECK(partial_pages >= static_cast<int>(expected.size()) - 1);
  CHECK(got == expected);
}

TEST_CASE("file_api: entries in span order, owner links, includes and path normalization (R10)") {
  const QueryFx fx = QueryFx::create(scratch("file-api"));
  QueryEngine e(options_for(fx));
  const Keys& k = fx.k;

  const Json r = api(e, "include/shapes.h");
  check_response(r, "file_api");
  CHECK(r["result"]["file"]["path"] == "include/shapes.h");
  CHECK(r["result"]["module"].is_null());
  CHECK(r["result"]["module_reason"].is_string());
  std::vector<std::string> order;
  for (const auto& en : r["result"]["entries"]) order.push_back(en["symbol"]["id"].get<std::string>());
  std::vector<std::string> expected{id(k.shape).value,  id(k.area).value,        id(k.draw).value,
                                    id(k.circle).value, id(k.circle_draw).value, id(k.square).value,
                                    id(k.square_draw).value, id(k.twice).value};
  for (const auto& f : k.fields) expected.push_back(id(f).value);
  CHECK(order == expected);  // ~Shape (implicit) is not file API
  const Json& area = r["result"]["entries"][1];
  REQUIRE(area["here"].size() == 1);
  CHECK(area["here"][0]["role"] == "declaration");
  CHECK(area["here"][0]["units"] == 2);  // per-unit copies merged
  REQUIRE(area["elsewhere"].size() == 1);
  CHECK(area["elsewhere"][0]["path"] == "src/a.cpp");
  CHECK(area["elsewhere"][0]["role"] == "definition");
  REQUIRE(r["result"]["includes"].size() == 1);
  CHECK(r["result"]["includes"][0]["target"] == "include/detail.inl");
  CHECK(r["result"]["includes"][0]["units"] == 2);

  const Json a = api(e, "src/a.cpp");
  REQUIRE(a["result"]["includes"].size() == 3);
  CHECK(a["result"]["includes"][0]["operand"] == "\"shapes.h\"");
  CHECK(a["result"]["includes"][1]["operand"] == "\"hidden.h\"");
  CHECK(a["result"]["includes"][2]["operand"] == "<vector>");
  CHECK(a["result"]["includes"][2]["target"].is_null());
  CHECK(a["result"]["includes"][2]["resolution"] == "resolved_external");

  // Absolute and repository-relative aliases name the same file.
  CHECK(api(e, path_to_utf8_generic(fx.at("include/shapes.h")))["result"] == r["result"]);
  CHECK(api(e, "include\\shapes.h")["result"] == r["result"]);
#ifdef _WIN32
  CHECK(api(e, "INCLUDE/Shapes.H")["result"] == r["result"]);  // case alias on a case-insensitive volume
#endif
  CHECK(api(e, "src/한글.cpp")["status"] == "success");
  const Json inl = api(e, "include/detail.inl");
  CHECK(inl["result"]["file"]["inl_context"].is_null());
  CHECK(inl["result"]["file"]["inl_context_reason"].is_string());

  CHECK(has_diagnostic(api(e, "../outside.h"), "invalid_path"));
  CHECK(has_diagnostic(api(e, "include/../src/a.cpp"), "invalid_path"));
  CHECK(has_diagnostic(api(e, "include"), "not_a_file"));
  CHECK(has_diagnostic(api(e, path_to_utf8_generic(fx.dir / "elsewhere.h")), "outside_workspace"));
  CHECK(has_diagnostic(api(e, "include/none.h"), "file_not_indexed"));
  CHECK(has_diagnostic(api(e, ""), "empty_path"));
  CHECK(has_diagnostic(api(e, "include/shapes.h", 0), "invalid_limit"));

  // Withheld files are indistinguishable from unknown ones, and their includes disappear.
  fx.exclude({"include/hidden.h"});
  const Json hidden = api(e, "include/hidden.h");
  CHECK(has_diagnostic(hidden, "file_not_indexed"));
  std::string hidden_message = hidden["diagnostics"][0]["message"].get<std::string>();
  hidden_message.replace(hidden_message.find("include/hidden.h"), std::string("include/hidden.h").size(), "include/none.h");
  CHECK(hidden_message == api(e, "include/none.h")["diagnostics"][0]["message"].get<std::string>());
  CHECK(api(e, "src/a.cpp")["result"]["includes"].size() == 2);
}

TEST_CASE("status: persisted stale units, failed slots, project kind and profile selection (R1)") {
  SUBCASE("stale") {
    QueryFx fx = QueryFx::create(scratch("status-stale"));
    // Republish A with its source as a recorded input, then let the writer
    // path persist a stale state for A by changing that file.
    Contribution a = unit_a(fx.k);
    InputObservation o;
    o.path_utf8 = path_to_utf8_generic(fx.at("src/a.cpp"));
    o.repo_path = repo("src/a.cpp");
    o.role = InputRole::source;
    std::ifstream in(fx.at("src/a.cpp"), std::ios::binary);
    o.content_hash = hash_file_content(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()));
    in.close();
    a.inputs.push_back(o);
    fx.publish({a});
    fx.write("src/a.cpp", "// changed\n");
    {
      Store w = fx.writer();
      InspectRequest req;
      req.profile = "default";
      REQUIRE(w.inspect_freshness(req).outcome == InspectionOutcome::complete);
    }
    QueryEngine e(options_for(fx));
    const Json r = find(e, "run_A");
    check_response(r, "find_code");
    CHECK(r["status"] == "stale");
    CHECK(has_diagnostic(r, "stale_result"));
    CHECK(r["layers"][3]["freshness"] == "stale");
    const Json shapes = find(e, "geo::Circle");  // primary location owned by A as well (shared header)
    CHECK(shapes["status"] != "error");
    CHECK(find(e, "two::dup")["status"] == "success");  // owned by B only
    // Partial wins over stale and keeps the stale_result diagnostic.
    TraceArgs t;
    t.selector = id(fx.k.area).value;
    t.all = true;
    t.node_budget = 1;
    const Json partial = e.trace_calls(t);
    CHECK(partial["status"] == "partial");
  }
  SUBCASE("failed slots") {
    QueryFx fx = QueryFx::create(scratch("status-failed"));
    {
      Store w = fx.writer();
      auto lock = w.acquire_writer(5s);
      w.record_rejected(*lock, "default", {"Z"}, "analysis rejected");
    }
    QueryEngine e(options_for(fx));
    const Json r = find(e, "run_A");
    check_response(r, "find_code");
    CHECK(r["status"] == "partial");
    CHECK(r["failed_slots"] == Json::array({"Z"}));
    CHECK(has_diagnostic(r, "failed_slots"));
  }
  SUBCASE("unreal project kind") {
    QueryFx fx = QueryFx::create(scratch("status-unreal"), true);
    QueryEngine e(options_for(fx));
    const Json r = find(e, "run_A");
    CHECK(r["project_kind"] == "unreal");
    CHECK(r["layers"][2]["applicable"] == true);
    CHECK(r["layers"][2]["freshness"] == "unknown");
  }
  SUBCASE("profile selection") {
    QueryFx fx = QueryFx::create(scratch("status-profiles"));
    {
      Store w = fx.writer();
      auto lock = w.acquire_writer(5s);
      w.ensure_profile(*lock, "other");
    }
    QueryEngine several(options_for(fx));
    const Json r = find(several, "run_A");
    check_response(r, "find_code");
    CHECK(has_diagnostic(r, "profile_required"));
    CHECK(r["result"]["profiles"] == Json::array({"default", "other"}));
    EngineOptions o = options_for(fx);
    o.profile = "default";
    QueryEngine chosen(o);
    CHECK(find(chosen, "run_A")["status"] == "success");
    o.profile = "nope";
    QueryEngine unknown(o);
    CHECK(has_diagnostic(find(unknown, "run_A"), "unknown_profile"));
  }
}

TEST_CASE("read-only: unusable databases create no file; every tool leaves DB, WAL and SHM bytes unchanged") {
  const fs::path dir = scratch("readonly");
  const auto unusable = [&](const fs::path& db, const std::string& code, std::optional<fs::path> root = std::nullopt) {
    const auto before = directory_listing(db.parent_path());
    EngineOptions o;
    o.db = db;
    o.root = root;
    QueryEngine e(o);
    for (const Json& r : {find(e, "x"), trace(e, "x"), api(e, "x"), e.check_freshness({})}) {
      check_response(r, r["tool"].get<std::string>());
      CHECK(r["status"] == "error");
      CHECK_MESSAGE(has_diagnostic(r, code), code);
    }
    CHECK(directory_listing(db.parent_path()) == before);
  };
  SUBCASE("missing") {
    fs::create_directories(dir / "missing");
    unusable(dir / "missing" / "index.sqlite", "db_missing");
    QueryEngine none{EngineOptions{}};
    CHECK(has_diagnostic(find(none, "x"), "db_missing"));
  }
  SUBCASE("foreign text file") {
    fs::create_directories(dir / "text");
    write_text(dir / "text" / "index.sqlite", "not a database");
    unusable(dir / "text" / "index.sqlite", "db_foreign");
  }
  SUBCASE("foreign SQLite WAL database") {
    fs::create_directories(dir / "foreign");
    raw_create(dir / "foreign" / "index.sqlite", "PRAGMA journal_mode=WAL; CREATE TABLE t(x);");
    REQUIRE(file_digest(sidecar(dir / "foreign" / "index.sqlite", "-wal")) == "absent");
    unusable(dir / "foreign" / "index.sqlite", "db_foreign");
  }
  SUBCASE("other schema version") {
    fs::create_directories(dir / "version");
    raw_create(dir / "version" / "index.sqlite",
               "PRAGMA journal_mode=WAL; PRAGMA application_id=1279479123; PRAGMA user_version=2; CREATE TABLE meta(key, value);");
    unusable(dir / "version" / "index.sqlite", "db_schema_version");
  }
  SUBCASE("workspace root mismatch") {
    const QueryFx fx = QueryFx::create(dir / "mismatch");
    REQUIRE(file_digest(sidecar(fx.db, "-wal")) == "absent");
    const std::string lock_before = file_digest(sidecar(fx.db, ".writer.lock"));
    unusable(fx.db, "db_root_mismatch", fx.dir / "another-root");
    CHECK(file_digest(sidecar(fx.db, ".writer.lock")) == lock_before);
    EngineOptions o = options_for(fx);
    o.root = fx.workspace;
    QueryEngine matching(o);
    CHECK(find(matching, "run_A")["status"] == "success");
  }
  SUBCASE("bytes unchanged") {
    const QueryFx fx = QueryFx::create(dir / "bytes");
    const auto run_all = [&](QueryEngine& e) {
      CHECK(find(e, "run_A")["status"] == "success");
      CHECK(trace(e, "run_A", Direction::incoming, 5)["status"] == "success");
      CHECK(trace(e, "run_A", Direction::outgoing, 5)["status"] == "success");
      CHECK(api(e, "include/shapes.h")["status"] == "success");
      CHECK(e.check_freshness({})["status"] != "error");
    };
    const std::string db_before = file_digest(fx.db);
    const std::string lock_before = file_digest(sidecar(fx.db, ".writer.lock"));
    REQUIRE(file_digest(sidecar(fx.db, "-wal")) == "absent");
    {
      QueryEngine e(options_for(fx));
      run_all(e);
      // D2: SQLite's read-only open created the WAL sidecars; the DB bytes are unchanged.
      CHECK(fs::file_size(sidecar(fx.db, "-wal")) == 0);
      CHECK(fs::exists(sidecar(fx.db, "-shm")));
      const std::string wal = file_digest(sidecar(fx.db, "-wal"));
      const auto shm_size = fs::file_size(sidecar(fx.db, "-shm"));
      run_all(e);
      CHECK(file_digest(sidecar(fx.db, "-wal")) == wal);
      CHECK(fs::file_size(sidecar(fx.db, "-shm")) == shm_size);
    }
    CHECK(file_digest(fx.db) == db_before);
    CHECK(file_digest(sidecar(fx.db, ".writer.lock")) == lock_before);
    CHECK(raw_count(fx.db, "SELECT count(*) FROM unit_freshness") == 0);
    CHECK(raw_count(fx.db, "SELECT count(*) FROM attempt") == 1);

    // With a live writer holding committed WAL frames.
    Store writer = fx.writer();
    {
      auto lock = writer.acquire_writer(5s);
      Batch b;
      b.profile = "default";
      b.install = {unit_c(fx.k)};
      REQUIRE(writer.publish(*lock, b).outcome == PublishOutcome::committed);
    }
    REQUIRE(fs::file_size(sidecar(fx.db, "-wal")) > 0);
    const std::string db_live = file_digest(fx.db);
    const std::string wal_live = file_digest(sidecar(fx.db, "-wal"));
    {
      QueryEngine e(options_for(fx));
      run_all(e);
      CHECK(find(e, "run_A")["fact_generation"] == 2);
    }
    CHECK(file_digest(fx.db) == db_live);
    CHECK(file_digest(sidecar(fx.db, "-wal")) == wal_live);
  }
}

TEST_CASE("check_freshness: read-only inspection reports success, stale and partial and persists nothing") {
  QueryFx fx = QueryFx::create(scratch("check"));
  // Republish the units with recorded inputs and verified build currency. C
  // also depends on 400 generated headers so that a 1 ms budget cannot finish.
  const auto observe = [&](const std::string& file, InputRole role) {
    InputObservation o;
    o.path_utf8 = path_to_utf8_generic(fx.at(file));
    o.repo_path = repo(file);
    o.role = role;
    std::ifstream in(fx.at(file), std::ios::binary);
    o.content_hash = hash_file_content(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()));
    return o;
  };
  const auto observed = [&](Contribution c, const std::string& file) {
    c.inputs.push_back(observe(file, InputRole::source));
    c.build_currency = BuildCurrency::verified;
    return c;
  };
  Contribution c = observed(unit_c(fx.k), "src/c.cpp");
  for (int i = 0; i < 400; ++i) {
    const std::string gen = "gen/g" + std::to_string(i) + ".h";
    fx.write(gen, std::string(2048, 'x'));
    c.inputs.push_back(observe(gen, InputRole::generated));
  }
  fx.publish({observed(unit_a(fx.k), "src/a.cpp"), observed(unit_b(fx.k), "src/b.cpp"), c});
  const auto persisted_rows = [&] { return raw_count(fx.db, "SELECT count(*) FROM unit_freshness"); };
  const std::string db_before = file_digest(fx.db);
  QueryEngine e(options_for(fx));

  CheckArgs args;
  args.budget = 10s;
  Json r = e.check_freshness(args);
  check_response(r, "check_freshness");
  CHECK(r["status"] == "success");
  CHECK(r["result"]["outcome"] == "complete");
  CHECK(r["result"]["persisted"] == false);
  CHECK(r["result"]["live"]["semantic_graph"] == "fresh");
  CHECK(r["result"]["live"]["uht_context"] == "not_applicable");
  CHECK(r["result"]["units"]["fresh"] == 3);
  CHECK(r["result"]["inputs_unchecked"] == 0);
  CHECK(r["layers"][3]["freshness"] == "unknown");  // persisted state is untouched and reported raw

  // The writer lock is never needed.
  {
    Store w = fx.writer();
    auto lock = w.acquire_writer(5s);
    REQUIRE(lock);
    CHECK(e.check_freshness(args)["result"]["outcome"] == "complete");
  }

  fx.write("src/b.cpp", "// edited\n");
  r = e.check_freshness(args);
  CHECK(r["status"] == "stale");
  CHECK(r["result"]["units"]["stale"] == 1);
  REQUIRE(r["result"]["changes"].size() == 1);
  CHECK(r["result"]["changes"][0]["path"] == "src/b.cpp");
  CHECK(r["result"]["changes"][0]["change"] == "modified");
  CHECK(r["result"]["guidance"].get<std::string>().find("livecodemap build") != std::string::npos);

  args.budget = 1ms;
  r = e.check_freshness(args);
  check_response(r, "check_freshness");
  CHECK(r["status"] == "partial");
  CHECK(r["result"]["outcome"] == "budget_exhausted");
  CHECK(r["result"]["units"]["fresh"] == 0);
  CHECK(has_diagnostic(r, "budget_exhausted"));
  // C1: unchecked inputs may still make units stale, so the stale-unit count is not exact.
  CHECK(r["result"]["units"]["stale"].get<std::uint64_t>() < 3);
  CHECK(r["pagination"]["complete"] == false);
  CHECK(r["pagination"]["total"].is_null());
  CHECK(r["pagination"]["omitted"].is_null());
  CHECK(r["pagination"]["limit_reason"] == "time_budget");
  CHECK(r["pagination"]["next_cursor"].is_null());

  args.budget = 0ms;
  CHECK(has_diagnostic(e.check_freshness(args), "invalid_budget"));
  CHECK(persisted_rows() == 0);
  CHECK(file_digest(fx.db) == db_before);

  // A publication between the passes is detected (generation_changed), never fresh.
  ReadOnlyStore ro = ReadOnlyStore::open(fx.db);
  InspectRequest req;
  req.profile = "default";
  req.budget = 10s;
  req.after_first_pass = [&] { fx.publish({unit_c(fx.k)}); };
  const FreshnessReport changed = ro.inspect_freshness(req);
  CHECK(changed.outcome == InspectionOutcome::generation_changed);
  CHECK_FALSE(changed.published);
  CHECK_FALSE(changed.persisted_downgrade);
  for (const auto& u : changed.units) CHECK(u.semantic != Freshness::fresh);
  CHECK(persisted_rows() == 0);
}

TEST_CASE("cli: subcommands, defaults, exit codes, JSON output and UTF-8") {
  const QueryFx fx = QueryFx::create(scratch("cli"));
  const std::string db = path_to_utf8_generic(fx.db);
  const auto run = [&](std::vector<std::string> args, std::optional<std::string> env = std::nullopt) {
    std::istringstream in;
    std::ostringstream out;
    std::ostringstream err;
    const int code = run_cli(args, env, in, out, err);
    return std::make_tuple(code, out.str(), err.str());
  };

  auto [code, out, err] = run({"--db", db, "--json", "ask", "run_A"});
  CHECK(code == 0);
  const Json cli = Json::parse(out);
  QueryEngine e(options_for(fx));
  FindCodeArgs a;
  a.query = "run_A";
  a.limit = 8;
  CHECK(cli == e.find_code(a));  // the CLI --json object is the response model itself

  std::tie(code, out, err) = run({"ask", "f", "--json"}, db);  // LIVECODEMAP_DB, options after the command
  CHECK(code == 0);
  CHECK(Json::parse(out)["pagination"]["shown"] == 8);  // CLI ask default
  std::tie(code, out, err) = run({"--db", db, "--json", "skeleton", "include/shapes.h"});
  CHECK(Json::parse(out)["pagination"]["shown"] == 20);
  std::tie(code, out, err) = run({"--db", db, "--json", "callers", "geo::Shape::area", "--depth", "2"});
  CHECK(code == 0);
  CHECK(Json::parse(out)["result"]["depth"] == 2);
  std::tie(code, out, err) = run({"--db", db, "callers", "run_A", "--outgoing"});
  CHECK(code == 0);
  CHECK(out.find("trace_calls: success") == 0);
  std::tie(code, out, err) = run({"--db", db, "--json", "ask", "조회_함수"});
  CHECK(out.find("src/한글.cpp") != std::string::npos);  // raw UTF-8, not escaped
  std::tie(code, out, err) = run({"--db", db, "check", "--budget", "5000"});
  CHECK(code == 0);

  std::tie(code, out, err) = run({"--db", db, "callers", "dup"});
  CHECK(code == 1);  // ambiguity is an error response
  CHECK(out.find("candidates") != std::string::npos);
  std::tie(code, out, err) = run({"--db", path_to_utf8_generic(fx.dir / "none.sqlite"), "ask", "x"});
  CHECK(code == 1);
  CHECK(out.find("db_missing") != std::string::npos);
  std::tie(code, out, err) = run({"--db", db, "frobnicate"});
  CHECK(code == 2);
  std::tie(code, out, err) = run({"--db", db, "ask", "x", "--limit", "abc"});
  CHECK(code == 2);
  std::tie(code, out, err) = run({"--db", db, "ask", "x", "--depth", "2"});
  CHECK(code == 2);
  std::tie(code, out, err) = run({});
  CHECK(code == 2);
  std::tie(code, out, err) = run({"--help"});
  CHECK(code == 0);
}

TEST_CASE("find_code: a search budget ends between tiers with a cursor that resumes at the next tier") {
  const QueryFx fx = QueryFx::create(scratch("search-budget"));
  QueryEngine complete(options_for(fx));
  const Json full = find(complete, "detail");
  REQUIRE(full["pagination"]["complete"] == true);
  EngineOptions o = options_for(fx);
  o.search_budget = 0ms;  // every tier check is over budget: one new tier per call
  QueryEngine e(o);
  std::vector<std::string> got;
  int calls = 0;
  for (const auto& page : chain([&](const std::optional<std::string>& c) { return find(e, "detail", 1000, c); })) {
    ++calls;
    check_response(page, "find_code");
    if (page["pagination"]["complete"] == false) {
      CHECK(page["status"] == "partial");
      CHECK(page["pagination"]["limit_reason"] == "time_budget");
    }
    for (const auto& r : page["result"]["results"]) got.push_back(r.dump());
  }
  CHECK(calls == 4);
  std::vector<std::string> expected;
  for (const auto& r : full["result"]["results"]) expected.push_back(r.dump());
  CHECK(got == expected);
}

TEST_CASE("trace_calls: an override whose only evidence is withheld creates no possible branch (F1)") {
  for (const bool deletion : {false, true}) {
    CAPTURE(deletion);
    const QueryFx fx = QueryFx::create(scratch(deletion ? "f1-deleted" : "f1-excluded"));
    // Base::go and Derived::go are declared in a visible header; run_D calls
    // Base::go through its virtual slot; the override's only evidence is in over.hpp.
    const CanonicalKey base = key("go", SymbolKind::method, "()", {cls("Base")});
    const CanonicalKey derived = key("go", SymbolKind::method, "()", {cls("Derived")});
    const CanonicalKey run = key("run_D");
    fx.write("include/base.hpp", "// base.hpp\n");
    fx.write("include/over.hpp", "// over.hpp\n");
    fx.write("src/d.cpp", "// d.cpp\n");
    FactSet f;
    f.add_symbol_location(base, SymbolLocation{LocationRole::declaration, loc("include/base.hpp", 10), std::nullopt, "D", {}});
    f.add_symbol_location(derived, SymbolLocation{LocationRole::declaration, loc("include/base.hpp", 20), std::nullopt, "D", {}});
    f.add_symbol_location(run, SymbolLocation{LocationRole::definition, loc("src/d.cpp", 10), sha256("run_D"), "D", {}});
    f.add_relation_evidence(RelationKey{id(run), id(base), RelationKind::calls, Confidence::confirmed},
                            call_at("D", "src/d.cpp", 20, DispatchKind::virtual_slot));
    Evidence over;
    over.analysis_unit = "D";
    over.location = loc("include/over.hpp", 5);
    f.add_relation_evidence(RelationKey{id(derived), id(base), RelationKind::overrides, Confidence::confirmed}, over);
    fx.publish({contribution("D", "D", "cmd-d", std::move(f))});
    QueryEngine e(options_for(fx));

    // Control: with the override visible, run_D is a possible caller of Derived::go.
    REQUIRE(record_for(trace(e, id(derived).value), id(run)) != nullptr);
    REQUIRE(record_for(trace(e, id(run).value, Direction::outgoing), id(derived)) != nullptr);

    if (deletion) {
      fs::remove(fx.at("include/over.hpp"));
    } else {
      fx.exclude({"include/over.hpp"});
    }
    const FactSet pub = fx.writer().read().load_facts("default", Projection::public_);
    REQUIRE(pub.find_symbol(id(base)) != nullptr);
    REQUIRE(pub.find_symbol(id(derived)) != nullptr);
    REQUIRE(pub.find_relation(RelationKey{id(derived), id(base), RelationKind::overrides, Confidence::confirmed}) == nullptr);

    // Incoming on the override: no possible caller through the hidden override.
    const Json in = trace(e, id(derived).value);
    check_response(in, "trace_calls");
    CHECK(in["result"]["records"].empty());
    CHECK(in["result"]["counts"]["unique_callers"] == 0);
    CHECK(in["result"]["counts"]["possible_callsites"] == 0);
    // Outgoing from the caller: the confirmed slot only, no override candidate.
    const Json out = trace(e, id(run).value, Direction::outgoing);
    check_response(out, "trace_calls");
    CHECK(record_for(out, id(derived)) == nullptr);
    REQUIRE(record_for(out, id(base)) != nullptr);
    CHECK((*record_for(out, id(base)))["confirmed"].size() == 1);
    CHECK(out["result"]["counts"]["possible_callsites"] == 0);
    CHECK(out.dump().find(id(derived).value) == std::string::npos);
    // Incoming on the base keeps its confirmed caller.
    CHECK(record_for(trace(e, id(base).value), id(run)) != nullptr);
    check_equivalence(fx, e);
  }
}

TEST_CASE("file_api: distinct public observations of one include directive are not merged (F2)") {
  const QueryFx fx = QueryFx::create(scratch("f2-includes"));
  fx.write("include/macro.hpp", "// macro.hpp\n");
  fx.write("include/api.inl", "// api.inl\n");
  // One macro include directive in macro.hpp seen by four units: two expand it
  // to "api.inl" over the same bytes, one to "./api.inl", one over other bytes.
  const auto unit = [&](const std::string& label, const std::string& expanded, const std::string& seed) {
    FactSet f;
    f.add_symbol_location(key("macro_user_" + label),
                          SymbolLocation{LocationRole::declaration, loc("include/macro.hpp", 40, seed), std::nullopt, label, {}});
    DirectIncludeFact inc;
    inc.includer = repo("include/macro.hpp");
    inc.operand_kind = IncludeOperandKind::macro_tokens;
    inc.operand_as_written = "API_HEADER";
    inc.expanded_spelling = expanded;
    inc.activity = IncludeActivity::active;
    inc.resolution = IncludeResolution::resolved_internal;
    inc.resolved_repo_target = repo("include/api.inl");
    inc.directive_location = loc("include/macro.hpp", 5, seed);
    inc.analysis_unit = label;
    f.add_direct_include(inc);
    return contribution(label, label, "cmd-" + label, std::move(f));
  };
  fx.publish({unit("E1", "api.inl", "v1"), unit("E2", "./api.inl", "v1"), unit("E3", "api.inl", "v1"),
              unit("E4", "api.inl", "v2")});
  QueryEngine e(options_for(fx));
  const Json r = api(e, "include/macro.hpp");
  check_response(r, "file_api");
  const Json& includes = r["result"]["includes"];
  REQUIRE(includes.size() == 3);
  std::multiset<std::string> seen;
  for (const auto& inc : includes) {
    seen.insert(inc["expanded"].get<std::string>() + "#" + inc["location"]["content_hash"].get<std::string>() + "#" +
                std::to_string(inc["units"].get<int>()));
  }
  const std::string v1 = loc("include/macro.hpp", 5, "v1").content_hash.digest.hex();
  const std::string v2 = loc("include/macro.hpp", 5, "v2").content_hash.digest.hex();
  CHECK(seen == std::multiset<std::string>{"api.inl#" + v1 + "#2", "./api.inl#" + v1 + "#1", "api.inl#" + v2 + "#1"});
  check_equivalence(fx, e);
}

TEST_CASE("check_freshness: the stale-unit count is exact only when complete or when every unit is already stale (C1)") {
  using store::InspectionOutcome;
  const auto page = [](InspectionOutcome o, std::size_t units, std::size_t stale) { return detail::freshness_page(o, units, stale); };
  CHECK(page(InspectionOutcome::complete, 5, 0).complete);
  CHECK_FALSE(page(InspectionOutcome::complete, 5, 0).limit_reason);
  CHECK(page(InspectionOutcome::complete, 0, 0).complete);
  const auto exhausted = page(InspectionOutcome::budget_exhausted, 5, 4);
  CHECK_FALSE(exhausted.complete);
  CHECK(exhausted.limit_reason == std::optional<std::string>("time_budget"));
  const auto changed = page(InspectionOutcome::generation_changed, 5, 0);
  CHECK_FALSE(changed.complete);
  CHECK(changed.limit_reason == std::optional<std::string>("generation_changed"));
  CHECK_FALSE(page(InspectionOutcome::budget_exhausted, 0, 0).complete);
  // Every unit already detected stale: no unchecked input can add one.
  CHECK(page(InspectionOutcome::budget_exhausted, 5, 5).complete);
  CHECK(page(InspectionOutcome::generation_changed, 3, 3).complete);
}

TEST_CASE("check_freshness: an all-stale budget_exhausted response has an exact count and a consistent diagnostic (v3)") {
  QueryFx fx = QueryFx::create(scratch("all-stale"));
  // Every unit (A, B, C) depends on aaa/deleted.h, which sorts first and is
  // deleted before the check; unit A also depends on zzz/last.h, which sorts
  // last. A probe hook waits, before zzz/last.h is probed in pass 1, until the
  // inspection's deadline has certainly passed: the budget then ends after the
  // deleted input was seen and before the inspection could finish, whatever
  // the host speed. No sleep of a fixed length and no large file are involved.
  fx.write("aaa/deleted.h", "// deleted\n");
  fx.write("zzz/last.h", "// last\n");
  const auto observe = [&](const std::string& file) {
    InputObservation o;
    o.path_utf8 = path_to_utf8_generic(fx.at(file));
    o.repo_path = repo(file);
    o.role = InputRole::header;
    std::ifstream in(fx.at(file), std::ios::binary);
    o.content_hash = hash_file_content(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()));
    return o;
  };
  Contribution a = unit_a(fx.k);
  Contribution b = unit_b(fx.k);
  Contribution c = unit_c(fx.k);
  for (Contribution* u : {&a, &b, &c}) u->inputs.push_back(observe("aaa/deleted.h"));
  a.inputs.push_back(observe("zzz/last.h"));
  fx.publish({a, b, c});
  fs::remove(fx.at("aaa/deleted.h"));

  // The deadline is start + budget, and the inspection starts before the
  // first hook call, so first_call + budget is at or after the deadline.
  constexpr auto budget = 2000ms;  // generous: plan loading of three units takes milliseconds
  std::optional<std::chrono::steady_clock::time_point> first_call;
  std::vector<std::string> probed;
  EngineOptions o = options_for(fx);
  o.freshness_probe_hook = [&](int pass, const std::string& path) {
    const auto now = std::chrono::steady_clock::now();
    if (!first_call) first_call = now;
    probed.push_back(std::to_string(pass) + ":" + path.substr(path.find_last_of('/') + 1));
    if (pass == 1 && path.size() >= 10 && path.compare(path.size() - 10, 10, "zzz/last.h") == 0) {
      while (std::chrono::steady_clock::now() < *first_call + budget) std::this_thread::yield();
    }
  };
  QueryEngine e(o);
  CheckArgs args;
  args.budget = budget;
  const Json r = e.check_freshness(args);
  check_response(r, "check_freshness");
  // Pass 1 probed the deleted input, then the hook ended the budget before zzz/last.h.
  CHECK(probed == std::vector<std::string>{"1:deleted.h", "1:last.h"});
  REQUIRE(r["result"]["outcome"] == "budget_exhausted");
  REQUIRE(r["result"]["units"]["stale"] == 3);
  CHECK(r["result"]["inputs_checked"] == 1);
  CHECK(r["status"] == "partial");
  // Pagination: every unit is already stale, so the count is exact.
  CHECK(r["pagination"]["complete"] == true);
  CHECK(r["pagination"]["total"] == 3);
  CHECK(r["pagination"]["omitted"] == 0);
  CHECK(r["pagination"]["limit_reason"].is_null());
  CHECK(r["pagination"]["next_cursor"].is_null());
  // The diagnostic agrees with the pagination.
  REQUIRE(has_diagnostic(r, "budget_exhausted"));
  std::string message;
  for (const auto& d : r["diagnostics"]) {
    if (d["code"] == "budget_exhausted") message = d["message"].get<std::string>();
  }
  CAPTURE(message);
  CHECK(message.find("not exact") == std::string::npos);
  CHECK(message.find("every unit") != std::string::npos);
  CHECK(message.find("exact") != std::string::npos);
}

TEST_CASE("check_freshness: the incomplete-inspection diagnostic follows the page and gives retry guidance (v3)") {
  using store::InspectionOutcome;
  const auto partial = detail::freshness_page(InspectionOutcome::budget_exhausted, 5, 2);
  const auto [code, inexact] = detail::freshness_diagnostic(InspectionOutcome::budget_exhausted, partial, 7);
  CHECK(code == "budget_exhausted");
  CHECK(inexact.find("7 input(s) unchecked") != std::string::npos);
  CHECK(inexact.find("not exact") != std::string::npos);
  CHECK(inexact.find("--budget") != std::string::npos);

  const auto all_stale = detail::freshness_page(InspectionOutcome::budget_exhausted, 5, 5);
  const auto [code2, exact] = detail::freshness_diagnostic(InspectionOutcome::budget_exhausted, all_stale, 7);
  CHECK(code2 == "budget_exhausted");
  CHECK(exact.find("not exact") == std::string::npos);
  CHECK(exact.find("every unit is already detected stale, so the stale count is exact") != std::string::npos);

  for (const std::size_t stale : {std::size_t{0}, std::size_t{5}}) {
    const auto page = detail::freshness_page(InspectionOutcome::generation_changed, 5, stale);
    const auto [code3, changed] = detail::freshness_diagnostic(InspectionOutcome::generation_changed, page, 0);
    CHECK(code3 == "generation_changed");
    CHECK(changed.find("publication") != std::string::npos);
    CHECK(changed.find("re-run check") != std::string::npos);
    CHECK(changed.find("larger --budget") != std::string::npos);
  }
}
