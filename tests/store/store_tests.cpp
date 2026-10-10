#include <doctest/doctest.h>

#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "lcm/facts.hpp"
#include "lcm/source.hpp"
#include "lcm/status.hpp"
#include "lcm/store/contribution_codec.hpp"
#include "lcm/store/store.hpp"
#include "store_fixture.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

using namespace lcm;
using namespace lcm::store;
using namespace lcm::store::test;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

fs::path scratch(const std::string& name) {
  static std::atomic<int> counter{0};
  const fs::path p = path_from_utf8(LCM_TEST_SCRATCH_DIR) /
                     path_from_utf8(name + "-" +
                                    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                                    std::to_string(counter++));
  fs::create_directories(p);
  return p;
}

struct Env {
  fs::path dir;
  fs::path workspace;
  fs::path db;
};

Env make_env(const std::string& name) {
  Env e;
  e.dir = scratch(name);
  e.workspace = e.dir / "repo";
  fs::create_directories(e.workspace);
  e.db = e.dir / "index.sqlite";
  return e;
}

Store open_store(const Env& e, JournalMode mode = JournalMode::wal) {
  StoreOptions o;
  o.journal = mode;
  return Store::open(e.db, e.workspace, o);
}

WriterLock take(Store& s) {
  auto l = s.acquire_writer(5s);
  REQUIRE(l);
  return std::move(*l);
}

void write_file(const fs::path& p, const std::string& text) {
  fs::create_directories(p.parent_path());
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out << text;
}

