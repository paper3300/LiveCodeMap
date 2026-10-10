// Actual analyzer results of two ordinary TUs sharing a header feed the store.
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "lcm/analyzer/analyzer.hpp"
#include "lcm/analyzer/store_adapter.hpp"
#include "lcm/compile_db.hpp"
#include "lcm/source.hpp"
#include "lcm/store/store.hpp"
#include "store/store_fixture.hpp"

using namespace lcm;
using namespace lcm::analyzer;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

struct TwoTu {
  fs::path dir;       // scratch root
  fs::path repo;      // workspace root
  fs::path external;  // outside the repository, inside the scratch tree
  fs::path ccdb;
  fs::path db;
};

void write_text(const fs::path& p, const std::string& text) {
  fs::create_directories(p.parent_path());
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out << text;
}

std::string read_text(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string json_path(const fs::path& p) {
  std::string out;
  for (char c : path_to_utf8_generic(p)) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out;
}

TwoTu make_two_tu(const std::string& name) {
  TwoTu t;
  t.dir = path_from_utf8(LCM_TEST_SCRATCH_DIR) /
          path_from_utf8("store-" + name + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  const fs::path fixture = path_from_utf8(LCM_TEST_FIXTURE_DIR) / "cpp" / "store";
  t.repo = t.dir / "repo";
  t.external = t.dir / "external";
  fs::create_directories(t.dir);
  fs::copy(fixture / "repo", t.repo, fs::copy_options::recursive);
  fs::copy(fixture / "external", t.external, fs::copy_options::recursive);
  t.ccdb = t.repo / "build" / "compile_commands.json";
  t.db = t.dir / "index.sqlite";
  return t;
}

CompilationDatabase load_db(const TwoTu& t, bool b_variant) {
  const std::string dir = json_path(t.repo);
  const std::string json =
      std::string("[\n") + R"({"directory":")" + dir +
      R"(","file":"src/a.cpp","arguments":["clang-cl.exe","/c","/std:c++20","/EHsc","/Iinclude","/Igen","/I../external","src/a.cpp"]},)" +
      "\n" + R"({"directory":")" + dir + R"(","file":"src/b.cpp","arguments":["clang-cl.exe","/c","/std:c++20","/EHsc","/Iinclude",)" +
      (b_variant ? R"("/DVARIANT",)" : "") + R"("src/b.cpp"]})" + "\n]\n";
  write_text(t.ccdb, json);
  auto parsed = load_compilation_database(t.ccdb, CommandSyntax::windows);
  REQUIRE(parsed.database);
  REQUIRE(parsed.database->commands.size() == 2);
  return *parsed.database;
}

AnalysisResult analyze(const TwoTu& t, const CompileCommand& command, const std::string& slot) {
  AnalysisRequest req;
  req.command = &command;
  req.repository_root = t.repo;
  req.resource_dir = path_from_utf8(LCM_CLANG_RESOURCE_DIR);
  req.analysis_unit = slot;  // fact label == logical slot
  AnalysisResult r = analyze_translation_unit(req);
  for (const auto& d : r.diagnostics) MESSAGE(slot << ": " << d.text());
  return r;
}

store::Contribution convert(const TwoTu& t, const AnalysisResult& r, const std::string& slot) {
  ContributionOptions o;
  o.slot = slot;
  o.role_overrides = {{"gen/generated.h", store::InputRole::generated},
                      {"include/excluded.h", store::InputRole::ignored_dependency}};
  o.context_inputs = {{t.ccdb, store::InputRole::build_database}};
  o.build_currency = store::BuildCurrency::verified;
  auto converted = to_contribution(r, o);
  REQUIRE_MESSAGE(converted.contribution, converted.reason);
  return std::move(*converted.contribution);
}

FactSet merged(const std::vector<const AnalysisResult*>& results) {
  FactSet out;
  for (const auto* r : results) store::test::merge_into(out, r->facts);
  return out;
}

store::Batch batch(std::vector<store::Contribution> install, std::vector<std::string> retire = {}) {
  store::Batch b;
  b.profile = "default";
  b.install = std::move(install);
  b.retire = std::move(retire);
  return b;
}

const Symbol* named(const FactSet& f, const std::string& name) {
  for (const auto& [id, s] : f.symbols()) {
    if (s.key.canonical_name == name) return &s;
  }
  return nullptr;
}

std::set<std::string> location_units(const Symbol& s) {
  std::set<std::string> out;
  for (const auto& l : s.locations) out.insert(l.analysis_unit);
  return out;
}

store::InputRole role_of(const store::UnitRecord& u, const std::string& file_suffix) {
  std::string seen;
  for (const auto& in : u.inputs) {
    const std::string generic = path_to_utf8_generic(path_from_utf8(in.path_utf8).lexically_normal());
    if (generic.size() >= file_suffix.size() &&
        generic.compare(generic.size() - file_suffix.size(), file_suffix.size(), file_suffix) == 0) {
      return in.role;
    }
    seen += in.path_utf8 + "\n";
  }
  FAIL("no input ending in " << file_suffix << "; inputs:\n" << seen);
  return store::InputRole::header;
}

