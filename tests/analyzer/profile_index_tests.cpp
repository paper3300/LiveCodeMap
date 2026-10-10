// Selected-profile multi-TU indexing through the actual analyzer and store.
#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "lcm/analyzer/analyzer.hpp"
#include "lcm/analyzer/profile_index.hpp"
#include "lcm/analyzer/store_adapter.hpp"
#include "lcm/compile_db.hpp"
#include "lcm/source.hpp"
#include "lcm/status.hpp"
#include "lcm/store/store.hpp"
#include "store/store_fixture.hpp"

using namespace lcm;
using namespace lcm::analyzer;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

using Args = std::vector<std::string>;

void write_text(const fs::path& p, const std::string& text) {
  fs::create_directories(p.parent_path());
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out << text;
}

std::string read_text(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void append_text(const fs::path& p, const std::string& text) { write_text(p, read_text(p) + text); }

void replace_text(const fs::path& p, const std::string& from, const std::string& to) {
  std::string s = read_text(p);
  const auto at = s.find(from);
  REQUIRE_MESSAGE(at != std::string::npos, "'" << from << "' not in " << path_to_utf8_generic(p));
  s.replace(at, from.size(), to);
  write_text(p, s);
}

std::string json_string(const std::string& s) {
  std::string out = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out + "\"";
}

Args ordinary(const std::string& file, Args extra = {}) {
  Args a{"cl.exe", "/c", "/std:c++20", "/EHsc", "/Iinclude", "/Igen", "/I../external"};
  a.insert(a.end(), extra.begin(), extra.end());
  a.push_back(file);
  return a;
}

// A scratch copy of tests/fixtures/cpp/store: two TUs sharing a header with a
// nested .inl, an independent header, generated/ignored-tagged headers and a
// dependency outside the repository.
struct Fx {
  fs::path dir;
  fs::path repo;
  fs::path external;
  fs::path ccdb;
  fs::path db;
  fs::path toolchain;  // caller-supplied compiler descriptor, outside the repository
  std::vector<std::pair<std::string, Args>> entries;

  void write_db(const fs::path& path) const {
    std::string json = "[\n";
    for (std::size_t i = 0; i < entries.size(); ++i) {
      json += "{\"directory\":" + json_string(path_to_utf8_generic(repo)) + ",\"file\":" + json_string(entries[i].first) +
              ",\"arguments\":[";
      for (std::size_t j = 0; j < entries[i].second.size(); ++j) {
        if (j) json += ",";
        json += json_string(entries[i].second[j]);
      }
      json += "]}";
      json += i + 1 < entries.size() ? ",\n" : "\n";
    }
    write_text(path, json + "]\n");
  }
  void write_db() const { write_db(ccdb); }

  std::string command_id(const fs::path& database, std::size_t entry) const {
    const auto parsed = load_compilation_database(database, CommandSyntax::windows);
    REQUIRE(parsed.database);
    return parsed.database->commands.at(entry).command_id;
  }
};

Fx make_fx(const std::string& name) {
  Fx f;
  f.dir = path_from_utf8(LCM_TEST_SCRATCH_DIR) /
          path_from_utf8("profile-" + name + "-" +
                         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  const fs::path fixture = path_from_utf8(LCM_TEST_FIXTURE_DIR) / "cpp" / "store";
  f.repo = f.dir / "repo";
  f.external = f.dir / "external";
  fs::create_directories(f.dir);
  fs::copy(fixture / "repo", f.repo, fs::copy_options::recursive);
  fs::copy(fixture / "external", f.external, fs::copy_options::recursive);
  f.ccdb = f.repo / "build" / "compile_commands.json";
  f.db = f.dir / "index.sqlite";
  f.toolchain = f.dir / "toolchain" / "cl-version.txt";
  write_text(f.toolchain, "cl 19.44 x64\n");
  f.entries = {{"src/a.cpp", ordinary("src/a.cpp")}, {"src/b.cpp", ordinary("src/b.cpp")}};
  f.write_db();
  return f;
}

SelectedAction act(const std::string& slot, const std::string& source, std::vector<std::string> members = {},
                   std::optional<std::string> command_id = std::nullopt) {
  SelectedAction a;
  a.slot = slot;
  a.source = path_from_utf8(source);
  a.command_id = std::move(command_id);
  for (const auto& m : members) a.unity_members.push_back(path_from_utf8(m));
  return a;
}

ProfileIndexRequest request(const Fx& f, std::vector<SelectedAction> actions, const std::string& profile = "default") {
  ProfileIndexRequest r;
  r.profile = profile;
  r.compile_database = f.ccdb;
  r.syntax = CommandSyntax::windows;
  r.actions = std::move(actions);
  r.context_inputs = {{f.toolchain, store::InputRole::compiler}};
  r.role_overrides = {{"gen/generated.h", store::InputRole::generated},
                      {"include/excluded.h", store::InputRole::ignored_dependency}};
  r.resource_dir = path_from_utf8(LCM_CLANG_RESOURCE_DIR);
  return r;
}

ProfileIndexRequest two_tu(const Fx& f, const std::string& profile = "default") {
  return request(f, {act("a", "src/a.cpp"), act("b", "src/b.cpp")}, profile);
}

std::string describe(const ProfileIndexReport& r) {
  std::string out = std::string(to_string(r.outcome)) + ": " + r.message;
  for (const auto& e : r.errors) out += "\n  error: " + e;
  for (const auto& s : r.slots) {
    out += "\n  " + s.slot + " " + std::string(to_string(s.plan)) + " -> " + std::string(to_string(s.result));
    for (const auto& why : s.reasons) out += "\n    " + why;
  }
  return out;
}

ProfileIndexReport run(store::Store& s, const ProfileIndexRequest& r) {
  ProfileIndexReport out = index_profile(s, r);
  MESSAGE(describe(out));
  return out;
}

ProfileIndexReport run_ok(store::Store& s, const ProfileIndexRequest& r) {
  ProfileIndexReport out = run(s, r);
  REQUIRE(out.outcome == IndexOutcome::committed);
  return out;
}

bool has_reason(const SlotReport* s, const std::string& text) {
  REQUIRE(s);
  for (const auto& r : s->reasons) {
    if (r.find(text) != std::string::npos) return true;
  }
  return false;
}

std::set<std::string> changed_slots(const ProfileIndexReport& r) {
  std::set<std::string> out;
  for (const auto& s : r.slots) {
    if (s.plan == SlotPlan::changed || s.plan == SlotPlan::new_action) out.insert(s.slot);
  }
  return out;
}

FactSet all_facts(const store::Store& s, const std::string& profile = "default") {
  return s.read().load_facts(profile, store::Projection::all);
}

FactSet public_facts(const store::Store& s, const std::string& profile = "default") {
  return s.read().load_facts(profile, store::Projection::public_);
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

std::optional<SymbolLocation> definition(const Symbol& s) {
  for (const auto& l : s.locations) {
    if (l.role == LocationRole::definition) return l;
  }
  return std::nullopt;
}

bool mentions_file(const FactSet& f, const std::string& prefix) {
  const auto in = [&](const RepoRelativePath& p) { return p.generic.rfind(prefix, 0) == 0; };
  for (const auto& [id, s] : f.symbols()) {
    for (const auto& l : s.locations) {
      if (in(l.location.file)) return true;
    }
  }
  for (const auto& [k, r] : f.relations()) {
    for (const auto& e : r.evidence) {
      if (in(e.location.file)) return true;
    }
  }
  for (const auto& u : f.unresolved_sites()) {
    if (in(u.location.file)) return true;
  }
  for (const auto& inc : f.direct_includes()) {
    if (in(inc.includer) || (inc.resolved_repo_target && in(*inc.resolved_repo_target))) return true;
  }
  return false;
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

std::string fingerprint(const store::Store& s, const std::string& slot, store::FingerprintKind kind) {
  const auto u = s.read().unit("default", slot);
  REQUIRE(u);
  return u->fingerprints.at(static_cast<std::size_t>(kind)).value.value_or("");
}

const LayerState& layer(const store::ProfileStatus& st, Layer l) { return st.layers.at(static_cast<std::size_t>(l)); }

AnalysisResult direct(const Fx& f, const std::string& file, const std::string& slot) {
  const auto parsed = load_compilation_database(f.ccdb, CommandSyntax::windows);
  REQUIRE(parsed.database);
  const auto choice = parsed.database->select_unique(f.repo / path_from_utf8(file));
  REQUIRE(choice.kind == CommandSelection::Kind::unique);
  AnalysisRequest req;
  req.command = choice.command;
  req.repository_root = f.repo;
  req.resource_dir = path_from_utf8(LCM_CLANG_RESOURCE_DIR);
  req.analysis_unit = slot;
  AnalysisResult r = analyze_translation_unit(req);
  REQUIRE(r.status == AnalysisStatus::ok);
  return r;
}

// State that a failed or rejected run must leave untouched.
struct Committed {
  std::int64_t generation = 0;
  Sha256Digest facts;
  std::int64_t last_success = 0;
  std::size_t units = 0;
};

Committed committed(const store::Store& s, const std::string& profile = "default") {
  const auto snap = s.read();
  Committed c;
  c.generation = snap.fact_generation(profile);
  c.facts = snap.load_facts(profile, store::Projection::all).fact_hash();
  const auto st = snap.status(profile);
  c.last_success = st.last_success ? st.last_success->id : 0;
  c.units = snap.units(profile).size();
  return c;
}

void check_preserved(const store::Store& s, const Committed& before, const ProfileIndexReport& r,
                     const std::string& profile = "default") {
  const Committed now = committed(s, profile);
  CHECK(now.generation == before.generation);
  CHECK(now.facts == before.facts);
  CHECK(now.last_success == before.last_success);
  CHECK(now.units == before.units);
  CHECK(r.generation_after == before.generation);
}

}  // namespace

TEST_CASE("profile index: initial two-TU publication, reopen, unchanged no-op and honest currency") {
  const Fx f = make_fx("initial");
  const ProfileIndexRequest req = two_tu(f);
  {
    store::Store s = store::Store::open(f.db, f.repo);
    const auto r = run_ok(s, req);
    CHECK(r.planned_analyses == 2);
    CHECK(r.actual_analyses == 2);
    CHECK(r.generation_before == 0);
    CHECK(r.generation_after == 1);
    CHECK(r.slot("a")->plan == SlotPlan::new_action);
    CHECK(r.slot("a")->result == SlotResult::installed);
    CHECK(r.slot("b")->result == SlotResult::installed);
  }
  store::Store s = store::Store::open(f.db, f.repo);  // reopened
  const AnalysisResult ga = direct(f, "src/a.cpp", "a");
  const AnalysisResult gb = direct(f, "src/b.cpp", "b");
  FactSet gold;
  store::test::merge_into(gold, ga.facts);
  store::test::merge_into(gold, gb.facts);
  {
    const auto snap = s.read();
    CHECK(snap.load_facts("default", store::Projection::all).fact_hash() == gold.fact_hash());
    const FactSet all = snap.load_facts("default", store::Projection::all);
    const Symbol* twice = named(all, "twice");
    REQUIRE(twice);
    CHECK(location_units(*twice) == std::set<std::string>{"a", "b"});
    const auto ua = snap.unit("default", "a");
    const auto ub = snap.unit("default", "b");
    REQUIRE(ua);
    REQUIRE(ub);
    CHECK(ua->fact_label == "a");
    CHECK(ua->provenance.command_id == ga.command_id);
    CHECK(ua->settings.analyzer == analyzer_identity());
    CHECK(ua->settings.resource_dir == path_to_utf8_generic(path_from_utf8(LCM_CLANG_RESOURCE_DIR).lexically_normal()));
    CHECK(ua->settings.repository_member == "repo");
    CHECK(ua->settings.publish_policy == "accepted_only");
    CHECK(ua->build_currency == store::BuildCurrency::unknown);
    CHECK(role_of(*ua, "src/a.cpp") == store::InputRole::source);
    CHECK(role_of(*ua, "build/compile_commands.json") == store::InputRole::build_database);
    CHECK(role_of(*ua, "toolchain/cl-version.txt") == store::InputRole::compiler);
    CHECK(role_of(*ua, "gen/generated.h") == store::InputRole::generated);
    CHECK(role_of(*ua, "include/detail.inl") == store::InputRole::inline_file);
    CHECK(role_of(*ua, "external/ext.h") == store::InputRole::external_dependency);
    CHECK(role_of(*ub, "include/excluded.h") == store::InputRole::ignored_dependency);
    const auto st = snap.status("default");
    CHECK_FALSE(layer(st, Layer::uht_context).applicable);
    CHECK(layer(st, Layer::retrieval).availability == Availability::missing);
    CHECK(layer(st, Layer::deep).availability == Availability::missing);
    CHECK(layer(st, Layer::compile_context).freshness != Freshness::fresh);
  }

  const std::int64_t attempt = s.read().status("default").last_attempt->id;
  const auto again = run(s, req);
  CHECK(again.outcome == IndexOutcome::unchanged);
  CHECK(again.planned_analyses == 0);
  CHECK(again.actual_analyses == 0);
  CHECK(again.generation_after == 1);
  CHECK(again.slot("a")->plan == SlotPlan::unchanged);
  CHECK(again.slot("b")->result == SlotResult::unchanged);
  CHECK(s.read().status("default").last_attempt->id == attempt);

  // Unverified build currency survives an inspection as not fresh and does not
  // force every later byte-identical run to rebuild.
  store::InspectRequest inspect;
  inspect.profile = "default";
  const auto fr = s.inspect_freshness(inspect);
  REQUIRE(fr.outcome == store::InspectionOutcome::complete);
  for (const auto& u : fr.units) {
    CHECK(u.compile_context != Freshness::fresh);
    CHECK(u.semantic != Freshness::fresh);
  }
  CHECK(summarize_layer(layer(s.read().status("default"), Layer::semantic_graph)) != UserStatus::ready);
  CHECK(run(s, req).outcome == IndexOutcome::unchanged);

  // Verified currency is caller evidence: it is a change of the committed unit.
  ProfileIndexRequest verified = req;
  verified.build_currency = store::BuildCurrency::verified;
  const auto v = run_ok(s, verified);
  CHECK(has_reason(v.slot("a"), "build currency"));
  CHECK(v.actual_analyses == 2);
  const auto fresh = s.inspect_freshness(inspect);
  REQUIRE(fresh.outcome == store::InspectionOutcome::complete);
  for (const auto& u : fresh.units) CHECK(u.semantic == Freshness::fresh);
  CHECK(run(s, verified).outcome == IndexOutcome::unchanged);
}

TEST_CASE("profile index: body, signature, source and dependency mutations reanalyse only dependent slots") {
  const Fx f = make_fx("mutations");
  store::Store s = store::Store::open(f.db, f.repo);
  const ProfileIndexRequest req = two_tu(f);
  run_ok(s, req);

  const auto step = [&](const std::set<std::string>& expected) {
    const std::int64_t gen = s.read().fact_generation("default");
    const auto r = run_ok(s, req);
    CHECK(r.generation_after == gen + 1);
    CHECK(changed_slots(r) == expected);
    CHECK(r.actual_analyses == expected.size());
    for (const auto& slot : {"a", "b"}) {
      if (!expected.count(slot)) CHECK(r.slot(slot)->result == SlotResult::unchanged);
    }
    return r;
  };

  // Body-only edit keeps the stable ID; only the body fingerprint moves.
  const StableId run_b = named(all_facts(s), "run_b")->id;
  const std::string api_b = fingerprint(s, "b", store::FingerprintKind::declaration_api);
  const std::string body_b = fingerprint(s, "b", store::FingerprintKind::symbol_body);
  const std::int64_t a_generation = s.read().unit("default", "a")->committed_generation;
  replace_text(f.repo / "src/b.cpp", "geo::apply(callback, 1)", "geo::apply(callback, 2)");
  CHECK(has_reason(step({"b"}).slot("b"), "input modified"));
  {
    const FactSet all = all_facts(s);
    REQUIRE(named(all, "run_b"));
    CHECK(named(all, "run_b")->id == run_b);
    CHECK(location_units(*named(all, "twice")) == std::set<std::string>{"a", "b"});
  }
  CHECK(fingerprint(s, "b", store::FingerprintKind::declaration_api) == api_b);
  CHECK(fingerprint(s, "b", store::FingerprintKind::symbol_body) != body_b);
  CHECK(s.read().unit("default", "a")->committed_generation == a_generation);  // not republished

  // Signature change replaces the old symbol; a's evidence survives.
  replace_text(f.repo / "src/b.cpp", "int (*callback)(int)) {", "int (*callback)(int), int extra) {");
  step({"b"});
  {
    const FactSet all = all_facts(s);
    CHECK(all.find_symbol(run_b) == nullptr);
    REQUIRE(named(all, "run_b"));
    CHECK(named(all, "run_b")->id != run_b);
    REQUIRE(named(all, "run_a"));
    CHECK(location_units(*named(all, "twice")) == std::set<std::string>{"a", "b"});
  }
  CHECK(fingerprint(s, "b", store::FingerprintKind::declaration_api) != api_b);

  append_text(f.repo / "src/a.cpp", "\nint added_a() { return 1; }\n");
  step({"a"});
  CHECK(named(all_facts(s), "added_a") != nullptr);

  append_text(f.repo / "include/detail.inl", "// nested shared edit\n");  // through shared.h
  step({"a", "b"});
  append_text(f.repo / "include/solo.h", "// independent edit\n");
  step({"a"});
  append_text(f.repo / "gen/generated.h", "// generated edit\n");
  step({"a"});
  append_text(f.repo / "include/excluded.h", "// ignored edit\n");
  step({"b"});
  append_text(f.external / "ext.h", "// external edit\n");
  step({"a"});
  CHECK(run(s, req).outcome == IndexOutcome::unchanged);
}

TEST_CASE("profile index: new, missing, retired and deleted actions; malformed and ambiguous databases") {
  Fx f = make_fx("actions");
  store::Store s = store::Store::open(f.db, f.repo);
  run_ok(s, two_tu(f));

  // A new action. Its database entry schedules only the new slot.
  write_text(f.repo / "src/c.cpp", "#include \"shared.h\"\nint run_c() { return geo::twice(3); }\n");
  f.entries.push_back({"src/c.cpp", ordinary("src/c.cpp")});
  f.write_db();
  ProfileIndexRequest abc = request(f, {act("a", "src/a.cpp"), act("b", "src/b.cpp"), act("c", "src/c.cpp")});
  {
    const auto r = run_ok(s, abc);
    CHECK(r.slot("c")->plan == SlotPlan::new_action);
    CHECK(r.slot("a")->plan == SlotPlan::unchanged);
    CHECK(r.slot("b")->plan == SlotPlan::unchanged);
    CHECK(r.actual_analyses == 1);
    CHECK(named(all_facts(s), "run_c") != nullptr);
  }

  // A declared new source without a command fails explicitly.
  write_text(f.repo / "src/d.cpp", "int run_d() { return 4; }\n");
  {
    const Committed before = committed(s);
    ProfileIndexRequest abcd = abc;
    abcd.actions.push_back(act("d", "src/d.cpp"));
    const auto r = run(s, abcd);
    CHECK(r.outcome == IndexOutcome::rejected);
    CHECK(r.actual_analyses == 0);
    REQUIRE_FALSE(r.errors.empty());
    CHECK(r.errors.front().find("no compile command") != std::string::npos);
    check_preserved(s, before, r);
    CHECK(r.failure_recorded);
    const auto st = s.read().status("default");
    CHECK(st.last_attempt->state == store::AttemptState::failed);
    CHECK(st.last_success->id != st.last_attempt->id);
  }

  // Absent from a valid authoritative selection: retired without analysis.
  {
    const auto r = run_ok(s, two_tu(f));
    CHECK(r.slot("c")->plan == SlotPlan::retired);
    CHECK(r.slot("c")->result == SlotResult::retired);
    CHECK(r.actual_analyses == 0);
    CHECK(s.read().units("default").size() == 2);
    CHECK(named(all_facts(s), "run_c") == nullptr);
  }

  // Missing or malformed databases never retire anything.
  const std::string good_db = read_text(f.ccdb);
  for (const std::string& broken : {std::string(), std::string("[{\"directory\": 5}")}) {
    CAPTURE(broken);
    const Committed before = committed(s);
    if (broken.empty()) {
      fs::remove(f.ccdb);
    } else {
      write_text(f.ccdb, broken);
    }
    ProfileIndexRequest one = request(f, {act("a", "src/a.cpp")});
    const auto r = run(s, one);
    CHECK(r.outcome == IndexOutcome::rejected);
    CHECK(r.actual_analyses == 0);
    check_preserved(s, before, r);
    write_text(f.ccdb, good_db);
  }

  // Several commands for one file: reported, never chosen; an explicit id resolves it.
  f.entries.push_back({"src/a.cpp", ordinary("src/a.cpp", {"/DOTHER"})});
  f.write_db();
  {
    const Committed before = committed(s);
    const auto r = run(s, two_tu(f));
    CHECK(r.outcome == IndexOutcome::rejected);
    REQUIRE_FALSE(r.errors.empty());
    CHECK(r.errors.front().find("several commands") != std::string::npos);
    check_preserved(s, before, r);
  }
  const std::string a_id = f.command_id(f.ccdb, 0);
  // The committed command of a is unchanged: resolved explicitly, nothing to analyse.
  CHECK(run(s, request(f, {act("a", "src/a.cpp", {}, a_id), act("b", "src/b.cpp")})).outcome == IndexOutcome::unchanged);
  CHECK(s.read().unit("default", "a")->provenance.command_id == a_id);

  // Inconsistent selections are refused before analysis.
  const std::string b_id = f.command_id(f.ccdb, 1);
  const std::vector<std::vector<SelectedAction>> inconsistent{
      {act("a", "src/a.cpp", {}, a_id), act("a", "src/b.cpp")},              // duplicate slot
      {act("a", "src/a.cpp", {}, a_id), act("b", "src/b.cpp"), act("b2", "src/b.cpp")},  // one file, two slots
      {act("a", "src/a.cpp", {}, b_id), act("b", "src/b.cpp")},              // command of another file
      {act("a", "src/a.cpp", {}, a_id), act("b", "src/b.cpp", {}, a_id)},    // command selected twice
      {act("", "src/a.cpp", {}, a_id)},                                      // empty slot
      {act("a", "src/a.cpp", {}, "no-such-command")},
  };
  for (const auto& actions : inconsistent) {
    const Committed before = committed(s);
    const auto r = run(s, request(f, actions));
    CHECK(r.outcome == IndexOutcome::rejected);
    CHECK_FALSE(r.errors.empty());
    CHECK(r.actual_analyses == 0);
    check_preserved(s, before, r);
  }

  // A deleted source that is still selected fails; its committed facts are kept
  // internally but withheld from public output at once.
  const ProfileIndexRequest explicit_ab = request(f, {act("a", "src/a.cpp", {}, a_id), act("b", "src/b.cpp")});
  const std::string b_source = read_text(f.repo / "src/b.cpp");
  fs::remove(f.repo / "src/b.cpp");
  {
    const Committed before = committed(s);
    const auto r = run(s, explicit_ab);
    CHECK(r.outcome == IndexOutcome::failed);
    CHECK(has_reason(r.slot("b"), "input deleted"));
    CHECK(r.slot("b")->result == SlotResult::failed);
    check_preserved(s, before, r);
    CHECK(named(all_facts(s), "run_b") != nullptr);
    CHECK(named(public_facts(s), "run_b") == nullptr);
    CHECK_FALSE(mentions_file(public_facts(s), "src/b.cpp"));
    const auto st = s.read().status("default");
    CHECK(std::find(st.failed_slots.begin(), st.failed_slots.end(), "b") != st.failed_slots.end());
  }
  // Restoring the committed bytes makes the selection unchanged again.
  write_text(f.repo / "src/b.cpp", b_source);
  CHECK(run(s, explicit_ab).outcome == IndexOutcome::unchanged);

  // Deleted and removed from database and selection: retired.
  fs::remove(f.repo / "src/b.cpp");
  f.entries.erase(f.entries.begin() + 1);
  f.write_db();
  {
    const auto r = run_ok(s, request(f, {act("a", "src/a.cpp", {}, f.command_id(f.ccdb, 0))}));
    CHECK(r.slot("b")->plan == SlotPlan::retired);
    CHECK(s.read().units("default").size() == 1);
    CHECK(named(all_facts(s), "run_b") == nullptr);
  }
}

TEST_CASE("profile index: an intended empty selection retires every slot; missing input never does") {
  const Fx f = make_fx("empty");
  store::Store s = store::Store::open(f.db, f.repo);
  run_ok(s, two_tu(f));

  ProfileIndexRequest empty = request(f, {});
  {
    const Committed before = committed(s);
    const auto r = run(s, empty);
    CHECK(r.outcome == IndexOutcome::rejected);  // not declared intentional
    check_preserved(s, before, r);
  }
  empty.empty_selection_intended = true;
  {
    ProfileIndexRequest missing = empty;
    missing.compile_database = f.repo / "build" / "missing.json";
    const Committed before = committed(s);
    const auto r = run(s, missing);
    CHECK(r.outcome == IndexOutcome::rejected);
    REQUIRE_FALSE(r.errors.empty());
    CHECK(r.errors.front().find("missing") != std::string::npos);
    check_preserved(s, before, r);
  }
  {
    ProfileIndexRequest contradictory = two_tu(f);
    contradictory.empty_selection_intended = true;
    const Committed before = committed(s);
    const auto r = run(s, contradictory);
    CHECK(r.outcome == IndexOutcome::rejected);
    check_preserved(s, before, r);
  }
  {
    const auto r = run_ok(s, empty);
    CHECK(r.slot("a")->result == SlotResult::retired);
    CHECK(r.slot("b")->result == SlotResult::retired);
    CHECK(r.generation_after == 2);
    CHECK(s.read().units("default").empty());
    CHECK(all_facts(s).symbols().empty());
  }
  CHECK(run(s, empty).outcome == IndexOutcome::unchanged);

  // An intended empty selection of a new profile creates the empty profile.
  ProfileIndexRequest fresh = request(f, {}, "fresh");
  fresh.empty_selection_intended = true;
  {
    const auto r = run(s, fresh);
    CHECK(r.outcome == IndexOutcome::unchanged);
    CHECK(r.generation_after == 0);
    const auto snap = s.read();
    const auto profiles = snap.profiles();
    CHECK(std::find(profiles.begin(), profiles.end(), "fresh") != profiles.end());
    CHECK(snap.fact_generation("fresh") == 0);
    CHECK(snap.units("fresh").empty());
    CHECK_FALSE(snap.status("fresh").last_attempt.has_value());
    CHECK(layer(snap.status("fresh"), Layer::semantic_graph).availability == Availability::missing);
  }
  CHECK(run(s, fresh).outcome == IndexOutcome::unchanged);

  // An empty profile key is rejected and records nothing.
  ProfileIndexRequest unnamed = two_tu(f, "");
  const auto r = run(s, unnamed);
  CHECK(r.outcome == IndexOutcome::rejected);
  CHECK_FALSE(r.failure_recorded);
  const auto profiles = s.read().profiles();
  CHECK(std::find(profiles.begin(), profiles.end(), "") == profiles.end());
}

TEST_CASE("profile index: first-run failures of a new profile are recorded without committed facts") {
  Fx f = make_fx("cold");
  const auto check_cold = [&](const store::Store& s, const std::string& profile, bool uht) {
    const auto snap = s.read();
    const auto profiles = snap.profiles();
    REQUIRE(std::find(profiles.begin(), profiles.end(), profile) != profiles.end());
    CHECK(snap.fact_generation(profile) == 0);
    CHECK(snap.units(profile).empty());
    CHECK(snap.load_facts(profile, store::Projection::all).symbols().empty());
    const auto st = snap.status(profile);
    REQUIRE(st.last_attempt);
    CHECK(st.last_attempt->state == store::AttemptState::failed);
    CHECK_FALSE(st.last_success.has_value());
    CHECK(layer(st, Layer::uht_context).applicable == uht);
    CHECK(layer(st, Layer::retrieval).availability == Availability::missing);
    CHECK(layer(st, Layer::deep).availability == Availability::missing);
    CHECK(summarize_layer(layer(st, Layer::semantic_graph)) != UserStatus::ready);
    return st;
  };
  {
    store::Store s = store::Store::open(f.db, f.repo);
    // Selection failure: a declared source without a command.
    write_text(f.repo / "src/d.cpp", "int run_d() { return 4; }\n");
    const auto r1 = run(s, request(f, {act("a", "src/a.cpp"), act("d", "src/d.cpp")}, "cold-select"));
    CHECK(r1.outcome == IndexOutcome::rejected);
    CHECK(r1.failure_recorded);
    CHECK(r1.generation_after == 0);
    // Analysis failure of the first run: nothing committed, attempt recorded.
    const std::string good_a = read_text(f.repo / "src/a.cpp");
    write_text(f.repo / "src/a.cpp", "#include \"shared.h\"\nint broken( {\n");
    ProfileIndexRequest analysis = two_tu(f, "cold-analysis");
    analysis.uht_applicable = true;
    const auto r2 = run(s, analysis);
    CHECK(r2.outcome == IndexOutcome::failed);
    CHECK(r2.failure_recorded);
    CHECK(r2.generation_after == 0);
    write_text(f.repo / "src/a.cpp", good_a);
  }
  store::Store s = store::Store::open(f.db, f.repo);  // reopened
  const auto st1 = check_cold(s, "cold-select", false);
  CHECK(std::find(st1.failed_slots.begin(), st1.failed_slots.end(), "d") != st1.failed_slots.end());
  const auto st2 = check_cold(s, "cold-analysis", true);
  CHECK(st2.failed_slots == std::vector<std::string>{"a", "b"});

  // The recorded profile keeps its applicability; a later valid run commits.
  CHECK(run(s, two_tu(f, "cold-analysis")).outcome == IndexOutcome::rejected);  // UHT false requested
  ProfileIndexRequest retry = two_tu(f, "cold-analysis");
  retry.uht_applicable = true;
  const auto ok = run_ok(s, retry);
  CHECK(ok.generation_after == 1);
  CHECK(s.read().status("cold-analysis").last_success.has_value());
}

TEST_CASE("profile index: response file, option, toolchain, configuration, role and setting changes") {
  Fx f = make_fx("context");
  write_text(f.repo / "rsp/a.rsp", "/c /std:c++20 /EHsc\n@rsp/a-inc.rsp\n\"src/a.cpp\"\n");
  write_text(f.repo / "rsp/a-inc.rsp", "/Iinclude /Igen /I../external\n");
  f.entries[0].second = {"cl.exe", "@rsp/a.rsp"};
  f.write_db();
  store::Store s = store::Store::open(f.db, f.repo);
  ProfileIndexRequest req = two_tu(f);
  run_ok(s, req);
  {
    const auto ua = s.read().unit("default", "a");
    REQUIRE(ua);
    CHECK(role_of(*ua, "rsp/a.rsp") == store::InputRole::response_file);
    CHECK(role_of(*ua, "rsp/a-inc.rsp") == store::InputRole::response_file);
    CHECK(ua->provenance.replay_id != ua->provenance.command_id);
  }
  const auto step = [&](const std::set<std::string>& expected, const std::string& reason) {
    const auto r = run_ok(s, req);
    CHECK(changed_slots(r) == expected);
    for (const auto& slot : expected) CHECK_MESSAGE(has_reason(r.slot(slot), reason), slot);
    CHECK(run(s, req).outcome == IndexOutcome::unchanged);
    return r;
  };

  append_text(f.repo / "rsp/a-inc.rsp", "/DRSP_EDIT\n");  // nested response file
  step({"a"}, "a-inc.rsp");

  f.entries[1].second = ordinary("src/b.cpp", {"/DVARIANT"});  // selected option change
  f.write_db();
  step({"b"}, "selected command changed");
  CHECK(named(all_facts(s), "run_b_variant") != nullptr);
  CHECK(named(all_facts(s), "run_b") == nullptr);

  append_text(f.toolchain, "cl 19.45 x64\n");  // toolchain descriptor
  step({"a", "b"}, "context input changed");

  const fs::path cache = f.dir / "build-config" / "CMakeCache.txt";  // new configuration input
  write_text(cache, "CMAKE_BUILD_TYPE:STRING=Debug\n");
  req.context_inputs.push_back({cache, store::InputRole::build_configuration});
  step({"a", "b"}, "context input added");
  append_text(cache, "CMAKE_CXX_FLAGS:STRING=/W4\n");
  step({"a", "b"}, "context input changed");

  req.role_overrides["include/solo.h"] = store::InputRole::generated;  // role tagging
  step({"a"}, "input role changed");
  CHECK(role_of(*s.read().unit("default", "a"), "include/solo.h") == store::InputRole::generated);

  req.repository_member = "member2";  // changes every in-root canonical key
  step({"a", "b"}, "repository member");
  CHECK(named(all_facts(s), "run_a")->key.repository_member == "member2");

  req.policy = PublishPolicy::allow_errors_and_degraded;
  step({"a", "b"}, "publish policy");
}

TEST_CASE("profile index: action-local context inputs and mixed build currency") {
  const Fx f = make_fx("local-context");
  const fs::path a_module = f.repo / "modules/a.build";
  const fs::path b_module = f.repo / "modules/b.build";
  write_text(a_module, "module A\n");
  write_text(b_module, "module B\n");
  SelectedAction a = act("a", "src/a.cpp");
  a.context_inputs = {{a_module, store::InputRole::build_configuration}};
  a.build_currency = store::BuildCurrency::verified;
  SelectedAction b = act("b", "src/b.cpp");
  b.context_inputs = {{b_module, store::InputRole::build_configuration}};
  ProfileIndexRequest req = request(f, {a, b});
  store::Store s = store::Store::open(f.db, f.repo);
  run_ok(s, req);
  const auto has_input = [&](const std::string& slot, const std::string& suffix) {
    const auto unit = s.read().unit("default", slot);
    REQUIRE(unit);
    for (const auto& in : unit->inputs) {
      const std::string g = path_to_utf8_generic(path_from_utf8(in.path_utf8));
      if (g.size() >= suffix.size() && g.compare(g.size() - suffix.size(), suffix.size(), suffix) == 0) return true;
    }
    return false;
  };
  {
    const auto snap = s.read();
    CHECK(snap.unit("default", "a")->build_currency == store::BuildCurrency::verified);
    CHECK(snap.unit("default", "b")->build_currency == store::BuildCurrency::unknown);
    CHECK(role_of(*snap.unit("default", "a"), "modules/a.build") == store::InputRole::build_configuration);
  }
  CHECK(has_input("a", "modules/a.build"));
  CHECK_FALSE(has_input("a", "modules/b.build"));
  CHECK(has_input("b", "modules/b.build"));
  CHECK_FALSE(has_input("b", "modules/a.build"));
  // Mixed currency is kept per unit: only the verified unit can become fresh.
  store::InspectRequest inspect;
  inspect.profile = "default";
  {
    const auto fr = s.inspect_freshness(inspect);
    REQUIRE(fr.outcome == store::InspectionOutcome::complete);
    std::map<std::string, store::UnitFreshness> by;
    for (const auto& u : fr.units) by[u.slot] = u;
    CHECK(by["a"].semantic == Freshness::fresh);
    CHECK(by["b"].semantic != Freshness::fresh);
    CHECK(by["b"].compile_context != Freshness::fresh);
  }
  CHECK(run(s, req).outcome == IndexOutcome::unchanged);

  // An action-local configuration change is stale for that action only in the
  // compiler-free inspection, and reanalyses only that action.
  append_text(b_module, "dependency X\n");
  {
    const auto fr = s.inspect_freshness(inspect);
    REQUIRE(fr.outcome == store::InspectionOutcome::complete);
    std::map<std::string, store::UnitFreshness> by;
    for (const auto& u : fr.units) by[u.slot] = u;
    CHECK(by["b"].compile_context == Freshness::stale);
    CHECK(by["b"].semantic == Freshness::stale);
    CHECK(by["a"].semantic == Freshness::fresh);
  }
  {
    const auto r = run_ok(s, req);
    CHECK(changed_slots(r) == std::set<std::string>{"b"});
    CHECK(has_reason(r.slot("b"), "context input changed"));
    CHECK(r.slot("a")->result == SlotResult::unchanged);
  }
  // Currency evidence for one action only.
  req.actions[1].build_currency = store::BuildCurrency::verified;
  {
    const auto r = run_ok(s, req);
    CHECK(changed_slots(r) == std::set<std::string>{"b"});
    CHECK(has_reason(r.slot("b"), "build currency"));
    CHECK(s.read().unit("default", "b")->build_currency == store::BuildCurrency::verified);
  }
  // A shared local input naming a module used by both.
  req.actions[1].context_inputs.push_back({a_module, store::InputRole::build_configuration});
  {
    const auto r = run_ok(s, req);
    CHECK(changed_slots(r) == std::set<std::string>{"b"});
    CHECK(has_reason(r.slot("b"), "context input added"));
  }
  append_text(a_module, "edited\n");
  CHECK(changed_slots(run_ok(s, req)) == std::set<std::string>{"a", "b"});
  // One file with two roles, or twice for one action, is refused.
  {
    ProfileIndexRequest bad = req;
    bad.actions[1].context_inputs.back().role = store::InputRole::project_descriptor;
    const Committed before = committed(s);
    const auto r = run(s, bad);
    CHECK(r.outcome == IndexOutcome::rejected);
    check_preserved(s, before, r);
    bad = req;
    bad.actions[0].context_inputs.push_back({a_module, store::InputRole::build_configuration});
    CHECK(run(s, bad).outcome == IndexOutcome::rejected);
  }
}

TEST_CASE("profile index: profiles stay isolated and cannot switch UHT applicability") {
  Fx f = make_fx("profiles");
  const fs::path variant_db = f.repo / "build-variant" / "compile_commands.json";
  {
    Fx v = f;
    v.entries[1].second = ordinary("src/b.cpp", {"/DVARIANT"});
    v.write_db(variant_db);
  }
  store::Store s = store::Store::open(f.db, f.repo);
  const ProfileIndexRequest debug = two_tu(f, "debug");
  ProfileIndexRequest variant = two_tu(f, "variant");
  variant.compile_database = variant_db;
  run_ok(s, debug);
  const auto rv = run_ok(s, variant);
  CHECK(rv.planned_analyses == 2);
  CHECK(named(all_facts(s, "debug"), "run_b") != nullptr);
  CHECK(named(all_facts(s, "debug"), "run_b_variant") == nullptr);
  CHECK(named(all_facts(s, "variant"), "run_b_variant") != nullptr);
  CHECK(named(all_facts(s, "variant"), "run_b") == nullptr);
  CHECK(run(s, debug).outcome == IndexOutcome::unchanged);
  CHECK(run(s, variant).outcome == IndexOutcome::unchanged);

  // Switching the database of one profile reanalyses that profile only.
  const Committed variant_before = committed(s, "variant");
  ProfileIndexRequest switched = debug;
  switched.compile_database = variant_db;
  const auto r = run_ok(s, switched);
  CHECK(has_reason(r.slot("a"), "context input added"));
  CHECK(has_reason(r.slot("a"), "context input removed"));
  CHECK(named(all_facts(s, "debug"), "run_b_variant") != nullptr);
  const Committed variant_after = committed(s, "variant");
  CHECK(variant_after.generation == variant_before.generation);
  CHECK(variant_after.facts == variant_before.facts);

  ProfileIndexRequest uht = switched;
  uht.uht_applicable = true;
  const Committed before = committed(s, "debug");
  const auto ru = run(s, uht);
  CHECK(ru.outcome == IndexOutcome::rejected);
  REQUIRE_FALSE(ru.errors.empty());
  CHECK(ru.errors.front().find("UHT") != std::string::npos);
  check_preserved(s, before, ru, "debug");
  CHECK_FALSE(layer(s.read().status("debug"), Layer::uht_context).applicable);
}

TEST_CASE("profile index: explicit unity membership keeps original files and remaps atomically") {
  Fx f = make_fx("unity");
  write_text(f.repo / "intermediate/unity_ab.cpp", "#include \"../src/a.cpp\"\n#include \"../src/b.cpp\"\n");
  write_text(f.repo / "intermediate/unity_a.cpp", "#include \"../src/a.cpp\"\n");
  f.entries.push_back({"intermediate/unity_ab.cpp", ordinary("intermediate/unity_ab.cpp")});
  f.entries.push_back({"intermediate/unity_a.cpp", ordinary("intermediate/unity_a.cpp")});
  f.write_db();
  store::Store s = store::Store::open(f.db, f.repo);
  const ProfileIndexRequest joined =
      request(f, {act("u1", "intermediate/unity_ab.cpp", {"src/a.cpp", "src/b.cpp"})}, "unity");
  run_ok(s, joined);
  const AnalysisResult ordinary_a = direct(f, "src/a.cpp", "a");
  {
    const FactSet all = all_facts(s, "unity");
    CHECK_FALSE(mentions_file(all, "intermediate/"));
    CHECK_FALSE(mentions_file(public_facts(s, "unity"), "intermediate/"));
    const Symbol* run_a = named(all, "run_a");
    const Symbol* run_b = named(all, "run_b");
    REQUIRE(run_a);
    REQUIRE(run_b);
    CHECK(location_units(*run_a) == std::set<std::string>{"u1"});
    const auto unity_def = definition(*run_a);
    const auto ordinary_def = definition(*named(ordinary_a.facts, "run_a"));
    REQUIRE(unity_def);
    REQUIRE(ordinary_def);
    CHECK(unity_def->location == ordinary_def->location);  // original file, span and hash
    CHECK(definition(*run_b)->location.file.generic == "src/b.cpp");
    bool member_include = false;
    for (const auto& inc : all.direct_includes()) member_include |= inc.includer.generic == "src/a.cpp";
    CHECK(member_include);
    const auto u = s.read().unit("unity", "u1");
    REQUIRE(u);
    CHECK(role_of(*u, "intermediate/unity_ab.cpp") == store::InputRole::source);
    CHECK(role_of(*u, "src/a.cpp") == store::InputRole::source);
    CHECK(role_of(*u, "src/b.cpp") == store::InputRole::source);
  }
  CHECK(run(s, joined).outcome == IndexOutcome::unchanged);

  // The declared membership must match what the analyzer observed.
  {
    const Committed before = committed(s, "unity");
    const auto r = run(s, request(f, {act("u1", "intermediate/unity_ab.cpp", {"src/a.cpp"})}, "unity"));
    CHECK(r.outcome == IndexOutcome::failed);
    CHECK(has_reason(r.slot("u1"), "membership"));
    CHECK(has_reason(r.slot("u1"), "undeclared"));
    check_preserved(s, before, r, "unity");
  }
  // Nested/intermediate unity is unsupported and never exposes fragment facts:
  // naming the originals behind a fragment, or the fragment itself, fails.
  write_text(f.repo / "intermediate/frag_ab.cpp", "#include \"../src/a.cpp\"\n#include \"../src/b.cpp\"\n");
  write_text(f.repo / "intermediate/unity_nested.cpp", "#include \"frag_ab.cpp\"\n");
  f.entries.push_back({"intermediate/unity_nested.cpp", ordinary("intermediate/unity_nested.cpp")});
  f.write_db();
  const std::string nested_id = f.command_id(f.ccdb, f.entries.size() - 1);
  CHECK(run(s, joined).outcome == IndexOutcome::unchanged);  // only another entry was added
  for (const auto& members : {std::vector<std::string>{"src/a.cpp", "src/b.cpp"},
                              std::vector<std::string>{"intermediate/frag_ab.cpp"}}) {
    CAPTURE(members.front());
    const Committed before = committed(s, "unity");
    const auto r = run(s, request(f, {act("u1", "intermediate/unity_nested.cpp", members, nested_id)}, "unity"));
    CHECK(r.outcome == IndexOutcome::failed);
    CHECK(r.slot("u1")->result == SlotResult::failed);
    check_preserved(s, before, r, "unity");
    CHECK_FALSE(mentions_file(all_facts(s, "unity"), "intermediate/"));
  }

  // A member claimed by another slot is refused before analysis.
  {
    const Committed before = committed(s, "unity");
    const auto r = run(s, request(f, {act("u1", "intermediate/unity_ab.cpp", {"src/a.cpp", "src/b.cpp"}),
                                      act("b", "src/b.cpp")},
                                  "unity"));
    CHECK(r.outcome == IndexOutcome::rejected);
    CHECK(r.actual_analyses == 0);
    check_preserved(s, before, r, "unity");
  }

  // Remapping: u1 keeps only a (new unity file), b becomes its own action, in one batch.
  const std::int64_t gen = s.read().fact_generation("unity");
  const ProfileIndexRequest split =
      request(f, {act("u1", "intermediate/unity_a.cpp", {"src/a.cpp"}), act("b", "src/b.cpp")}, "unity");
  {
    const auto r = run_ok(s, split);
    CHECK(r.generation_after == gen + 1);
    CHECK(r.slot("u1")->plan == SlotPlan::changed);
    CHECK(r.slot("b")->plan == SlotPlan::new_action);
    const FactSet all = all_facts(s, "unity");
    CHECK(location_units(*named(all, "run_b")) == std::set<std::string>{"b"});
    CHECK(location_units(*named(all, "run_a")) == std::set<std::string>{"u1"});
    CHECK(location_units(*named(all, "twice")) == std::set<std::string>{"u1", "b"});
    CHECK_FALSE(mentions_file(all, "intermediate/"));
  }
  CHECK(run(s, split).outcome == IndexOutcome::unchanged);

  // Back into one unity action, retiring b in the same batch.
  {
    const auto r = run_ok(s, joined);
    CHECK(r.slot("b")->result == SlotResult::retired);
    CHECK(r.generation_after == gen + 2);
    CHECK(location_units(*named(all_facts(s, "unity"), "run_b")) == std::set<std::string>{"u1"});
    CHECK(s.read().units("unity").size() == 1);
  }
}

TEST_CASE("profile index: a failed analysis or publication publishes nothing") {
  const Fx f = make_fx("failure");
  store::Store s = store::Store::open(f.db, f.repo);
  ProfileIndexRequest req = two_tu(f);
  run_ok(s, req);
  const std::string good_a = read_text(f.repo / "src/a.cpp");
  const std::string good_b = read_text(f.repo / "src/b.cpp");

  // a fails first: b is not analysed and nothing is published.
  write_text(f.repo / "src/a.cpp", "#include \"shared.h\"\nint broken( {\n");
  replace_text(f.repo / "src/b.cpp", "geo::apply(callback, 1)", "geo::apply(callback, 3)");
  {
    const Committed before = committed(s);
    const auto r = run(s, req);
    CHECK(r.outcome == IndexOutcome::failed);
    CHECK(r.slot("a")->result == SlotResult::failed);
    CHECK(r.slot("b")->result == SlotResult::not_analyzed);
    CHECK(r.planned_analyses == 2);
    CHECK(r.actual_analyses == 1);
    CHECK(r.failure_recorded);
    check_preserved(s, before, r);
    const auto st = s.read().status("default");
    CHECK(st.failed_slots == std::vector<std::string>{"a", "b"});
  }

  // b fails after a analysed: a's good analysis is not published either.
  write_text(f.repo / "src/a.cpp", good_a + "\nint extra_a() { return 2; }\n");
  write_text(f.repo / "src/b.cpp", "#include \"shared.h\"\nint broken( {\n");
  {
    const Committed before = committed(s);
    const auto r = run(s, req);
    CHECK(r.outcome == IndexOutcome::failed);
    CHECK(r.slot("a")->result == SlotResult::not_published);
    CHECK(r.slot("b")->result == SlotResult::failed);
    CHECK(r.actual_analyses == 2);
    check_preserved(s, before, r);
    CHECK(named(all_facts(s), "extra_a") == nullptr);
  }

  // A SQL failure inside the replacement transaction rolls everything back.
  write_text(f.repo / "src/b.cpp", good_b);
  {
    ProfileIndexRequest faulty = req;
    faulty.publish_observer = [](store::PublishPhase phase) {
      if (phase == store::PublishPhase::old_removed) throw std::runtime_error("injected failure");
    };
    const Committed before = committed(s);
    const auto r = run(s, faulty);
    CHECK(r.outcome == IndexOutcome::failed);
    CHECK(r.message.find("injected failure") != std::string::npos);
    CHECK(r.failure_recorded);
    check_preserved(s, before, r);
  }
  const auto r = run_ok(s, req);
  CHECK(changed_slots(r) == std::set<std::string>{"a"});
  CHECK(named(all_facts(s), "extra_a") != nullptr);
}

TEST_CASE("profile index: changes during a run are rejected under the writer lock") {
  const Fx f = make_fx("race");
  store::Store s = store::Store::open(f.db, f.repo);
  const ProfileIndexRequest req = two_tu(f);
  run_ok(s, req);
  int edit = 0;
  const auto edit_b = [&] {
    replace_text(f.repo / "src/b.cpp", "geo::apply(callback, ", "geo::apply(callback, " + std::to_string(++edit) + " + ");
  };
  const auto expect_rejected = [&](ProfileIndexRequest r, const std::string& text) {
    const Committed before = committed(s);
    const auto out = run(s, r);
    CHECK(out.outcome == IndexOutcome::failed);
    CHECK_MESSAGE(out.message.find(text) != std::string::npos, out.message);
    check_preserved(s, before, out);
  };

  // A source edited after its analysis.
  edit_b();
  {
    ProfileIndexRequest r = req;
    r.observer = [&](IndexPhase p) {
      if (p == IndexPhase::analyzed) append_text(f.repo / "src/b.cpp", "// raced\n");
    };
    expect_rejected(r, "input changed after analysis");
  }
  // A dependency of an unchanged slot edited during the run.
  {
    ProfileIndexRequest r = req;
    r.observer = [&](IndexPhase p) {
      if (p == IndexPhase::analyzed) append_text(f.repo / "include/solo.h", "// raced\n");
    };
    expect_rejected(r, "unchanged slot 'a'");
  }
  CHECK(changed_slots(run_ok(s, req)) == std::set<std::string>{"a", "b"});

  // An unchanged run is revalidated under the lock too.
  {
    ProfileIndexRequest r = req;
    r.observer = [&](IndexPhase p) {
      if (p == IndexPhase::planned) append_text(f.repo / "include/solo.h", "// raced no-op\n");
    };
    const Committed before = committed(s);
    const auto out = run(s, r);
    CHECK(out.planned_analyses == 0);
    CHECK(out.outcome == IndexOutcome::failed);
    CHECK(out.message.find("unchanged slot 'a'") != std::string::npos);
    check_preserved(s, before, out);
  }
  CHECK(changed_slots(run_ok(s, req)) == std::set<std::string>{"a"});
  {
    ProfileIndexRequest r = req;
    r.observer = [&](IndexPhase p) {
      if (p == IndexPhase::planned) append_text(f.toolchain, "raced no-op\n");
    };
    const auto out = run(s, r);
    CHECK(out.outcome == IndexOutcome::failed);
    CHECK(out.message.find("context input changed during the run") != std::string::npos);
  }
  run_ok(s, req);
  {
    ProfileIndexRequest r = req;
    r.observer = [&](IndexPhase p) {
      if (p != IndexPhase::planned) return;
      store::Store other = store::Store::open(f.db, f.repo);
      auto lock = other.acquire_writer(5s);
      REQUIRE(lock);
      store::Batch b;
      b.profile = "default";
      b.retire = {"b"};
      REQUIRE(other.publish(*lock, b).outcome == store::PublishOutcome::committed);
    };
    const auto out = run(s, r);
    CHECK(out.outcome == IndexOutcome::failed);
    CHECK(out.message.find("published concurrently") != std::string::npos);
  }
  run_ok(s, req);

  // The compile database changed after the lock was taken, before revalidation.
  edit_b();
  {
    ProfileIndexRequest r = req;
    r.observer = [&](IndexPhase p) {
      if (p == IndexPhase::locked) append_text(f.ccdb, "\n");
    };
    expect_rejected(r, "context input changed during the run");
  }
  run_ok(s, req);
  // A toolchain descriptor changed during analysis.
  edit_b();
  {
    ProfileIndexRequest r = req;
    r.observer = [&](IndexPhase p) {
      if (p == IndexPhase::analyzed) append_text(f.toolchain, "raced\n");
    };
    expect_rejected(r, "context input changed during the run");
  }
  run_ok(s, req);

  // Another publication between planning and the lock.
  edit_b();
  {
    ProfileIndexRequest r = req;
    r.observer = [&](IndexPhase p) {
      if (p != IndexPhase::analyzed) return;
      store::Store other = store::Store::open(f.db, f.repo);
      auto lock = other.acquire_writer(5s);
      REQUIRE(lock);
      store::Batch b;
      b.profile = "default";
      b.retire = {"a"};
      REQUIRE(other.publish(*lock, b).outcome == store::PublishOutcome::committed);
    };
    const auto out = run(s, r);
    CHECK(out.outcome == IndexOutcome::failed);
    CHECK(out.message.find("published concurrently") != std::string::npos);
    CHECK(s.read().unit("default", "a") == std::nullopt);  // only the other writer's change
  }
  run_ok(s, req);

  // Another writer holds the lock: busy, nothing published.
  edit_b();
  {
    store::Store other = store::Store::open(f.db, f.repo);
    std::optional<store::WriterLock> held;
    ProfileIndexRequest r = req;
    r.lock_wait = 100ms;
    r.observer = [&](IndexPhase p) {
      if (p == IndexPhase::analyzed) held = other.acquire_writer(5s);
    };
    const Committed before = committed(s);
    const auto out = run(s, r);
    REQUIRE(held);
    CHECK(out.outcome == IndexOutcome::busy);
    check_preserved(s, before, out);
  }
  CHECK(changed_slots(run_ok(s, req)) == std::set<std::string>{"b"});
}

TEST_CASE("profile index: a concurrent publication fails the run and is recorded in WAL and rollback modes") {
  for (const auto mode : {store::JournalMode::wal, store::JournalMode::rollback}) {
    const std::string mode_name(store::to_string(mode));
    CAPTURE(mode_name);
    const Fx f = make_fx("generation-" + mode_name);
    store::StoreOptions options;
    options.journal = mode;
    options.busy_timeout = 1000ms;
    store::Store s = store::Store::open(f.db, f.repo, options);
    REQUIRE(s.journal_mode() == mode);
    const ProfileIndexRequest req = two_tu(f);
    run_ok(s, req);

    // With a planned change (race after analysis) and for an unchanged run (race after planning).
    for (const IndexPhase phase : {IndexPhase::analyzed, IndexPhase::planned}) {
      const bool planned_change = phase == IndexPhase::analyzed;
      if (planned_change) replace_text(f.repo / "src/b.cpp", "geo::apply(callback, ", "geo::apply(callback, 7 + ");
      const std::string victim = planned_change ? "a" : "b";
      std::int64_t other_attempt = 0;
      ProfileIndexRequest r = req;
      r.observer = [&](IndexPhase p) {
        if (p != phase) return;
        store::Store other = store::Store::open(f.db, f.repo, options);
        REQUIRE(other.journal_mode() == mode);
        auto lock = other.acquire_writer(5s);
        REQUIRE(lock);
        store::Batch b;
        b.profile = "default";
        b.retire = {victim};
        const auto published = other.publish(*lock, b);
        REQUIRE(published.outcome == store::PublishOutcome::committed);
        other_attempt = published.attempt_id.value();
      };
      const std::int64_t gen = s.read().fact_generation("default");
      const auto out = run(s, r);
      CHECK(out.outcome == IndexOutcome::failed);
      CHECK(out.message.find("published concurrently") != std::string::npos);
      CHECK(out.message.find("not recorded") == std::string::npos);
      CHECK(out.planned_analyses == (planned_change ? 1u : 0u));
      CHECK(out.failure_recorded);
      REQUIRE(out.attempt_id);
      CHECK(out.generation_after == gen + 1);
      {
        const auto snap = s.read();
        CHECK(snap.fact_generation("default") == gen + 1);  // the concurrent commit stays intact
        CHECK_FALSE(snap.unit("default", victim).has_value());
        CHECK(snap.units("default").size() == 1);
        const auto st = snap.status("default");
        REQUIRE(st.last_attempt);
        CHECK(st.last_attempt->id == *out.attempt_id);
        CHECK(st.last_attempt->state == store::AttemptState::failed);
        REQUIRE(st.last_success);
        CHECK(st.last_success->id == other_attempt);
      }
      const auto recovered = run_ok(s, req);
      CHECK(recovered.slot(victim)->plan == SlotPlan::new_action);
    }
  }
}

TEST_CASE("profile index: exclusion and deletion are withheld publicly without faking layers") {
  const Fx f = make_fx("visibility");
  store::Store s = store::Store::open(f.db, f.repo);
  const ProfileIndexRequest req = two_tu(f);
  run_ok(s, req);
  CHECK(mentions_file(public_facts(s), "include/excluded.h"));
  {
    auto lock = s.acquire_writer(5s);
    REQUIRE(lock);
    s.set_excluded_files(*lock, "default", {store::test::repo("include/excluded.h")});
  }
  CHECK_FALSE(mentions_file(public_facts(s), "include/excluded.h"));
  CHECK(mentions_file(all_facts(s), "include/excluded.h"));
  CHECK(run(s, req).outcome == IndexOutcome::unchanged);  // a projection input, not an analysis change

  fs::remove(f.repo / "include/solo.h");
  CHECK_FALSE(mentions_file(public_facts(s), "include/solo.h"));
  const Committed before = committed(s);
  const auto r = run(s, req);
  CHECK(r.outcome == IndexOutcome::failed);
  CHECK(has_reason(r.slot("a"), "input deleted"));
  check_preserved(s, before, r);
  CHECK_FALSE(mentions_file(public_facts(s), "include/solo.h"));
  CHECK(named(public_facts(s), "solo_value") == nullptr);
  CHECK(named(public_facts(s), "run_b") != nullptr);
  const auto st = s.read().status("default");
  CHECK(layer(st, Layer::retrieval).availability == Availability::missing);
  CHECK(layer(st, Layer::deep).availability == Availability::missing);
  CHECK_FALSE(layer(st, Layer::uht_context).applicable);
  CHECK(st.last_attempt->state == store::AttemptState::failed);
}

TEST_CASE("profile index: an explicit PCH producer is reused and must apply") {
  Fx f = make_fx("pch");
  const std::string repo = path_to_utf8_generic(f.repo);
  write_text(f.repo / "pch/shared_decl.h", "#pragma once\nstruct FromPch { int value; };\nint pch_only_function();\n");
  write_text(f.repo / "pch/Wrapper.h",
             "#pragma once\n#define PCH_MACRO 41\n#include \"shared_decl.h\"\n#if defined(PCH_MACRO)\n"
             "#define PCH_CONDITIONAL 1\n#endif\n");
  write_text(f.repo / "pch/Wrapper.h.pch", "VCPCH0 not a clang pch at all");
  write_text(f.repo / "pch/Wrapper.cpp", "#include \"Wrapper.h\"\n");
  write_text(f.repo / "src/main.cpp",
             "#ifndef PCH_CONDITIONAL\n#error the PCH prefix did not reach this translation unit\n#endif\n"
             "int uses_pch() { FromPch v{PCH_MACRO}; return v.value + pch_only_function(); }\n");
  const Args base{"cl.exe", "/c", "/std:c++20", "/EHsc", "/TP"};
  Args producer = base;
  producer.insert(producer.end(), {"/YcWrapper.h", "/Fp" + repo + "/pch/Wrapper.h.pch", repo + "/pch/Wrapper.cpp"});
  Args consumer = base;
  consumer.insert(consumer.end(), {"/FI" + repo + "/pch/Wrapper.h", "/Yu" + repo + "/pch/Wrapper.h",
                                   "/Fp" + repo + "/pch/Wrapper.h.pch", repo + "/src/main.cpp"});
  f.entries.push_back({repo + "/pch/Wrapper.cpp", producer});
  f.entries.push_back({repo + "/src/main.cpp", consumer});
  f.write_db();
  const std::string producer_id = f.command_id(f.ccdb, 2);
  store::Store s = store::Store::open(f.db, f.repo);

  // Without the producer the consumer rejects, failing the run.
  CHECK(run(s, request(f, {act("main", "src/main.cpp")})).outcome == IndexOutcome::failed);

  SelectedAction with_producer = act("main", "src/main.cpp");
  with_producer.pch_producer_command_id = producer_id;
  const ProfileIndexRequest req = request(f, {with_producer});
  run_ok(s, req);
  const auto u = s.read().unit("default", "main");
  REQUIRE(u);
  CHECK(u->provenance.pch_producer_command_id == producer_id);
  CHECK(u->provenance.pch_producer_replay_id.has_value());
  for (const auto& in : u->inputs) CHECK_MESSAGE(fs::exists(path_from_utf8(in.path_utf8)), in.path_utf8);  // no virtual guard
  CHECK(named(all_facts(s), "uses_pch") != nullptr);
  CHECK(run(s, req).outcome == IndexOutcome::unchanged);

  // The producer's wrapper header is a dependency of the consumer.
  append_text(f.repo / "pch/shared_decl.h", "// edited\n");
  CHECK(changed_slots(run_ok(s, req)) == std::set<std::string>{"main"});

  // A producer supplied for a unit that uses no PCH is an inconsistent mapping.
  SelectedAction plain = act("a", "src/a.cpp");
  plain.pch_producer_command_id = producer_id;
  const Committed before = committed(s);
  const auto r = run(s, request(f, {with_producer, plain}));
  CHECK(r.outcome == IndexOutcome::failed);
  CHECK(has_reason(r.slot("a"), "not used"));
  check_preserved(s, before, r);
}

namespace {

// Facts of `f` located in (or, for includes, written in) the repository file `generic`.
struct LocatedIn {
  std::size_t locations = 0;
  std::size_t evidence = 0;
  std::size_t unresolved = 0;
  std::size_t captures = 0;
  std::vector<DirectIncludeFact> includes;
};

LocatedIn located_in_file(const FactSet& f, const std::string& generic) {
  LocatedIn out;
  for (const auto& [id, s] : f.symbols()) {
    for (const auto& l : s.locations) out.locations += l.location.file.generic == generic;
  }
  for (const auto& [k, r] : f.relations()) {
    for (const auto& e : r.evidence) out.evidence += e.location.file.generic == generic;
  }
  for (const auto& u : f.unresolved_sites()) out.unresolved += u.location.file.generic == generic;
  for (const auto& c : f.captures()) out.captures += c.location.file.generic == generic;
  for (const auto& inc : f.direct_includes()) {
    if (inc.includer.generic == generic) out.includes.push_back(inc);
  }
  return out;
}

}  // namespace

TEST_CASE("profile index: includes-only unity members are originals; a member including a TU source is a fragment") {
  Fx f = make_fx("members");
  write_text(f.repo / "src/m_header.cpp", "#include \"solo.h\"\n");
  write_text(f.repo / "src/m_macro.cpp", "#define MEMBER_HEADER \"shared.h\"\n#include MEMBER_HEADER\n");
  write_text(f.repo / "src/m_external.cpp", "#define MEMBER_EXTERNAL <ext.h>\n#include MEMBER_EXTERNAL\n");
  write_text(f.repo / "src/m_empty.cpp", "// nothing but a comment\n");
  write_text(f.repo / "src/m_inactive.cpp", "#if 0\n#include \"b.cpp\"\n#endif\n#include \"solo.h\"\n");
  const std::vector<std::string> only{"src/m_header.cpp", "src/m_macro.cpp", "src/m_external.cpp", "src/m_empty.cpp",
                                      "src/m_inactive.cpp"};
  std::vector<std::string> members{"src/a.cpp"};
  members.insert(members.end(), only.begin(), only.end());
  const auto unity_of = [](const std::vector<std::string>& ms) {
    std::string text;
    for (const auto& m : ms) text += "#include \"../" + m + "\"\n";
    return text;
  };
  write_text(f.repo / "intermediate/unity_members.cpp", unity_of(members));
  f.entries.push_back({"intermediate/unity_members.cpp", ordinary("intermediate/unity_members.cpp")});
  f.write_db();
  store::Store s = store::Store::open(f.db, f.repo);
  const ProfileIndexRequest req = request(f, {act("u", "intermediate/unity_members.cpp", members)}, "members");
  run_ok(s, req);
  {
    const FactSet all = all_facts(s, "members");
    CHECK(named(all, "run_a") != nullptr);
    CHECK_FALSE(mentions_file(all, "intermediate/"));
    const auto u = s.read().unit("members", "u");
    REQUIRE(u);
    for (const auto& m : only) {
      CAPTURE(m);
      CHECK(role_of(*u, m) == store::InputRole::source);
      // No facts from the member's own bytes: only its direct includes.
      const LocatedIn in = located_in_file(all, m);
      CHECK(in.locations == 0);
      CHECK(in.evidence == 0);
      CHECK(in.unresolved == 0);
      CHECK(in.captures == 0);
    }
    const auto includes_of = [&](const std::string& m) { return located_in_file(all, m).includes; };
    REQUIRE(includes_of("src/m_header.cpp").size() == 1);
    CHECK(includes_of("src/m_header.cpp")[0].resolution == IncludeResolution::resolved_internal);
    CHECK(includes_of("src/m_header.cpp")[0].resolved_repo_target->generic == "include/solo.h");
    const auto macro = includes_of("src/m_macro.cpp");
    REQUIRE(macro.size() == 1);
    CHECK(macro[0].operand_kind == IncludeOperandKind::macro_tokens);
    CHECK(macro[0].activity == IncludeActivity::active);
    CHECK(macro[0].resolved_repo_target->generic == "include/shared.h");
    const auto external = includes_of("src/m_external.cpp");
    REQUIRE(external.size() == 1);
    CHECK(external[0].resolution == IncludeResolution::resolved_external);
    CHECK(includes_of("src/m_empty.cpp").empty());
    const auto inactive = includes_of("src/m_inactive.cpp");
    REQUIRE(inactive.size() == 2);
    std::set<IncludeActivity> activities;
    for (const auto& inc : inactive) activities.insert(inc.activity);
    CHECK(activities == std::set<IncludeActivity>{IncludeActivity::active, IncludeActivity::inactive});
    // The member's include facts are public like any original file's.
    CHECK(located_in_file(public_facts(s, "members"), "src/m_macro.cpp").includes.size() == 1);
  }
  CHECK(run(s, req).outcome == IndexOutcome::unchanged);

  // A byte change of an includes-only member reanalyses its action.
  append_text(f.repo / "src/m_header.cpp", "// edited\n");
  {
    const auto r = run_ok(s, req);
    CHECK(changed_slots(r) == std::set<std::string>{"u"});
    CHECK(has_reason(r.slot("u"), "m_header.cpp"));
  }

  // A member directly including another translation-unit source is a nested
  // fragment, with or without content of its own, through a macro operand and
  // for a generated .gen.cpp; nothing is published.
  write_text(f.repo / "gen/thing.gen.cpp", "int generated_thing() { return 3; }\n");
  const std::vector<std::pair<std::string, std::string>> fragments{
      {"src/f_plain.cpp", "#include \"b.cpp\"\n"},
      {"src/f_content.cpp", "#include \"b.cpp\"\nint own_fragment() { return 7; }\n"},
      {"src/f_macro.cpp", "#define FRAGMENT \"b.cpp\"\n#include FRAGMENT\n"},
      {"src/f_gen.cpp", "#include \"../gen/thing.gen.cpp\"\n"},
  };
  for (const auto& [member, text] : fragments) {
    CAPTURE(member);
    write_text(f.repo / member, text);
    const std::string unity = "intermediate/unity_" + fs::path(member).stem().string() + ".cpp";
    write_text(f.repo / unity, unity_of({"src/a.cpp", member}));
    f.entries.push_back({unity, ordinary(unity)});
    f.write_db();
    const Committed before = committed(s, "members");
    const auto r = run(s, request(f, {act("u", unity, {"src/a.cpp", member})}, "members"));
    CHECK(r.outcome == IndexOutcome::failed);
    CHECK(r.slot("u")->result == SlotResult::failed);
    CHECK(has_reason(r.slot("u"), "directly includes the translation-unit source"));
    CHECK(r.failure_recorded);
    check_preserved(s, before, r, "members");
    CHECK(located_in_file(all_facts(s, "members"), member).includes.empty());
    CHECK(named(all_facts(s, "members"), "own_fragment") == nullptr);
  }
  CHECK(run(s, req).outcome == IndexOutcome::unchanged);
}

TEST_CASE("profile index: database entries schedule only their own slots; the database is still validated") {
  Fx f = make_fx("entries");
  write_text(f.repo / "src/c.cpp", "#include \"shared.h\"\nint run_c() { return geo::twice(3); }\n");
  f.entries.push_back({"src/c.cpp", ordinary("src/c.cpp")});  // never selected
  f.write_db();
  store::Store s = store::Store::open(f.db, f.repo);
  ProfileIndexRequest req = two_tu(f);
  req.build_currency = store::BuildCurrency::verified;
  run_ok(s, req);
  store::InspectRequest inspect;
  inspect.profile = "default";
  {
    const auto fr = s.inspect_freshness(inspect);
    REQUIRE(fr.outcome == store::InspectionOutcome::complete);
    for (const auto& u : fr.units) CHECK(u.compile_context == Freshness::fresh);
  }
  const std::string id_a = f.command_id(f.ccdb, 0);
  const std::string id_b = f.command_id(f.ccdb, 1);
  const auto unchanged_after = [&](const char* what) {
    CAPTURE(what);
    const Committed before = committed(s);
    const auto r = run(s, req);
    CHECK(r.outcome == IndexOutcome::unchanged);
    CHECK(r.planned_analyses == 0);
    CHECK(r.actual_analyses == 0);
    CHECK(r.slot("a")->command_id == id_a);
    CHECK(r.slot("b")->command_id == id_b);
    check_preserved(s, before, r);
  };
  write_text(f.ccdb, "\n  " + read_text(f.ccdb) + "\n\n");
  unchanged_after("whitespace");
  std::swap(f.entries[0], f.entries[2]);
  f.write_db();
  unchanged_after("entry order");
  std::swap(f.entries[0], f.entries[2]);
  f.entries[2].second = ordinary("src/c.cpp", {"/DC_VARIANT"});
  f.write_db();
  unchanged_after("unselected entry changed");

  // Documented state after an unrelated database edit: the units keep the
  // database hash they were analysed with, so a (compiler-free) inspection,
  // which does not parse the database, reports the compile context stale.
  // Scheduling stays per entry: the next run is still unchanged.
  {
    const auto fr = s.inspect_freshness(inspect);
    REQUIRE(fr.outcome == store::InspectionOutcome::complete);
    for (const auto& u : fr.units) {
      CHECK(u.compile_context == Freshness::stale);
      CHECK(u.semantic == Freshness::stale);
    }
    CHECK_FALSE(fr.guidance.empty());
  }
  unchanged_after("after inspection");
  f.entries.pop_back();
  f.write_db();
  unchanged_after("unselected entry removed");
  f.entries.push_back({"src/c.cpp", ordinary("src/c.cpp")});
  f.write_db();
  unchanged_after("unselected entry added");
  // Back to the analysed bytes: the same inspection finds the context fresh again.
  {
    const auto fr = s.inspect_freshness(inspect);
    REQUIRE(fr.outcome == store::InspectionOutcome::complete);
    for (const auto& u : fr.units) CHECK(u.compile_context == Freshness::fresh);
  }

  // The selected entry of one slot changes: only that slot is reanalysed.
  f.entries[0].second = ordinary("src/a.cpp", {"/DA_VARIANT"});
  f.write_db();
  {
    const auto r = run_ok(s, req);
    CHECK(changed_slots(r) == std::set<std::string>{"a"});
    CHECK(has_reason(r.slot("a"), "selected command changed"));
    CHECK(r.actual_analyses == 1);
  }

  // Invalid databases still reject before any analysis.
  const auto rejected = [&](const char* what, const std::string& error) {
    CAPTURE(what);
    const Committed before = committed(s);
    const auto r = run(s, req);
    CHECK(r.outcome == IndexOutcome::rejected);
    CHECK(r.actual_analyses == 0);
    REQUIRE_FALSE(r.errors.empty());
    CHECK(r.errors.front().find(error) != std::string::npos);
    check_preserved(s, before, r);
  };
  const auto saved = f.entries;
  f.entries.erase(f.entries.begin() + 1);
  f.write_db();
  rejected("selected entry removed", "no compile command");
  f.entries = saved;
  f.entries.push_back({"src/b.cpp", ordinary("src/b.cpp", {"/DOTHER"})});
  f.write_db();
  rejected("new ambiguity", "several commands");
  f.entries = saved;
  write_text(f.ccdb, "[{\"directory\": 1");
  rejected("malformed", "malformed compile database");
  fs::remove(f.ccdb);
  rejected("deleted", "missing or unreadable");
  f.write_db();
  CHECK(run(s, req).outcome == IndexOutcome::unchanged);
}

TEST_CASE("profile index: a shared PCH producer entry and its response file schedule every consumer naming it") {
  Fx f = make_fx("producer");
  const std::string repo = path_to_utf8_generic(f.repo);
  write_text(f.repo / "pch/Wrapper.h", "#pragma once\n#define PCH_MACRO 41\nstruct FromPch { int value; };\n");
  write_text(f.repo / "pch/Wrapper.h.pch", "VCPCH0 not a clang pch at all");
  write_text(f.repo / "pch/Wrapper.cpp", "#include \"Wrapper.h\"\n");
  write_text(f.repo / "pch/producer.rsp", "/c /std:c++20 /EHsc /TP\n");
  write_text(f.repo / "src/one.cpp", "int one() { FromPch v{PCH_MACRO}; return v.value; }\n");
  write_text(f.repo / "src/two.cpp", "int two() { FromPch v{PCH_MACRO}; return v.value + 1; }\n");
  const Args producer{"cl.exe", "@pch/producer.rsp", "/YcWrapper.h", "/Fp" + repo + "/pch/Wrapper.h.pch",
                      repo + "/pch/Wrapper.cpp"};
  const auto consumer = [&](const std::string& file) {
    return Args{"cl.exe", "/c", "/std:c++20", "/EHsc", "/TP", "/FI" + repo + "/pch/Wrapper.h",
                "/Yu" + repo + "/pch/Wrapper.h", "/Fp" + repo + "/pch/Wrapper.h.pch", repo + "/" + file};
  };
  f.entries.push_back({repo + "/pch/Wrapper.cpp", producer});
  f.entries.push_back({repo + "/src/one.cpp", consumer("src/one.cpp")});
  f.entries.push_back({repo + "/src/two.cpp", consumer("src/two.cpp")});
  f.write_db();
  store::Store s = store::Store::open(f.db, f.repo);
  const auto req = [&] {
    std::vector<SelectedAction> actions{act("a", "src/a.cpp"), act("one", "src/one.cpp"), act("two", "src/two.cpp")};
    actions[1].pch_producer_command_id = f.command_id(f.ccdb, 2);
    actions[2].pch_producer_command_id = f.command_id(f.ccdb, 2);
    return request(f, actions);
  };
  run_ok(s, req());
  CHECK(run(s, req()).outcome == IndexOutcome::unchanged);

  // The producer's response file: exactly its consumers.
  append_text(f.repo / "pch/producer.rsp", "/W4\n");
  {
    const auto r = run_ok(s, req());
    CHECK(changed_slots(r) == std::set<std::string>{"one", "two"});
    CHECK(has_reason(r.slot("one"), "producer.rsp"));
  }
  // The producer entry itself: every consumer naming it.
  f.entries[2].second.insert(f.entries[2].second.begin() + 1, "/W3");
  f.write_db();
  {
    const auto r = run_ok(s, req());
    CHECK(changed_slots(r) == std::set<std::string>{"one", "two"});
    CHECK(has_reason(r.slot("two"), "PCH producer selection changed"));
  }
  CHECK(run(s, req()).outcome == IndexOutcome::unchanged);
}

TEST_CASE("profile index: bounded parallel analysis publishes the serial result and fails atomically") {
  Fx f = make_fx("jobs");
  write_text(f.repo / "src/c.cpp", "#include \"shared.h\"\nint run_c() { return geo::twice(3); }\n");
  write_text(f.repo / "src/d.cpp", "#include \"solo.h\"\nint run_d() { return solo_value(); }\n");
  f.entries.push_back({"src/c.cpp", ordinary("src/c.cpp")});
  f.entries.push_back({"src/d.cpp", ordinary("src/d.cpp")});
  f.write_db();
  const auto four = [&](std::size_t jobs) {
    ProfileIndexRequest r = request(f, {act("a", "src/a.cpp"), act("b", "src/b.cpp"), act("c", "src/c.cpp"),
                                        act("d", "src/d.cpp")});
    r.jobs = jobs;
    return r;
  };
  store::Store serial = store::Store::open(f.dir / "serial.sqlite", f.repo);
  store::Store s = store::Store::open(f.db, f.repo);
  run_ok(serial, four(1));
  {
    const auto r = run_ok(s, four(4));
    CHECK(r.actual_analyses == 4);
    std::vector<std::string> order;
    for (const auto& slot : r.slots) {
      order.push_back(slot.slot);
      CHECK(slot.result == SlotResult::installed);
    }
    CHECK(order == std::vector<std::string>{"a", "b", "c", "d"});
  }
  // Identical facts and dependency records.
  CHECK(all_facts(serial).fact_hash() == all_facts(s).fact_hash());
  CHECK(public_facts(serial).fact_hash() == public_facts(s).fact_hash());
  for (const char* slot : {"a", "b", "c", "d"}) {
    CAPTURE(slot);
    const auto x = serial.read().unit("default", slot);
    const auto y = s.read().unit("default", slot);
    REQUIRE(x);
    REQUIRE(y);
    CHECK(x->provenance == y->provenance);
    CHECK(x->includes == y->includes);
    CHECK(x->diagnostics == y->diagnostics);
    CHECK(x->limits == y->limits);
    REQUIRE(x->fingerprints.size() == y->fingerprints.size());
    for (std::size_t i = 0; i < x->fingerprints.size(); ++i) CHECK(x->fingerprints[i].value == y->fingerprints[i].value);
  }
  CHECK(run(s, four(4)).outcome == IndexOutcome::unchanged);

  // Two failures while every planned analysis runs (jobs >= planned): both are
  // reported, the good analyses are not published, and the run names the first
  // failure in selection order.
  const std::string good_b = read_text(f.repo / "src/b.cpp");
  const std::string good_d = read_text(f.repo / "src/d.cpp");
  append_text(f.repo / "src/a.cpp", "\nint extra_a() { return 2; }\n");
  write_text(f.repo / "src/b.cpp", "#include \"shared.h\"\nint broken_b( {\n");
  append_text(f.repo / "src/c.cpp", "\nint extra_c() { return 3; }\n");
  write_text(f.repo / "src/d.cpp", "#include \"solo.h\"\nint broken_d( {\n");
  {
    const Committed before = committed(s);
    const auto r = run(s, four(8));
    CHECK(r.outcome == IndexOutcome::failed);
    CHECK(r.actual_analyses == 4);
    CHECK(r.slot("a")->result == SlotResult::not_published);
    CHECK(r.slot("b")->result == SlotResult::failed);
    CHECK(r.slot("c")->result == SlotResult::not_published);
    CHECK(r.slot("d")->result == SlotResult::failed);
    CHECK(r.message.find("slot 'b'") != std::string::npos);
    CHECK(r.failure_recorded);
    check_preserved(s, before, r);
    CHECK(named(all_facts(s), "extra_a") == nullptr);
    const auto st = s.read().status("default");
    CHECK(st.failed_slots == std::vector<std::string>{"a", "b", "c", "d"});
  }
  // With fewer jobs than planned analyses, a slot is either analysed (and
  // reported) or not started; nothing is published either way.
  {
    const Committed before = committed(s);
    const auto r = run(s, four(2));
    CHECK(r.outcome == IndexOutcome::failed);
    CHECK(r.slot("b")->result == SlotResult::failed);
    CHECK(r.message.find("slot 'b'") != std::string::npos);
    for (const char* slot : {"a", "c", "d"}) {
      CAPTURE(slot);
      const SlotResult res = r.slot(slot)->result;
      CHECK((res == SlotResult::not_published || res == SlotResult::not_analyzed || res == SlotResult::failed));
    }
    check_preserved(s, before, r);
  }
  write_text(f.repo / "src/b.cpp", good_b);
  write_text(f.repo / "src/d.cpp", good_d);

  // Another writer holds the lock after the parallel analyses: busy, nothing published.
  {
    store::Store other = store::Store::open(f.db, f.repo);
    std::optional<store::WriterLock> held;
    ProfileIndexRequest r = four(4);
    r.lock_wait = 100ms;
    r.observer = [&](IndexPhase p) {
      if (p == IndexPhase::analyzed) held = other.acquire_writer(5s);
    };
    const Committed before = committed(s);
    const auto out = run(s, r);
    REQUIRE(held);
    CHECK(out.outcome == IndexOutcome::busy);
    CHECK(out.actual_analyses == 2);  // b and d are back to their committed bytes
    check_preserved(s, before, out);
  }
  CHECK(changed_slots(run_ok(s, four(4))) == std::set<std::string>{"a", "c"});
  CHECK(named(all_facts(s), "extra_c") != nullptr);
}

TEST_CASE("profile index: concurrent PCH consumers run their driver phase one at a time and agree") {
  // Regression: concurrent Clang driver phases corrupted the heap (about 1 in 12
  // two-job runs before DriverPhase). Every round analyses all consumers at once.
  Fx f = make_fx("concurrent");
  const std::string repo = path_to_utf8_generic(f.repo);
  write_text(f.repo / "pch/Wrapper.h", "#pragma once\n#define PCH_MACRO 41\nstruct FromPch { int value; };\n");
  write_text(f.repo / "pch/Wrapper.h.pch", "VCPCH0 not a clang pch at all");
  write_text(f.repo / "pch/Wrapper.cpp", "#include \"Wrapper.h\"\n");
  f.entries.push_back({repo + "/pch/Wrapper.cpp",
                       {"cl.exe", "/c", "/std:c++20", "/EHsc", "/TP", "/YcWrapper.h", "/Fp" + repo + "/pch/Wrapper.h.pch",
                        repo + "/pch/Wrapper.cpp"}});
  std::vector<SelectedAction> actions;
  for (int i = 0; i < 8; ++i) {
    const std::string file = "src/c" + std::to_string(i) + ".cpp";
    write_text(f.repo / file, "int c" + std::to_string(i) + "() { FromPch v{PCH_MACRO}; return v.value + " +
                                  std::to_string(i) + "; }\n");
    f.entries.push_back({repo + "/" + file,
                         {"cl.exe", "/c", "/std:c++20", "/EHsc", "/TP", "/FI" + repo + "/pch/Wrapper.h",
                          "/Yu" + repo + "/pch/Wrapper.h", "/Fp" + repo + "/pch/Wrapper.h.pch", repo + "/" + file}});
    actions.push_back(act("c" + std::to_string(i), file));
  }
  f.write_db();
  const std::string producer_id = f.command_id(f.ccdb, 2);
  for (auto& a : actions) a.pch_producer_command_id = producer_id;
  store::Store s = store::Store::open(f.db, f.repo);
  std::optional<Sha256Digest> first;
  for (int round = 0; round < 5; ++round) {
    CAPTURE(round);
    ProfileIndexRequest req = request(f, actions, "round" + std::to_string(round));
    req.jobs = 8;
    const auto r = run_ok(s, req);
    CHECK(r.actual_analyses == 8);
    const Sha256Digest facts = all_facts(s, req.profile).fact_hash();
    if (!first) first = facts;
    CHECK(facts == *first);
  }
}

TEST_CASE("profile index: finished analyses are spooled next to the database and the spool is removed") {
  const Fx f = make_fx("spooled");
  const auto spool_files = [&] {
    std::size_t n = 0;
    for (const auto& e : fs::directory_iterator(f.db.parent_path())) {
      n += path_to_utf8_generic(e.path().filename()).rfind("lcm-spool-", 0) == 0 ? 1 : 0;
    }
    return n;
  };
  store::Store s = store::Store::open(f.db, f.repo);
  ProfileIndexRequest req = two_tu(f);
  req.jobs = 2;
  bool seen_during_analysis = false;
  req.observer = [&](IndexPhase p) {
    if (p == IndexPhase::analyzed) seen_during_analysis = spool_files() == 1;
  };
  run_ok(s, req);
  CHECK(seen_during_analysis);
  CHECK(spool_files() == 0);
  CHECK(named(all_facts(s), "run_a") != nullptr);
  // A failing run removes its spool too and publishes nothing.
  const std::string good_b = read_text(f.repo / "src/b.cpp");
  append_text(f.repo / "src/a.cpp", "\nint spooled_extra() { return 1; }\n");
  write_text(f.repo / "src/b.cpp", "#include \"shared.h\"\nint broken( {\n");
  const Committed before = committed(s);
  const auto r = run(s, req);
  CHECK(r.outcome == IndexOutcome::failed);
  check_preserved(s, before, r);
  CHECK(spool_files() == 0);
  write_text(f.repo / "src/b.cpp", good_b);
  CHECK(changed_slots(run_ok(s, req)) == std::set<std::string>{"a"});
  CHECK(named(all_facts(s), "spooled_extra") != nullptr);
  CHECK(spool_files() == 0);
}