std::string read_file(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::int64_t raw_count(const fs::path& db, const std::string& sql) {
  sqlite3* h = nullptr;
  REQUIRE(sqlite3_open_v2(path_to_utf8_generic(db).c_str(), &h, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK);
  sqlite3_stmt* st = nullptr;
  REQUIRE(sqlite3_prepare_v2(h, sql.c_str(), -1, &st, nullptr) == SQLITE_OK);
  std::int64_t n = -1;
  if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int64(st, 0);
  sqlite3_finalize(st);
  sqlite3_close(h);
  return n;
}

void raw_exec(const fs::path& db, const std::string& sql) {
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

// Everything fact_hash() and operator== ignore: display spellings.
std::string written_dump(const FactSet& f) {
  std::string out;
  const auto add = [&](const std::string& s) { out += s + "\n"; };
  for (const auto& [id, s] : f.symbols()) {
    if (const auto* internal = std::get_if<InternalLinkage>(&s.key.linkage)) add(internal->file_scope.as_written);
    for (const auto& l : s.locations) add(id.value + " " + l.location.file.as_written);
  }
  for (const auto& [k, r] : f.relations()) {
    for (const auto& e : r.evidence) add(e.location.file.as_written);
  }
  for (const auto& u : f.unresolved_sites()) add(u.location.file.as_written);
  for (const auto& c : f.captures()) add(c.location.file.as_written);
  for (const auto& i : f.direct_includes()) {
    add(i.includer.as_written + "|" + (i.resolved_repo_target ? i.resolved_repo_target->as_written : "-") + "|" +
        i.directive_location.file.as_written);
    for (const auto& b : i.condition_path) {
      if (b.own_condition) add(b.own_condition->location.file.as_written);
      for (const auto& t : b.preceding_branches) add(t.location.file.as_written);
    }
  }
  return out;
}

Batch batch(std::vector<Contribution> install, std::vector<std::string> retire = {}, std::string profile = "default") {
  Batch b;
  b.profile = std::move(profile);
  b.install = std::move(install);
  b.retire = std::move(retire);
  return b;
}

std::string hex(const FactSet& f) { return f.fact_hash().hex(); }

const Symbol* by_name(const FactSet& f, const std::string& name) {
  for (const auto& [id, s] : f.symbols()) {
    if (s.key.canonical_name == name) return &s;
  }
  return nullptr;
}

const LayerState& layer(const ProfileStatus& st, Layer l) { return st.layers.at(static_cast<std::size_t>(l)); }

}  // namespace

// =============================================================================

TEST_CASE("roundtrip: every supported fact field and the unit provenance survive reopening") {
  const Env env = make_env("roundtrip");
  TuOptions full;
  full.with_conditional_include = true;
  full.with_capture = true;
  Contribution c = shared_tu("A", "src/a.cpp", "cmd-a-1", full);
  c.provenance.context_id = "ctx:" + sha256("args").hex();
  c.provenance.pch_producer_command_id = "cmd-pch";
  c.provenance.pch_producer_replay_id = "replay-pch";
  c.analysis_status = "completed_with_errors";
  c.build_currency = BuildCurrency::verified;
  c.inputs.push_back(InputObservation{path_to_utf8_generic(env.workspace / "src/a.cpp"), repo("src/a.cpp", "src/A.cpp"),
                                      InputRole::source, hash_file_content("a"), false});
  c.inputs.push_back(InputObservation{path_to_utf8_generic(env.dir / "sdk/ext.h"), std::nullopt,
                                      InputRole::external_dependency, hash_file_content("ext"), true});
  c.includes.push_back(IncludeEdge{std::nullopt, "pch.h", false, path_to_utf8_generic(env.dir / "pch.h"), 0});
  c.includes.push_back(IncludeEdge{path_to_utf8_generic(env.workspace / "src/a.cpp"), "shared.h", true, std::nullopt, 3});
  c.diagnostics.push_back(UnitDiagnostic{"error", "use of undeclared identifier", std::string("src/a.cpp"), 3, 7});
  c.diagnostics.push_back(UnitDiagnostic{"note", "no file", std::nullopt, 0, 0});
  c.limits = {"limit one", "limit two"};
  const std::string expected_hash = hex(c.facts);
  const std::string expected_written = written_dump(c.facts);
  REQUIRE(c.facts.captures().size() == 1);
  REQUIRE(c.facts.direct_includes().size() == 3);

  {
    Store store = open_store(env);
    CHECK(store.journal_mode() == JournalMode::wal);
    WriterLock lock = take(store);
    store.ensure_profile(lock, "default");
    const PublishResult r = store.publish(lock, batch({c}));
    REQUIRE_MESSAGE(r.outcome == PublishOutcome::committed, r.message);
    CHECK(r.fact_generation == 1);
  }
  Store store = open_store(env);
  const ReadSnapshot snap = store.read();
  const FactSet loaded = snap.load_facts("default", Projection::all);
  CHECK(hex(loaded) == expected_hash);
  CHECK(written_dump(loaded) == expected_written);
  for (const auto& [id, s] : c.facts.symbols()) {
    const Symbol* l = loaded.find_symbol(id);
    REQUIRE(l);
    CHECK(l->key == s.key);
    CHECK(l->presence == s.presence);
    CHECK(l->locations == s.locations);
  }
  CHECK(loaded.direct_includes() == c.facts.direct_includes());
  CHECK(loaded.captures() == c.facts.captures());
  CHECK(loaded.unresolved_sites() == c.facts.unresolved_sites());

  const auto unit = snap.unit("default", "A");
  REQUIRE(unit);
  CHECK(unit->fact_label == "A");
  CHECK(unit->provenance == c.provenance);
  CHECK(unit->analysis_status == "completed_with_errors");
  CHECK(unit->build_currency == BuildCurrency::verified);
  CHECK(unit->committed_generation == 1);
  REQUIRE(unit->inputs.size() == 2);
  for (const auto& in : c.inputs) {
    const bool found = std::any_of(unit->inputs.begin(), unit->inputs.end(), [&](const InputObservation& o) {
      return o.path_utf8 == in.path_utf8 && o.role == in.role && o.content_hash == in.content_hash &&
             o.is_system == in.is_system && o.repo_path.has_value() == in.repo_path.has_value() &&
             (!o.repo_path || (o.repo_path->generic == in.repo_path->generic &&
                               o.repo_path->as_written == in.repo_path->as_written));
    });
    CHECK_MESSAGE(found, in.path_utf8);
  }
  CHECK(unit->includes == c.includes);
  CHECK_FALSE(unit->includes.front().includer_path_utf8.has_value());  // forced include keeps no owner
  CHECK(unit->diagnostics == c.diagnostics);
  CHECK(unit->limits == c.limits);
  REQUIRE(unit->fingerprints.size() == 6);
  for (const auto& f : unit->fingerprints) {
    const bool derived = f.kind == FingerprintKind::file_bytes || f.kind == FingerprintKind::declaration_api ||
                         f.kind == FingerprintKind::symbol_body || f.kind == FingerprintKind::compile_action;
    CHECK_MESSAGE(f.value.has_value() == derived, to_string(f.kind));
  }
  CHECK(unit->freshness.semantic == Freshness::unknown);  // never inspected
  CHECK_FALSE(unit->freshness.checked_at.has_value());
}

TEST_CASE("database identity: foreign, newer-schema and other-workspace databases are refused and left untouched") {
  const Env env = make_env("identity");
  raw_exec(env.db, "CREATE TABLE unrelated(x); INSERT INTO unrelated VALUES(1);");
  const std::string before = read_file(env.db);
  CHECK_THROWS_AS(open_store(env), StoreError);
  CHECK(read_file(env.db) == before);

  const Env newer = make_env("newer");
  { Store s = open_store(newer); }
  for (const int version : {kSchemaVersion + 1, kSchemaVersion - 1}) {
    CAPTURE(version);
    raw_exec(newer.db, "PRAGMA user_version=" + std::to_string(version) + ";");
    const std::string newer_bytes = read_file(newer.db);
    try {
      (void)open_store(newer);
      FAIL("other schema version accepted");
    } catch (const StoreError& e) {
      CHECK(std::string(e.what()).find("unsupported store schema version " + std::to_string(version)) !=
            std::string::npos);
    }
    CHECK(read_file(newer.db) == newer_bytes);
  }

  const Env other = make_env("workspace");
  { Store s = open_store(other); }
  CHECK_THROWS_AS(Store::open(other.db, other.dir / "elsewhere"), StoreError);
}

TEST_CASE("profiles and same-basename sources stay separate") {
  const Env env = make_env("isolation");
  Store store = open_store(env);
  WriterLock lock = take(store);
  store.ensure_profile(lock, "debug");
  store.ensure_profile(lock, "release");
  CHECK_THROWS_AS(store.ensure_profile(lock, "release", true), std::invalid_argument);

  // Two different files named x.cpp in different directories, two slots.
  const Contribution left = shared_tu("left/x", "left/x.cpp", "cmd-left");
  const Contribution right = shared_tu("right/x", "right/x.cpp", "cmd-right");
  REQUIRE(store.publish(lock, batch({left, right}, {}, "debug")).outcome == PublishOutcome::committed);
  const Contribution rel = shared_tu("left/x", "left/x.cpp", "cmd-left-release", TuOptions{"release-body"});
  REQUIRE(store.publish(lock, batch({rel}, {}, "release")).outcome == PublishOutcome::committed);

  const ReadSnapshot snap = store.read();
  CHECK(snap.profiles() == std::vector<std::string>{"debug", "release"});
  CHECK(hex(snap.load_facts("debug", Projection::all)) == hex(oracle({&left, &right})));
  CHECK(hex(snap.load_facts("release", Projection::all)) == hex(oracle({&rel})));
  CHECK(snap.fact_generation("debug") == 1);
  CHECK(snap.fact_generation("release") == 1);
  CHECK(snap.unit("debug", "left/x")->provenance.command_id == "cmd-left");
  CHECK(snap.unit("release", "left/x")->provenance.command_id == "cmd-left-release");
  CHECK(snap.units("debug").size() == 2);
  CHECK_THROWS_AS((void)snap.load_facts("missing"), std::invalid_argument);
}

TEST_CASE("replacement, removal and shared evidence survival follow the fresh-build oracle") {
  const Env env = make_env("replace");
  Store store = open_store(env);
  WriterLock lock = take(store);
  store.ensure_profile(lock, "default");
  TuOptions a_opts;
  a_opts.with_capture = true;
  const Contribution a = shared_tu("A", "src/a.cpp", "cmd-a-1", a_opts);
  const Contribution b = shared_tu("B", "src/b.cpp", "cmd-b-1", TuOptions{"body-b", "()", true, false});
  REQUIRE(store.publish(lock, batch({a, b})).outcome == PublishOutcome::committed);
  {
    const ReadSnapshot snap = store.read();
    const FactSet both = snap.load_facts("default", Projection::all);
    CHECK(hex(both) == hex(oracle({&a, &b})));
    // Shared header symbol has one location per unit; the header relation has evidence from both.
    const Symbol* twice = by_name(both, "twice");
    REQUIRE(twice);
    CHECK(twice->locations.size() == 2);
    const Symbol* area = by_name(both, "area");
    REQUIRE(area);
    const Relation* header_rel =
        both.find_relation(RelationKey{twice->id, area->id, RelationKind::calls, Confidence::confirmed});
    REQUIRE(header_rel);
    CHECK(header_rel->evidence.size() == 2);
  }

  // Command change under the same slot replaces, never appends.
  const Contribution b2 = shared_tu("B", "src/b.cpp", "cmd-b-2", TuOptions{"body-b2", "()", true, false});
  const PublishResult r2 = store.publish(lock, batch({b2}));
  REQUIRE(r2.outcome == PublishOutcome::committed);
  CHECK(r2.fact_generation == 2);
  {
    const ReadSnapshot snap = store.read();
    CHECK(hex(snap.load_facts("default", Projection::all)) == hex(oracle({&a, &b2})));
    CHECK(snap.units("default").size() == 2);
    CHECK(snap.unit("default", "B")->provenance.command_id == "cmd-b-2");
    CHECK(snap.unit("default", "B")->committed_generation == 2);
    CHECK(snap.unit("default", "A")->committed_generation == 1);
  }
  CHECK(raw_count(env.db, "SELECT count(*) FROM evidence e JOIN unit u ON u.id=e.unit_id WHERE u.slot='B'") ==
        static_cast<std::int64_t>([&] {
          std::size_t n = 0;
          for (const auto& [k, r] : b2.facts.relations()) n += r.evidence.size();
          return n;
        }()));

  // Removing A keeps every surviving B fact (header locations, header relation
  // evidence, unresolved owner) and matches both oracles.
  const PublishResult r3 = store.publish(lock, batch({}, {"A"}));
  REQUIRE(r3.outcome == PublishOutcome::committed);
  {
    const ReadSnapshot snap = store.read();
    const FactSet only_b = snap.load_facts("default", Projection::all);
    CHECK(hex(only_b) == hex(oracle({&b2})));
    FactSet incremental = oracle({&a, &b2});
    incremental.remove_contribution("A");
    CHECK(hex(only_b) == hex(incremental));
    CHECK(by_name(only_b, "run_A") == nullptr);
    CHECK(by_name(only_b, "") == nullptr);  // A's lambda is gone with its captures
    CHECK(only_b.captures().empty());
    const Symbol* twice = by_name(only_b, "twice");
    REQUIRE(twice);
    CHECK(twice->locations.size() == 1);
    CHECK(only_b.unresolved_sites().size() == 1);
    CHECK(only_b.unresolved_sites().front().enclosing == twice->id);
  }

  // Last removal leaves no common rows behind.
  REQUIRE(store.publish(lock, batch({}, {"B"})).outcome == PublishOutcome::committed);
  for (const char* table : {"symbol", "symbol_owner", "relation", "evidence", "file", "file_version", "unit",
                            "unit_symbol", "symbol_location", "unresolved", "capture", "direct_include", "include_term",
                            "input", "dependency", "include_edge", "include_site"}) {
    CHECK_MESSAGE(raw_count(env.db, std::string("SELECT count(*) FROM ") + table) == 0, table);
  }
  const ReadSnapshot snap = store.read();
  CHECK(snap.fact_generation("default") == 4);
  CHECK(snap.load_facts("default", Projection::all).symbols().empty());
}

TEST_CASE("schema v3: dependency, include and file-version rows are shared, survive removal and are pruned last") {
  const Env env = make_env("shared-rows");
  Store store = open_store(env);
  WriterLock lock = take(store);
  store.ensure_profile(lock, "default");
  write_file(env.workspace / "src/a.cpp", "int a;\n");
  write_file(env.workspace / "src/b.cpp", "int b;\n");
  write_file(env.workspace / "include/shared.h", "#pragma once\n");
  write_file(env.dir / "sdk/prefix.h", "#pragma once\n");
  const auto with_dependencies = [&](Contribution c, const std::string& source, unsigned line) {
    const std::string src = path_to_utf8_generic(env.workspace / source);
    const std::string hdr = path_to_utf8_generic(env.workspace / "include/shared.h");
    const std::string pre = path_to_utf8_generic(env.dir / "sdk/prefix.h");
    c.inputs.push_back(InputObservation{src, repo(source, source), InputRole::source,
                                        hash_file_content(read_file(env.workspace / source)), false});
    c.inputs.push_back(InputObservation{hdr, repo("include/shared.h", "include/shared.h"), InputRole::header,
                                        hash_file_content(read_file(env.workspace / "include/shared.h")), false});
    c.inputs.push_back(InputObservation{pre, std::nullopt, InputRole::external_dependency,
                                        hash_file_content(read_file(env.dir / "sdk/prefix.h")), true});
    c.includes.push_back(IncludeEdge{std::nullopt, "prefix.h", false, pre, 0});    // identical in both units
    c.includes.push_back(IncludeEdge{src, "shared.h", false, hdr, line});           // unit-specific includer
    return c;
  };
  const Contribution a = with_dependencies(shared_tu("A", "src/a.cpp", "cmd-a"), "src/a.cpp", 1);
  const Contribution b = with_dependencies(shared_tu("B", "src/b.cpp", "cmd-b"), "src/b.cpp", 2);
  REQUIRE(store.publish(lock, batch({a, b})).outcome == PublishOutcome::committed);
  // Shared: the header and prefix observations and the forced include exist once.
  CHECK(raw_count(env.db, "SELECT count(*) FROM input") == 6);
  CHECK(raw_count(env.db, "SELECT count(*) FROM dependency") == 4);
  CHECK(raw_count(env.db, "SELECT count(*) FROM include_edge") == 4);
  CHECK(raw_count(env.db, "SELECT count(*) FROM include_site") == 3);
  // Located rows share one version per observed file state.
  const std::int64_t versions = raw_count(env.db, "SELECT count(*) FROM file_version");
  CHECK(versions < raw_count(env.db, "SELECT count(*) FROM symbol_location"));
  const auto records_equal = [&](const Contribution& c) {
    const auto u = store.read().unit("default", c.slot);
    REQUIRE(u);
    CHECK(u->includes == c.includes);
    std::vector<std::string> expected;
    for (const auto& in : c.inputs) expected.push_back(in.path_utf8 + "|" + in.content_hash.digest.hex());
    std::vector<std::string> got;
    for (const auto& in : u->inputs) got.push_back(in.path_utf8 + "|" + in.content_hash.digest.hex());
    std::sort(expected.begin(), expected.end());
    std::sort(got.begin(), got.end());
    CHECK(got == expected);
  };
  records_equal(a);
  records_equal(b);
  CHECK(hex(store.read().load_facts("default", Projection::all)) == hex(oracle({&a, &b})));

  // Removing A keeps B's shared rows; B still reads back exactly.
  REQUIRE(store.publish(lock, batch({}, {"A"})).outcome == PublishOutcome::committed);
  records_equal(b);
  CHECK(raw_count(env.db, "SELECT count(*) FROM dependency") == 3);
  CHECK(raw_count(env.db, "SELECT count(*) FROM include_site") == 2);
  CHECK(hex(store.read().load_facts("default", Projection::all)) == hex(oracle({&b})));
}

TEST_CASE("body edit keeps the stable id; a signature change replaces the owned facts") {
  const Env env = make_env("identity-edits");
  Store store = open_store(env);
  WriterLock lock = take(store);
  store.ensure_profile(lock, "default");
  const Contribution v1 = shared_tu("A", "src/a.cpp", "cmd-a", TuOptions{"body-1"});
  const Contribution body = shared_tu("A", "src/a.cpp", "cmd-a", TuOptions{"body-2"});
  const Contribution sig = shared_tu("A", "src/a.cpp", "cmd-a", TuOptions{"body-2", "(long)"});
  const auto fingerprint = [&](FingerprintKind k) {
    return store.read().unit("default", "A")->fingerprints.at(static_cast<std::size_t>(k)).value.value();
  };
  REQUIRE(store.publish(lock, batch({v1})).outcome == PublishOutcome::committed);
  const StableId old_id = by_name(store.read().load_facts("default", Projection::all), "run_A")->id;
  const std::string api1 = fingerprint(FingerprintKind::declaration_api);
  const std::string body1 = fingerprint(FingerprintKind::symbol_body);
  const std::string action1 = fingerprint(FingerprintKind::compile_action);

  REQUIRE(store.publish(lock, batch({body})).outcome == PublishOutcome::committed);
  {
    const FactSet f = store.read().load_facts("default", Projection::all);
    const Symbol* run = by_name(f, "run_A");
    REQUIRE(run);
    CHECK(run->id == old_id);
    CHECK(run->locations.front().body_hash == sha256("body-2"));
  }
  CHECK(fingerprint(FingerprintKind::declaration_api) == api1);
  CHECK(fingerprint(FingerprintKind::symbol_body) != body1);
  CHECK(fingerprint(FingerprintKind::compile_action) == action1);

  REQUIRE(store.publish(lock, batch({sig})).outcome == PublishOutcome::committed);
  const FactSet f = store.read().load_facts("default", Projection::all);
  CHECK(f.find_symbol(old_id) == nullptr);
  const Symbol* run = by_name(f, "run_A");
  REQUIRE(run);
  CHECK(run->id != old_id);
  for (const auto& [k, r] : f.relations()) CHECK(k.source != old_id);
  CHECK(fingerprint(FingerprintKind::declaration_api) != api1);
  CHECK(hex(f) == hex(oracle({&sig})));

  // Analysis settings are explicit columns that survive a reopen and are part
  // of the compile-action fingerprint; facts and API are unaffected.
  const std::string action_sig = fingerprint(FingerprintKind::compile_action);
  const std::string api_sig = fingerprint(FingerprintKind::declaration_api);
  CHECK(store.read().unit("default", "A")->settings == AnalysisSettings{});
  Contribution configured = sig;
  configured.settings = AnalysisSettings{"clang 22.1.0", "C:/sdk/lib/clang/22", "repo", "accepted_only"};
  REQUIRE(store.publish(lock, batch({configured})).outcome == PublishOutcome::committed);
  CHECK(fingerprint(FingerprintKind::compile_action) != action_sig);
  CHECK(fingerprint(FingerprintKind::declaration_api) == api_sig);
  {
    Store reopened = open_store(env);
    CHECK(reopened.read().unit("default", "A")->settings == configured.settings);
  }
}

TEST_CASE("batch remapping, validation refusals and no-op rules") {
  const Env env = make_env("batch");
  Store store = open_store(env);
  WriterLock lock = take(store);
  store.ensure_profile(lock, "default");
  const Contribution p1 = shared_tu("unity/part1", "src/p1.cpp", "cmd-p1");
  const Contribution p2 = shared_tu("unity/part2", "src/p2.cpp", "cmd-p2");
  REQUIRE(store.publish(lock, batch({p1, p2})).outcome == PublishOutcome::committed);

  // Remap: retire both old slots and install the new selected action at once.
  FactSet merged = shared_tu_facts("unity/all", "src/p1.cpp", "run_p1");
  merge_into(merged, shared_tu_facts("unity/all", "src/p2.cpp", "run_p2"));
  const Contribution all = contribution("unity/all", "unity/all", "cmd-unity", merged);
  const PublishResult remap = store.publish(lock, batch({all}, {"unity/part1", "unity/part2"}));
  REQUIRE_MESSAGE(remap.outcome == PublishOutcome::committed, remap.message);
  CHECK(remap.fact_generation == 2);
  const std::string after_remap = hex(store.read().load_facts("default", Projection::all));
  CHECK(after_remap == hex(oracle({&all})));
  CHECK(raw_count(env.db, "SELECT count(*) FROM unit") == 1);

  const auto refused = [&](const Batch& b, const char* expected) {
    const PublishResult r = store.publish(lock, b);
    CHECK(r.outcome == PublishOutcome::rejected);
    CHECK_MESSAGE(r.message.find(expected) != std::string::npos, r.message);
    CHECK(r.fact_generation == 2);
    const ReadSnapshot snap = store.read();
    CHECK(hex(snap.load_facts("default", Projection::all)) == after_remap);
    CHECK(snap.fact_generation("default") == 2);
    CHECK(snap.status("default").last_attempt->state == AttemptState::failed);
    CHECK(snap.status("default").last_attempt->id == r.attempt_id.value());
  };
  refused(batch({p1, p1}), "appears more than once");
  refused(batch({p1}, {"unity/part1"}), "both installed/retired");
  refused(batch({}, {"x", "x"}), "retired twice");
  // Mixed ownership: one evidence item labelled by another unit.
  {
    Contribution mixed = shared_tu("M", "src/m.cpp", "cmd-m");
    FactSet foreign = shared_tu_facts("someone-else", "src/m.cpp", "run_M");
    merge_into(mixed.facts, foreign);
    refused(batch({mixed}), "owned by 'someone-else'");
  }
  // Two live slots may never share a fact label (ownership would collapse).
  {
    Contribution clash = contribution("other-slot", "unity/all", "cmd-o", shared_tu_facts("unity/all", "src/o.cpp", "run_o"));
    refused(batch({clash}), "owned by live slot 'unity/all'");
  }
  // An evidence endpoint the contribution does not define.
  {
    Contribution dangling = shared_tu("D", "src/d.cpp", "cmd-d");
    Evidence e;
    e.analysis_unit = "D";
    e.location = loc("src/d.cpp", 90);
    dangling.facts.add_relation_evidence(
        RelationKey{make_stable_id(key("run_D")), make_stable_id(key("nowhere")), RelationKind::calls, Confidence::confirmed}, e);
    refused(batch({dangling}), "relation endpoint without a symbol");
  }
  {
    Contribution no_command = shared_tu("N", "src/n.cpp", "");
    refused(batch({no_command}), "missing command identity");
  }

  // Empty batch and retiring unknown slots are no-ops without an attempt.
  const std::int64_t attempts = raw_count(env.db, "SELECT count(*) FROM attempt");
  CHECK(store.publish(lock, batch({})).outcome == PublishOutcome::no_op);
  CHECK(store.publish(lock, batch({}, {"never-existed"})).outcome == PublishOutcome::no_op);
  CHECK(raw_count(env.db, "SELECT count(*) FROM attempt") == attempts);
  CHECK(store.read().fact_generation("default") == 2);

  // Identical republication is an explicit success and advances once.
  const PublishResult again = store.publish(lock, batch({all}));
  CHECK(again.outcome == PublishOutcome::committed);
  CHECK(again.fact_generation == 3);
  CHECK(hex(store.read().load_facts("default", Projection::all)) == after_remap);
}

TEST_CASE("failed publications keep the committed graph, generation and last success") {
  for (const JournalMode mode : {JournalMode::wal, JournalMode::rollback}) {
    CAPTURE(to_string(mode));
    const Env env = make_env(std::string("rollback-") + std::string(to_string(mode)));
    Store store = open_store(env, mode);
    CHECK(store.journal_mode() == mode);
    MESSAGE("journal mode in effect: " << to_string(store.journal_mode()));
    WriterLock lock = take(store);
    store.ensure_profile(lock, "default");
    const Contribution a = shared_tu("A", "src/a.cpp", "cmd-a");
    const Contribution b = shared_tu("B", "src/b.cpp", "cmd-b");
    const PublishResult first = store.publish(lock, batch({a, b}));
    REQUIRE(first.outcome == PublishOutcome::committed);
    const std::string committed = hex(store.read().load_facts("default", Projection::all));
    const Contribution b2 = shared_tu("B", "src/b.cpp", "cmd-b-2", TuOptions{"body-2"});

    const auto unchanged = [&](const PublishResult& r) {
      CHECK(r.outcome == PublishOutcome::failed);
      CHECK(r.fact_generation == 1);
      const ReadSnapshot snap = store.read();
      CHECK(snap.fact_generation("default") == 1);
      CHECK(hex(snap.load_facts("default", Projection::all)) == committed);
      CHECK(snap.unit("default", "B")->provenance.command_id == "cmd-b");
      const ProfileStatus st = snap.status("default");
      REQUIRE(st.last_attempt);
      REQUIRE(st.last_success);
      CHECK(st.last_attempt->state == AttemptState::failed);
      CHECK(st.last_attempt->id == r.attempt_id.value());
      CHECK(st.last_success->id == first.attempt_id.value());
      CHECK(st.last_attempt->id != st.last_success->id);
      CHECK(st.failed_slots == std::vector<std::string>{"B"});
      CHECK(layer(st, Layer::semantic_graph).availability == Availability::partial);
    };

    // Failure injected by the harness between deletion and insertion.
    unchanged(store.publish(lock, batch({b2}), [](PublishPhase p) {
      if (p == PublishPhase::old_removed) throw std::runtime_error("injected after removal");
    }));
    // A genuine SQL failure during insertion (persistent trigger installed by the harness).
    raw_exec(env.db, "CREATE TRIGGER inject BEFORE INSERT ON evidence BEGIN SELECT RAISE(ABORT, 'injected sql'); END;");
    const PublishResult sql = store.publish(lock, batch({b2}));
    CHECK(sql.message.find("injected sql") != std::string::npos);
    unchanged(sql);
    raw_exec(env.db, "DROP TRIGGER inject;");
    // Rejected analysis recorded separately.
    const std::int64_t rejected = store.record_rejected(lock, "default", {"B"}, "front end rejected the unit");
    {
      const ProfileStatus st = store.read().status("default");
      CHECK(st.last_attempt->id == rejected);
      CHECK(st.last_attempt->message == "front end rejected the unit");
      CHECK(st.last_success->id == first.attempt_id.value());
    }
    // Recovery: the next successful publication clears the failed slot.
    const PublishResult ok = store.publish(lock, batch({b2}));
    REQUIRE(ok.outcome == PublishOutcome::committed);
    CHECK(ok.fact_generation == 2);
    const ProfileStatus st = store.read().status("default");
    CHECK(st.failed_slots.empty());
    CHECK(layer(st, Layer::semantic_graph).availability == Availability::ready);
    CHECK(hex(store.read().load_facts("default", Projection::all)) == hex(oracle({&a, &b2})));
  }
}

TEST_CASE("status layers: auxiliary failures never touch facts; nothing unproduced is available") {
  const Env env = make_env("status");
  Store store = open_store(env);
  WriterLock lock = take(store);
  store.ensure_profile(lock, "default");
  {
    const ProfileStatus empty = store.read().status("default");
    CHECK(layer(empty, Layer::semantic_graph).availability == Availability::missing);
    CHECK_FALSE(layer(empty, Layer::uht_context).applicable);
    CHECK(summarize_overall(empty.layers) == UserStatus::missing);
  }
  const Contribution a = shared_tu("A", "src/a.cpp", "cmd-a");
  REQUIRE(store.publish(lock, batch({a})).outcome == PublishOutcome::committed);
  const std::string committed = hex(store.read().load_facts("default", Projection::all));
  store.record_auxiliary_failure(lock, "default", Layer::retrieval, "ranking provider failed");
  CHECK_THROWS_AS(store.record_auxiliary_failure(lock, "default", Layer::semantic_graph, "x"), std::invalid_argument);
  CHECK_THROWS_AS(store.record_auxiliary_failure(lock, "default", Layer::structure, "x"), std::invalid_argument);

  const ReadSnapshot snap = store.read();
  const ProfileStatus st = snap.status("default");
  CHECK(st.fact_generation == 1);
  CHECK(st.ranking_generation == 0);
  CHECK(hex(snap.load_facts("default", Projection::all)) == committed);
  CHECK(layer(st, Layer::retrieval).availability == Availability::failed);
  CHECK(layer(st, Layer::retrieval).reason == "ranking provider failed");
  CHECK(layer(st, Layer::deep).availability == Availability::missing);
  CHECK(layer(st, Layer::structure).availability == Availability::missing);
  CHECK(layer(st, Layer::semantic_graph).availability == Availability::ready);
  CHECK(layer(st, Layer::semantic_graph).freshness == Freshness::unknown);  // never inspected
  CHECK(layer(st, Layer::semantic_graph).units.total == 1);
  CHECK(layer(st, Layer::semantic_graph).units.succeeded == 1);
  CHECK(layer(st, Layer::semantic_graph).last_success.has_value());
  CHECK(st.last_attempt->id == st.last_success->id);  // auxiliary attempts are not semantic attempts
  CHECK_FALSE(layer(st, Layer::uht_context).applicable);
  CHECK(summarize_layer(layer(st, Layer::semantic_graph)) == UserStatus::stale);
}

// ---------------------------------------------------------------------------
// Cross-process behaviour.

#ifdef _WIN32
namespace {

struct Child {
  PROCESS_INFORMATION pi{};
  Child() = default;
  Child(const Child&) = delete;
  Child& operator=(const Child&) = delete;
  ~Child() { kill(); }
  void kill() {
    if (pi.hProcess) {
      TerminateProcess(pi.hProcess, 99);
      WaitForSingleObject(pi.hProcess, 10000);
      CloseHandle(pi.hProcess);
      CloseHandle(pi.hThread);
      pi = {};
    }
  }
  DWORD wait(DWORD ms) {
    REQUIRE(WaitForSingleObject(pi.hProcess, ms) == WAIT_OBJECT_0);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    return code;
  }
};

void spawn(Child& child, const std::vector<std::wstring>& args) {
  std::wstring cmd = L"\"" + path_from_utf8(LCM_STORE_CHILD_EXE).wstring() + L"\"";
  for (const auto& a : args) cmd += L" \"" + a + L"\"";
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  REQUIRE(CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                         &child.pi));
}

std::vector<std::wstring> child_args(const std::string& mode, const Env& env, JournalMode journal, const fs::path& ready) {
  return {path_from_utf8(mode).wstring(), env.db.wstring(), env.workspace.wstring(),
          journal == JournalMode::wal ? L"wal" : L"rollback", ready.wstring()};
}

bool wait_for(const fs::path& ready, std::chrono::milliseconds timeout = 30s) {
  const auto end = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < end) {
    std::error_code ec;
    if (fs::exists(ready, ec) && fs::file_size(ready, ec) > 0) return true;
    std::this_thread::sleep_for(10ms);
  }
  return false;
}

}  // namespace