bool public_has_file(store::Store& s, const std::string& file) {
  const FactSet f = s.read().load_facts("default", store::Projection::public_);
  for (const auto& [id, sym] : f.symbols()) {
    for (const auto& l : sym.locations) {
      if (l.location.file.generic == file) return true;
    }
  }
  return false;
}

}  // namespace

TEST_CASE("store: actual two-TU contributions roundtrip, replace on command change and keep shared evidence") {
  const TwoTu t = make_two_tu("roundtrip");
  const CompilationDatabase db = load_db(t, false);
  const AnalysisResult ra = analyze(t, db.commands[0], "a");
  const AnalysisResult rb = analyze(t, db.commands[1], "b");
  REQUIRE(ra.status == AnalysisStatus::ok);
  REQUIRE(rb.status == AnalysisStatus::ok);
  REQUIRE_FALSE(ra.facts.unresolved_sites().empty());
  REQUIRE_FALSE(ra.facts.captures().empty());

  {
    store::Store s = store::Store::open(t.db, t.repo);
    auto lock = s.acquire_writer(5s);
    REQUIRE(lock);
    s.ensure_profile(*lock, "default");
    const auto r = s.publish(*lock, batch({convert(t, ra, "a"), convert(t, rb, "b")}));
    REQUIRE_MESSAGE(r.outcome == store::PublishOutcome::committed, r.message);
  }
  store::Store s = store::Store::open(t.db, t.repo);  // reopened
  {
    const auto snap = s.read();
    const FactSet all = snap.load_facts("default", store::Projection::all);
    CHECK(all.fact_hash() == merged({&ra, &rb}).fact_hash());
    CHECK(snap.load_facts("default", store::Projection::public_).fact_hash() == all.fact_hash());
    const Symbol* twice = named(all, "twice");
    REQUIRE(twice);
    CHECK(location_units(*twice) == std::set<std::string>{"a", "b"});
    const auto ua = snap.unit("default", "a");
    const auto ub = snap.unit("default", "b");
    REQUIRE(ua);
    REQUIRE(ub);
    CHECK(ua->provenance.command_id == db.commands[0].command_id);
    CHECK(ua->provenance.replay_id == ra.replay_id);
    CHECK(ua->provenance.context_id.has_value());
    CHECK(role_of(*ua, "src/a.cpp") == store::InputRole::source);
    CHECK(role_of(*ua, "include/shared.h") == store::InputRole::header);
    CHECK(role_of(*ua, "include/detail.inl") == store::InputRole::inline_file);
    CHECK(role_of(*ua, "gen/generated.h") == store::InputRole::generated);
    CHECK(role_of(*ua, "external/ext.h") == store::InputRole::external_dependency);
    CHECK(role_of(*ua, "build/compile_commands.json") == store::InputRole::build_database);
    CHECK(role_of(*ub, "include/excluded.h") == store::InputRole::ignored_dependency);
    CHECK(ua->includes.size() == ra.includes.size());
    for (const auto& in : ua->inputs) {
      if (in.role == store::InputRole::external_dependency) CHECK_FALSE(in.repo_path.has_value());
    }
  }

  auto lock = s.acquire_writer(5s);
  REQUIRE(lock);

  // Selected command change for slot b: the previous contribution is replaced.
  const CompilationDatabase db2 = load_db(t, true);
  const AnalysisResult rb2 = analyze(t, db2.commands[1], "b");
  REQUIRE(rb2.status == AnalysisStatus::ok);
  REQUIRE(rb2.command_id != rb.command_id);
  const store::Contribution cb2 = convert(t, rb2, "b");
  REQUIRE(s.publish(*lock, batch({cb2})).outcome == store::PublishOutcome::committed);
  {
    const auto snap = s.read();
    const FactSet all = snap.load_facts("default", store::Projection::all);
    CHECK(all.fact_hash() == merged({&ra, &rb2}).fact_hash());
    CHECK(named(all, "run_b") == nullptr);
    CHECK(named(all, "run_b_variant") != nullptr);
    CHECK(snap.units("default").size() == 2);
    CHECK(snap.unit("default", "b")->provenance.command_id == rb2.command_id);
    CHECK(snap.fact_generation("default") == 2);
  }
  const auto committed = s.read().load_facts("default", store::Projection::all).fact_hash();

  // A failed analysis of b preserves the committed graph and generation.
  const std::string good_b = read_text(t.repo / "src/b.cpp");
  write_text(t.repo / "src/b.cpp", "#include \"shared.h\"\nint broken( {\n");
  const AnalysisResult broken = analyze(t, db2.commands[1], "b");
  CHECK(broken.status == AnalysisStatus::completed_with_errors);
  ContributionOptions refuse;
  refuse.slot = "b";
  const auto refused = to_contribution(broken, refuse);
  REQUIRE_FALSE(refused.contribution);
  s.record_rejected(*lock, "default", {"b"}, refused.reason);
  {
    const auto snap = s.read();
    CHECK(snap.load_facts("default", store::Projection::all).fact_hash() == committed);
    CHECK(snap.fact_generation("default") == 2);
    const auto st = snap.status("default");
    CHECK(st.last_attempt->state == store::AttemptState::failed);
    CHECK(st.last_success->id != st.last_attempt->id);
    CHECK(st.failed_slots == std::vector<std::string>{"b"});
  }
  write_text(t.repo / "src/b.cpp", good_b);

  // Removing a keeps every b fact in the shared header.
  REQUIRE(s.publish(*lock, batch({}, {"a"})).outcome == store::PublishOutcome::committed);
  {
    const FactSet all = s.read().load_facts("default", store::Projection::all);
    CHECK(all.fact_hash() == merged({&rb2}).fact_hash());
    FactSet incremental = merged({&ra, &rb2});
    incremental.remove_contribution("a");
    CHECK(all.fact_hash() == incremental.fact_hash());
    const Symbol* twice = named(all, "twice");
    REQUIRE(twice);
    CHECK(location_units(*twice) == std::set<std::string>{"b"});
    CHECK(named(all, "run_a") == nullptr);
  }
}