namespace {

// The spool path a child reported after "<state>\n".
fs::path reported_spool(const fs::path& ready, const std::string& state) {
  const std::string text = read_file(ready);
  REQUIRE(text.rfind(state + "\n", 0) == 0);
  return path_from_utf8(text.substr(state.size() + 1));
}

// A file deleted by the OS on handle close can take a moment after the owner exits.
bool gone_within(const fs::path& p, std::chrono::milliseconds timeout = 5s) {
  const auto end = std::chrono::steady_clock::now() + timeout;
  for (;;) {
    std::error_code ec;
    if (!fs::exists(p, ec)) return true;
    if (std::chrono::steady_clock::now() >= end) return false;
    std::this_thread::sleep_for(10ms);
  }
}

}  // namespace

TEST_CASE("a spool is deleted by the OS when its owner is killed and never reclaimed while its owner lives") {
  const Env env = make_env("spool-owner");
  { Store s = open_store(env); }
  const fs::path ready = env.dir / "spool.ready";
  Child child;
  spawn(child, child_args("hold-spool", env, JournalMode::wal, ready));
  REQUIRE(wait_for(ready));
  const fs::path live = reported_spool(ready, "holding");
  CHECK(fs::exists(live));
  // Neither a new spool (which reclaims leftovers first) nor writer recovery
  // touches a spool whose owner is alive.
  { ContributionSpool mine(env.dir); }
  CHECK(fs::exists(live));
  {
    Store s = open_store(env);
    auto lock = s.acquire_writer(2s);
    REQUIRE(lock);
    CHECK(fs::exists(live));
  }
  child.kill();
  CHECK(gone_within(live));

  // A leftover without an owner (as after a power loss) is reclaimed by the next
  // spool and by writer recovery; other files are left alone.
  const fs::path stale_a = env.dir / "lcm-spool-1-0.bin";
  const fs::path stale_b = env.dir / "lcm-spool-2-0.bin";
  const fs::path unrelated = env.dir / "lcm-spool-notes.txt";
  write_file(stale_a, "stale");
  write_file(unrelated, "keep");
  { ContributionSpool mine(env.dir); }
  CHECK_FALSE(fs::exists(stale_a));
  write_file(stale_b, "stale");
  {
    Store s = open_store(env);
    REQUIRE(s.acquire_writer(2s));
  }
  CHECK_FALSE(fs::exists(stale_b));
  CHECK(fs::exists(unrelated));
}

TEST_CASE("a writer killed inside a spooled publication leaves no spool and the old generation intact") {
  const Env env = make_env("spool-crash");
  std::string committed;
  {
    Store store = open_store(env);
    WriterLock lock = take(store);
    store.ensure_profile(lock, "default");
    REQUIRE(store.publish(lock, batch({shared_tu("A", "src/a.cpp", "cmd-a"), shared_tu("B", "src/b.cpp", "cmd-b")}))
                .outcome == PublishOutcome::committed);
    committed = hex(store.read().load_facts("default", Projection::all));
  }
  const fs::path ready = env.dir / "spool-crash.ready";
  Child child;
  spawn(child, child_args("crash-spooled-publish", env, JournalMode::wal, ready));
  REQUIRE(wait_for(ready));
  const fs::path spool = reported_spool(ready, "inside-transaction");
  CHECK(fs::exists(spool));
  child.kill();
  CHECK(gone_within(spool));
  Store store = open_store(env);
  CHECK(store.read().fact_generation("default") == 1);
  CHECK(hex(store.read().load_facts("default", Projection::all)) == committed);
  {
    auto lock = store.acquire_writer(2s);
    REQUIRE(lock);
  }
  CHECK(store.read().status("default").last_attempt->state == AttemptState::interrupted);
  for (const auto& e : fs::directory_iterator(env.dir)) {
    CHECK_MESSAGE(path_to_utf8_generic(e.path().filename()).rfind("lcm-spool-", 0) != 0, e.path().string());
  }
}

TEST_CASE("OS writer lock: exclusion in-process and across processes, release on termination") {
  const Env env = make_env("lock");
  Store first = open_store(env);
  Store second = open_store(env);
  {
    auto held = first.acquire_writer(1s);
    REQUIRE(held);
    CHECK_FALSE(second.acquire_writer(100ms).has_value());
    CHECK_FALSE(first.acquire_writer(0ms).has_value());  // one writer even within one store
  }
  CHECK(second.acquire_writer(1s).has_value());

  const fs::path ready = env.dir / "hold.ready";
  Child child;
  spawn(child, child_args("hold-lock", env, JournalMode::wal, ready));
  REQUIRE(wait_for(ready));
  CHECK(read_file(ready) == "holding");
  const auto t0 = std::chrono::steady_clock::now();
  CHECK_FALSE(first.acquire_writer(200ms).has_value());
  CHECK(std::chrono::steady_clock::now() - t0 < 5s);  // bounded busy, not a hang
  child.kill();
  CHECK(fs::exists(env.db.string() + ".writer.lock"));  // released by the OS, not by deleting a file
  CHECK(first.acquire_writer(2s).has_value());
}

TEST_CASE("a writer terminated inside the replacement transaction leaves the old generation intact") {
  for (const JournalMode mode : {JournalMode::wal, JournalMode::rollback}) {
    CAPTURE(to_string(mode));
    const Env env = make_env(std::string("crash-") + std::string(to_string(mode)));
    std::string committed;
    std::int64_t success_id = 0;
    {
      Store store = open_store(env, mode);
      MESSAGE("journal mode in effect: " << to_string(store.journal_mode()));
      REQUIRE(store.journal_mode() == mode);
      WriterLock lock = take(store);
      store.ensure_profile(lock, "default");
      const Contribution a = shared_tu("A", "src/a.cpp", "cmd-a");
      const Contribution b = shared_tu("B", "src/b.cpp", "cmd-b");
      const PublishResult r = store.publish(lock, batch({a, b}));
      REQUIRE(r.outcome == PublishOutcome::committed);
      success_id = r.attempt_id.value();
      committed = hex(store.read().load_facts("default", Projection::all));
    }
    const fs::path ready = env.dir / "crash.ready";
    Child child;
    spawn(child, child_args("crash-before-commit", env, mode, ready));
    REQUIRE(wait_for(ready));
    CHECK(read_file(ready) == "inside-transaction");
    child.kill();

    Store store = open_store(env, mode);
    {
      const ReadSnapshot snap = store.read();
      CHECK(snap.fact_generation("default") == 1);
      CHECK(hex(snap.load_facts("default", Projection::all)) == committed);
      CHECK(snap.unit("default", "B")->provenance.command_id == "cmd-b");
      CHECK(snap.status("default").last_attempt->state == AttemptState::in_progress);  // not yet recovered
    }
    auto lock = store.acquire_writer(2s);
    REQUIRE(lock);  // the terminated writer's lock was released by the OS
    const ProfileStatus st = store.read().status("default");
    REQUIRE(st.last_attempt);
    CHECK(st.last_attempt->state == AttemptState::interrupted);
    CHECK_FALSE(st.last_attempt->finished.has_value());  // no invented completion time
    CHECK(st.last_success->id == success_id);
    CHECK(st.failed_slots == std::vector<std::string>{"B"});
    CHECK(st.fact_generation == 1);
  }
}

TEST_CASE("readers stay on one committed snapshot while another process commits") {
  for (const JournalMode mode : {JournalMode::wal, JournalMode::rollback}) {
    CAPTURE(to_string(mode));
    const Env env = make_env(std::string("reader-") + std::string(to_string(mode)));
    Store store = open_store(env, mode);
    MESSAGE("journal mode in effect: " << to_string(store.journal_mode()));
    const Contribution a = shared_tu("A", "src/a.cpp", "cmd-a");
    const Contribution b = shared_tu("B", "src/b.cpp", "cmd-b");
    TuOptions v2;
    v2.body_seed = "body-2";
    const Contribution b2 = shared_tu("B", "src/b.cpp", "cmd-b-2", v2);  // what the child publishes
    {
      WriterLock lock = take(store);
      store.ensure_profile(lock, "default");
      REQUIRE(store.publish(lock, batch({a, b})).outcome == PublishOutcome::committed);
    }
    const std::string old_hash = hex(oracle({&a, &b}));
    const std::string new_hash = hex(oracle({&a, &b2}));
    const fs::path ready = env.dir / "publish.ready";
    {
      const ReadSnapshot snap = store.read();
      CHECK(snap.fact_generation("default") == 1);
      Child child;
      auto args = child_args("publish", env, mode, ready);
      args.push_back(mode == JournalMode::wal ? L"5000" : L"300");
      spawn(child, args);
      const DWORD code = child.wait(60000);
      REQUIRE(wait_for(ready));
      MESSAGE("child: " << read_file(ready));
      if (mode == JournalMode::wal) {
        CHECK(code == 0);  // committed while the reader was open
      } else {
        CHECK(code == 3);  // the open reader made the rollback-journal commit busy
        CHECK(read_file(ready).find("failed") == 0);
      }
      // The open snapshot never mixes: same generation, same facts, same provenance.
      CHECK(snap.fact_generation("default") == 1);
      CHECK(hex(snap.load_facts("default", Projection::all)) == old_hash);
      CHECK(snap.unit("default", "B")->provenance.command_id == "cmd-b");
      CHECK(snap.unit("default", "B")->committed_generation == 1);
    }
    if (mode == JournalMode::rollback) {
      fs::remove(ready);
      Child child;
      auto args = child_args("publish", env, mode, ready);
      args.push_back(L"5000");
      spawn(child, args);
      CHECK(child.wait(60000) == 0);
    }
    const ReadSnapshot fresh = store.read();
    CHECK(fresh.fact_generation("default") == 2);
    CHECK(hex(fresh.load_facts("default", Projection::all)) == new_hash);
    CHECK(fresh.unit("default", "B")->provenance.command_id == "cmd-b-2");
  }
}
#endif