TEST_CASE("store: actual two-TU dependency freshness and public suppression") {
  const TwoTu t = make_two_tu("freshness");
  const CompilationDatabase db = load_db(t, false);
  const AnalysisResult ra = analyze(t, db.commands[0], "a");
  const AnalysisResult rb = analyze(t, db.commands[1], "b");
  REQUIRE(ra.status == AnalysisStatus::ok);
  REQUIRE(rb.status == AnalysisStatus::ok);
  store::Store s = store::Store::open(t.db, t.repo);
  {
    auto lock = s.acquire_writer(5s);
    REQUIRE(lock);
    s.ensure_profile(*lock, "default");
    REQUIRE(s.publish(*lock, batch({convert(t, ra, "a"), convert(t, rb, "b")})).outcome ==
            store::PublishOutcome::committed);
  }
  const auto inspect = [&] {
    store::InspectRequest req;
    req.profile = "default";
    // This case checks what a complete inspection concludes, not the budget
    // (the store tests cover budget ends): a generous explicit budget keeps a
    // loaded host from turning a 10-25 ms check into a spurious budget_exhausted.
    req.budget = std::chrono::milliseconds(60000);
    const auto r = s.inspect_freshness(req);
    REQUIRE(r.outcome == store::InspectionOutcome::complete);
    std::map<std::string, store::UnitFreshness> out;
    for (const auto& u : r.units) out[u.slot] = u;
    return out;
  };
  auto base = inspect();
  CHECK(base["a"].semantic == Freshness::fresh);
  CHECK(base["b"].semantic == Freshness::fresh);

  const auto mutate = [&](const fs::path& p, std::set<std::string> stale, bool context) {
    CAPTURE(path_to_utf8_generic(p));
    const std::string original = read_text(p);
    write_text(p, original + "\n// edited\n");
    auto m = inspect();
    for (const char* slot : {"a", "b"}) {
      CHECK_MESSAGE((m[slot].semantic == Freshness::stale) == (stale.count(slot) != 0), slot);
      if (stale.count(slot)) CHECK((m[slot].compile_context == Freshness::stale) == context);
    }
    write_text(p, original);
    auto back = inspect();
    CHECK(back["a"].semantic == Freshness::fresh);
    CHECK(back["b"].semantic == Freshness::fresh);
  };
  mutate(t.repo / "src/a.cpp", {"a"}, false);
  mutate(t.repo / "include/detail.inl", {"a", "b"}, false);  // nested .inl through the shared header
  mutate(t.repo / "gen/generated.h", {"a"}, false);
  mutate(t.repo / "include/excluded.h", {"b"}, false);
  mutate(t.external / "ext.h", {"a"}, false);
  mutate(t.ccdb, {"a", "b"}, true);

  // Deletion is withheld from public output at once; metadata remains.
  CHECK(public_has_file(s, "include/solo.h"));
  fs::remove(t.repo / "include/solo.h");
  CHECK_FALSE(public_has_file(s, "include/solo.h"));
  auto after_delete = inspect();
  CHECK(after_delete["a"].semantic == Freshness::stale);
  CHECK(after_delete["b"].semantic == Freshness::fresh);

  // New exclusion: withheld from public output, still a dependency of b.
  CHECK(public_has_file(s, "include/excluded.h"));
  {
    auto lock = s.acquire_writer(5s);
    REQUIRE(lock);
    s.set_excluded_files(*lock, "default", {store::test::repo("include/excluded.h")});
  }
  CHECK_FALSE(public_has_file(s, "include/excluded.h"));
  CHECK(public_has_file(s, "include/shared.h"));
  write_text(t.repo / "include/excluded.h", read_text(t.repo / "include/excluded.h") + "// edit\n");
  CHECK(inspect()["b"].semantic == Freshness::stale);
}