// ---------------------------------------------------------------------------
// Dependency freshness.

namespace {

struct FreshEnv {
  Env env;
  fs::path external;  // outside the repository, inside the scratch tree

  fs::path at(const std::string& rel) const { return env.workspace / path_from_utf8(rel); }
};

InputObservation observe(const fs::path& file, const fs::path& root, InputRole role) {
  InputObservation o;
  o.path_utf8 = path_to_utf8_generic(file);
  if (auto rel = make_repo_relative(root, file)) o.repo_path = *rel;
  o.role = role;
  o.content_hash = hash_file_content(read_file(file));
  return o;
}

// Unit facts located in real files (content hashes are synthetic; only the
// inputs carry real hashes).
Contribution fresh_unit(const FreshEnv& fx, const std::string& slot, const std::string& source,
                        const std::vector<std::pair<std::string, InputRole>>& repo_inputs,
                        const std::vector<std::pair<fs::path, InputRole>>& other_inputs, BuildCurrency currency) {
  FactSet f;
  const StableId run = f.add_symbol_location(key("run_" + slot),
                                             SymbolLocation{LocationRole::definition, loc(source, 10), sha256(slot), slot, {}});
  for (const auto& [rel, role] : repo_inputs) {
    if (role == InputRole::source) continue;
    std::string name = rel.substr(rel.find_last_of('/') + 1);
    name = name.substr(0, name.find('.'));
    const StableId sym = f.add_symbol_location(key("decl_" + name),
                                               SymbolLocation{LocationRole::declaration, loc(rel, 3), std::nullopt, slot, {}});
    Evidence e;
    e.analysis_unit = slot;
    e.location = loc(source, 12);
    f.add_relation_evidence(RelationKey{run, sym, RelationKind::references, Confidence::confirmed}, e);
  }
  Contribution c = contribution(slot, slot, "cmd-" + slot, std::move(f));
  c.build_currency = currency;
  for (const auto& [rel, role] : repo_inputs) c.inputs.push_back(observe(fx.at(rel), fx.env.workspace, role));
  for (const auto& [path, role] : other_inputs) c.inputs.push_back(observe(path, fx.env.workspace, role));
  return c;
}

FreshEnv make_fresh_env(const std::string& name) {
  FreshEnv fx;
  fx.env = make_env(name);
  fx.external = fx.env.dir / "external";
  write_file(fx.at("src/a.cpp"), "int run_A() { return 1; }\n");
  write_file(fx.at("src/b.cpp"), "int run_B() { return 2; }\n");
  write_file(fx.at("include/shared.h"), "#include \"detail.inl\"\n");
  write_file(fx.at("include/detail.inl"), "inline int detail() { return 3; }\n");
  write_file(fx.at("include/solo.h"), "int solo();\n");
  write_file(fx.at("gen/gen.h"), "// generated\nint gen();\n");
  write_file(fx.at("include/excluded.h"), "int excluded();\n");
  write_file(fx.at("build/a.rsp"), "/DA=1\n");
  write_file(fx.at("build/compile_commands.json"), "[]\n");
  write_file(fx.external / "ext.h", "int ext();\n");
  return fx;
}

std::vector<Contribution> fresh_units(const FreshEnv& fx) {
  return {
      fresh_unit(fx, "A", "src/a.cpp",
                 {{"src/a.cpp", InputRole::source},
                  {"include/shared.h", InputRole::header},
                  {"include/detail.inl", InputRole::inline_file},
                  {"include/solo.h", InputRole::header},
                  {"gen/gen.h", InputRole::generated},
                  {"build/a.rsp", InputRole::response_file},
                  {"build/compile_commands.json", InputRole::build_database}},
                 {{fx.external / "ext.h", InputRole::external_dependency}}, BuildCurrency::verified),
      fresh_unit(fx, "B", "src/b.cpp",
                 {{"src/b.cpp", InputRole::source},
                  {"include/shared.h", InputRole::header},
                  {"include/detail.inl", InputRole::inline_file},
                  {"include/excluded.h", InputRole::ignored_dependency}},
                 {}, BuildCurrency::unknown),
  };
}

const UnitFreshness& unit_of(const FreshnessReport& r, const std::string& slot) {
  for (const auto& u : r.units) {
    if (u.slot == slot) return u;
  }
  FAIL("no unit " << slot);
  return r.units.front();
}

FreshnessReport inspect(Store& store, std::chrono::milliseconds budget = 2000ms) {
  InspectRequest req;
  req.profile = "default";
  req.budget = budget;
  return store.inspect_freshness(req);
}

}  // namespace

TEST_CASE("freshness: verified units become fresh; each dependency role invalidates the right units") {
  FreshEnv fx = make_fresh_env("fresh-roles");
  Store store = open_store(fx.env);
  {
    WriterLock lock = take(store);
    store.ensure_profile(lock, "default");
    REQUIRE(store.publish(lock, batch(fresh_units(fx))).outcome == PublishOutcome::committed);
  }
  FreshnessReport r = inspect(store);
  REQUIRE(r.outcome == InspectionOutcome::complete);
  CHECK(r.published);
  CHECK(r.inputs_total == 10);
  CHECK(r.changes.empty());
  CHECK(unit_of(r, "A").semantic == Freshness::fresh);
  CHECK(unit_of(r, "A").compile_context == Freshness::fresh);
  CHECK_FALSE(unit_of(r, "A").uht.has_value());
  // B's inputs match, but nobody verified its build action: never promoted.
  CHECK(unit_of(r, "B").semantic == Freshness::unknown);
  CHECK(unit_of(r, "B").compile_context == Freshness::unknown);
  CHECK_FALSE(r.guidance.empty());
  {
    const ReadSnapshot snap = store.read();
    CHECK(snap.unit("default", "A")->freshness.semantic == Freshness::fresh);
    CHECK(snap.unit("default", "A")->freshness.checked_at.has_value());
    const ProfileStatus st = snap.status("default");
    CHECK(layer(st, Layer::semantic_graph).freshness == Freshness::unknown);
    CHECK(summarize_layer(layer(st, Layer::semantic_graph)) == UserStatus::stale);
  }

  const auto mutate = [&](const fs::path& p, const std::string& text, std::set<std::string> stale_slots,
                          bool context_stale_for_a) {
    const std::string original = read_file(p);
    write_file(p, text);
    const FreshnessReport m = inspect(store);
    CAPTURE(path_to_utf8_generic(p));
    REQUIRE(m.outcome == InspectionOutcome::complete);
    for (const char* slot : {"A", "B"}) {
      const bool expect_stale = stale_slots.count(slot) != 0;
      CHECK_MESSAGE((unit_of(m, slot).semantic == Freshness::stale) == expect_stale, slot);
    }
    CHECK((unit_of(m, "A").compile_context == Freshness::stale) == context_stale_for_a);
    REQUIRE(m.changes.size() == 1);
    CHECK(m.changes.front().kind == ChangeKind::modified);
    CHECK(std::set<std::string>(m.changes.front().slots.begin(), m.changes.front().slots.end()) == stale_slots);
    write_file(p, original);
    const FreshnessReport back = inspect(store);
    CHECK(unit_of(back, "A").semantic == Freshness::fresh);
  };
  mutate(fx.at("src/a.cpp"), "int run_A() { return 10; }\n", {"A"}, false);
  mutate(fx.at("include/detail.inl"), "inline int detail() { return 4; }\n", {"A", "B"}, false);
  mutate(fx.at("include/shared.h"), "#include \"detail.inl\" // edit\n", {"A", "B"}, false);
  mutate(fx.at("gen/gen.h"), "// generated v2\nint gen();\n", {"A"}, false);
  mutate(fx.at("include/excluded.h"), "int excluded(int);\n", {"B"}, false);
  mutate(fx.external / "ext.h", "int ext(int);\n", {"A"}, false);
  mutate(fx.at("build/a.rsp"), "/DA=2\n", {"A"}, true);
  mutate(fx.at("build/compile_commands.json"), "[ ]\n", {"A"}, true);
}

TEST_CASE("freshness: deletion and exclusion withhold public facts immediately; dependency metadata survives") {
  FreshEnv fx = make_fresh_env("fresh-visibility");
  Store store = open_store(fx.env);
  WriterLock lock = take(store);
  store.ensure_profile(lock, "default");
  REQUIRE(store.publish(lock, batch(fresh_units(fx))).outcome == PublishOutcome::committed);
  const auto public_has = [&](const std::string& file) {
    const FactSet f = store.read().load_facts("default", Projection::public_);
    for (const auto& [id, s] : f.symbols()) {
      for (const auto& l : s.locations) {
        if (l.location.file.generic == file) return true;
      }
    }
    return false;
  };
  CHECK(public_has("include/solo.h"));
  const std::size_t all_symbols = store.read().load_facts("default", Projection::all).symbols().size();

  // Deleted: withheld with no inspection at all.
  fs::remove(fx.at("include/solo.h"));
  CHECK_FALSE(public_has("include/solo.h"));
  {
    const FactSet pub = store.read().load_facts("default", Projection::public_);
    // Owned only by the deleted file: withheld with its incident reference.
    CHECK(by_name(pub, "decl_solo") == nullptr);
    const StableId solo_id = make_stable_id(key("decl_solo"));
    for (const auto& [k, r] : pub.relations()) CHECK((k.source != solo_id && k.target != solo_id));
    CHECK(by_name(pub, "run_A") != nullptr);
    CHECK(by_name(pub, "decl_shared") != nullptr);
    CHECK(store.read().load_facts("default", Projection::all).symbols().size() == all_symbols);
  }
  // ...and independently of how an inspection ends.
  {
    InspectRequest req;
    req.profile = "default";
    req.lock_wait = 20ms;
    const FreshnessReport busy = store.inspect_freshness(req);  // we hold the writer lock
    CHECK(busy.outcome == InspectionOutcome::lock_busy);
    CHECK_FALSE(busy.published);
    CHECK_FALSE(busy.persisted_downgrade);
    CHECK_FALSE(public_has("include/solo.h"));
    bool deletion_reported = false;
    for (const auto& c : busy.changes) deletion_reported |= c.kind == ChangeKind::deleted;
    CHECK(deletion_reported);
    CHECK(unit_of(busy, "A").semantic == Freshness::stale);
  }
  CHECK(inspect(store, 0ms).outcome == InspectionOutcome::budget_exhausted);
  CHECK_FALSE(public_has("include/solo.h"));

  // Newly excluded file: withheld from the public projection, kept for invalidation.
  store.set_excluded_files(lock, "default", {repo("include/shared.h")});
  CHECK_FALSE(public_has("include/shared.h"));
  CHECK(public_has("src/a.cpp"));
  const auto a = store.read().unit("default", "A");
  CHECK(std::any_of(a->inputs.begin(), a->inputs.end(), [](const InputObservation& o) {
    return o.repo_path && o.repo_path->generic == "include/shared.h";
  }));
  const auto withheld = store.read().withheld_files("default");
  std::set<std::string> names;
  for (const auto& w : withheld) names.insert(w.generic);
  CHECK(names == std::set<std::string>{"include/shared.h", "include/solo.h"});
  CHECK_THROWS_AS(store.set_excluded_files(lock, "default", {repo("../outside.h")}), std::invalid_argument);
}

TEST_CASE("freshness: changes during the check, lock contention and budget ends are never promoted to fresh") {
  FreshEnv fx = make_fresh_env("fresh-races");
  Store store = open_store(fx.env);
  {
    WriterLock lock = take(store);
    store.ensure_profile(lock, "default");
    REQUIRE(store.publish(lock, batch(fresh_units(fx))).outcome == PublishOutcome::committed);
  }
  REQUIRE(unit_of(inspect(store), "A").semantic == Freshness::fresh);
  const fs::path shared = fx.at("include/shared.h");
  const std::string original = read_file(shared);
  const auto persisted_a = [&] { return store.read().unit("default", "A")->freshness; };

  SUBCASE("edit between the unlocked pass and the locked recheck") {
    InspectRequest req;
    req.profile = "default";
    req.after_first_pass = [&] { write_file(shared, "// edited while checking\n"); };
    const FreshnessReport r = store.inspect_freshness(req);
    CHECK(r.outcome == InspectionOutcome::complete);
    CHECK(unit_of(r, "A").semantic == Freshness::stale);
    CHECK(unit_of(r, "B").semantic == Freshness::stale);
    CHECK(persisted_a().semantic == Freshness::stale);
  }
  SUBCASE("a change that is undone during the check still is not fresh") {
    write_file(shared, "// transient\n");
    InspectRequest req;
    req.profile = "default";
    req.after_first_pass = [&] { write_file(shared, original); };
    const FreshnessReport r = store.inspect_freshness(req);
    CHECK(r.outcome == InspectionOutcome::complete);
    CHECK(unit_of(r, "A").semantic == Freshness::stale);
    bool during = false;
    for (const auto& c : r.changes) during |= c.kind == ChangeKind::changed_during_check;
    CHECK(during);
  }
  SUBCASE("lock held by another writer: bounded, unpublished, and not fresh") {
    Store other = open_store(fx.env);
    auto held = other.acquire_writer(1s);
    REQUIRE(held);
    InspectRequest req;
    req.profile = "default";
    req.lock_wait = 50ms;
    const FreshnessReport r = store.inspect_freshness(req);
    CHECK(r.outcome == InspectionOutcome::lock_busy);
    CHECK_FALSE(r.published);
    CHECK_FALSE(r.persisted_downgrade);
    for (const auto& u : r.units) {
      CHECK(u.semantic != Freshness::fresh);
      CHECK_FALSE(u.checked_at.has_value());  // nothing in this report is a new verification
    }
    CHECK_FALSE(r.guidance.empty());
  }
  SUBCASE("budget exhausted on the final input of the unlocked pass downgrades the persisted state") {
    write_file(shared, "// changed\n");
    InspectRequest req;
    req.profile = "default";
    req.budget = 300ms;
    std::string last;
    for (const auto& unit : store.read().units("default")) {
      for (const auto& in : unit.inputs) last = std::max(last, path_key(path_from_utf8(in.path_utf8)));
    }
    REQUIRE_FALSE(last.empty());
    req.before_probe = [&](int pass, const std::string& path) {
      if (pass == 1 && path_key(path_from_utf8(path)) == last) std::this_thread::sleep_for(400ms);
    };
    const FreshnessReport r = store.inspect_freshness(req);
    CHECK(r.outcome == InspectionOutcome::budget_exhausted);
    CHECK(r.inputs_checked == r.inputs_total - 1);
    CHECK_FALSE(r.published);
    CHECK(r.persisted_downgrade);
    for (const auto& u : r.units) CHECK(u.semantic != Freshness::fresh);
    CHECK(persisted_a().semantic != Freshness::fresh);
    CHECK(persisted_a().compile_context != Freshness::fresh);
    CHECK_FALSE(r.guidance.empty());
  }
  SUBCASE("budget exhausted on the final input of the locked recheck") {
    InspectRequest req;
    req.profile = "default";
    req.budget = 400ms;
    int pass2 = 0;
    req.before_probe = [&](int pass, const std::string&) {
      if (pass == 2 && ++pass2 == 10) std::this_thread::sleep_for(500ms);
    };
    const FreshnessReport r = store.inspect_freshness(req);
    CHECK(r.outcome == InspectionOutcome::budget_exhausted);
    CHECK_FALSE(r.published);
    CHECK(r.persisted_downgrade);
    CHECK(persisted_a().semantic == Freshness::unknown);
    for (const auto& u : r.units) CHECK(u.semantic != Freshness::fresh);
  }
  SUBCASE("budget exhausted after the locked recheck, before publication") {
    InspectRequest req;
    req.profile = "default";
    req.budget = 300ms;
    req.before_publish = [] { std::this_thread::sleep_for(400ms); };
    const FreshnessReport r = store.inspect_freshness(req);
    CHECK(r.outcome == InspectionOutcome::budget_exhausted);
    CHECK_FALSE(r.published);
    CHECK(r.persisted_downgrade);
    CHECK(persisted_a().semantic == Freshness::unknown);
  }
  write_file(shared, original);
}

TEST_CASE("freshness: a large header fan-out returns stale within the two-second budget") {
  FreshEnv fx = make_fresh_env("fresh-fanout");
  Store store = open_store(fx.env);
  constexpr int kUnits = 3000;
  for (int i = 0; i < 10; ++i) write_file(fx.at("src/s" + std::to_string(i) + ".cpp"), "int s" + std::to_string(i) + ";\n");
  {
    WriterLock lock = take(store);
    store.ensure_profile(lock, "default");
    std::vector<Contribution> units;
    for (int i = 0; i < kUnits; ++i) {
      const std::string slot = "u" + std::to_string(i);
      const std::string source = "src/s" + std::to_string(i % 10) + ".cpp";
      FactSet f;
      f.add_symbol_location(key("fn_" + slot), SymbolLocation{LocationRole::definition, loc(source, 10), sha256(slot), slot, {}});
      Contribution c = contribution(slot, slot, "cmd-" + slot, std::move(f));
      c.build_currency = BuildCurrency::verified;
      c.inputs.push_back(observe(fx.at(source), fx.env.workspace, InputRole::source));
      c.inputs.push_back(observe(fx.at("include/shared.h"), fx.env.workspace, InputRole::header));
      units.push_back(std::move(c));
    }
    const auto t0 = std::chrono::steady_clock::now();
    REQUIRE(store.publish(lock, batch(std::move(units))).outcome == PublishOutcome::committed);
    MESSAGE("published " << kUnits << " units in "
                         << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count()
                         << " ms");
  }
  write_file(fx.at("include/shared.h"), "// fan-out edit\n");
  const auto t0 = std::chrono::steady_clock::now();
  const FreshnessReport r = inspect(store, 2000ms);
  const auto wall = std::chrono::steady_clock::now() - t0;
  MESSAGE("fan-out inspection: " << to_string(r.outcome) << " in " << r.elapsed.count() << " ms");
  CHECK(wall < 2000ms);
  CHECK(r.outcome == InspectionOutcome::complete);
  CHECK(r.units.size() == kUnits);
  for (const auto& u : r.units) CHECK(u.semantic == Freshness::stale);
  REQUIRE(r.changes.size() == 1);
  CHECK(r.changes.front().slots.size() == kUnits);
  CHECK(r.guidance.find("livecodemap build") != std::string::npos);
}

TEST_CASE("public projection: symbols owned only by excluded or deleted files leak no key, name, path or edge") {
  FreshEnv fx = make_fresh_env("public-withhold");
  write_file(fx.at("include/hidden.h"), "int hidden_fn();\nstatic int counter;\n");
  const auto hidden_h = repo("include/hidden.h");

  // Unit A: a visible caller referencing hidden-only symbols, plus a shared
  // symbol declared in the hidden header and defined in a visible file.
  const auto unit = [&](const std::string& slot, const std::string& source) {
    FactSet f;
    const StableId run = f.add_symbol_location(key("run_" + slot),
                                               SymbolLocation{LocationRole::definition, loc(source, 10), sha256(slot), slot, {}});
    const StableId hidden_fn =
        f.add_symbol_location(key("hidden_fn"), SymbolLocation{LocationRole::declaration, loc(hidden_h, 1), std::nullopt, slot, {}});
    CanonicalKey counter = key("hidden_counter", SymbolKind::variable, "");
    counter.linkage = InternalLinkage{hidden_h};
    const StableId counter_id =
        f.add_symbol_location(counter, SymbolLocation{LocationRole::definition, loc(hidden_h, 20), std::nullopt, slot, {}});
    // Internal-linkage identity scoped to the hidden file although its location is visible.
    CanonicalKey scoped = key("scoped_helper", SymbolKind::function, "()");
    scoped.linkage = InternalLinkage{hidden_h};
    const StableId scoped_id =
        f.add_symbol_location(scoped, SymbolLocation{LocationRole::definition, loc(source, 30), sha256("s"), slot, {}});
    CanonicalKey lambda = key("", SymbolKind::lambda, "() const", {[] {
                                OwnerComponent o;
                                o.kind = SymbolKind::function;
                                o.name = "hidden_fn";
                                o.normalized_signature = "()";
                                return o;
                              }()});
    lambda.linkage = LocalScope{hidden_fn, "lambda", 0};
    const StableId lambda_id =
        f.add_symbol_location(lambda, SymbolLocation{LocationRole::definition, loc(source, 40), sha256("l"), slot, {}});
    const StableId shared = f.add_symbol_location(
        key("shared_fn"), SymbolLocation{LocationRole::declaration, loc(hidden_h, 5), std::nullopt, slot, {}});
    f.add_symbol_location(key("shared_fn"), SymbolLocation{LocationRole::definition, loc(source, 50), sha256("d"), slot, {}});
    Evidence e;
    e.analysis_unit = slot;
    e.location = loc(source, 12);
    for (const StableId& target : {hidden_fn, counter_id, scoped_id, shared, lambda_id}) {
      f.add_relation_evidence(RelationKey{run, target, RelationKind::references, Confidence::confirmed}, e);
    }
    UnresolvedSite site;
    site.enclosing = hidden_fn;
    site.expression = "callback()";
    site.location = loc(source, 14);
    site.reason = UnresolvedReason::indirect_callee;
    site.analysis_unit = slot;
    f.add_unresolved_site(site);
    CaptureFact cap;
    cap.lambda = lambda_id;
    cap.name = "x";
    cap.location = loc(source, 41);
    cap.analysis_unit = slot;
    f.add_capture(cap);
    return contribution(slot, slot, "cmd-" + slot, std::move(f));
  };
  const Contribution a = unit("A", "src/a.cpp");
  const Contribution b = unit("B", "src/b.cpp");
  Store store = open_store(fx.env);
  WriterLock lock = take(store);
  store.ensure_profile(lock, "default");
  REQUIRE(store.publish(lock, batch({a, b})).outcome == PublishOutcome::committed);
  const std::string all_hash = hex(oracle({&a, &b}));
  CHECK(hex(store.read().load_facts("default", Projection::public_)) == all_hash);  // nothing withheld yet

  const std::set<std::string> leaked_names{"hidden_fn", "hidden_counter", "scoped_helper", ""};
  const auto check_withheld = [&] {
    const FactSet pub = store.read().load_facts("default", Projection::public_);
    std::set<StableId> gone;
    for (const auto& [id, s] : a.facts.symbols()) {
      if (leaked_names.count(s.key.canonical_name)) gone.insert(id);
    }
    REQUIRE(gone.size() == 4);
    for (const auto& [id, s] : pub.symbols()) {
      CHECK_MESSAGE(gone.count(id) == 0, s.key.canonical_name);
      CHECK(serialize_canonical_key(s.key).find("include/hidden.h") == std::string::npos);
      for (const auto& l : s.locations) CHECK(l.location.file.generic != "include/hidden.h");
    }
    for (const auto& [k, r] : pub.relations()) CHECK((gone.count(k.source) == 0 && gone.count(k.target) == 0));
    CHECK(pub.unresolved_sites().empty());
    CHECK(pub.captures().empty());
    // Still-visible ownership survives: callers, and the shared symbol's visible definitions.
    const Symbol* shared = by_name(pub, "shared_fn");
    REQUIRE(shared);
    CHECK(shared->presence == SymbolPresence::indexed);
    CHECK(shared->locations.size() == 2);  // one definition per unit, no hidden declaration
    for (const char* run : {"run_A", "run_B"}) {
      const Symbol* caller = by_name(pub, run);
      REQUIRE(caller);
      CHECK(pub.find_relation(RelationKey{caller->id, shared->id, RelationKind::references, Confidence::confirmed}) != nullptr);
    }
    // Internal projection and dependency metadata are unchanged.
    CHECK(hex(store.read().load_facts("default", Projection::all)) == all_hash);
  };

  store.set_excluded_files(lock, "default", {hidden_h});
  check_withheld();
  store.set_excluded_files(lock, "default", {});
  CHECK(hex(store.read().load_facts("default", Projection::public_)) == all_hash);
  fs::remove(fx.at("include/hidden.h"));
  check_withheld();
}

TEST_CASE("public projection: a withheld-only identity does not resurface after its owners are replaced or retired") {
  for (const JournalMode mode : {JournalMode::wal, JournalMode::rollback}) {
    for (const bool deletion : {false, true}) {
      CAPTURE(to_string(mode));
      CAPTURE(deletion);
      FreshEnv fx = make_fresh_env(std::string("resurface-") + std::string(to_string(mode)) + (deletion ? "-del" : "-excl"));
      const auto hidden_h = repo("include/excluded.h");
      const CanonicalKey hidden_key = key("hidden_api");
      const StableId hidden_id = make_stable_id(hidden_key);
      const StableId caller_id = make_stable_id(key("caller"));
      const StableId ext_id = make_stable_id(key("api", SymbolKind::function, "()", {ns("ext")}));

      // A: visible caller that only names hidden_api (no location), plus an
      // external placeholder it calls (control: must stay public).
      FactSet fa;
      fa.add_symbol_location(key("caller"), SymbolLocation{LocationRole::definition, loc("src/a.cpp", 10), sha256("c"), "A", {}});
      fa.add_endpoint_only(hidden_key);
      fa.add_external_placeholder(key("api", SymbolKind::function, "()", {ns("ext")}));
      Evidence call;
      call.analysis_unit = "A";
      call.location = loc("src/a.cpp", 12);
      fa.add_relation_evidence(RelationKey{caller_id, hidden_id, RelationKind::calls, Confidence::confirmed}, call);
      fa.add_relation_evidence(RelationKey{caller_id, ext_id, RelationKind::calls, Confidence::confirmed}, call);
      const Contribution a = contribution("A", "A", "cmd-a", fa);
      // B: owns hidden_api in the file that becomes withheld. C: a visible owner.
      FactSet fb;
      fb.add_symbol_location(hidden_key, SymbolLocation{LocationRole::definition, loc(hidden_h, 5), sha256("h"), "B", {}});
      const Contribution b = contribution("B", "B", "cmd-b", fb);
      FactSet fc;
      fc.add_symbol_location(hidden_key, SymbolLocation{LocationRole::definition, loc("src/b.cpp", 5), sha256("h"), "C", {}});
      const Contribution c = contribution("C", "C", "cmd-c", fc);
      FactSet fc2;  // C's replacement no longer defines hidden_api
      fc2.add_symbol_location(key("other_c"), SymbolLocation{LocationRole::definition, loc("src/b.cpp", 7), sha256("o"), "C", {}});
      const Contribution c2 = contribution("C", "C", "cmd-c-2", fc2);

      Store store = open_store(fx.env, mode);
      REQUIRE(store.journal_mode() == mode);
      {
        WriterLock lock = take(store);
        store.ensure_profile(lock, "default");
        REQUIRE(store.publish(lock, batch({a, b, c})).outcome == PublishOutcome::committed);
        if (deletion) {
          fs::remove(fx.at("include/excluded.h"));
        } else {
          store.set_excluded_files(lock, "default", {hidden_h});
        }
      }
      const RelationKey edge{caller_id, hidden_id, RelationKind::calls, Confidence::confirmed};
      const RelationKey ext_edge{caller_id, ext_id, RelationKind::calls, Confidence::confirmed};
      {
        // Genuinely visible ownership (C) keeps the identity and its edge public.
        const FactSet pub = store.read().load_facts("default", Projection::public_);
        const Symbol* h = pub.find_symbol(hidden_id);
        REQUIRE(h);
        REQUIRE(h->locations.size() == 1);
        CHECK(h->locations.front().location.file.generic == "src/b.cpp");
        CHECK(pub.find_relation(edge) != nullptr);
      }
      const auto withheld_publicly = [&](Store& s, SymbolPresence internal_presence) {
        const FactSet pub = s.read().load_facts("default", Projection::public_);
        CHECK(pub.find_symbol(hidden_id) == nullptr);
        CHECK(pub.find_relation(edge) == nullptr);
        for (const auto& [id, sym] : pub.symbols()) CHECK(sym.key.canonical_name != "hidden_api");
        CHECK(pub.find_symbol(caller_id) != nullptr);
        CHECK(pub.find_relation(ext_edge) != nullptr);  // placeholder control stays public
        REQUIRE(pub.find_symbol(ext_id));
        CHECK(pub.find_symbol(ext_id)->presence == SymbolPresence::external_placeholder);
        const FactSet all = s.read().load_facts("default", Projection::all);
        const Symbol* internal = all.find_symbol(hidden_id);
        REQUIRE(internal);  // internal survivor semantics are unchanged
        CHECK(internal->presence == internal_presence);
        CHECK(all.find_relation(edge) != nullptr);
      };
      {
        WriterLock lock = take(store);
        // Replacement removes the only visible owner.
        REQUIRE(store.publish(lock, batch({c2})).outcome == PublishOutcome::committed);
        withheld_publicly(store, SymbolPresence::indexed);
        CHECK(by_name(store.read().load_facts("default", Projection::public_), "other_c") != nullptr);
        // Retiring the withheld owner leaves a location-less endpoint.
        REQUIRE(store.publish(lock, batch({}, {"B"})).outcome == PublishOutcome::committed);
        withheld_publicly(store, SymbolPresence::endpoint_only);
      }
      Store reopened = open_store(fx.env, mode);
      withheld_publicly(reopened, SymbolPresence::endpoint_only);
      if (deletion) {
        // Even with the file restored, nothing committed proves a visible origin any more.
        write_file(fx.at("include/excluded.h"), "int hidden_api();\n");
      } else {
        WriterLock lock = take(reopened);
        reopened.set_excluded_files(lock, "default", {});
      }
      withheld_publicly(reopened, SymbolPresence::endpoint_only);
    }
  }
}

TEST_CASE("freshness: UHT context stays unknown unless verified, and reflected inputs make it stale") {
  FreshEnv fx = make_fresh_env("fresh-uht");
  write_file(fx.at("include/actor.h"), "UCLASS() class AActor {};\n");
  Store store = open_store(fx.env);
  {
    WriterLock lock = take(store);
    store.ensure_profile(lock, "default", true);
    Contribution c = fresh_unit(fx, "A", "src/a.cpp",
                                {{"src/a.cpp", InputRole::source}, {"include/actor.h", InputRole::reflected_header}}, {},
                                BuildCurrency::verified);
    REQUIRE(store.publish(lock, batch({c})).outcome == PublishOutcome::committed);
  }
  const FreshnessReport r = inspect(store);
  REQUIRE(r.outcome == InspectionOutcome::complete);
  CHECK(unit_of(r, "A").semantic == Freshness::fresh);
  REQUIRE(unit_of(r, "A").uht.has_value());
  CHECK(*unit_of(r, "A").uht == Freshness::unknown);
  {
    const ProfileStatus st = store.read().status("default");
    CHECK(layer(st, Layer::uht_context).applicable);
    CHECK(layer(st, Layer::uht_context).freshness == Freshness::unknown);
    CHECK(layer(st, Layer::uht_context).availability == Availability::missing);
  }
  write_file(fx.at("include/actor.h"), "UCLASS() class AActor { int x; };\n");
  const FreshnessReport m = inspect(store);
  CHECK(*unit_of(m, "A").uht == Freshness::stale);
  CHECK(unit_of(m, "A").semantic == Freshness::stale);
  CHECK(layer(store.read().status("default"), Layer::uht_context).freshness == Freshness::stale);
}

namespace {

// A contribution exercising every Contribution field.
Contribution full_contribution(const Env& env, const std::string& slot) {
  TuOptions full;
  full.with_conditional_include = true;
  full.with_capture = true;
  Contribution c = shared_tu(slot, "src/" + slot + ".cpp", "cmd-" + slot, full);
  c.facts.add_endpoint_only(key("only_endpoint_" + slot, SymbolKind::function, "()"));
  c.provenance.context_id = "ctx:" + sha256("args").hex();
  c.provenance.pch_producer_command_id = "cmd-pch";
  c.provenance.pch_producer_replay_id = "replay-pch";
  c.settings = AnalysisSettings{"clang 22", "D:/sdk/lib/clang/22", "repo", "accepted_only"};
  c.analysis_status = "completed_with_errors";
  c.build_currency = BuildCurrency::verified;
  c.inputs.push_back(InputObservation{path_to_utf8_generic(env.workspace / ("src/" + slot + ".cpp")),
                                      repo("src/" + slot + ".cpp", "src/" + slot + ".CPP"), InputRole::source,
                                      hash_file_content(slot), false});
  c.inputs.push_back(InputObservation{path_to_utf8_generic(env.dir / "sdk/ext.h"), std::nullopt,
                                      InputRole::external_dependency, hash_file_content("ext"), true});
  c.includes.push_back(IncludeEdge{std::nullopt, "pch.h", false, path_to_utf8_generic(env.dir / "pch.h"), 0});
  c.includes.push_back(IncludeEdge{path_to_utf8_generic(env.workspace / "src/a.cpp"), "shared.h", true, std::nullopt, 3});
  c.diagnostics.push_back(UnitDiagnostic{"error", "use of undeclared identifier", std::string("src/a.cpp"), 3, 7});
  c.diagnostics.push_back(UnitDiagnostic{"note", "no file", std::nullopt, 0, 0});
  c.limits = {"limit one", "limit two"};
  return c;
}

void check_same(const Contribution& got, const Contribution& want) {
  CHECK(got.slot == want.slot);
  CHECK(got.fact_label == want.fact_label);
  CHECK(got.provenance == want.provenance);
  CHECK(got.settings == want.settings);
  CHECK(got.analysis_status == want.analysis_status);
  CHECK(got.build_currency == want.build_currency);
  CHECK(hex(got.facts) == hex(want.facts));
  CHECK(written_dump(got.facts) == written_dump(want.facts));
  REQUIRE(got.facts.symbols().size() == want.facts.symbols().size());
  for (const auto& [id, s] : want.facts.symbols()) {
    const Symbol* g = got.facts.find_symbol(id);
    REQUIRE(g);
    CHECK(g->key == s.key);
    CHECK(g->presence == s.presence);
    CHECK(g->locations == s.locations);
  }
  CHECK(got.facts.direct_includes() == want.facts.direct_includes());
  CHECK(got.facts.captures() == want.facts.captures());
  CHECK(got.facts.unresolved_sites() == want.facts.unresolved_sites());
  REQUIRE(got.inputs.size() == want.inputs.size());
  for (std::size_t i = 0; i < want.inputs.size(); ++i) {
    CHECK(got.inputs[i].path_utf8 == want.inputs[i].path_utf8);
    CHECK(got.inputs[i].role == want.inputs[i].role);
    CHECK(got.inputs[i].content_hash == want.inputs[i].content_hash);
    CHECK(got.inputs[i].is_system == want.inputs[i].is_system);
    REQUIRE(got.inputs[i].repo_path.has_value() == want.inputs[i].repo_path.has_value());
    if (want.inputs[i].repo_path) {
      CHECK(got.inputs[i].repo_path->generic == want.inputs[i].repo_path->generic);
      CHECK(got.inputs[i].repo_path->as_written == want.inputs[i].repo_path->as_written);
    }
  }
  CHECK(got.includes == want.includes);
  CHECK(got.diagnostics == want.diagnostics);
  CHECK(got.limits == want.limits);
}

}  // namespace

TEST_CASE("contribution codec: every field, display spellings included, survives; damaged bytes are refused") {
  const Env env = make_env("codec");
  const Contribution c = full_contribution(env, "A");
  const std::string bytes = encode_contribution(c);
  check_same(decode_contribution(bytes), c);
  CHECK(encode_contribution(decode_contribution(bytes)) == bytes);  // deterministic
  CHECK_THROWS_AS((void)decode_contribution(bytes.substr(0, bytes.size() - 1)), StoreError);
  CHECK_THROWS_AS((void)decode_contribution(bytes + "x"), StoreError);
  CHECK_THROWS_AS((void)decode_contribution("not a contribution"), StoreError);
  std::string flipped = bytes;
  flipped[flipped.size() / 2] = static_cast<char>(flipped[flipped.size() / 2] ^ 0x5a);
  bool refused_or_different = false;
  try {
    refused_or_different = encode_contribution(decode_contribution(flipped)) != bytes;
  } catch (const StoreError&) {
    refused_or_different = true;
  }
  CHECK(refused_or_different);
}

TEST_CASE("contribution spool: validated on add, exclusive while it lives, removed afterwards") {
  const Env env = make_env("spool");
  fs::path file;
  {
    ContributionSpool spool(env.dir);
    file = spool.path();
#ifdef _WIN32
    CHECK(fs::exists(file));  // listed while it lives; POSIX unlinks the name at once
#endif
    const Contribution a = full_contribution(env, "A");
    const Contribution b = full_contribution(env, "B");
    std::size_t ea = 99;
    std::size_t eb = 99;
    CHECK(spool.add(a, &ea).empty());
    CHECK(spool.add(b, &eb).empty());
    CHECK(ea == 0);
    CHECK(eb == 1);
    CHECK(spool.size() == 2);
    CHECK(spool.slot(1) == "B");
    CHECK(spool.fact_label(0) == "A");
    check_same(spool.load(1), b);
    check_same(spool.load(0), a);
    // A contribution publish() would reject is refused and not stored.
    Contribution foreign = full_contribution(env, "C");
    foreign.fact_label = "someone-else";
    const std::string why = spool.add(foreign);
    CHECK(why.find("owned by") != std::string::npos);
    CHECK(spool.size() == 2);
    // Nobody else can open the spool while it lives, so its bytes cannot be
    // changed behind its back (load() still checks their digest).
    {
      std::fstream raw(file, std::ios::binary | std::ios::in | std::ios::out);
      CHECK_FALSE(raw.is_open());
    }
    check_same(spool.load(1), b);
  }
  CHECK_FALSE(fs::exists(file));
}

TEST_CASE("publication from a spool equals publication of the same contributions in memory") {
  const Env direct_env = make_env("spool-direct");
  const Env spooled_env = make_env("spool-streamed");
  std::vector<Contribution> contributions{full_contribution(direct_env, "A"), full_contribution(direct_env, "B"),
                                          shared_tu("C", "src/c.cpp", "cmd-c")};
  {
    Store store = open_store(direct_env);
    WriterLock lock = take(store);
    store.ensure_profile(lock, "default");
    REQUIRE(store.publish(lock, batch(contributions)).outcome == PublishOutcome::committed);
  }
  Store store = open_store(spooled_env);
  WriterLock lock = take(store);
  store.ensure_profile(lock, "default");
  ContributionSpool spool(spooled_env.dir);
  for (const auto& c : contributions) REQUIRE(spool.add(c).empty());
  Batch b = batch({});
  b.spool = &spool;
  b.spooled = {2, 0, 1};  // the given order is the install order
  const PublishResult r = store.publish(lock, b);
  REQUIRE_MESSAGE(r.outcome == PublishOutcome::committed, r.message);
  CHECK(r.fact_generation == 1);
  Store direct = open_store(direct_env);
  CHECK(hex(store.read().load_facts("default", Projection::all)) ==
        hex(direct.read().load_facts("default", Projection::all)));
  CHECK(written_dump(store.read().load_facts("default", Projection::all)) ==
        written_dump(direct.read().load_facts("default", Projection::all)));
  for (const char* slot : {"A", "B", "C"}) {
    CAPTURE(slot);
    const auto x = store.read().unit("default", slot);
    const auto y = direct.read().unit("default", slot);
    REQUIRE(x);
    REQUIRE(y);
    CHECK(x->provenance == y->provenance);
    CHECK(x->includes == y->includes);
    CHECK(x->diagnostics == y->diagnostics);
    CHECK(x->limits == y->limits);
    REQUIRE(x->inputs.size() == y->inputs.size());
    for (std::size_t i = 0; i < x->inputs.size(); ++i) CHECK(x->inputs[i].content_hash == y->inputs[i].content_hash);
    for (std::size_t i = 0; i < x->fingerprints.size(); ++i) CHECK(x->fingerprints[i].value == y->fingerprints[i].value);
  }

  // Batch rules hold for spooled installs: a slot twice is refused before
  // anything changes; a failure inside the transaction rolls everything back.
  const std::int64_t gen = store.read().fact_generation("default");
  Batch twice = batch({full_contribution(spooled_env, "A")});
  twice.spool = &spool;
  twice.spooled = {0};
  CHECK(store.publish(lock, twice).outcome == PublishOutcome::rejected);
  Batch unknown = batch({});
  unknown.spool = &spool;
  unknown.spooled = {7};
  CHECK(store.publish(lock, unknown).outcome == PublishOutcome::rejected);
  Batch again = batch({});
  again.spool = &spool;
  again.spooled = {0, 1};
  const PublishResult failed = store.publish(lock, again, [](PublishPhase p) {
    if (p == PublishPhase::before_commit) throw std::runtime_error("injected");
  });
  CHECK(failed.outcome == PublishOutcome::failed);
  CHECK(store.read().fact_generation("default") == gen);
  CHECK(hex(store.read().load_facts("default", Projection::all)) ==
        hex(direct.read().load_facts("default", Projection::all)));
}

TEST_CASE("unit detail levels read the same records, set-based") {
  const Env env = make_env("unit-detail");
  Store store = open_store(env);
  WriterLock lock = take(store);
  store.ensure_profile(lock, "default");
  REQUIRE(store.publish(lock, batch({full_contribution(env, "A"), full_contribution(env, "B")})).outcome ==
          PublishOutcome::committed);
  const ReadSnapshot snap = store.read();
  const auto full = snap.units("default");
  const auto inputs = snap.units("default", UnitDetail::inputs);
  const auto summary = snap.units("default", UnitDetail::summary);
  REQUIRE(full.size() == 2);
  REQUIRE(inputs.size() == 2);
  REQUIRE(summary.size() == 2);
  for (std::size_t i = 0; i < 2; ++i) {
    CAPTURE(full[i].slot);
    CHECK(inputs[i].slot == full[i].slot);
    CHECK(inputs[i].provenance == full[i].provenance);
    CHECK(inputs[i].settings == full[i].settings);
    CHECK(inputs[i].build_currency == full[i].build_currency);
    REQUIRE(inputs[i].inputs.size() == full[i].inputs.size());
    for (std::size_t k = 0; k < full[i].inputs.size(); ++k) {
      CHECK(inputs[i].inputs[k].path_utf8 == full[i].inputs[k].path_utf8);
      CHECK(inputs[i].inputs[k].content_hash == full[i].inputs[k].content_hash);
      CHECK(inputs[i].inputs[k].role == full[i].inputs[k].role);
    }
    CHECK(inputs[i].includes.empty());
    CHECK(inputs[i].diagnostics.empty());
    CHECK(inputs[i].limits.empty());
    CHECK_FALSE(full[i].includes.empty());
    CHECK(summary[i].slot == full[i].slot);
    CHECK(summary[i].inputs.empty());
    CHECK(summary[i].freshness.semantic == full[i].freshness.semantic);
    CHECK(summary[i].freshness.compile_context == full[i].freshness.compile_context);
  }
}
