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

#include <llvm/Support/VirtualFileSystem.h>

#include "lcm/analyzer/analyzer.hpp"
#include "lcm/analyzer/pch_replay_internal.hpp"
#include "lcm/analyzer/safety.hpp"
#include "lcm/compile_context.hpp"
#include "lcm/compile_db.hpp"
#include "lcm/facts.hpp"
#include "lcm/source.hpp"
#include "support/case_sensitive_dir.hpp"

using namespace lcm;
using namespace lcm::analyzer;
namespace fs = std::filesystem;

namespace {

const fs::path& fixture_root() {
  static const fs::path root = path_from_utf8(LCM_TEST_FIXTURE_DIR) / "cpp" / "phase1a";
  return root;
}
const fs::path& repo_root() {
  static const fs::path root = fixture_root() / "repo";
  return root;
}
fs::path resource_dir() { return path_from_utf8(LCM_CLANG_RESOURCE_DIR); }

std::string json_string(const fs::path& p) {
  std::string s = path_to_utf8_generic(p);
  std::string out;
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out;
}

void write_file(const fs::path& p, const std::string& text) {
  fs::create_directories(p.parent_path());
  std::ofstream out(p, std::ios::binary);
  out << text;
}

std::string read_file(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Writes a real compilation database for `repo` into `out_dir` and loads it.
CompilationDatabase load_fixture_database(const fs::path& repo, const fs::path& out_dir) {
  fs::create_directories(out_dir);
  const std::string dir = json_string(repo);
  const std::string json = std::string("[\n") +
      R"({"directory":")" + dir + R"(","file":"src/shape_area.cpp","arguments":["clang-cl.exe","/c","/std:c++20","/EHsc","/W4","/Iinclude","src/shape_area.cpp","/Foobj/shape_area.obj"]},)" + "\n" +
      R"({"directory":")" + dir + R"(","file":"src/shape_scaled.cpp","arguments":["clang-cl.exe","/c","/std:c++20","/EHsc","/Iinclude","src/shape_scaled.cpp"]},)" + "\n" +
      R"({"directory":")" + dir + R"(","file":"src/use.cpp","arguments":["clang-cl.exe","/c","/std:c++20","/EHsc","/Iinclude","/I../external_sdk","src/use.cpp"]},)" + "\n" +
      R"({"directory":")" + dir + R"(","file":"src/feature.cpp","arguments":["clang-cl.exe","/c","/std:c++20","/Iinclude","src/feature.cpp"]},)" + "\n" +
      R"({"directory":")" + dir + R"(","file":"src/feature.cpp","arguments":["clang-cl.exe","/c","/std:c++20","/Iinclude","/DFEATURE_X","src/feature.cpp"]},)" + "\n" +
      R"({"directory":")" + dir + R"(","file":"src/semantics.cpp","arguments":["clang-cl.exe","/c","/std:c++20","/EHsc","src/semantics.cpp"]},)" + "\n" +
      R"({"directory":")" + dir + R"(","file":"src/callables.cpp","arguments":["clang-cl.exe","/c","/std:c++20","/EHsc","src/callables.cpp"]},)" + "\n" +
      R"({"directory":")" + dir + R"(","file":"src/closures.cpp","arguments":["clang-cl.exe","/c","/std:c++20","/EHsc","src/closures.cpp"]},)" + "\n" +
      R"({"directory":")" + dir + R"(","file":"src/missing_include.cpp","arguments":["clang-cl.exe","/c","/std:c++20","/Iinclude","src/missing_include.cpp"]},)" + "\n" +
      R"({"directory":")" + dir + R"(","file":"src/broken.cpp","arguments":["clang++","-std=c++20","-Iinclude","-c","src/broken.cpp","-o","obj/broken.o"]})" + "\n]\n";
  const fs::path db_path = out_dir / "compile_commands.json";
  write_file(db_path, json);
  const auto parsed = load_compilation_database(db_path, CommandSyntax::windows);
  REQUIRE_MESSAGE(parsed.database, "fixture database failed to parse");
  for (const auto& d : parsed.diagnostics) CHECK(d.severity != DiagnosticSeverity::error);
  return *parsed.database;
}

fs::path scratch_dir(const char* name) {
  return fs::temp_directory_path() /
         path_from_utf8(std::string("lcm-phase1a-") + name + "-" +
                        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
}

DatabaseAnalysisOptions options_for(const fs::path& repo) {
  DatabaseAnalysisOptions o;
  o.repository_root = repo;
  o.repository_member = "repo";
  o.resource_dir = resource_dir();
  return o;
}

const CompileCommand* command_for(const CompilationDatabase& db, const fs::path& repo, const char* rel,
                                  const char* distinguishing_arg = nullptr) {
  for (const auto& c : db.commands) {
    if (path_key(c.file) != path_key(repo / rel)) continue;
    if (distinguishing_arg &&
        std::find(c.arguments.begin(), c.arguments.end(), distinguishing_arg) == c.arguments.end()) {
      continue;
    }
    if (!distinguishing_arg && path_key(c.file) == path_key(repo / "src/feature.cpp") &&
        std::find(c.arguments.begin(), c.arguments.end(), "/DFEATURE_X") != c.arguments.end()) {
      continue;
    }
    return &c;
  }
  return nullptr;
}

AnalysisResult analyze_one(const CompilationDatabase& db, const fs::path& repo, const char* rel,
                           const char* distinguishing_arg = nullptr) {
  const CompileCommand* c = command_for(db, repo, rel, distinguishing_arg);
  REQUIRE_MESSAGE(c, "no command for " << rel);
  UnitSelection sel = select_by_command_ids(db, {c->command_id});
  REQUIRE(sel.selected.size() == 1);
  auto results = analyze_selected(sel, options_for(repo));
  REQUIRE(results.size() == 1);
  for (const auto& d : results.front().diagnostics) MESSAGE(d.text());
  return std::move(results.front());
}

// Builds a command by hand with its true content identity.
CompileCommand make_command(const fs::path& directory, const fs::path& file, std::vector<std::string> argv) {
  CompileCommand c;
  c.directory = directory;
  c.file = file;
  c.arguments = std::move(argv);
  c.command_id = compute_command_id(c);
  return c;
}

// Runs the invocation gate for a hand-built command against a repository root.
AnalysisResult gate(const fs::path& repo, const fs::path& file, std::vector<std::string> argv,
                    const std::string& label = "") {
  const CompileCommand c = make_command(repo, file, std::move(argv));
  AnalysisRequest req;
  req.command = &c;
  req.repository_root = repo;
  req.resource_dir = resource_dir();
  req.analysis_unit = label;
  return analyze_translation_unit(req);
}

// --- lookup helpers over a FactSet -------------------------------------------
const Symbol* find_symbol(const FactSet& facts, const std::string& name, const std::string& signature = "",
                          SymbolKind kind = SymbolKind::function, TemplateRole role = TemplateRole::none) {
  for (const auto& [id, symbol] : facts.symbols()) {
    const auto& k = symbol.key;
    if (k.canonical_name == name && k.kind == kind && (signature.empty() || k.normalized_signature == signature) &&
        k.template_role == role) {
      return &symbol;
    }
  }
  return nullptr;
}

std::vector<const Symbol*> find_symbols(const FactSet& facts, const std::string& name) {
  std::vector<const Symbol*> out;
  for (const auto& [id, symbol] : facts.symbols()) {
    if (symbol.key.canonical_name == name) out.push_back(&symbol);
  }
  return out;
}

const Symbol* method_of(const FactSet& facts, const std::string& owner, const std::string& name,
                        const std::string& signature = "") {
  for (const auto& [id, symbol] : facts.symbols()) {
    const auto& k = symbol.key;
    if (k.canonical_name != name || k.owner_chain.empty() || k.owner_chain.back().name != owner) continue;
    if (!signature.empty() && k.normalized_signature != signature) continue;
    return &symbol;
  }
  return nullptr;
}

std::vector<const Relation*> relations_from(const FactSet& facts, const StableId& source, RelationKind kind) {
  std::vector<const Relation*> out;
  for (const auto& [key, rel] : facts.relations()) {
    if (key.source == source && key.kind == kind) out.push_back(&rel);
  }
  return out;
}

bool has_relation(const FactSet& facts, const StableId& source, const StableId& target, RelationKind kind) {
  return facts.find_relation(RelationKey{source, target, kind, Confidence::confirmed}) != nullptr;
}

const Relation* relation(const FactSet& facts, const StableId& source, const StableId& target, RelationKind kind) {
  return facts.find_relation(RelationKey{source, target, kind, Confidence::confirmed});
}

const Evidence& first_evidence(const FactSet& facts, const StableId& source, const StableId& target,
                               RelationKind kind = RelationKind::calls) {
  const Relation* r = relation(facts, source, target, kind);
  REQUIRE(r);
  REQUIRE_FALSE(r->evidence.empty());
  return r->evidence.front();
}

std::string owner_path(const Symbol& s) {
  std::string out;
  for (const auto& o : s.key.owner_chain) {
    if (!out.empty()) out += "::";
    out += o.name.empty() ? "(anonymous)" : o.name;
  }
  return out;
}

bool plain_aspects(const Evidence& e) {
  return e.evaluation == EvaluationContext::body && e.dispatch == DispatchKind::static_target &&
         e.template_use == TemplateUse::none && !e.immediately_invoked_lambda && e.template_arguments.empty();
}

}  // namespace

// =============================================================================
TEST_CASE("selection is explicit: ambiguous define contexts are returned as choices, never merged") {
  const fs::path work = scratch_dir("select");
  const CompilationDatabase db = load_fixture_database(repo_root(), work);
  REQUIRE(db.commands.size() == 10);

  const UnitSelection by_file = select_by_files(db, {repo_root() / "src/feature.cpp", repo_root() / "src/use.cpp",
                                                     repo_root() / "src/nope.cpp"});
  CHECK(by_file.selected.size() == 1);
  CHECK(path_key(by_file.selected.front()->file) == path_key(repo_root() / "src/use.cpp"));
  REQUIRE(by_file.ambiguous.size() == 1);
  CHECK(by_file.ambiguous.front().candidates.size() == 2);
  CHECK(by_file.missing_files.size() == 1);

  const UnitSelection by_id = select_by_command_ids(db, {db.commands[3].command_id, "not-an-id"});
  CHECK(by_id.selected.size() == 1);
  CHECK(by_id.unknown_command_ids == std::vector<std::string>{"not-an-id"});

  std::error_code ec;
  fs::remove_all(work, ec);
}

TEST_CASE("the safety gate accepts only spelled-out option forms") {
  CHECK(check_analyzer_arguments({"/DFOO", "/Iinclude", "/std:c++20", "/EHsc", "/MD", "/O2", "/Ob1", "/FIpre.h",
                                  "/Zc:__cplusplus", "/arch:AVX2", "/permissive-", "/GR-", "/utf-8", "/Zp8", "/vmg"},
                                 DriverFlavor::msvc, CompilerKind::native_cl)
            .safe);
  CHECK(check_analyzer_arguments({"-DFOO", "-Iinc", "-std=c++20", "-fno-exceptions", "-include", "pre.h", "-x", "c++",
                                  "-O2", "-m64", "-march=x86-64", "-fvisibility=hidden", "--target=x86_64-pc-windows-msvc"},
                                 DriverFlavor::gnu, CompilerKind::clang)
            .safe);
  // Reviewer gate cases: misleading prefixes and unknown forms.
  for (const char* bad : {"-multi-lib-config=cfg.yaml", "-munknown-lcm-option", "-fexceptions-unknown", "-mllvm",
                          "-Xclang", "-fplugin=x.dll", "-fpass-plugin=x", "-load", "--config", "-ivfsoverlay",
                          "-fmodules-cache-path=x", "-o", "-Wp,-DX", "-fweird-flag", "other.cpp", "-###", "-O2x",
                          "-std=", "-fvisibility=", "--"}) {
    CAPTURE(bad);
    CHECK_FALSE(check_analyzer_arguments({"-DFOO", bad}, DriverFlavor::gnu, CompilerKind::clang).safe);
  }
  for (const char* bad : {"/OUT:side-effect.exe", "/Ofast", "/clang:-Xclang", "/clang:-load", "/Qsomething", "/Fofoo.obj",
                          "@resp.rsp", "src/other.cpp", "/link", "/EHx", "/std:gnu++20", "/Zp3", "/vmq", "/arch:FOO",
                          "/D", "/I", "/external:I", "--", "/imsvc", "-Wno-deprecated-declarations", "-Wno-other"}) {
    CAPTURE(bad);
    CHECK_FALSE(check_analyzer_arguments({"/DFOO", bad}, DriverFlavor::msvc, CompilerKind::clang_cl).safe);
  }
  // The clang-cl system include is accepted for clang-cl only: the same spelling in a native cl.exe
  // command is an unknown token and must not slip through the shared MSVC-syntax allowlist.
  CHECK(check_analyzer_arguments({"/imsvcC:/sdk/inc"}, DriverFlavor::msvc, CompilerKind::clang_cl).safe);
  CHECK(check_analyzer_arguments({"-imsvcC:/sdk/inc"}, DriverFlavor::msvc, CompilerKind::clang_cl).safe);
  CHECK_FALSE(check_analyzer_arguments({"/imsvcC:/sdk/inc"}, DriverFlavor::msvc, CompilerKind::native_cl).safe);
  CHECK_FALSE(check_analyzer_arguments({"-imsvcC:/sdk/inc"}, DriverFlavor::msvc, CompilerKind::native_cl).safe);
  CHECK_FALSE(check_analyzer_arguments({"/imsvcC:/sdk/inc"}, DriverFlavor::msvc, CompilerKind::unknown).safe);
  CHECK_FALSE(check_analyzer_arguments({"-include"}, DriverFlavor::gnu, CompilerKind::clang).safe);
  CHECK_FALSE(check_analyzer_arguments({"-include", "-DX"}, DriverFlavor::gnu, CompilerKind::clang).safe);
  CHECK_FALSE(check_analyzer_arguments({"-DX"}, DriverFlavor::unknown, CompilerKind::unknown).safe);
}

TEST_CASE("adapter compatibility: clang-cl end-of-options, -imsvc resolution, audited warning; native and alias negatives") {
  const fs::path work = scratch_dir("adapter");
  const fs::path repo = work / "repo";
  fs::create_directories(repo / "sys one");
  fs::create_directories(repo / "sys two");
  fs::create_directories(repo / "user mid");
  write_file(repo / "main.cc", "int main_fn() { return 1; }\n");
  write_file(repo / "extra.cc", "int extra_fn() { return 2; }\n");
  write_file(repo / "-dash.cc", "int dash_fn() { return 3; }\n");
  write_file(repo / "harmless.rsp", "extra.cc\n");
  write_file(repo / "sys one" / "sysonly.h", "int sys_only();\n");
  write_file(repo / "sys two" / "second.h", "int second_only();\n");
  write_file(repo / "user mid" / "usermid.h", "int user_mid();\n");
  write_file(repo / "uses.cc",
             "#include <sysonly.h>\n#include <second.h>\n#include \"usermid.h\"\n"
             "int uses() { return sys_only() + second_only() + user_mid(); }\n");
  const auto rejected_names = [](const AnalysisResult& r, const char* token) {
    return std::any_of(r.rejected_arguments.begin(), r.rejected_arguments.end(),
                       [&](const std::string& s) { return s.find(token) == 0; });
  };

  SUBCASE("1: option-shaped source after `--` is analysed through the analyzer's own source delimiter") {
    const AnalysisResult r = gate(repo, repo / "-dash.cc", {"clang-cl.exe", "-TP", "-c", "--", "-dash.cc"});
    for (const auto& d : r.diagnostics) MESSAGE(d.text());
    REQUIRE(r.status == AnalysisStatus::ok);
    CHECK(find_symbol(r.facts, "dash_fn", "()") != nullptr);
    CHECK(r.context.analyzer_arguments == std::vector<std::string>{"/TP"});
    // Paired native cl.exe: `--` is ignored by cl.exe, so nothing verified names the source.
    const AnalysisResult n = gate(repo, repo / "-dash.cc", {"cl.exe", "-TP", "-c", "--", "-dash.cc"});
    CHECK(n.status == AnalysisStatus::rejected);
    CHECK_FALSE(n.context.source_identified);
    CHECK(n.files.empty());
    // GNU aliases stay conservative; clang++ is verified.
    const AnalysisResult g = gate(repo, repo / "-dash.cc", {"g++", "-c", "--", "-dash.cc"});
    CHECK(g.status == AnalysisStatus::rejected);
    CHECK(g.files.empty());
    const AnalysisResult cxx = gate(repo, repo / "-dash.cc", {"clang++", "-c", "--", "-dash.cc"});
    for (const auto& d : cxx.diagnostics) MESSAGE(d.text());
    REQUIRE(cxx.status == AnalysisStatus::ok);
    CHECK(find_symbol(cxx.facts, "dash_fn", "()") != nullptr);
  }
  SUBCASE("2: any extra input after `--` rejects the whole request, whatever it looks like") {
    for (const char* extra : {"extra.cc", "-DSTEALTH=1", "/Iextra", "--"}) {
      CAPTURE(extra);
      const AnalysisResult r = gate(repo, repo / "main.cc", {"clang-cl.exe", "-TP", "-c", "--", "main.cc", extra});
      CHECK(r.status == AnalysisStatus::rejected);
      CHECK(r.context_status == NormalizationStatus::degraded);
      CHECK(r.context.source_identified);
      CHECK(r.context.defines.empty());
      CHECK(r.context.includes.empty());
      CHECK(r.context.analyzer_arguments == std::vector<std::string>{"/TP"});  // never reached the string gate
      CHECK(r.files.empty());
      CHECK(r.facts.symbols().empty());
      const auto& d = r.context.dispositions.back();
      CHECK(d.raw == extra);
      CHECK(d.action == OptionAction::dropped_unsupported);
    }
  }
  SUBCASE("3: `--` operands are literal include directories and the unit still analyses") {
    const AnalysisResult r =
        gate(repo, repo / "main.cc", {"clang-cl.exe", "-TP", "/I", "--", "-imsvc", "--", "-c", "--", "main.cc"});
    for (const auto& d : r.diagnostics) MESSAGE(d.text());
    REQUIRE(r.status == AnalysisStatus::ok);
    CHECK(find_symbol(r.facts, "main_fn", "()") != nullptr);
    REQUIRE(r.context.includes.size() == 2);
    CHECK(r.context.includes[0].kind == IncludeKind::user);
    CHECK(r.context.includes[1].kind == IncludeKind::internal_system);
  }
  SUBCASE("4: headers resolve through joined and separated -imsvc directories as system includes") {
    const AnalysisResult r = gate(repo, repo / "uses.cc",
                                  {"clang-cl.exe", "-TP", "-c", "-imsvcsys one", "-I", "user mid", "-imsvc", "sys two", "uses.cc"});
    for (const auto& d : r.diagnostics) MESSAGE(d.text());
    REQUIRE(r.status == AnalysisStatus::ok);
    const Symbol* uses = find_symbol(r.facts, "uses", "()");
    const Symbol* sys_only = find_symbol(r.facts, "sys_only", "()");
    const Symbol* second_only = find_symbol(r.facts, "second_only", "()");
    const Symbol* user_mid = find_symbol(r.facts, "user_mid", "()");
    REQUIRE(uses);
    REQUIRE(sys_only);
    REQUIRE(second_only);
    REQUIRE(user_mid);
    CHECK(has_relation(r.facts, uses->id, sys_only->id, RelationKind::calls));
    CHECK(has_relation(r.facts, uses->id, second_only->id, RelationKind::calls));
    CHECK(has_relation(r.facts, uses->id, user_mid->id, RelationKind::calls));
    // The system-group headers are inside the root yet entered as system files; the user one is not.
    const auto file_named = [&](const char* name) {
      return std::find_if(r.files.begin(), r.files.end(),
                          [&](const FileObservation& o) { return o.path_utf8.find(name) != std::string::npos; });
    };
    REQUIRE(file_named("sysonly.h") != r.files.end());
    REQUIRE(file_named("second.h") != r.files.end());
    REQUIRE(file_named("usermid.h") != r.files.end());
    CHECK(file_named("sysonly.h")->is_system);
    CHECK(file_named("sysonly.h")->in_root);
    CHECK(file_named("second.h")->is_system);
    CHECK_FALSE(file_named("usermid.h")->is_system);
    CHECK(file_named("usermid.h")->in_root);
    REQUIRE(r.context.includes.size() == 3);
    CHECK(r.context.includes[0].kind == IncludeKind::internal_system);
    CHECK(r.context.includes[1].kind == IncludeKind::user);
    CHECK(r.context.includes[2].kind == IncludeKind::internal_system);
    // Native cl.exe: the same tokens are unknown and the request rejects, naming them.
    const AnalysisResult n = gate(repo, repo / "uses.cc",
                                  {"cl.exe", "-TP", "-c", "-imsvcsys one", "-I", "user mid", "-imsvc", "sys two", "uses.cc"});
    CHECK(n.status == AnalysisStatus::rejected);
    CHECK(rejected_names(n, "-imsvcsys one"));
    CHECK(rejected_names(n, "-imsvc"));
    CHECK(n.files.empty());
  }
  SUBCASE("5: a response file after `--` degrades and rejects; nothing is expanded or analysed") {
    const AnalysisResult r = gate(repo, repo / "main.cc", {"clang-cl.exe", "-TP", "-c", "--", "@harmless.rsp", "main.cc"});
    CHECK(r.status == AnalysisStatus::rejected);
    CHECK(r.context_status == NormalizationStatus::degraded);
    CHECK(r.context.unexpanded_response_files == std::vector<std::string>{"harmless.rsp"});
    CHECK(r.files.empty());
    CHECK(r.facts.symbols().empty());
    // A response token consumed as an include operand is the same boundary: the driver would expand it
    // into the include path, so no literal directory is invented and the request rejects.
    for (const char* option : {"/I", "-imsvc"}) {
      CAPTURE(option);
      const AnalysisResult op = gate(repo, repo / "main.cc", {"clang-cl.exe", "-TP", option, "@harmless.rsp", "-c", "main.cc"});
      CHECK(op.status == AnalysisStatus::rejected);
      CHECK(op.context_status == NormalizationStatus::degraded);
      CHECK(op.context.includes.empty());
      CHECK(op.context.unexpanded_response_files == std::vector<std::string>{"harmless.rsp"});
      CHECK(op.files.empty());
    }
    // Joined `/I@literal` is a literal (non-existent) directory and the unit analyses normally.
    const AnalysisResult joined = gate(repo, repo / "main.cc", {"clang-cl.exe", "-TP", "/I@literal", "-c", "main.cc"});
    for (const auto& d : joined.diagnostics) MESSAGE(d.text());
    REQUIRE(joined.status == AnalysisStatus::ok);
    REQUIRE(joined.context.includes.size() == 1);
    CHECK(joined.context.includes.front().as_written == "@literal");
  }
  SUBCASE("6: the audited warning alone passes; unknown warnings and /clang: forwarding still reject") {
    const AnalysisResult ok = gate(repo, repo / "main.cc", {"clang-cl.exe", "-TP", "-c", "-Wno-deprecated-declarations", "main.cc"});
    for (const auto& d : ok.diagnostics) MESSAGE(d.text());
    REQUIRE(ok.status == AnalysisStatus::ok);
    CHECK(ok.context.analyzer_arguments == std::vector<std::string>{"/TP"});
    const AnalysisResult bad = gate(repo, repo / "main.cc",
                                    {"clang-cl.exe", "-TP", "-c", "-Wno-deprecated-declarations", "-Wno-bogus-unknown-thing",
                                     "/clang:-load", "/clang:plugin.dll", "main.cc"});
    CHECK(bad.status == AnalysisStatus::rejected);
    CHECK(rejected_names(bad, "-Wno-bogus-unknown-thing"));
    CHECK(rejected_names(bad, "/clang:-load"));
    CHECK(rejected_names(bad, "/clang:plugin.dll"));
    CHECK_FALSE(rejected_names(bad, "-Wno-deprecated-declarations"));
    CHECK(bad.files.empty());
    const AnalysisResult native = gate(repo, repo / "main.cc", {"cl.exe", "-TP", "-c", "-Wno-deprecated-declarations", "main.cc"});
    CHECK(native.status == AnalysisStatus::rejected);
    CHECK(rejected_names(native, "-Wno-deprecated-declarations"));
  }
  SUBCASE("7: a duplicate selected source is a second driver job: rejected before the front end") {
    for (const std::vector<std::string> argv :
         {std::vector<std::string>{"clang-cl.exe", "-TP", "-c", "main.cc", "main.cc"},
          std::vector<std::string>{"clang-cl.exe", "-TP", "-c", "main.cc", "./main.cc"},
          std::vector<std::string>{"clang-cl.exe", "-TP", "-c", "main.cc", "--", "main.cc"},
          std::vector<std::string>{"clang-cl.exe", "-c", "/Tpmain.cc", "main.cc"},
          std::vector<std::string>{"clang-cl.exe", "-c", "main.cc", "/Tp", "./main.cc"},
          std::vector<std::string>{"clang++", "-c", "main.cc", "./main.cc"}}) {
      CAPTURE(argv.back());
      const AnalysisResult r = gate(repo, repo / "main.cc", argv);
      CHECK(r.status == AnalysisStatus::rejected);
      CHECK(r.context_status == NormalizationStatus::degraded);
      CHECK(r.context.source_identified);
      CHECK(r.files.empty());
      CHECK(r.facts.symbols().empty());
      CHECK(std::any_of(r.context.dispositions.begin(), r.context.dispositions.end(), [](const OptionDisposition& d) {
        return d.action == OptionAction::dropped_unsupported && d.reason.find("second time") != std::string::npos;
      }));
    }
    // Positive controls: operands equal to the file name are not a second source; the unit analyses.
    for (const std::vector<std::string> argv : {std::vector<std::string>{"clang-cl.exe", "-TP", "-c", "/Imain.cc", "main.cc"},
                                                 std::vector<std::string>{"clang-cl.exe", "-TP", "-c", "/D", "main.cc", "main.cc"},
                                                 std::vector<std::string>{"clang-cl.exe", "-c", "/Imain.cc", "/Tpmain.cc"}}) {
      CAPTURE(argv[3]);
      const AnalysisResult r = gate(repo, repo / "main.cc", argv);
      for (const auto& d : r.diagnostics) MESSAGE(d.text());
      REQUIRE(r.status == AnalysisStatus::ok);
      CHECK(find_symbol(r.facts, "main_fn", "()") != nullptr);
    }
  }
  SUBCASE("every fixture path is a plain input to the analyzer: nothing was written") {
    CHECK_FALSE(fs::exists(repo / "main.obj"));
    CHECK_FALSE(fs::exists(repo / "-dash.obj"));
    CHECK_FALSE(fs::exists(repo / "uses.obj"));
  }
  std::error_code ec;
  fs::remove_all(work, ec);
}

TEST_CASE("units are rejected without running when the context is unsafe or does not name its source") {
  const fs::path work = scratch_dir("reject");
  const CompilationDatabase db = load_fixture_database(repo_root(), work);
  const CompileCommand& use = *command_for(db, repo_root(), "src/use.cpp");

  SUBCASE("plugin passthrough") {
    std::vector<std::string> argv = use.arguments;
    argv.insert(argv.begin() + 1, "-Xclang");
    argv.insert(argv.begin() + 2, "-load");
    const AnalysisResult r = gate(repo_root(), use.file, argv);
    CHECK(r.status == AnalysisStatus::rejected);
    CHECK_FALSE(r.rejected_arguments.empty());
    CHECK(r.facts.symbols().empty());
    CHECK(r.files.empty());
  }
  SUBCASE("source consumed as an option value") {
    const AnalysisResult r = gate(repo_root(), use.file, {"clang-cl.exe", "/c", "/D", "src/use.cpp"});
    CHECK_FALSE(r.context.source_identified);
    CHECK(r.status == AnalysisStatus::rejected);
    CHECK(r.facts.symbols().empty());
  }
  SUBCASE("degraded context rejects before the front end and publishes nothing") {
    std::vector<std::string> argv = command_for(db, repo_root(), "src/feature.cpp")->arguments;
    argv.insert(argv.begin() + 1, "@missing.rsp");
    const AnalysisResult r = gate(repo_root(), repo_root() / "src/feature.cpp", argv);
    REQUIRE(r.context.status == NormalizationStatus::degraded);
    CHECK(r.status == AnalysisStatus::rejected);
    CHECK_FALSE(r.context_complete);
    CHECK(std::any_of(r.diagnostics.begin(), r.diagnostics.end(),
                      [](const AnalysisDiagnostic& d) { return d.message.find("missing.rsp") != std::string::npos; }));
    CHECK(r.files.empty());
    FactSet target;
    CHECK_FALSE(publish_contribution(target, r).published);
    CHECK_FALSE(publish_contribution(target, r, PublishPolicy::allow_errors_and_degraded).published);
    CHECK(target.symbols().empty());
  }
  std::error_code ec;
  fs::remove_all(work, ec);
}

TEST_CASE("provenance: the context is derived from the actual command and its identity is bound") {
  const fs::path work = scratch_dir("provenance");
  const CompilationDatabase db = load_fixture_database(repo_root(), work);
  const CompileCommand& safe = *command_for(db, repo_root(), "src/feature.cpp");

  SUBCASE("unsafe raw command cannot ride on a safe context") {
    // There is no way to pass a foreign context: the analyzer normalizes the command it is given.
    const AnalysisResult r =
        gate(repo_root(), safe.file, {"clang-cl.exe", "/c", "/clang:-Xclang", "/clang:-load", "/clang:not-loaded.dll", "src/feature.cpp"},
             "logical-unit");
    CHECK(r.status == AnalysisStatus::rejected);
    CHECK_FALSE(r.rejected_arguments.empty());
    CHECK(r.analysis_unit == "logical-unit");
    CHECK(r.command_id != "logical-unit");  // the label never replaces provenance
    CHECK(r.command_id.size() == 32);
  }
  SUBCASE("a mismatched command_id is refused") {
    CompileCommand forged = safe;
    forged.command_id = "safe-unit";  // hand-written label in place of the content identity
    AnalysisRequest req;
    req.command = &forged;
    req.repository_root = repo_root();
    req.resource_dir = resource_dir();
    const AnalysisResult r = analyze_translation_unit(req);
    CHECK(r.status == AnalysisStatus::rejected);
    CHECK(r.command_id == compute_command_id(safe));
    CHECK(std::any_of(r.diagnostics.begin(), r.diagnostics.end(),
                      [](const AnalysisDiagnostic& d) { return d.message.find("does not match") != std::string::npos; }));
  }
  SUBCASE("an empty command_id is computed; a custom label is kept next to the true identity") {
    CompileCommand c = safe;
    c.command_id.clear();
    AnalysisRequest req;
    req.command = &c;
    req.repository_root = repo_root();
    req.resource_dir = resource_dir();
    req.analysis_unit = "feature-plain";
    const AnalysisResult r = analyze_translation_unit(req);
    REQUIRE(r.status == AnalysisStatus::ok);
    CHECK(r.command_id == compute_command_id(safe));
    CHECK(r.analysis_unit == "feature-plain");
    const Symbol* always = find_symbol(r.facts, "always", "()");
    REQUIRE(always);
    CHECK(always->locations.front().analysis_unit == "feature-plain");
    // The normalized context reported is the one derived from this very command.
    CHECK(r.context.defines.empty());
    const AnalysisResult with_define = analyze_one(db, repo_root(), "src/feature.cpp", "/DFEATURE_X");
    CHECK(with_define.context.defines == std::vector<std::string>{"FEATURE_X"});
    CHECK(with_define.command_id != r.command_id);
  }
  std::error_code ec;
  fs::remove_all(work, ec);
}

TEST_CASE("the host process working directory never changes; each unit resolves includes from its own directory") {
  const fs::path work = scratch_dir("cwd");
  const fs::path repo = work / "repo";
  write_file(repo / "unit_a/inc/which.h", "int from_a();\n");
  write_file(repo / "unit_a/main.cpp", "#include \"which.h\"\nint a_main() { return from_a(); }\n");
  write_file(repo / "unit_b/inc/which.h", "int from_b();\n");
  write_file(repo / "unit_b/main.cpp", "#include \"which.h\"\nint b_main() { return from_b(); }\n");
  write_file(repo / "unit_c/main.cpp", "#include \"nope.h\"\nint c_main() { return 0; }\n");

  const fs::path host_cwd = fs::current_path();
  const auto check_cwd = [&] { CHECK(fs::equivalent(fs::current_path(), host_cwd)); };

  const AnalysisResult a = gate(repo, repo / "unit_a/main.cpp", {"clang-cl.exe", "/c", "/Iinc", "main.cpp"});
  check_cwd();
  const AnalysisResult b = gate(repo, repo / "unit_b/main.cpp", {"clang-cl.exe", "/c", "/Iinc", "main.cpp"});
  check_cwd();
  // gate() uses `repo` as the command directory; use the unit directories explicitly instead.
  const CompileCommand ca = make_command(repo / "unit_a", repo / "unit_a/main.cpp", {"clang-cl.exe", "/c", "/Iinc", "main.cpp"});
  const CompileCommand cb = make_command(repo / "unit_b", repo / "unit_b/main.cpp", {"clang-cl.exe", "/c", "/Iinc", "main.cpp"});
  const CompileCommand cc = make_command(repo / "unit_c", repo / "unit_c/main.cpp", {"clang-cl.exe", "/c", "main.cpp"});
  const CompileCommand cr = make_command(repo / "unit_a", repo / "unit_a/main.cpp", {"clang-cl.exe", "/c", "/clang:-load", "main.cpp"});
  DatabaseAnalysisOptions opts = options_for(repo);
  UnitSelection sel;
  sel.selected = {&ca, &cb, &cc, &cr};
  const auto results = analyze_selected(sel, opts);
  check_cwd();
  REQUIRE(results.size() == 4);
  REQUIRE(results[0].status == AnalysisStatus::ok);
  REQUIRE(results[1].status == AnalysisStatus::ok);
  CHECK(results[2].status == AnalysisStatus::completed_with_errors);  // missing include
  CHECK(results[3].status == AnalysisStatus::rejected);
  // Each unit saw its own header through its own working directory.
  const Symbol* a_main = find_symbol(results[0].facts, "a_main", "()");
  const Symbol* from_a = find_symbol(results[0].facts, "from_a", "()");
  const Symbol* b_main = find_symbol(results[1].facts, "b_main", "()");
  const Symbol* from_b = find_symbol(results[1].facts, "from_b", "()");
  REQUIRE(a_main);
  REQUIRE(from_a);
  REQUIRE(b_main);
  REQUIRE(from_b);
  CHECK(has_relation(results[0].facts, a_main->id, from_a->id, RelationKind::calls));
  CHECK(has_relation(results[1].facts, b_main->id, from_b->id, RelationKind::calls));
  CHECK(find_symbol(results[0].facts, "from_b", "()") == nullptr);
  CHECK(std::any_of(results[0].includes.begin(), results[0].includes.end(), [](const IncludeObservation& i) {
    return i.resolved_repo_path && i.resolved_repo_path->generic == "unit_a/inc/which.h";
  }));
  CHECK(std::any_of(results[1].includes.begin(), results[1].includes.end(), [](const IncludeObservation& i) {
    return i.resolved_repo_path && i.resolved_repo_path->generic == "unit_b/inc/which.h";
  }));
  check_cwd();
  std::error_code ec;
  fs::remove_all(work, ec);
}

TEST_CASE("invocation-safety matrix (independent reviewer cases)") {
  const fs::path work = scratch_dir("gate");
  const fs::path repo = work / "repo";
  fs::create_directories(repo / "obj");
  write_file(repo / "src/a.cpp", "int a_fn() { return 1; }\n");
  write_file(repo / "src/b.cpp", "int b_fn() { return 2; }\n");
  const fs::path a_cpp = repo / "src/a.cpp";

  SUBCASE("1: compiler wrapper is rejected, never executed, no argv shifting") {
    const AnalysisResult r = gate(repo, a_cpp, {"sccache.exe", "clang-cl.exe", "/c", "src/a.cpp"});
    CHECK(r.status == AnalysisStatus::rejected);
    CHECK(r.context_status == NormalizationStatus::unusable);
    CHECK(r.files.empty());
  }
  SUBCASE("2: clang-cl passthrough wrapper spellings never reach the front end") {
    const AnalysisResult r = gate(repo, a_cpp, {"clang-cl.exe", "/c", "/clang:-Xclang", "/clang:-load",
                                                "/clang:C:/plugins/x.dll", "src/a.cpp"});
    CHECK(r.status == AnalysisStatus::rejected);
    CHECK(std::any_of(r.rejected_arguments.begin(), r.rejected_arguments.end(),
                      [](const std::string& s) { return s.find("/clang:-load") == 0; }));
    CHECK(r.files.empty());
  }
  SUBCASE("3: include operands that look like options or the source are preserved literally") {
    const AnalysisResult r = gate(repo, a_cpp, {"clang++", "-I", "-DNAME", "-I", "src/a.cpp", "-c", "src/a.cpp"});
    REQUIRE(r.status == AnalysisStatus::ok);
    CHECK(find_symbol(r.facts, "a_fn", "()") != nullptr);
    REQUIRE(r.context.includes.size() == 2);
    CHECK(r.context.includes[0].as_written == "-DNAME");
    CHECK(r.context.includes[1].as_written == "src/a.cpp");
    CHECK(r.context.defines.empty());
    CHECK(r.context.source_identified);
  }
  SUBCASE("4: quoted Windows command with punctuation and Unicode paths, no shell") {
    const fs::path uroot = work / path_from_utf8("Repo & 100%");
    const fs::path inc = uroot / path_from_utf8("SDK & Tools/inc(1)");
    write_file(inc / "sdk.h", "int sdk_value();\n");
    write_file(uroot / path_from_utf8("src/한 글#x.cpp"), "#include \"sdk.h\"\nint uses_sdk() { return sdk_value(); }\n");
    const std::string json = std::string("[{\"directory\":\"") + json_string(uroot) +
                             "\",\"file\":\"src/한 글#x.cpp\",\"command\":\"clang-cl.exe /c /std:c++20 \\\"/I" +
                             json_string(inc) + "\\\" \\\"src/한 글#x.cpp\\\"\"}]";
    CompileDbParseOptions po;
    po.syntax = CommandSyntax::windows;
    po.database_directory = uroot;
    const auto parsed = parse_compilation_database(json, po);
    REQUIRE(parsed.database);
    REQUIRE(parsed.database->commands.size() == 1);
    const CompileCommand& c = parsed.database->commands.front();
    REQUIRE(c.arguments.size() == 5);
    CHECK(c.arguments[3] == "/I" + path_to_utf8_generic(inc));
    CHECK(c.arguments[4] == "src/한 글#x.cpp");
    AnalysisRequest req;
    req.command = &c;
    req.repository_root = uroot;
    req.resource_dir = resource_dir();
    const AnalysisResult r = analyze_translation_unit(req);
    for (const auto& d : r.diagnostics) MESSAGE(d.text());
    REQUIRE(r.status == AnalysisStatus::ok);
    const Symbol* uses = find_symbol(r.facts, "uses_sdk", "()");
    REQUIRE(uses);
    CHECK(uses->locations.front().location.file.generic == "src/한 글#x.cpp");
    CHECK(std::any_of(r.files.begin(), r.files.end(), [](const FileObservation& f) {
      return f.repo_path && f.repo_path->generic == "SDK & Tools/inc(1)/sdk.h";
    }));
  }
  SUBCASE("5: semantic injection and cache-writing switches reject before the front end") {
    for (const std::vector<std::string> extra : {std::vector<std::string>{"@opts.rsp"},
                                                  std::vector<std::string>{"--config=cfg"},
                                                  std::vector<std::string>{"-ivfsoverlay", "map.yaml"},
                                                  std::vector<std::string>{"-fmodules-cache-path=cache"},
                                                  std::vector<std::string>{"-multi-lib-config=cfg.yaml"}}) {
      std::vector<std::string> argv = {"clang++", "-c"};
      argv.insert(argv.end(), extra.begin(), extra.end());
      argv.push_back("src/a.cpp");
      CAPTURE(extra.front());
      const AnalysisResult r = gate(repo, a_cpp, argv);
      CHECK(r.status == AnalysisStatus::rejected);
      CHECK(r.files.empty());
      CHECK(r.facts.symbols().empty());
    }
    const AnalysisResult out = gate(repo, a_cpp, {"clang-cl.exe", "/c", "/OUT:side-effect.exe", "src/a.cpp"});
    CHECK(out.status == AnalysisStatus::rejected);
    CHECK_FALSE(fs::exists(repo / "side-effect.exe"));
  }
  SUBCASE("6: output and dependency options are audited away; nothing is written") {
    const AnalysisResult r = gate(repo, a_cpp, {"clang++", "-c", "src/a.cpp", "-o", "obj/a.o", "-MD", "-MF", "obj/a.d"});
    REQUIRE(r.status == AnalysisStatus::ok);
    CHECK(find_symbol(r.facts, "a_fn", "()") != nullptr);
    CHECK_FALSE(fs::exists(repo / "obj/a.o"));
    CHECK_FALSE(fs::exists(repo / "obj/a.d"));
    CHECK(fs::is_empty(repo / "obj"));
  }
  SUBCASE("7: an additional source input rejects; no first-source selection") {
    const AnalysisResult r = gate(repo, a_cpp, {"clang++", "-c", "src/a.cpp", "src/b.cpp"});
    CHECK(r.status == AnalysisStatus::rejected);
    CHECK(std::any_of(r.rejected_arguments.begin(), r.rejected_arguments.end(),
                      [](const std::string& s) { return s.find("src/b.cpp") == 0; }));
    CHECK(r.facts.symbols().empty());
  }
  SUBCASE("8: source consumed as an operand rejects; the source is never appended") {
    const AnalysisResult r = gate(repo, a_cpp, {"clang++", "-D", "src/a.cpp"});
    CHECK(r.status == AnalysisStatus::rejected);
    CHECK(r.files.empty());
  }
  std::error_code ec;
  fs::remove_all(work, ec);
}

TEST_CASE("two TUs sharing a header: one logical symbol, header and split-implementation locations") {
  const fs::path work = scratch_dir("shared");
  const CompilationDatabase db = load_fixture_database(repo_root(), work);
  const AnalysisResult area_tu = analyze_one(db, repo_root(), "src/shape_area.cpp");
  const AnalysisResult scaled_tu = analyze_one(db, repo_root(), "src/shape_scaled.cpp");
  REQUIRE(area_tu.status == AnalysisStatus::ok);
  REQUIRE(scaled_tu.status == AnalysisStatus::ok);
  CHECK(area_tu.context_complete);

  FactSet graph;
  REQUIRE(publish_contribution(graph, area_tu).published);
  REQUIRE(publish_contribution(graph, scaled_tu).published);

  const Symbol* area = method_of(graph, "Shape", "area", "() const");
  const Symbol* scaled = method_of(graph, "Shape", "scaled", "(double) const");
  REQUIRE(area);
  REQUIRE(scaled);
  CHECK(owner_path(*area) == "geo::Shape");
  CHECK(area->key.kind == SymbolKind::method);
  CHECK(std::holds_alternative<ExternalLinkage>(area->key.linkage));

  REQUIRE(area->locations.size() == 3);
  std::map<std::string, int> roles;
  for (const auto& l : area->locations) roles[l.location.file.generic + ":" + (l.role == LocationRole::definition ? "def" : "decl")]++;
  CHECK(roles["include/geo.h:decl"] == 2);
  CHECK(roles["src/shape_area.cpp:def"] == 1);
  CHECK(std::count_if(scaled->locations.begin(), scaled->locations.end(),
                      [](const SymbolLocation& l) { return l.role == LocationRole::definition && l.location.file.generic == "src/shape_scaled.cpp"; }) == 1);

  const Symbol* shape = find_symbol(graph, "Shape", "", SymbolKind::struct_);
  REQUIRE(shape);
  CHECK(has_relation(graph, shape->id, area->id, RelationKind::contains));
  CHECK(has_relation(graph, shape->id, scaled->id, RelationKind::contains));
  CHECK(plain_aspects(first_evidence(graph, scaled->id, area->id)));

  const auto def = std::find_if(area->locations.begin(), area->locations.end(),
                                [](const SymbolLocation& l) { return l.role == LocationRole::definition; });
  REQUIRE(def != area->locations.end());
  const std::string bytes = read_file(repo_root() / "src/shape_area.cpp");
  const auto snippet = read_snippet(bytes, def->location.content_hash, def->location.span);
  REQUIRE(std::holds_alternative<Snippet>(snippet));
  CHECK(std::get<Snippet>(snippet).text.find("double Shape::area() const") == 0);
  CHECK(def->body_hash.has_value());

  const auto helpers = find_symbols(graph, "helper");
  REQUIRE(helpers.size() == 2);
  for (const auto* h : helpers) CHECK(std::holds_alternative<InternalLinkage>(h->key.linkage));
  CHECK(std::get<InternalLinkage>(helpers[0]->key.linkage).file_scope != std::get<InternalLinkage>(helpers[1]->key.linkage).file_scope);
  const auto details = find_symbols(graph, "detail_fn");
  REQUIRE(details.size() == 2);
  CHECK(details[0]->id != details[1]->id);

  const auto header = std::find_if(area_tu.files.begin(), area_tu.files.end(),
                                   [](const FileObservation& f) { return f.repo_path && f.repo_path->generic == "include/geo.h"; });
  REQUIRE(header != area_tu.files.end());
  CHECK(header->in_root);
  REQUIRE(area_tu.includes.size() == 1);
  CHECK(area_tu.includes.front().spelling == "geo.h");
  CHECK_FALSE(area_tu.includes.front().angled);
  CHECK(area_tu.includes.front().resolved_in_root);
  CHECK(area_tu.includes.front().includer_repo_path->generic == "src/shape_area.cpp");

  std::error_code ec;
  fs::remove_all(work, ec);
}

TEST_CASE("overloads, owners, virtual slot, references, lambdas, indirect calls, decoys and external boundary") {
  const fs::path work = scratch_dir("use");
  const CompilationDatabase db = load_fixture_database(repo_root(), work);
  const AnalysisResult r = analyze_one(db, repo_root(), "src/use.cpp");
  REQUIRE_MESSAGE(r.status == AnalysisStatus::ok, "system header resolution (<cstdint>) or fixture failed");
  const FactSet& f = r.facts;

  const Symbol* use = find_symbol(f, "use");
  REQUIRE(use);
  const Symbol* print_int = find_symbol(f, "print", "(int)");
  const Symbol* print_double = find_symbol(f, "print", "(double)");
  const Symbol* other_print = method_of(f, "Other", "print", "(int)");
  const Symbol* shape_area = method_of(f, "Shape", "area", "() const");
  const Symbol* shape_name = method_of(f, "Shape", "name", "() const");
  const Symbol* circle_name = method_of(f, "Circle", "name", "() const");
  REQUIRE(print_int);
  REQUIRE(print_double);
  REQUIRE(other_print);
  REQUIRE(shape_area);
  REQUIRE(shape_name);
  REQUIRE(circle_name);
  CHECK(print_int->id != print_double->id);
  CHECK(print_int->id != other_print->id);
  CHECK(owner_path(*other_print) == "geo::Other");

  const auto calls = relations_from(f, use->id, RelationKind::calls);
  CHECK(has_relation(f, use->id, print_int->id, RelationKind::calls));
  CHECK(has_relation(f, use->id, print_double->id, RelationKind::calls));
  CHECK(has_relation(f, use->id, other_print->id, RelationKind::calls));
  CHECK(has_relation(f, use->id, shape_area->id, RelationKind::calls));
  CHECK(first_evidence(f, use->id, shape_name->id).dispatch == DispatchKind::virtual_slot);
  CHECK_FALSE(has_relation(f, use->id, circle_name->id, RelationKind::calls));  // runtime candidates are query-time
  CHECK(has_relation(f, circle_name->id, shape_name->id, RelationKind::overrides));
  const Symbol* circle = find_symbol(f, "Circle", "", SymbolKind::struct_);
  const Symbol* shape = find_symbol(f, "Shape", "", SymbolKind::struct_);
  REQUIRE(circle);
  REQUIRE(shape);
  CHECK(has_relation(f, circle->id, shape->id, RelationKind::extends));
  CHECK(has_relation(f, use->id, print_int->id, RelationKind::references));

  const Symbol* lambda = nullptr;
  for (const auto& [id, s] : f.symbols()) {
    if (s.key.kind == SymbolKind::lambda) lambda = &s;
  }
  REQUIRE(lambda);
  CHECK(std::holds_alternative<LocalScope>(lambda->key.linkage));
  CHECK(std::get<LocalScope>(lambda->key.linkage).enclosing == use->id);
  CHECK(has_relation(f, lambda->id, print_int->id, RelationKind::calls));
  CHECK(has_relation(f, use->id, lambda->id, RelationKind::calls));
  CHECK_FALSE(first_evidence(f, use->id, lambda->id).immediately_invoked_lambda);  // called through a variable
  CHECK(has_relation(f, use->id, lambda->id, RelationKind::contains));
  CHECK(relation(f, use->id, print_int->id, RelationKind::calls)->evidence.size() == 1);

  REQUIRE(f.unresolved_sites().size() == 1);
  CHECK(f.unresolved_sites().front().expression == "fp");
  CHECK(f.unresolved_sites().front().reason == UnresolvedReason::indirect_callee);
  CHECK(f.unresolved_sites().front().enclosing == use->id);

  const std::string bytes = read_file(repo_root() / "src/use.cpp");
  LineIndex lines(bytes);
  const auto comment_line = lines.line_column(static_cast<std::uint32_t>(bytes.find("// \"print(99)\""))).first;
  const auto string_line = lines.line_column(static_cast<std::uint32_t>(bytes.find("const char* text"))).first;
  for (const auto* rel : calls) {
    for (const auto& e : rel->evidence) {
      CHECK(e.location.span.begin_line != comment_line);
      CHECK(e.location.span.begin_line != string_line);
    }
  }

  const Symbol* ext = find_symbol(f, "ext_api", "(int)");
  REQUIRE(ext);
  CHECK(ext->presence == SymbolPresence::external_placeholder);
  CHECK(ext->locations.empty());
  CHECK(ext->key.repository_member == "external");
  CHECK(has_relation(f, use->id, ext->id, RelationKind::calls));
  CHECK(find_symbol(f, "ext_inline", "()") == nullptr);
  for (const auto& [id, s] : f.symbols()) {
    for (const auto& l : s.locations) CHECK(l.location.file.generic.find("external_sdk") == std::string::npos);
  }
  CHECK(std::any_of(r.files.begin(), r.files.end(), [](const FileObservation& o) {
    return !o.in_root && o.path_utf8.find("external_sdk") != std::string::npos;
  }));
  CHECK(std::any_of(r.files.begin(), r.files.end(), [](const FileObservation& o) { return o.is_system && !o.in_root; }));
  const auto ext_include = std::find_if(r.includes.begin(), r.includes.end(),
                                        [](const IncludeObservation& i) { return i.spelling == "ext.h"; });
  REQUIRE(ext_include != r.includes.end());
  CHECK_FALSE(ext_include->resolved_in_root);
  CHECK(ext_include->resolved_path_utf8.has_value());

  std::error_code ec;
  fs::remove_all(work, ec);
}

TEST_CASE("define-dependent branches: each selected command is its own unit and neither is chosen for the other") {
  const fs::path work = scratch_dir("define");
  const CompilationDatabase db = load_fixture_database(repo_root(), work);
  const AnalysisResult plain = analyze_one(db, repo_root(), "src/feature.cpp");
  const AnalysisResult with_x = analyze_one(db, repo_root(), "src/feature.cpp", "/DFEATURE_X");
  REQUIRE(plain.status == AnalysisStatus::ok);
  REQUIRE(with_x.status == AnalysisStatus::ok);
  CHECK(plain.analysis_unit != with_x.analysis_unit);
  CHECK(plain.command_id == plain.analysis_unit);
  CHECK(find_symbol(plain.facts, "feature_only", "()") == nullptr);
  CHECK(find_symbol(with_x.facts, "feature_only", "()") != nullptr);
  CHECK(find_symbol(plain.facts, "always", "()") != nullptr);
  CHECK(find_symbol(with_x.facts, "always", "()") != nullptr);

  FactSet graph;
  REQUIRE(publish_contribution(graph, plain).published);
  REQUIRE(publish_contribution(graph, with_x).published);
  const Symbol* always = find_symbol(graph, "always", "()");
  REQUIRE(always);
  CHECK(always->locations.size() == 2);
  std::error_code ec;
  fs::remove_all(work, ec);
}

TEST_CASE("semantic matrix and audit boundaries") {
  const fs::path work = scratch_dir("matrix");
  const CompilationDatabase db = load_fixture_database(repo_root(), work);
  const AnalysisResult r = analyze_one(db, repo_root(), "src/semantics.cpp");
  REQUIRE(r.status == AnalysisStatus::ok);
  const FactSet& f = r.facts;
  const std::string bytes = read_file(repo_root() / "src/semantics.cpp");
  LineIndex lines(bytes);
  const auto line_of = [&](const char* needle) {
    const auto pos = bytes.find(needle);
    REQUIRE(pos != std::string::npos);
    return lines.line_column(static_cast<std::uint32_t>(pos)).first;
  };

  SUBCASE("case 1: default argument evaluated at the use site") {
    const auto calcs = find_symbols(f, "calculate");
    REQUIRE(calcs.size() == 1);
    const Symbol* calculate = calcs.front();
    CHECK(calculate->locations.size() == 2);
    const Symbol* seed = find_symbol(f, "seed", "()");
    const Symbol* default_caller = find_symbol(f, "default_caller", "()");
    const Symbol* explicit_caller = find_symbol(f, "explicit_caller", "()");
    REQUIRE(seed);
    REQUIRE(default_caller);
    REQUIRE(explicit_caller);
    CHECK(has_relation(f, default_caller->id, calculate->id, RelationKind::calls));
    const Relation* to_seed = relation(f, default_caller->id, seed->id, RelationKind::calls);
    REQUIRE(to_seed);
    REQUIRE(to_seed->evidence.size() == 1);
    CHECK(to_seed->evidence.front().evaluation == EvaluationContext::default_argument);
    CHECK(to_seed->evidence.front().dispatch == DispatchKind::static_target);
    CHECK(to_seed->evidence.front().location.span.begin_line == line_of("return calculate();"));
    CHECK_FALSE(has_relation(f, calculate->id, seed->id, RelationKind::calls));
    CHECK_FALSE(has_relation(f, explicit_caller->id, seed->id, RelationKind::calls));
    CHECK(has_relation(f, explicit_caller->id, calculate->id, RelationKind::calls));
  }

  SUBCASE("case 2: init-capture lambda invoked immediately") {
    const Symbol* caller = find_symbol(f, "lambda_caller", "()");
    const Symbol* make_value = find_symbol(f, "make_value", "()");
    const Symbol* consume = find_symbol(f, "consume", "(int)");
    REQUIRE(caller);
    REQUIRE(make_value);
    REQUIRE(consume);
    const Symbol* lambda = nullptr;
    for (const auto& [id, s] : f.symbols()) {
      if (s.key.kind == SymbolKind::lambda && std::get<LocalScope>(s.key.linkage).enclosing == caller->id) lambda = &s;
    }
    REQUIRE(lambda);
    CHECK(first_evidence(f, caller->id, make_value->id).evaluation == EvaluationContext::init_capture_initializer);
    const Evidence& to_lambda = first_evidence(f, caller->id, lambda->id);
    CHECK(to_lambda.immediately_invoked_lambda);
    CHECK(to_lambda.evaluation == EvaluationContext::body);
    CHECK(has_relation(f, lambda->id, consume->id, RelationKind::calls));
    CHECK_FALSE(has_relation(f, caller->id, consume->id, RelationKind::calls));
    CHECK_FALSE(has_relation(f, lambda->id, make_value->id, RelationKind::calls));
    for (const auto& [id, s] : f.symbols()) {
      if (!s.key.owner_chain.empty() && s.key.owner_chain.back().kind == SymbolKind::lambda) {
        CHECK(s.key.kind != SymbolKind::method);
      }
    }
  }

  SUBCASE("case 3: virtual slot, final target, qualified base, same-class qualification") {
    const Symbol* base_run = method_of(f, "Base", "run", "() const");
    const Symbol* derived_run = method_of(f, "Derived", "run", "() const");
    const Symbol* base_dispatch = find_symbol(f, "base_dispatch", "(const Base &)");
    const Symbol* final_target = find_symbol(f, "final_target", "(const Derived &)");
    const Symbol* qualified = find_symbol(f, "qualified_base", "(const Derived &)");
    const Symbol* same_class = find_symbol(f, "same_class_qualified", "(const Derived &)");
    REQUIRE(base_run);
    REQUIRE(derived_run);
    REQUIRE(base_dispatch);
    REQUIRE(final_target);
    REQUIRE(qualified);
    REQUIRE(same_class);
    CHECK(first_evidence(f, base_dispatch->id, base_run->id).dispatch == DispatchKind::virtual_slot);
    CHECK_FALSE(has_relation(f, base_dispatch->id, derived_run->id, RelationKind::calls));
    CHECK(first_evidence(f, final_target->id, derived_run->id).dispatch == DispatchKind::devirtualized);
    CHECK(first_evidence(f, qualified->id, base_run->id).dispatch == DispatchKind::qualified);
    CHECK_FALSE(has_relation(f, qualified->id, derived_run->id, RelationKind::calls));
    // d.Derived::run() names the derived method itself: qualified, exact target, never "base".
    CHECK(first_evidence(f, same_class->id, derived_run->id).dispatch == DispatchKind::qualified);
    CHECK_FALSE(has_relation(f, same_class->id, base_run->id, RelationKind::calls));
    // Qualification is recorded independently of virtual-ness (re-review follow-up): a non-virtual
    // method called as p.Plain::run() is `qualified`; the ordinary p.run() stays `static_target`.
    const Symbol* plain_run = method_of(f, "Plain", "run", "() const");
    const Symbol* plain_qualified = find_symbol(f, "plain_qualified", "(const Plain &)");
    const Symbol* plain_unqualified = find_symbol(f, "plain_unqualified", "(const Plain &)");
    REQUIRE(plain_run);
    REQUIRE(plain_qualified);
    REQUIRE(plain_unqualified);
    CHECK(first_evidence(f, plain_qualified->id, plain_run->id).dispatch == DispatchKind::qualified);
    CHECK(first_evidence(f, plain_unqualified->id, plain_run->id).dispatch == DispatchKind::static_target);
    CHECK(has_relation(f, derived_run->id, base_run->id, RelationKind::overrides));
    const Symbol* base = find_symbol(f, "Base", "", SymbolKind::struct_);
    const Symbol* derived = find_symbol(f, "Derived", "", SymbolKind::struct_);
    REQUIRE(base);
    REQUIRE(derived);
    CHECK(has_relation(f, derived->id, base->id, RelationKind::extends));
  }

  SUBCASE("case 4: template primary, explicit specialization, implicit use") {
    const Symbol* primary = find_symbol(f, "token", "()", SymbolKind::function, TemplateRole::primary);
    const Symbol* spec = find_symbol(f, "token", "()", SymbolKind::function, TemplateRole::explicit_specialization);
    REQUIRE(primary);
    REQUIRE(spec);
    CHECK(primary->id != spec->id);
    CHECK(primary->key.template_parameters == "<typename>");
    CHECK(spec->key.template_arguments == "<int>");
    CHECK(find_symbols(f, "token").size() == 2);
    const Symbol* uses_int = find_symbol(f, "uses_int", "()");
    const Symbol* uses_double = find_symbol(f, "uses_double", "()");
    REQUIRE(uses_int);
    REQUIRE(uses_double);
    CHECK(first_evidence(f, uses_int->id, spec->id).template_use == TemplateUse::explicit_specialization);
    CHECK_FALSE(has_relation(f, uses_int->id, primary->id, RelationKind::calls));
    const Evidence& implicit = first_evidence(f, uses_double->id, primary->id);
    CHECK(implicit.template_use == TemplateUse::primary_implicit);
    CHECK(implicit.template_arguments == "<double>");
    CHECK(implicit.evaluation == EvaluationContext::body);
    CHECK_FALSE(has_relation(f, uses_double->id, spec->id, RelationKind::calls));
  }

  SUBCASE("audit: primaries with different template-parameter kinds are distinct; renaming is not") {
    std::vector<const Symbol*> collisions;
    for (const auto& [id, s] : f.symbols()) {
      if (s.key.canonical_name == "collision") collisions.push_back(&s);
    }
    REQUIRE(collisions.size() == 2);  // the renamed redeclaration `template<class U>` merged into the type primary
    CHECK(collisions[0]->id != collisions[1]->id);
    const Symbol* type_primary = nullptr;
    const Symbol* value_primary = nullptr;
    for (const auto* c : collisions) {
      CHECK(c->key.template_role == TemplateRole::primary);
      if (c->key.template_parameters == "<typename>") type_primary = c;
      if (c->key.template_parameters == "<int>") value_primary = c;
    }
    REQUIRE(type_primary);
    REQUIRE(value_primary);
    CHECK(type_primary->locations.size() == 2);  // definition + renamed redeclaration
    const Symbol* type_use = find_symbol(f, "type_use", "()");
    const Symbol* value_use = find_symbol(f, "value_use", "()");
    REQUIRE(type_use);
    REQUIRE(value_use);
    CHECK(has_relation(f, type_use->id, type_primary->id, RelationKind::calls));
    CHECK_FALSE(has_relation(f, type_use->id, value_primary->id, RelationKind::calls));
    CHECK(has_relation(f, value_use->id, value_primary->id, RelationKind::calls));
    CHECK_FALSE(has_relation(f, value_use->id, type_primary->id, RelationKind::calls));
    CHECK(first_evidence(f, type_use->id, type_primary->id).template_arguments == "<int>");
    CHECK(first_evidence(f, value_use->id, value_primary->id).template_arguments == "<1>");
  }

  SUBCASE("audit: call aspects are orthogonal") {
    const Symbol* default_user = find_symbol(f, "default_user", "()");
    const Symbol* base_run = method_of(f, "Base", "run", "() const");
    const Symbol* global_base = find_symbol(f, "global_base", "()");
    REQUIRE(default_user);
    REQUIRE(base_run);
    REQUIRE(global_base);
    const Evidence& dflt_virtual = first_evidence(f, default_user->id, base_run->id);
    CHECK(dflt_virtual.evaluation == EvaluationContext::default_argument);
    CHECK(dflt_virtual.dispatch == DispatchKind::virtual_slot);
    CHECK(dflt_virtual.location.span.begin_line == line_of("return with_default();"));
    CHECK(first_evidence(f, default_user->id, global_base->id).evaluation == EvaluationContext::default_argument);

    const Symbol* lambda_cross = find_symbol(f, "lambda_cross", "()");
    const Symbol* folded = find_symbol(f, "folded", "()", SymbolKind::function, TemplateRole::primary);
    REQUIRE(lambda_cross);
    REQUIRE(folded);
    const Evidence& cross = first_evidence(f, lambda_cross->id, folded->id);
    CHECK(cross.evaluation == EvaluationContext::init_capture_initializer);
    CHECK(cross.template_use == TemplateUse::primary_implicit);
    CHECK(cross.template_arguments == "<double>");
    CHECK(cross.dispatch == DispatchKind::static_target);
  }

  SUBCASE("audit: anonymous namespace-scope and member types have anchored, distinct identities") {
    for (const auto& l : r.limits) MESSAGE("limit: " << l);
    std::vector<const Symbol*> anon;
    for (const auto& [id, s] : f.symbols()) {
      if (s.key.kind == SymbolKind::anonymous_type) {
        anon.push_back(&s);
        MESSAGE("anonymous type at line " << s.locations.front().location.span.begin_line << " owner " << owner_path(s));
      }
    }
    // anonymous_one, anonymous_two, two unnamed unions in Holder, named_member's struct.
    REQUIRE(anon.size() == 5);
    for (std::size_t i = 0; i < anon.size(); ++i) {
      CHECK(std::holds_alternative<LocalScope>(anon[i]->key.linkage));
      CHECK(anon[i]->locations.size() == 1);
      for (std::size_t j = i + 1; j < anon.size(); ++j) CHECK(anon[i]->id != anon[j]->id);
    }
    const Symbol* one = find_symbol(f, "anonymous_one", "", SymbolKind::variable);
    const Symbol* two = find_symbol(f, "anonymous_two", "", SymbolKind::variable);
    const Symbol* holder = find_symbol(f, "Holder", "", SymbolKind::struct_);
    REQUIRE(one);
    REQUIRE(two);
    REQUIRE(holder);
    const auto anchored_to = [&](const StableId& enclosing) {
      return std::count_if(anon.begin(), anon.end(), [&](const Symbol* s) {
        return std::get<LocalScope>(s->key.linkage).enclosing == enclosing;
      });
    };
    CHECK(anchored_to(one->id) == 1);
    CHECK(anchored_to(two->id) == 1);
    CHECK(anchored_to(holder->id) == 2);  // the two unnamed unions, ordinals 0 and 1
    const Symbol* named_member = nullptr;
    for (const auto& [id, s] : f.symbols()) {
      if (s.key.canonical_name == "named_member" && s.key.kind == SymbolKind::field) named_member = &s;
    }
    REQUIRE(named_member);
    CHECK(anchored_to(named_member->id) == 1);
    // A typedef'd anonymous struct takes the typedef name and is not anonymous.
    CHECK(find_symbol(f, "TaggedAnon", "", SymbolKind::struct_) != nullptr);
    // Fields of anonymous types are owned by them and stay distinct.
    std::vector<const Symbol*> firsts;
    for (const auto& [id, s] : f.symbols()) {
      if (s.key.canonical_name == "first" && s.key.kind == SymbolKind::field) firsts.push_back(&s);
    }
    CHECK(firsts.size() == 1);
  }

  SUBCASE("boundary: same-named local classes in separate blocks are distinct") {
    std::vector<const Symbol*> locals;
    for (const auto& [id, s] : f.symbols()) {
      if (s.key.canonical_name == "Local" && s.key.kind == SymbolKind::struct_) locals.push_back(&s);
    }
    REQUIRE(locals.size() == 2);
    CHECK(locals[0]->id != locals[1]->id);
    for (const auto* l : locals) CHECK(std::holds_alternative<LocalScope>(l->key.linkage));
    std::vector<const Symbol*> vs;
    for (const auto& [id, s] : f.symbols()) {
      if (s.key.canonical_name == "v" && s.key.kind == SymbolKind::method) vs.push_back(&s);
    }
    CHECK(vs.size() == 2);
  }
  std::error_code ec;
  fs::remove_all(work, ec);
}

TEST_CASE("missing include and broken TU: errors are actionable, facts stay provisional, prior graph untouched") {
  const fs::path work = scratch_dir("errors");
  const CompilationDatabase db = load_fixture_database(repo_root(), work);
  FactSet graph;
  const AnalysisResult good = analyze_one(db, repo_root(), "src/shape_area.cpp");
  REQUIRE(publish_contribution(graph, good).published);
  const auto before = graph.fact_hash();

  const AnalysisResult missing = analyze_one(db, repo_root(), "src/missing_include.cpp");
  CHECK(missing.status == AnalysisStatus::completed_with_errors);
  const auto not_found = std::find_if(missing.diagnostics.begin(), missing.diagnostics.end(), [](const AnalysisDiagnostic& d) {
    return d.message.find("does_not_exist.h") != std::string::npos && d.file.has_value() && d.line == 1;
  });
  CHECK(not_found != missing.diagnostics.end());

  const AnalysisResult broken = analyze_one(db, repo_root(), "src/broken.cpp");
  CHECK(broken.status == AnalysisStatus::completed_with_errors);
  CHECK(broken.has_errors());
  const Symbol* g = find_symbol(broken.facts, "g", "()");
  REQUIRE(g);
  const Symbol* fdecl = find_symbol(broken.facts, "f", "(int)");
  if (fdecl) CHECK_FALSE(has_relation(broken.facts, g->id, fdecl->id, RelationKind::calls));

  CHECK_FALSE(publish_contribution(graph, missing).published);
  CHECK_FALSE(publish_contribution(graph, broken).published);
  CHECK(graph.fact_hash() == before);
  std::error_code ec;
  fs::remove_all(work, ec);
}

TEST_CASE("file move and body edit keep the external id, change location and body hash, and move the static scope") {
  const fs::path work = scratch_dir("move");
  const fs::path copy = work / "repo";
  fs::create_directories(copy);
  fs::copy(repo_root(), copy, fs::copy_options::recursive);
  const CompilationDatabase original_db = load_fixture_database(repo_root(), work / "db-original");
  const AnalysisResult before = analyze_one(original_db, repo_root(), "src/shape_area.cpp");
  REQUIRE(before.status == AnalysisStatus::ok);

  fs::create_directories(copy / "src/moved");
  fs::rename(copy / "src/shape_area.cpp", copy / "src/moved/shape_area.cpp");
  {
    std::string text = read_file(copy / "src/moved/shape_area.cpp");
    const auto pos = text.find("return 1.0 * helper()");
    REQUIRE(pos != std::string::npos);
    text.replace(pos, std::string("return 1.0 * helper()").size(), "return 2.0 * helper()");
    write_file(copy / "src/moved/shape_area.cpp", text);
  }
  CompilationDatabase moved_db;
  moved_db.commands.push_back(make_command(copy, copy / "src/moved/shape_area.cpp",
                                           {"clang-cl.exe", "/c", "/std:c++20", "/EHsc", "/Iinclude", "src/moved/shape_area.cpp"}));
  UnitSelection sel = select_by_command_ids(moved_db, {moved_db.commands.front().command_id});
  auto results = analyze_selected(sel, options_for(copy));
  REQUIRE(results.size() == 1);
  const AnalysisResult& after = results.front();
  for (const auto& d : after.diagnostics) MESSAGE(d.text());
  REQUIRE(after.status == AnalysisStatus::ok);

  const Symbol* area_before = method_of(before.facts, "Shape", "area", "() const");
  const Symbol* area_after = method_of(after.facts, "Shape", "area", "() const");
  REQUIRE(area_before);
  REQUIRE(area_after);
  CHECK(area_before->id == area_after->id);
  const auto def_before = std::find_if(area_before->locations.begin(), area_before->locations.end(),
                                       [](const SymbolLocation& l) { return l.role == LocationRole::definition; });
  const auto def_after = std::find_if(area_after->locations.begin(), area_after->locations.end(),
                                      [](const SymbolLocation& l) { return l.role == LocationRole::definition; });
  REQUIRE(def_before != area_before->locations.end());
  REQUIRE(def_after != area_after->locations.end());
  CHECK(def_before->location.file.generic == "src/shape_area.cpp");
  CHECK(def_after->location.file.generic == "src/moved/shape_area.cpp");
  CHECK(def_before->body_hash != def_after->body_hash);
  CHECK(def_before->location.content_hash != def_after->location.content_hash);

  const auto helper_before = find_symbols(before.facts, "helper");
  const auto helper_after = find_symbols(after.facts, "helper");
  REQUIRE(helper_before.size() == 1);
  REQUIRE(helper_after.size() == 1);
  CHECK(helper_before.front()->id != helper_after.front()->id);
  std::error_code ec;
  fs::remove_all(work, ec);
}

#ifdef _WIN32
TEST_CASE("filesystem-authoritative root containment: a case-distinct sibling directory stays external") {
  const fs::path work = scratch_dir("sibling");
  fs::create_directories(work);
  if (!lcm::test_support::try_enable_case_sensitive_directory(work)) {
    MESSAGE("LIMITATION: per-directory case sensitivity unavailable; sibling regression not executed");
    std::error_code ec;
    fs::remove_all(work, ec);
    return;
  }
  const fs::path root = work / "CaseRoot";
  const fs::path sibling = work / "caseroot";
  fs::create_directories(root);
  fs::create_directories(sibling);
  REQUIRE_FALSE(fs::equivalent(root, sibling));
  write_file(sibling / "external.hpp", "int external_api();\n");
  write_file(root / "a.cpp", "#include \"external.hpp\"\nint uses() { return external_api(); }\n");
  const AnalysisResult r = gate(root, root / "a.cpp", {"clang-cl.exe", "/c", "/I../caseroot", "a.cpp"});
  for (const auto& d : r.diagnostics) MESSAGE(d.text());
  REQUIRE(r.status == AnalysisStatus::ok);
  const Symbol* ext = find_symbol(r.facts, "external_api", "()");
  REQUIRE(ext);
  CHECK(ext->presence == SymbolPresence::external_placeholder);
  CHECK(ext->key.repository_member == "external");
  CHECK(ext->locations.empty());
  const auto header = std::find_if(r.files.begin(), r.files.end(), [](const FileObservation& o) {
    return o.path_utf8.find("external.hpp") != std::string::npos;
  });
  REQUIRE(header != r.files.end());
  CHECK_FALSE(header->in_root);
  std::error_code ec;
  fs::remove_all(work, ec);
}
#endif

// =============================================================================
// Phase 1B: callables, hidden implicit members, initializers, CFG lifetime,
// parameter ABI ownership, captures and closure lifetime.
namespace {

std::vector<Evidence> evidence_list(const FactSet& f, const StableId& source, const StableId& target,
                                    RelationKind kind = RelationKind::calls) {
  const Relation* r = relation(f, source, target, kind);
  return r ? r->evidence : std::vector<Evidence>{};
}

template <typename Pred>
std::size_t count_evidence(const std::vector<Evidence>& list, Pred pred) {
  return static_cast<std::size_t>(std::count_if(list.begin(), list.end(), pred));
}

const SymbolLocation* location_with_role(const Symbol& s, LocationRole role) {
  for (const auto& l : s.locations) {
    if (l.role == role) return &l;
  }
  return nullptr;
}

std::vector<const Symbol*> lambdas_enclosed_by(const FactSet& f, const StableId& enclosing) {
  std::vector<const Symbol*> out;
  for (const auto& [id, s] : f.symbols()) {
    if (s.key.kind != SymbolKind::lambda || !std::holds_alternative<LocalScope>(s.key.linkage)) continue;
    if (std::get<LocalScope>(s.key.linkage).enclosing == enclosing) out.push_back(&s);
  }
  std::sort(out.begin(), out.end(), [](const Symbol* a, const Symbol* b) {
    return std::get<LocalScope>(a->key.linkage).ordinal < std::get<LocalScope>(b->key.linkage).ordinal;
  });
  return out;
}

const Symbol* closure_destructor(const FactSet& f, const StableId& lambda) {
  for (const auto& [id, s] : f.symbols()) {
    if (s.key.kind != SymbolKind::destructor || !std::holds_alternative<LocalScope>(s.key.linkage)) continue;
    const auto& scope = std::get<LocalScope>(s.key.linkage);
    if (scope.enclosing == lambda && scope.anchor == "closure-dtor") return &s;
  }
  return nullptr;
}

std::vector<CaptureFact> captures_of(const FactSet& f, const StableId& lambda) {
  std::vector<CaptureFact> out;
  for (const auto& c : f.captures()) {
    if (c.lambda == lambda) out.push_back(c);
  }
  return out;
}

const CaptureFact* capture_named(const std::vector<CaptureFact>& list, const std::string& name) {
  for (const auto& c : list) {
    if (c.name == name) return &c;
  }
  return nullptr;
}

}  // namespace

TEST_CASE("callable state: orthogonal defaulted/deleted flags per declaration, hidden used implicit members") {
  const fs::path work = scratch_dir("callables-a");
  const CompilationDatabase db = load_fixture_database(repo_root(), work);
  const AnalysisResult r = analyze_one(db, repo_root(), "src/callables.cpp");
  REQUIRE(r.status == AnalysisStatus::ok);
  const FactSet& f = r.facts;

  // Policy: state per declaration, out-of-line `= default` only on the definition.
  const Symbol* policy_default = method_of(f, "Policy", "Policy", "()");
  const Symbol* policy_copy = method_of(f, "Policy", "Policy", "(const Policy &)");
  const Symbol* policy_move = method_of(f, "Policy", "Policy", "(Policy &&)");
  const Symbol* policy_dtor = method_of(f, "Policy", "~Policy", "()");
  REQUIRE(policy_default);
  REQUIRE(policy_copy);
  REQUIRE(policy_move);
  REQUIRE(policy_dtor);
  REQUIRE(policy_default->locations.size() == 1);
  CHECK(policy_default->locations.front().callable.explicitly_defaulted);
  CHECK_FALSE(policy_default->locations.front().callable.deleted_as_written);
  CHECK_FALSE(policy_default->locations.front().callable.implicitly_deleted);
  REQUIRE(policy_copy->locations.size() == 1);
  CHECK(policy_copy->locations.front().callable.deleted_as_written);
  CHECK_FALSE(policy_copy->locations.front().callable.explicitly_defaulted);
  CHECK_FALSE(policy_copy->locations.front().callable.implicitly_deleted);
  CHECK(policy_move->locations.front().callable.explicitly_defaulted);
  CHECK(policy_dtor->locations.size() == 2);
  const SymbolLocation* dtor_decl = location_with_role(*policy_dtor, LocationRole::declaration);
  const SymbolLocation* dtor_def = location_with_role(*policy_dtor, LocationRole::definition);
  REQUIRE(dtor_decl);
  REQUIRE(dtor_def);
  CHECK_FALSE(dtor_decl->callable.explicitly_defaulted);
  CHECK(dtor_def->callable.explicitly_defaulted);
  CHECK_FALSE(dtor_def->body_hash.has_value());  // `= default;` has no body to hash
  // The deleted copy constructor is a symbol but never a call target.
  for (const auto& [key, rel] : f.relations()) {
    CHECK_FALSE((key.target == policy_copy->id && key.kind == RelationKind::calls));
  }

  // Wrap: explicitly defaulted AND implicitly deleted at once; both flags survive.
  const Symbol* wrap_copy = method_of(f, "Wrap", "Wrap", "(const Wrap &)");
  const Symbol* wrap_assign = method_of(f, "Wrap", "operator=", "(const Wrap &)");
  const Symbol* wrap_dtor = method_of(f, "Wrap", "~Wrap", "()");
  const Symbol* wrap_default = method_of(f, "Wrap", "Wrap", "()");
  REQUIRE(wrap_copy);
  REQUIRE(wrap_assign);
  REQUIRE(wrap_dtor);
  REQUIRE(wrap_default);
  CHECK(wrap_copy->locations.front().callable.explicitly_defaulted);
  CHECK(wrap_copy->locations.front().callable.implicitly_deleted);
  CHECK_FALSE(wrap_copy->locations.front().callable.deleted_as_written);
  CHECK(wrap_assign->locations.front().callable.deleted_as_written);
  CHECK_FALSE(wrap_assign->locations.front().callable.implicitly_deleted);
  CHECK(wrap_dtor->locations.front().callable.explicitly_defaulted);
  CHECK(wrap_dtor->locations.front().callable.trivial);
  REQUIRE(wrap_default->locations.size() == 2);
  CHECK_FALSE(location_with_role(*wrap_default, LocationRole::declaration)->callable.explicitly_defaulted);
  CHECK(location_with_role(*wrap_default, LocationRole::definition)->callable.explicitly_defaulted);

  // Plain: the copied object references the implicit copy constructor and destructor only.
  const Symbol* copy_plain = find_symbol(f, "copy_plain", "(const Plain &)");
  const Symbol* plain = find_symbol(f, "Plain", "", SymbolKind::struct_);
  const Symbol* plain_copy = method_of(f, "Plain", "Plain", "(const Plain &)");
  const Symbol* plain_dtor = method_of(f, "Plain", "~Plain", "()");
  const Symbol* tracker_copy = method_of(f, "Tracker", "Tracker", "(const Tracker &)");
  const Symbol* tracker_dtor = method_of(f, "Tracker", "~Tracker", "()");
  REQUIRE(copy_plain);
  REQUIRE(plain);
  REQUIRE(plain_copy);
  REQUIRE(plain_dtor);
  REQUIRE(tracker_copy);
  REQUIRE(tracker_dtor);
  CHECK(plain_copy->presence == SymbolPresence::implicit_hidden);
  CHECK(plain_dtor->presence == SymbolPresence::implicit_hidden);
  REQUIRE(plain_copy->locations.size() == 1);
  CHECK(plain_copy->locations.front().role == LocationRole::implicit_declaration);
  CHECK(plain_copy->locations.front().analysis_unit == r.analysis_unit);
  CHECK_FALSE(plain_copy->locations.front().callable.trivial);  // Tracker has a user copy constructor
  CHECK_FALSE(plain_copy->locations.front().body_hash.has_value());
  CHECK(plain_copy->locations.front().location.file.generic == "src/callables.cpp");
  CHECK(plain_dtor->locations.front().role == LocationRole::implicit_declaration);
  CHECK_FALSE(plain_dtor->locations.front().callable.trivial);  // Tracker has a user destructor
  CHECK(has_relation(f, plain->id, plain_copy->id, RelationKind::contains));
  CHECK(has_relation(f, plain->id, plain_dtor->id, RelationKind::contains));
  // The copy is a confirmed call from copy_plain; the hidden copy constructor owns its member copy.
  CHECK(plain_aspects(first_evidence(f, copy_plain->id, plain_copy->id)));
  const auto member_copy = evidence_list(f, plain_copy->id, tracker_copy->id);
  REQUIRE(member_copy.size() == 1);
  CHECK(member_copy.front().evaluation == EvaluationContext::implicit_mem_initializer);
  CHECK(member_copy.front().subobject_role == SubobjectRole::member);
  CHECK(member_copy.front().subobject == "t");
  CHECK_FALSE(has_relation(f, copy_plain->id, tracker_copy->id, RelationKind::calls));  // not flattened
  // b's destruction belongs to copy_plain as implicit lifetime; ~Plain owns ~Tracker for t.
  const auto b_dtor = evidence_list(f, copy_plain->id, plain_dtor->id);
  REQUIRE(b_dtor.size() == 1);
  CHECK(b_dtor.front().lifetime == LifetimeKind::automatic_object);
  CHECK_FALSE(b_dtor.front().potentially_elided);
  const auto t_dtor = evidence_list(f, plain_dtor->id, tracker_dtor->id);
  REQUIRE(t_dtor.size() == 1);
  CHECK(t_dtor.front().lifetime == LifetimeKind::subobject);
  CHECK(t_dtor.front().subobject_role == SubobjectRole::member);
  CHECK(t_dtor.front().subobject == "t");
  CHECK_FALSE(has_relation(f, copy_plain->id, tracker_dtor->id, RelationKind::calls));
  // Unused implicit members are not nodes; unreferenced implicit destructors neither.
  CHECK(method_of(f, "Plain", "Plain", "(Plain &&)") == nullptr);
  CHECK(method_of(f, "Plain", "Plain", "()") == nullptr);
  CHECK(method_of(f, "Plain", "operator=") == nullptr);
  CHECK(method_of(f, "Unreferenced", "~Unreferenced") == nullptr);
  CHECK(method_of(f, "Unreferenced", "Unreferenced", "(const Unreferenced &)") == nullptr);
  for (const auto& [id, s] : f.symbols()) {
    if (s.presence == SymbolPresence::implicit_hidden) {
      CHECK(s.locations.size() == 1);
      CHECK(s.locations.front().role == LocationRole::implicit_declaration);
      CHECK(s.key.repository_member == "repo");
    }
  }

  std::error_code ec;
  fs::remove_all(work, ec);
}

TEST_CASE("initializers, construction, conversion, user operators and compiler-CFG lifetime facts") {
  const fs::path work = scratch_dir("callables-b");
  const CompilationDatabase db = load_fixture_database(repo_root(), work);
  const AnalysisResult r = analyze_one(db, repo_root(), "src/callables.cpp");
  REQUIRE(r.status == AnalysisStatus::ok);
  const FactSet& f = r.facts;
  const std::string bytes = read_file(repo_root() / "src/callables.cpp");
  LineIndex lines(bytes);
  const auto line_of = [&](const char* text) {
    const auto pos = bytes.find(text);
    REQUIRE(pos != std::string::npos);
    return lines.line_column(static_cast<std::uint32_t>(pos)).first;
  };

  const Symbol* box_ctor = method_of(f, "Box", "Box", "(int, Part)");
  const Symbol* base_ctor = method_of(f, "Base", "Base", "(int)");
  const Symbol* base_dtor = method_of(f, "Base", "~Base", "()");
  const Symbol* part_move = method_of(f, "Part", "Part", "(Part &&)");
  const Symbol* part_copy = method_of(f, "Part", "Part", "(const Part &)");
  const Symbol* part_dtor = method_of(f, "Part", "~Part", "()");
  const Symbol* part_conv = method_of(f, "Part", "operator int", "() const");
  const Symbol* tracker_default = method_of(f, "Tracker", "Tracker", "()");
  const Symbol* tracker_move = method_of(f, "Tracker", "Tracker", "(Tracker &&)");
  const Symbol* tracker_dtor = method_of(f, "Tracker", "~Tracker", "()");
  const Symbol* plus = find_symbol(f, "operator+", "(const Part &, const Part &)", SymbolKind::operator_function);
  const Symbol* build = find_symbol(f, "build", "(bool, Part)");
  REQUIRE(box_ctor);
  REQUIRE(base_ctor);
  REQUIRE(base_dtor);
  REQUIRE(part_move);
  REQUIRE(part_copy);
  REQUIRE(part_dtor);
  REQUIRE(part_conv);
  REQUIRE(tracker_default);
  REQUIRE(tracker_move);
  REQUIRE(tracker_dtor);
  REQUIRE(plus);
  REQUIRE(build);

  // Written base/member initializers and the synthesized member initializer belong to Box::Box.
  const auto base_init = evidence_list(f, box_ctor->id, base_ctor->id);
  REQUIRE(base_init.size() == 1);
  CHECK(base_init.front().evaluation == EvaluationContext::mem_initializer);
  CHECK(base_init.front().subobject_role == SubobjectRole::base);
  CHECK(base_init.front().subobject == "Base");
  const auto part_init = evidence_list(f, box_ctor->id, part_move->id);
  REQUIRE(part_init.size() == 1);
  CHECK(part_init.front().evaluation == EvaluationContext::mem_initializer);
  CHECK(part_init.front().subobject_role == SubobjectRole::member);
  CHECK(part_init.front().subobject == "part");
  const auto tracker_init = evidence_list(f, box_ctor->id, tracker_default->id);
  REQUIRE(tracker_init.size() == 1);
  CHECK(tracker_init.front().evaluation == EvaluationContext::implicit_mem_initializer);
  CHECK(tracker_init.front().subobject_role == SubobjectRole::member);
  CHECK(tracker_init.front().subobject == "tracker");
  CHECK_FALSE(has_relation(f, build->id, base_ctor->id, RelationKind::calls));   // no flattening
  CHECK_FALSE(has_relation(f, build->id, part_move->id, RelationKind::calls));

  // build: constructor, argument copy, user operator+, explicit conversion; no built-in callable.
  CHECK(plain_aspects(first_evidence(f, build->id, box_ctor->id)));
  CHECK(has_relation(f, build->id, part_copy->id, RelationKind::calls));  // src copied into the parameter
  CHECK(plain_aspects(first_evidence(f, build->id, plus->id)));
  CHECK(plain_aspects(first_evidence(f, build->id, part_conv->id)));
  CHECK(find_symbols(f, "operator+").size() == 2);  // the free Part operator+ and Sink::operator+
  for (const auto& [id, s] : f.symbols()) {
    if (s.key.kind != SymbolKind::operator_function) continue;
    const bool user_declared = s.key.canonical_name == "operator+" || s.key.canonical_name == "operator-" ||
                               s.key.canonical_name == "operator()" || s.key.canonical_name == "operator=";
    CHECK_MESSAGE(user_declared, s.key.canonical_name);
    CHECK_FALSE(s.locations.empty());  // every operator node is a user declaration, never a built-in
  }

  // Automatic destruction from the compiler CFG: once per object, anchored at its declaration,
  // regardless of the early return and the nested block; arrays once with the element destructor.
  const auto build_trackers = evidence_list(f, build->id, tracker_dtor->id);
  REQUIRE(build_trackers.size() == 3);
  std::vector<std::uint32_t> tracker_lines;
  for (const auto& e : build_trackers) {
    CHECK(e.lifetime == LifetimeKind::automatic_object);
    CHECK_FALSE(e.potentially_elided);
    tracker_lines.push_back(e.location.span.begin_line);
  }
  std::sort(tracker_lines.begin(), tracker_lines.end());
  CHECK(tracker_lines == std::vector<std::uint32_t>{line_of("Tracker inner;"), line_of("Tracker nested;"),
                                                     line_of("Tracker arr[2];")});
  const Symbol* box_dtor = method_of(f, "Box", "~Box", "()");
  REQUIRE(box_dtor);
  CHECK(box_dtor->presence == SymbolPresence::implicit_hidden);
  const auto box_dtors = evidence_list(f, build->id, box_dtor->id);
  REQUIRE(box_dtors.size() == 1);
  CHECK(box_dtors.front().lifetime == LifetimeKind::automatic_object);
  CHECK(box_dtors.front().location.span.begin_line == line_of("Box box(1, src);"));
  // ~Box (hidden) owns its subobjects; build does not.
  const auto box_parts = evidence_list(f, box_dtor->id, part_dtor->id);
  REQUIRE(box_parts.size() == 1);
  CHECK(box_parts.front().lifetime == LifetimeKind::subobject);
  CHECK(box_parts.front().subobject == "part");
  CHECK(evidence_list(f, box_dtor->id, tracker_dtor->id).size() == 1);
  CHECK(relations_from(f, box_dtor->id, RelationKind::calls).size() == 3);  // ~Part, ~Tracker, ~Base only
  const auto box_base = evidence_list(f, box_dtor->id, base_dtor->id);
  REQUIRE(box_base.size() == 1);
  CHECK(box_base.front().subobject_role == SubobjectRole::base);
  CHECK_FALSE(has_relation(f, build->id, base_dtor->id, RelationKind::calls));

  // Guaranteed prvalue elision versus a discarded temporary.
  const Symbol* elision = find_symbol(f, "elision", "()");
  REQUIRE(elision);
  const auto elision_dtors = evidence_list(f, elision->id, tracker_dtor->id);
  REQUIRE(elision_dtors.size() == 2);
  CHECK(count_evidence(elision_dtors, [](const Evidence& e) { return e.lifetime == LifetimeKind::automatic_object; }) == 1);
  CHECK(count_evidence(elision_dtors, [](const Evidence& e) { return e.lifetime == LifetimeKind::temporary; }) == 1);
  for (const auto& e : elision_dtors) {
    if (e.lifetime == LifetimeKind::temporary) CHECK(e.location.span.begin_line == line_of("make();                 // discarded"));
  }
  // `Part sum = src + src;` initializes sum directly: one automatic destruction, no temporary.
  const auto build_parts = evidence_list(f, build->id, part_dtor->id);
  CHECK(count_evidence(build_parts, [](const Evidence& e) { return e.lifetime == LifetimeKind::temporary; }) == 0);
  CHECK(count_evidence(build_parts, [](const Evidence& e) { return e.lifetime == LifetimeKind::automatic_object; }) == 1);

  // NRVO candidate: destruction and return moves are potentially elided; two named returns are not.
  const Symbol* pure_nrvo = find_symbol(f, "pure_nrvo", "(bool)");
  const Symbol* two_names = find_symbol(f, "two_names", "(bool)");
  REQUIRE(pure_nrvo);
  REQUIRE(two_names);
  const auto nrvo_dtor = evidence_list(f, pure_nrvo->id, tracker_dtor->id);
  REQUIRE(nrvo_dtor.size() == 1);
  CHECK(nrvo_dtor.front().potentially_elided);
  CHECK(nrvo_dtor.front().lifetime == LifetimeKind::automatic_object);
  const auto nrvo_moves = evidence_list(f, pure_nrvo->id, tracker_move->id);
  REQUIRE(nrvo_moves.size() == 2);
  for (const auto& e : nrvo_moves) CHECK(e.potentially_elided);
  const auto two_dtors = evidence_list(f, two_names->id, tracker_dtor->id);
  REQUIRE(two_dtors.size() == 2);
  for (const auto& e : two_dtors) CHECK_FALSE(e.potentially_elided);
  for (const auto& e : evidence_list(f, two_names->id, tracker_move->id)) CHECK_FALSE(e.potentially_elided);

  // A user destructor owns its subobject destruction (non-trivial members and the base only).
  const Symbol* owner_dtor = method_of(f, "Owner", "~Owner", "()");
  REQUIRE(owner_dtor);
  const auto owner_members = evidence_list(f, owner_dtor->id, tracker_dtor->id);
  REQUIRE(owner_members.size() == 2);
  std::vector<std::string> names;
  for (const auto& e : owner_members) {
    CHECK(e.lifetime == LifetimeKind::subobject);
    CHECK(e.subobject_role == SubobjectRole::member);
    names.push_back(e.subobject);
  }
  std::sort(names.begin(), names.end());
  CHECK(names == std::vector<std::string>{"a", "c"});
  const auto owner_base = evidence_list(f, owner_dtor->id, base_dtor->id);
  REQUIRE(owner_base.size() == 1);
  CHECK(owner_base.front().subobject_role == SubobjectRole::base);
  CHECK(owner_base.front().subobject == "Base");

  // A used implicit destructor: hidden identity, real CFG member/base elements.
  const Symbol* use_implicit = find_symbol(f, "use_implicit", "()");
  const Symbol* implicit_dtor = method_of(f, "Implicit", "~Implicit", "()");
  const Symbol* implicit_record = find_symbol(f, "Implicit", "", SymbolKind::struct_);
  REQUIRE(use_implicit);
  REQUIRE(implicit_dtor);
  REQUIRE(implicit_record);
  CHECK(implicit_dtor->presence == SymbolPresence::implicit_hidden);
  CHECK(has_relation(f, implicit_record->id, implicit_dtor->id, RelationKind::contains));
  CHECK(evidence_list(f, use_implicit->id, implicit_dtor->id).front().lifetime == LifetimeKind::automatic_object);
  const auto implicit_member = evidence_list(f, implicit_dtor->id, tracker_dtor->id);
  REQUIRE(implicit_member.size() == 1);
  CHECK(implicit_member.front().subobject == "m");
  CHECK(evidence_list(f, implicit_dtor->id, base_dtor->id).size() == 1);
  CHECK_FALSE(has_relation(f, use_implicit->id, tracker_dtor->id, RelationKind::calls));
  CHECK_FALSE(has_relation(f, use_implicit->id, base_dtor->id, RelationKind::calls));

  // Every lifetime evidence points into the fixture file with a verifiable span.
  for (const auto& [key, rel] : f.relations()) {
    for (const auto& e : rel.evidence) {
      if (e.lifetime == LifetimeKind::none) continue;
      CHECK(e.location.file.generic == "src/callables.cpp");
      CHECK(e.location.span_hash.has_value());
      CHECK(e.location.span.end_offset > e.location.span.begin_offset);
    }
  }
  CHECK(std::none_of(r.limits.begin(), r.limits.end(),
                     [](const std::string& l) { return l.find("control-flow graph unavailable") != std::string::npos; }));

  std::error_code ec;
  fs::remove_all(work, ec);
}

TEST_CASE("by-value parameter destruction follows the compiler's ABI decision, not the driver or a definition") {
  const fs::path work = scratch_dir("callables-abi");
  const CompilationDatabase db = load_fixture_database(repo_root(), work);
  const fs::path file = repo_root() / "src/callables.cpp";

  const auto check_callee_destroyed = [&](const FactSet& f, const char* mode) {
    CAPTURE(mode);
    const Symbol* take_def = find_symbol(f, "take_def", "(Tracker)");
    const Symbol* take_decl = find_symbol(f, "take_decl", "(Tracker)");
    const Symbol* call_both = find_symbol(f, "call_both", "(const Tracker &)");
    const Symbol* tracker_dtor = method_of(f, "Tracker", "~Tracker", "()");
    const Symbol* tracker_copy = method_of(f, "Tracker", "Tracker", "(const Tracker &)");
    const Symbol* build = find_symbol(f, "build", "(bool, Part)");
    const Symbol* box_ctor = method_of(f, "Box", "Box", "(int, Part)");
    const Symbol* part_dtor = method_of(f, "Part", "~Part", "()");
    REQUIRE(take_def);
    REQUIRE(take_decl);
    REQUIRE(call_both);
    REQUIRE(tracker_dtor);
    REQUIRE(tracker_copy);
    REQUIRE(build);
    REQUIRE(box_ctor);
    REQUIRE(part_dtor);
    // The caller copies the arguments; it does not destroy them.
    CHECK(evidence_list(f, call_both->id, tracker_copy->id).size() == 2);
    CHECK(evidence_list(f, call_both->id, tracker_dtor->id).empty());
    // The defining callee owns its parameter: definition evidence plus the callsite observation.
    const auto def_dtors = evidence_list(f, take_def->id, tracker_dtor->id);
    REQUIRE(def_dtors.size() == 2);
    for (const auto& e : def_dtors) CHECK(e.lifetime == LifetimeKind::parameter);
    // A declaration-only in-root callee still owns it (callsite plus ABI flag), but no body is manufactured.
    const auto decl_dtors = evidence_list(f, take_decl->id, tracker_dtor->id);
    REQUIRE(decl_dtors.size() == 1);
    CHECK(decl_dtors.front().lifetime == LifetimeKind::parameter);
    CHECK(take_decl->locations.size() == 1);
    CHECK(take_decl->locations.front().role == LocationRole::declaration);
    // Box(int, Part p): p is destroyed by Box::Box, not by build; build still destroys `sum` and `src`.
    CHECK(count_evidence(evidence_list(f, box_ctor->id, part_dtor->id),
                         [](const Evidence& e) { return e.lifetime == LifetimeKind::parameter; }) == 2);
    const auto build_parts = evidence_list(f, build->id, part_dtor->id);
    CHECK(count_evidence(build_parts, [](const Evidence& e) { return e.lifetime == LifetimeKind::temporary; }) == 0);
    CHECK(count_evidence(build_parts, [](const Evidence& e) { return e.lifetime == LifetimeKind::parameter; }) == 1);
    CHECK(count_evidence(build_parts, [](const Evidence& e) { return e.lifetime == LifetimeKind::automatic_object; }) == 1);

    // Cross-composition (coordinator review risk): closure operator(), member operator()/operator+,
    // free operator-, template instantiation, unknown callee. The caller never owns these destructions.
    const Symbol* compose = find_symbol(f, "compose", "(const Tracker &, void (*)(Tracker), const Sink &)");
    const Symbol* sink_call = method_of(f, "Sink", "operator()", "(Tracker) const");
    const Symbol* sink_plus = method_of(f, "Sink", "operator+", "(Tracker) const");
    const Symbol* free_minus = find_symbol(f, "operator-", "(const Sink &, Tracker)", SymbolKind::operator_function);
    const Symbol* generic = find_symbol(f, "generic_take", "", SymbolKind::function, TemplateRole::primary);
    REQUIRE(compose);
    REQUIRE(sink_call);
    REQUIRE(sink_plus);
    REQUIRE(free_minus);
    REQUIRE(generic);
    const auto compose_lambdas = lambdas_enclosed_by(f, compose->id);
    REQUIRE(compose_lambdas.size() == 1);
    const Symbol* take_lambda = compose_lambdas.front();
    // (a) closure operator()(Tracker): the lambda owns it (definition plus callsite), not compose.
    const auto lambda_dtors = evidence_list(f, take_lambda->id, tracker_dtor->id);
    REQUIRE(lambda_dtors.size() == 2);
    for (const auto& e : lambda_dtors) CHECK(e.lifetime == LifetimeKind::parameter);
    // (b) member operators: one callsite evidence each; the object argument produced nothing.
    const auto call_op = evidence_list(f, sink_call->id, tracker_dtor->id);
    REQUIRE(call_op.size() == 1);
    CHECK(call_op.front().lifetime == LifetimeKind::parameter);
    const auto plus_op = evidence_list(f, sink_plus->id, tracker_dtor->id);
    REQUIRE(plus_op.size() == 1);
    CHECK(plus_op.front().lifetime == LifetimeKind::parameter);
    const auto minus_op = evidence_list(f, free_minus->id, tracker_dtor->id);
    REQUIRE(minus_op.size() == 1);
    CHECK(minus_op.front().lifetime == LifetimeKind::parameter);
    // (c) template instantiation: concrete parameter types decide ownership, then the owner folds to
    // the primary with the use-site arguments; the pattern's own parameter adds the definition evidence.
    const auto generic_dtors = evidence_list(f, generic->id, tracker_dtor->id);
    REQUIRE(generic_dtors.size() == 2);
    CHECK(count_evidence(generic_dtors, [](const Evidence& e) {
            return e.lifetime == LifetimeKind::parameter && e.template_use == TemplateUse::primary_implicit &&
                   e.template_arguments == "<int>";
          }) == 1);
    CHECK(count_evidence(generic_dtors, [](const Evidence& e) {
            return e.lifetime == LifetimeKind::parameter && e.template_use == TemplateUse::none;
          }) == 1);
    // (c') dependent parameter type: after folding, the pattern's parameter is `T` and could never decide
    // anything; the concrete instantiation (Tracker) did. One callee-owned observation with the concrete
    // destructor and the use-site arguments; the pattern definition itself adds nothing.
    const Symbol* dependent = find_symbol(f, "dependent_take", "", SymbolKind::function, TemplateRole::primary);
    REQUIRE(dependent);
    const auto dependent_dtors = evidence_list(f, dependent->id, tracker_dtor->id);
    REQUIRE(dependent_dtors.size() == 1);
    CHECK(dependent_dtors.front().lifetime == LifetimeKind::parameter);
    CHECK(dependent_dtors.front().template_use == TemplateUse::primary_implicit);
    CHECK(dependent_dtors.front().template_arguments == "<Tracker>");
    CHECK(relations_from(f, dependent->id, RelationKind::calls).size() == 1);  // only ~Tracker, nothing invented
    // (d) unknown callee: no caller-owned temporary; the destruction is an unresolved site of compose.
    CHECK(evidence_list(f, compose->id, tracker_dtor->id).empty());
    std::size_t argument_sites = 0;
    for (const auto& site : f.unresolved_sites()) {
      if (site.enclosing != compose->id) continue;
      if (site.expression == "t") {
        ++argument_sites;
        CHECK(site.reason == UnresolvedReason::indirect_callee);
      } else {
        CHECK(site.expression == "sink");
      }
    }
    CHECK(argument_sites == 1);
    // compose copies t into every by-value argument (seven calls), and that is all it does to Tracker.
    CHECK(evidence_list(f, compose->id, tracker_copy->id).size() == 7);

    // (e) pointer to member function through .* and ->*: the callee is unknown, the parameter prototype is
    // the member pointer's pointee. Callee-destroyed here: no caller-owned temporary, one unresolved
    // argument destruction per call, the callee sites stay unresolved, Mem::take is never confirmed.
    const Symbol* indirect_member =
        find_symbol(f, "indirect_member", "(Mem &, Mem *, void (Mem::*)(Tracker), const Tracker &)");
    const Symbol* mem_take = method_of(f, "Mem", "take", "(Tracker)");
    REQUIRE(indirect_member);
    REQUIRE(mem_take);
    CHECK(evidence_list(f, indirect_member->id, tracker_dtor->id).empty());
    CHECK(evidence_list(f, indirect_member->id, tracker_copy->id).size() == 2);
    CHECK_FALSE(has_relation(f, indirect_member->id, mem_take->id, RelationKind::calls));
    CHECK(relations_from(f, mem_take->id, RelationKind::calls).empty());
    std::size_t member_callee_sites = 0;
    std::size_t member_argument_sites = 0;
    for (const auto& site : f.unresolved_sites()) {
      if (site.enclosing != indirect_member->id) continue;
      CHECK(site.reason == UnresolvedReason::indirect_callee);
      if (site.expression == "value") {
        ++member_argument_sites;
      } else {
        CHECK((site.expression == "object.*member" || site.expression == "pointer->*member"));
        ++member_callee_sites;
      }
    }
    CHECK(member_callee_sites == 2);
    CHECK(member_argument_sites == 2);
  };

  SUBCASE("default cl driver mode targets the Windows MSVC ABI: callee destroys") {
    const AnalysisResult r = analyze_one(db, repo_root(), "src/callables.cpp");
    REQUIRE(r.status == AnalysisStatus::ok);
    check_callee_destroyed(r.facts, "cl");
  }
  SUBCASE("default g++ driver mode on this host also targets the MSVC ABI: callee destroys") {
    const AnalysisResult r = gate(repo_root(), file, {"clang++", "-std=c++20", "-c", "src/callables.cpp"});
    for (const auto& d : r.diagnostics) MESSAGE(d.text());
    REQUIRE(r.status == AnalysisStatus::ok);
    check_callee_destroyed(r.facts, "g++");
  }
  SUBCASE("explicit Itanium target: the caller destroys the argument temporaries") {
    const AnalysisResult r = gate(repo_root(), file,
                                  {"clang++", "-std=c++20", "-target", "x86_64-unknown-linux-gnu", "-c", "src/callables.cpp"});
    for (const auto& d : r.diagnostics) MESSAGE(d.text());
    REQUIRE(r.status == AnalysisStatus::ok);
    const FactSet& f = r.facts;
    const Symbol* take_def = find_symbol(f, "take_def", "(Tracker)");
    const Symbol* take_decl = find_symbol(f, "take_decl", "(Tracker)");
    const Symbol* call_both = find_symbol(f, "call_both", "(const Tracker &)");
    const Symbol* tracker_dtor = method_of(f, "Tracker", "~Tracker", "()");
    const Symbol* build = find_symbol(f, "build", "(bool, Part)");
    const Symbol* box_ctor = method_of(f, "Box", "Box", "(int, Part)");
    const Symbol* part_dtor = method_of(f, "Part", "~Part", "()");
    REQUIRE(take_def);
    REQUIRE(take_decl);
    REQUIRE(call_both);
    REQUIRE(tracker_dtor);
    REQUIRE(build);
    REQUIRE(box_ctor);
    REQUIRE(part_dtor);
    const auto caller_dtors = evidence_list(f, call_both->id, tracker_dtor->id);
    REQUIRE(caller_dtors.size() == 2);
    for (const auto& e : caller_dtors) CHECK(e.lifetime == LifetimeKind::temporary);
    CHECK(evidence_list(f, take_def->id, tracker_dtor->id).empty());
    CHECK(evidence_list(f, take_decl->id, tracker_dtor->id).empty());
    CHECK(evidence_list(f, box_ctor->id, part_dtor->id).empty());
    const auto build_parts = evidence_list(f, build->id, part_dtor->id);
    CHECK(count_evidence(build_parts, [](const Evidence& e) { return e.lifetime == LifetimeKind::temporary; }) == 1);
    CHECK(count_evidence(build_parts, [](const Evidence& e) { return e.lifetime == LifetimeKind::parameter; }) == 0);
    CHECK(count_evidence(build_parts, [](const Evidence& e) { return e.lifetime == LifetimeKind::automatic_object; }) == 1);
    // Cross-composition under Itanium: the caller destroys all seven argument temporaries, including the
    // one passed to the unknown callee and the dependent-parameter instantiation; no callee owns anything
    // and no argument site is unresolved.
    const Symbol* compose = find_symbol(f, "compose", "(const Tracker &, void (*)(Tracker), const Sink &)");
    const Symbol* sink_call = method_of(f, "Sink", "operator()", "(Tracker) const");
    const Symbol* generic = find_symbol(f, "generic_take", "", SymbolKind::function, TemplateRole::primary);
    const Symbol* dependent = find_symbol(f, "dependent_take", "", SymbolKind::function, TemplateRole::primary);
    REQUIRE(compose);
    REQUIRE(sink_call);
    REQUIRE(generic);
    REQUIRE(dependent);
    const auto compose_dtors = evidence_list(f, compose->id, tracker_dtor->id);
    REQUIRE(compose_dtors.size() == 7);
    for (const auto& e : compose_dtors) CHECK(e.lifetime == LifetimeKind::temporary);
    CHECK(evidence_list(f, sink_call->id, tracker_dtor->id).empty());
    CHECK(evidence_list(f, generic->id, tracker_dtor->id).empty());
    CHECK(evidence_list(f, dependent->id, tracker_dtor->id).empty());
    CHECK(relations_from(f, dependent->id, RelationKind::calls).empty());
    const auto compose_lambdas = lambdas_enclosed_by(f, compose->id);
    REQUIRE(compose_lambdas.size() == 1);
    CHECK(evidence_list(f, compose_lambdas.front()->id, tracker_dtor->id).empty());
    for (const auto& site : f.unresolved_sites()) {
      if (site.enclosing == compose->id) CHECK(site.expression == "sink");
    }
    // Pointer to member through .* and ->* under Itanium: the caller destroys the two argument
    // temporaries, no argument site is unresolved, the callee sites stay unresolved, Mem::take unconfirmed.
    const Symbol* indirect_member =
        find_symbol(f, "indirect_member", "(Mem &, Mem *, void (Mem::*)(Tracker), const Tracker &)");
    const Symbol* mem_take = method_of(f, "Mem", "take", "(Tracker)");
    REQUIRE(indirect_member);
    REQUIRE(mem_take);
    const auto member_dtors = evidence_list(f, indirect_member->id, tracker_dtor->id);
    REQUIRE(member_dtors.size() == 2);
    for (const auto& e : member_dtors) CHECK(e.lifetime == LifetimeKind::temporary);
    CHECK_FALSE(has_relation(f, indirect_member->id, mem_take->id, RelationKind::calls));
    std::size_t member_callee_sites = 0;
    for (const auto& site : f.unresolved_sites()) {
      if (site.enclosing != indirect_member->id) continue;
      CHECK(site.expression != "value");
      ++member_callee_sites;
    }
    CHECK(member_callee_sites == 2);
  }

  std::error_code ec;
  fs::remove_all(work, ec);
}

TEST_CASE("captures, closure lifetime as a hidden destructor, nested lambdas and provable indirect calls") {
  const fs::path work = scratch_dir("callables-c");
  const CompilationDatabase db = load_fixture_database(repo_root(), work);
  const AnalysisResult r = analyze_one(db, repo_root(), "src/callables.cpp");
  REQUIRE(r.status == AnalysisStatus::ok);
  const FactSet& f = r.facts;

  const Symbol* run = method_of(f, "Worker", "run", "(int (*)(int))");
  const Symbol* target = find_symbol(f, "target", "(int)");
  const Symbol* make_value = find_symbol(f, "make_value", "()");
  const Symbol* big_copy = method_of(f, "Big", "Big", "(const Big &)");
  const Symbol* big_dtor = method_of(f, "Big", "~Big", "()");
  const Symbol* worker_copy = method_of(f, "Worker", "Worker", "(const Worker &)");
  REQUIRE(run);
  REQUIRE(target);
  REQUIRE(make_value);
  REQUIRE(big_copy);
  REQUIRE(big_dtor);
  REQUIRE(worker_copy);
  const auto lambdas = lambdas_enclosed_by(f, run->id);
  REQUIRE(lambdas.size() == 3);
  const Symbol* l1 = lambdas[0];
  const Symbol* l2 = lambdas[1];
  const Symbol* l3 = lambdas[2];
  const auto nested = lambdas_enclosed_by(f, l3->id);
  REQUIRE(nested.size() == 1);
  const Symbol* inner = nested.front();

  // l1: explicit value/reference/this/init/class-copy captures with target descriptors.
  const auto c1 = captures_of(f, l1->id);
  REQUIRE(c1.size() == 5);
  const CaptureFact* x = capture_named(c1, "x");
  const CaptureFact* y = capture_named(c1, "y");
  const CaptureFact* self = capture_named(c1, "this");
  const CaptureFact* z = capture_named(c1, "z");
  const CaptureFact* big = capture_named(c1, "big");
  REQUIRE(x);
  REQUIRE(y);
  REQUIRE(self);
  REQUIRE(z);
  REQUIRE(big);
  CHECK(x->kind == CaptureKind::by_copy);
  CHECK(x->explicit_capture);
  CHECK_FALSE(x->init_capture);
  CHECK(y->kind == CaptureKind::by_reference);
  CHECK(y->explicit_capture);
  CHECK(self->kind == CaptureKind::this_pointer);
  CHECK(self->explicit_capture);
  CHECK(self->target_descriptor.empty());
  CHECK(z->kind == CaptureKind::by_copy);
  CHECK(z->init_capture);
  CHECK(big->kind == CaptureKind::by_copy);
  CHECK_FALSE(big->init_capture);
  CHECK_FALSE(x->target_descriptor.empty());
  CHECK(x->target_descriptor != y->target_descriptor);
  CHECK(x->target_descriptor != z->target_descriptor);
  for (const auto& c : c1) {
    CHECK(c.location.file.generic == "src/callables.cpp");
    CHECK(c.analysis_unit == r.analysis_unit);
  }
  // Locals are not public symbols.
  CHECK(find_symbol(f, "x", "", SymbolKind::variable) == nullptr);
  CHECK(find_symbol(f, "big", "", SymbolKind::variable) == nullptr);
  CHECK(find_symbol(f, "x", "", SymbolKind::local_variable) == nullptr);

  // l2: implicit by-reference default captures x and this.
  const auto c2 = captures_of(f, l2->id);
  REQUIRE(c2.size() == 2);
  REQUIRE(capture_named(c2, "x"));
  REQUIRE(capture_named(c2, "this"));
  CHECK(capture_named(c2, "x")->kind == CaptureKind::by_reference);
  CHECK_FALSE(capture_named(c2, "x")->explicit_capture);
  CHECK_FALSE(capture_named(c2, "this")->explicit_capture);
  CHECK(capture_named(c2, "this")->kind == CaptureKind::this_pointer);
  CHECK(capture_named(c2, "x")->target_descriptor == x->target_descriptor);  // same local declaration

  // l3: *this by copy; nested lambda captures this explicitly and is owned by l3.
  const auto c3 = captures_of(f, l3->id);
  REQUIRE(c3.size() == 1);
  CHECK(c3.front().kind == CaptureKind::star_this);
  CHECK(c3.front().explicit_capture);
  const auto c_inner = captures_of(f, inner->id);
  REQUIRE(c_inner.size() == 1);
  CHECK(c_inner.front().kind == CaptureKind::this_pointer);
  CHECK(c_inner.front().explicit_capture);

  // Initializer versus body ownership: init-capture call and capture copies belong to run.
  const auto mv = evidence_list(f, run->id, make_value->id);
  REQUIRE(mv.size() == 1);
  CHECK(mv.front().evaluation == EvaluationContext::init_capture_initializer);
  const auto big_copies = evidence_list(f, run->id, big_copy->id);
  REQUIRE(big_copies.size() == 1);
  CHECK(big_copies.front().evaluation == EvaluationContext::capture_copy);
  const auto worker_copies = evidence_list(f, run->id, worker_copy->id);
  REQUIRE(worker_copies.size() == 1);
  CHECK(worker_copies.front().evaluation == EvaluationContext::capture_copy);
  CHECK(worker_copy->presence == SymbolPresence::implicit_hidden);
  CHECK_FALSE(has_relation(f, l1->id, make_value->id, RelationKind::calls));
  CHECK_FALSE(has_relation(f, l1->id, big_copy->id, RelationKind::calls));
  // Body calls belong to the lambdas; run only calls target through the directly provable (*&target)(3).
  CHECK(has_relation(f, l1->id, target->id, RelationKind::calls));
  CHECK(has_relation(f, l2->id, target->id, RelationKind::calls));
  CHECK(has_relation(f, inner->id, target->id, RelationKind::calls));
  CHECK_FALSE(has_relation(f, l3->id, target->id, RelationKind::calls));
  CHECK(has_relation(f, l3->id, inner->id, RelationKind::calls));
  const auto run_targets = evidence_list(f, run->id, target->id);
  REQUIRE(run_targets.size() == 1);
  CHECK(run_targets.front().dispatch == DispatchKind::static_target);
  CHECK(run_targets.front().evaluation == EvaluationContext::body);
  std::vector<UnresolvedSite> run_sites;
  for (const auto& site : f.unresolved_sites()) {
    if (site.enclosing == run->id) run_sites.push_back(site);
  }
  REQUIRE(run_sites.size() == 1);
  CHECK(run_sites.front().expression == "fp");
  CHECK(run_sites.front().reason == UnresolvedReason::indirect_callee);
  CHECK(has_relation(f, run->id, l1->id, RelationKind::calls));
  CHECK(has_relation(f, run->id, l3->id, RelationKind::calls));

  // Closure lifetime: l1's used closure destructor is a hidden destructor under the lambda, owns ~Big for
  // the `big` capture, and is what run destroys; nothing folds into the lambda's body calls.
  const Symbol* l1_dtor = closure_destructor(f, l1->id);
  REQUIRE(l1_dtor);
  CHECK(l1_dtor->presence == SymbolPresence::implicit_hidden);
  CHECK(l1_dtor->key.canonical_name == "~");
  CHECK(l1_dtor->key.owner_chain.back().kind == SymbolKind::lambda);
  CHECK(has_relation(f, l1->id, l1_dtor->id, RelationKind::contains));
  const auto closure_dtors = evidence_list(f, run->id, l1_dtor->id);
  REQUIRE(closure_dtors.size() == 1);
  CHECK(closure_dtors.front().lifetime == LifetimeKind::automatic_object);
  const auto capture_dtors = evidence_list(f, l1_dtor->id, big_dtor->id);
  REQUIRE(capture_dtors.size() == 1);
  CHECK(capture_dtors.front().lifetime == LifetimeKind::subobject);
  CHECK(capture_dtors.front().subobject_role == SubobjectRole::member);
  CHECK(capture_dtors.front().subobject == "big");
  CHECK_FALSE(has_relation(f, l1->id, big_dtor->id, RelationKind::calls));
  // run also destroys its own `big`; the trivially destructible closures l2/l3 have no destructor node.
  CHECK(count_evidence(evidence_list(f, run->id, big_dtor->id),
                       [](const Evidence& e) { return e.lifetime == LifetimeKind::automatic_object; }) == 1);
  CHECK(closure_destructor(f, l2->id) == nullptr);
  CHECK(closure_destructor(f, l3->id) == nullptr);
  CHECK(closure_destructor(f, inner->id) == nullptr);
  for (const auto& [id, s] : f.symbols()) {
    if (!s.key.owner_chain.empty() && s.key.owner_chain.back().kind == SymbolKind::lambda) {
      CHECK(s.key.kind != SymbolKind::constructor);  // closure copy/move constructors are never nodes
      CHECK(s.key.kind != SymbolKind::method);
    }
    if (s.key.kind == SymbolKind::destructor && s.key.canonical_name == "~") {
      CHECK(s.id == l1_dtor->id);
    }
  }

  std::error_code ec;
  fs::remove_all(work, ec);
}

TEST_CASE("hidden members and captures are contribution-owned: mixed units, removal, liveness, publication") {
  const fs::path work = scratch_dir("callables-live");
  const CompilationDatabase db = load_fixture_database(repo_root(), work);
  const CompileCommand* c = command_for(db, repo_root(), "src/callables.cpp");
  REQUIRE(c);
  const auto run_as = [&](const std::string& label) {
    AnalysisRequest req;
    req.command = c;
    req.repository_root = repo_root();
    req.resource_dir = resource_dir();
    req.analysis_unit = label;
    AnalysisResult r = analyze_translation_unit(req);
    REQUIRE(r.status == AnalysisStatus::ok);
    return r;
  };
  const AnalysisResult a = run_as("unit:A");
  const AnalysisResult b = run_as("unit:B");
  CHECK(a.facts.fact_hash() != b.facts.fact_hash());  // labels differ, content otherwise equal

  FactSet graph;
  const PublishOutcome pa = publish_contribution(graph, a);
  const PublishOutcome pb = publish_contribution(graph, b);
  REQUIRE(pa.published);
  REQUIRE(pb.published);
  CHECK(pa.captures == a.facts.captures().size());
  CHECK(pa.captures > 0);

  const Symbol* plain_copy = method_of(graph, "Plain", "Plain", "(const Plain &)");
  const Symbol* copy_plain = find_symbol(graph, "copy_plain", "(const Plain &)");
  REQUIRE(plain_copy);
  REQUIRE(copy_plain);
  CHECK(plain_copy->presence == SymbolPresence::implicit_hidden);
  REQUIRE(plain_copy->locations.size() == 2);  // one implicit_declaration per contributing unit, no overwrite
  for (const auto& l : plain_copy->locations) {
    CHECK(l.role == LocationRole::implicit_declaration);
    CHECK_FALSE(l.callable.trivial);
  }
  CHECK(std::count_if(plain_copy->locations.begin(), plain_copy->locations.end(),
                      [](const SymbolLocation& l) { return l.analysis_unit == "unit:A"; }) == 1);
  const Symbol* run = method_of(graph, "Worker", "run", "(int (*)(int))");
  REQUIRE(run);
  const auto lambdas = lambdas_enclosed_by(graph, run->id);
  REQUIRE(lambdas.size() == 3);
  const StableId plain_copy_id = plain_copy->id;  // ids are copied: removals may erase the symbols
  const StableId l1_id = lambdas.front()->id;
  CHECK(captures_of(graph, l1_id).size() == 10);  // five captures from each unit
  const Symbol* l1_dtor = closure_destructor(graph, l1_id);
  REQUIRE(l1_dtor);
  const StableId l1_dtor_id = l1_dtor->id;
  CHECK(l1_dtor->locations.size() == 2);

  graph.remove_contribution("unit:A");
  const Symbol* after = graph.find_symbol(plain_copy_id);
  REQUIRE(after);
  CHECK(after->presence == SymbolPresence::implicit_hidden);
  REQUIRE(after->locations.size() == 1);
  CHECK(after->locations.front().analysis_unit == "unit:B");
  CHECK(captures_of(graph, l1_id).size() == 5);
  for (const auto& cap : graph.captures()) CHECK(cap.analysis_unit == "unit:B");
  REQUIRE(graph.find_symbol(l1_dtor_id));
  CHECK(graph.find_symbol(l1_dtor_id)->locations.size() == 1);
  CHECK(graph.fact_hash() == b.facts.fact_hash());  // exactly B's contribution remains

  graph.remove_contribution("unit:B");
  CHECK(graph.symbols().empty());
  CHECK(graph.relations().empty());
  CHECK(graph.captures().empty());
  CHECK(graph.unresolved_sites().empty());

  // Endpoint liveness: a surviving capture keeps its lambda alive as endpoint_only.
  FactSet partial;
  REQUIRE(publish_contribution(partial, a).published);
  const auto partial_captures = captures_of(partial, l1_id);
  REQUIRE_FALSE(partial_captures.empty());
  CaptureFact foreign = partial_captures.front();
  foreign.analysis_unit = "unit:C";
  partial.add_capture(foreign);
  partial.remove_contribution("unit:A");
  REQUIRE(partial.find_symbol(l1_id));
  CHECK(partial.find_symbol(l1_id)->presence == SymbolPresence::endpoint_only);
  CHECK(partial.captures().size() == 1);
  partial.remove_contribution("unit:C");
  CHECK(partial.symbols().empty());

  std::error_code ec;
  fs::remove_all(work, ec);
}

// =============================================================================
// Phase 1C: closure member folding, hidden closure members, unnamed constructors,
// collision-safe local type atoms and namespace-scope lambdas.
namespace {

const Symbol* closure_member(const FactSet& f, const StableId& lambda, const char* anchor, SymbolKind kind,
                             const std::string& signature_part = "") {
  for (const auto& [id, s] : f.symbols()) {
    if (s.key.kind != kind || !std::holds_alternative<LocalScope>(s.key.linkage)) continue;
    const auto& scope = std::get<LocalScope>(s.key.linkage);
    if (scope.enclosing != lambda || scope.anchor != anchor) continue;
    if (!signature_part.empty() && s.key.normalized_signature.find(signature_part) == std::string::npos) continue;
    return &s;
  }
  return nullptr;
}

std::vector<const Symbol*> symbols_named(const FactSet& f, const std::string& name, SymbolKind kind) {
  std::vector<const Symbol*> out;
  for (const auto& [id, s] : f.symbols()) {
    if (s.key.canonical_name == name && s.key.kind == kind) out.push_back(&s);
  }
  std::sort(out.begin(), out.end(), [](const Symbol* a, const Symbol* b) { return a->id.value < b->id.value; });
  return out;
}

bool leaks_source_spelling(const std::string& text) {
  return text.find(".cpp") != std::string::npos || text.find("(lambda at") != std::string::npos ||
         text.find("unnamed at") != std::string::npos || text.find("unnamed struct") != std::string::npos ||
         text.find("D:/") != std::string::npos || text.find("D:\\") != std::string::npos;
}

std::set<std::string> id_set(const FactSet& f) {
  std::set<std::string> out;
  for (const auto& [id, s] : f.symbols()) out.insert(id.value);
  return out;
}

}  // namespace

TEST_CASE("Phase 1C: only the real closure call operator folds; other closure members are hidden") {
  const fs::path work = scratch_dir("closures-a");
  const CompilationDatabase db = load_fixture_database(repo_root(), work);
  const AnalysisResult r = analyze_one(db, repo_root(), "src/closures.cpp");
  for (const auto& l : r.limits) MESSAGE("limit: " << l);
  REQUIRE(r.status == AnalysisStatus::ok);
  const FactSet& f = r.facts;

  const Symbol* exercise = find_symbol(f, "exercise", "(Payload)");
  const Symbol* assign_only = find_symbol(f, "assign_only");
  const Symbol* payload_copy = method_of(f, "Payload", "Payload", "(const Payload &)");
  REQUIRE(exercise);
  REQUIRE(assign_only);
  REQUIRE(payload_copy);
  const auto lambdas = lambdas_enclosed_by(f, exercise->id);
  REQUIRE(lambdas.size() == 4);  // captured, empty, generic, immediate
  const Symbol* captured = lambdas[0];
  const Symbol* empty = lambdas[1];
  const Symbol* generic = lambdas[2];
  const Symbol* immediate = lambdas[3];

  // 1. Folding: copied(), copied.operator()(), moved() all execute `captured`.
  const auto captured_calls = evidence_list(f, exercise->id, captured->id);
  CHECK(captured_calls.size() == 3);
  for (const auto& e : captured_calls) CHECK(plain_aspects(e));
  // Generic lambda: both syntaxes, each with its deduced arguments.
  const auto generic_calls = evidence_list(f, exercise->id, generic->id);
  REQUIRE(generic_calls.size() == 2);
  for (const auto& e : generic_calls) CHECK(e.template_use == TemplateUse::primary_implicit);
  CHECK(count_evidence(generic_calls, [](const Evidence& e) { return e.template_arguments == "<int>"; }) == 1);
  CHECK(count_evidence(generic_calls, [](const Evidence& e) { return e.template_arguments == "<double>"; }) == 1);
  // Immediate invocation keeps its flag.
  const auto immediate_calls = evidence_list(f, exercise->id, immediate->id);
  REQUIRE(immediate_calls.size() == 1);
  CHECK(immediate_calls.front().immediately_invoked_lambda);
  // `empty` is executed exactly once (empty_copy()); the two assignments are not executions.
  CHECK(evidence_list(f, exercise->id, empty->id).size() == 1);
  // No method node ever appears under a lambda.
  for (const auto& [id, s] : f.symbols()) {
    if (!s.key.owner_chain.empty() && s.key.owner_chain.back().kind == SymbolKind::lambda) {
      CHECK(s.key.kind != SymbolKind::method);
      CHECK(s.presence == SymbolPresence::implicit_hidden);
    }
  }

  // 2. Hidden closure members with distinct anchors; the destructor keeps `closure-dtor`.
  const Symbol* copy_ctor = closure_member(f, captured->id, "closure-ctor", SymbolKind::constructor, "const ");
  const Symbol* move_ctor = closure_member(f, captured->id, "closure-ctor", SymbolKind::constructor, "rref(");
  const Symbol* captured_dtor = closure_destructor(f, captured->id);
  REQUIRE(copy_ctor);
  REQUIRE(move_ctor);
  REQUIRE(captured_dtor);
  CHECK(copy_ctor->id != move_ctor->id);
  CHECK(copy_ctor->key.canonical_name == "(ctor)");
  CHECK(copy_ctor->presence == SymbolPresence::implicit_hidden);
  const bool copy_signature_ok =
      copy_ctor->key.normalized_signature == "(ref(const [lambda " + captured->id.value + "]))";
  CHECK_MESSAGE(copy_signature_ok, copy_ctor->key.normalized_signature);
  CHECK_FALSE(leaks_source_spelling(copy_ctor->key.normalized_signature));
  CHECK(has_relation(f, captured->id, copy_ctor->id, RelationKind::contains));
  CHECK(evidence_list(f, exercise->id, copy_ctor->id).size() == 1);
  CHECK(evidence_list(f, exercise->id, move_ctor->id).size() == 1);
  // The synthesized copy constructor's own body copies the capture; the enclosing function does not.
  const auto member_copy = evidence_list(f, copy_ctor->id, payload_copy->id);
  REQUIRE(member_copy.size() == 1);
  CHECK(member_copy.front().evaluation == EvaluationContext::implicit_mem_initializer);
  CHECK(member_copy.front().subobject == "payload");
  // Assignment: hidden operator= (copy via `=`, move via `.operator=`), never lambda execution.
  const Symbol* copy_assign = closure_member(f, empty->id, "closure-assign", SymbolKind::operator_function, "const ");
  const Symbol* move_assign = closure_member(f, empty->id, "closure-assign", SymbolKind::operator_function, "rref(");
  REQUIRE(copy_assign);
  REQUIRE(move_assign);
  CHECK(copy_assign->key.canonical_name == "operator=");
  CHECK(evidence_list(f, exercise->id, copy_assign->id).size() == 1);
  CHECK(evidence_list(f, exercise->id, move_assign->id).size() == 1);
  // Conversion to function pointer: hidden conversion member; the call through fp stays unresolved.
  const Symbol* conversion = closure_member(f, empty->id, "closure-conv", SymbolKind::conversion_function);
  REQUIRE(conversion);
  CHECK(conversion->key.canonical_name == "operator int (*)()");
  CHECK(evidence_list(f, exercise->id, conversion->id).size() == 1);
  CHECK(std::any_of(f.unresolved_sites().begin(), f.unresolved_sites().end(), [&](const UnresolvedSite& s) {
    return s.enclosing == exercise->id && s.expression == "fp";
  }));
  CHECK(symbols_named(f, "__invoke", SymbolKind::method).empty());
  // Assignment-only function: zero enclosing-to-lambda execution calls, one hidden assignment target.
  const Symbol* g1_var = find_symbol(f, "g1", "", SymbolKind::variable);
  REQUIRE(g1_var);
  const auto g1_lambdas = lambdas_enclosed_by(f, g1_var->id);
  REQUIRE(g1_lambdas.size() == 1);
  CHECK_FALSE(has_relation(f, assign_only->id, g1_lambdas.front()->id, RelationKind::calls));
  const Symbol* g1_assign = closure_member(f, g1_lambdas.front()->id, "closure-assign", SymbolKind::operator_function);
  REQUIRE(g1_assign);
  CHECK(evidence_list(f, assign_only->id, g1_assign->id).size() == 1);
  // Closure destruction is owned by the enclosing function (3 objects) and the hidden destructor owns the capture.
  CHECK(count_evidence(evidence_list(f, exercise->id, captured_dtor->id),
                       [](const Evidence& e) { return e.lifetime == LifetimeKind::automatic_object; }) == 3);

  // 3. Unnamed record constructors: `(ctor)` anchored to the actual anonymous type.
  const Symbol* a1_var = find_symbol(f, "a1", "", SymbolKind::variable);
  REQUIRE(a1_var);
  const Symbol* a1_type = nullptr;
  for (const auto& [id, s] : f.symbols()) {
    if (s.key.kind == SymbolKind::anonymous_type && std::get<LocalScope>(s.key.linkage).enclosing == a1_var->id) a1_type = &s;
  }
  REQUIRE(a1_type);
  const auto ctors = symbols_named(f, "(ctor)", SymbolKind::constructor);
  std::vector<const Symbol*> anon_ctors;
  for (const auto* c : ctors) {
    if (std::get<LocalScope>(c->key.linkage).enclosing == a1_type->id) anon_ctors.push_back(c);
  }
  // Default (used by the global a1 itself), copy and move; a2 only ever uses its default constructor.
  REQUIRE(anon_ctors.size() == 3);
  std::size_t default_ctors = 0;
  for (const auto* c : anon_ctors) {
    CHECK(c->presence == SymbolPresence::implicit_hidden);
    CHECK(std::get<LocalScope>(c->key.linkage).anchor == "(ctor)");
    CHECK(c->key.owner_chain.back().kind == SymbolKind::anonymous_type);
    CHECK_FALSE(leaks_source_spelling(c->key.normalized_signature));
    if (c->key.normalized_signature == "()") {
      ++default_ctors;
      CHECK(evidence_list(f, a1_var->id, c->id).size() == 1);  // the variable's own initialization
      CHECK_FALSE(has_relation(f, exercise->id, c->id, RelationKind::calls));
      continue;
    }
    CHECK(c->key.normalized_signature.find("[anon " + a1_type->id.value + "]") != std::string::npos);
    CHECK(evidence_list(f, exercise->id, c->id).size() == 1);
    const Symbol* payload_ctor = method_of(f, "Payload", "Payload",
                                           c->key.normalized_signature.find("rref(") != std::string::npos ? "(Payload &&)" : "(const Payload &)");
    REQUIRE(payload_ctor);
    const auto init = evidence_list(f, c->id, payload_ctor->id);
    REQUIRE(init.size() == 1);
    CHECK(init.front().evaluation == EvaluationContext::implicit_mem_initializer);
    CHECK(init.front().subobject == "payload");
  }
  CHECK(default_ctors == 1);
  {
    std::size_t anonymous_ctors = 0;  // a1: default/copy/move, a2: default only (closure ctors are named (ctor) too)
    for (const auto* c : symbols_named(f, "(ctor)", SymbolKind::constructor)) {
      if (c->key.owner_chain.back().kind == SymbolKind::anonymous_type) ++anonymous_ctors;
    }
    CHECK(anonymous_ctors == 4);
  }
  CHECK(anon_ctors[0]->id != anon_ctors[1]->id);
  const Symbol* payload_default = method_of(f, "Payload", "Payload", "()");
  REQUIRE(payload_default);
  // Aggregate `decltype(a1) anonymous{}` is not a constructor call of the anonymous type; the compiler's
  // semantic init-list form constructs the Payload member, owned by `exercise`. Its truthful anchor is the
  // compiler-provided location, the closing brace of `{}`: pinned as original bytes and span hash.
  {
    const std::string bytes = read_file(repo_root() / "src/closures.cpp");
    LineIndex lines(bytes);
    const auto aggregate_line = lines.line_column(static_cast<std::uint32_t>(bytes.find("decltype(a1) anonymous{};"))).first;
    const auto edges = evidence_list(f, exercise->id, payload_default->id);
    std::size_t at_aggregate = 0;
    for (const auto& e : edges) {
      if (e.location.span.begin_line != aggregate_line) continue;
      ++at_aggregate;
      const std::string text = bytes.substr(e.location.span.begin_offset, e.location.span.end_offset - e.location.span.begin_offset);
      CHECK(text == "}");
      REQUIRE(e.location.span_hash.has_value());
      CHECK(e.location.span_hash == hash_span(bytes, e.location.span));
      CHECK(e.evaluation == EvaluationContext::body);
    }
    CHECK(at_aggregate == 1);
  }

  std::error_code ec;
  fs::remove_all(work, ec);
}

TEST_CASE("Phase 1C: namespace-scope lambdas, local type atoms, template argument kinds, ABI and stability") {
  const fs::path work = scratch_dir("closures-b");
  const CompilationDatabase db = load_fixture_database(repo_root(), work);
  const AnalysisResult r = analyze_one(db, repo_root(), "src/closures.cpp");
  REQUIRE(r.status == AnalysisStatus::ok);
  const FactSet& f = r.facts;

  // 4. Namespace-scope declarator lambdas.
  const Symbol* g1_var = find_symbol(f, "g1", "", SymbolKind::variable);
  const Symbol* g2_var = find_symbol(f, "g2", "", SymbolKind::variable);
  const Symbol* g3_var = find_symbol(f, "g3", "", SymbolKind::variable);
  const Symbol* pair_var = find_symbol(f, "pair_init", "", SymbolKind::variable);
  const Symbol* exercise = find_symbol(f, "exercise", "(Payload)");
  REQUIRE(g1_var);
  REQUIRE(g2_var);
  REQUIRE(g3_var);
  REQUIRE(pair_var);
  REQUIRE(exercise);
  const auto g1_l = lambdas_enclosed_by(f, g1_var->id);
  const auto g2_l = lambdas_enclosed_by(f, g2_var->id);
  const auto g3_l = lambdas_enclosed_by(f, g3_var->id);
  const auto pair_l = lambdas_enclosed_by(f, pair_var->id);
  REQUIRE(g1_l.size() == 1);
  REQUIRE(g2_l.size() == 1);
  REQUIRE(g3_l.size() == 1);
  REQUIRE(pair_l.size() == 2);
  CHECK(std::get<LocalScope>(g1_l[0]->key.linkage).anchor == std::string(kLambdaDeclInitAnchor));
  CHECK(g1_l[0]->key.owner_chain.empty());
  CHECK(g1_l[0]->id != g2_l[0]->id);
  CHECK(std::get<LocalScope>(g3_l[0]->key.linkage).anchor == "lambda");  // namespace owner: established anchor
  REQUIRE(g3_l[0]->key.owner_chain.size() == 1);
  CHECK(g3_l[0]->key.owner_chain.front().name == "ns");
  CHECK(std::get<LocalScope>(pair_l[0]->key.linkage).ordinal == 0);
  CHECK(std::get<LocalScope>(pair_l[1]->key.linkage).ordinal == 1);
  CHECK(has_relation(f, exercise->id, pair_l[1]->id, RelationKind::calls));   // pair_init() is the second closure
  CHECK_FALSE(has_relation(f, exercise->id, pair_l[0]->id, RelationKind::calls));
  CHECK(has_relation(f, exercise->id, g1_l[0]->id, RelationKind::calls));
  CHECK(has_relation(f, exercise->id, g3_l[0]->id, RelationKind::calls));
  // Namespace-scope closure members are anchored to the lambda too (g1 is copy-assigned in assign_only).
  REQUIRE(closure_member(f, g1_l[0]->id, "closure-assign", SymbolKind::operator_function));
  CHECK(closure_member(f, g1_l[0]->id, "closure-assign", SymbolKind::operator_function)->key.owner_chain.size() == 1);

  // 5. Collision-safe atoms: overloads on closure and anonymous types, compositions, local named types.
  const auto consume = symbols_named(f, "consume", SymbolKind::function);
  REQUIRE(consume.size() == 2);
  CHECK(consume[0]->key.normalized_signature != consume[1]->key.normalized_signature);
  for (const auto* c : consume) {
    CHECK(c->key.normalized_signature.find("[lambda cm1:") != std::string::npos);
    CHECK_FALSE(leaks_source_spelling(c->key.normalized_signature));
  }
  const auto consume_anon = symbols_named(f, "consume_anon", SymbolKind::function);
  REQUIRE(consume_anon.size() == 2);
  CHECK(consume_anon[0]->key.normalized_signature != consume_anon[1]->key.normalized_signature);
  const Symbol* consume_ptr = find_symbol(f, "consume_ptr");
  REQUIRE(consume_ptr);
  CHECK(consume_ptr->key.normalized_signature.find("ptr([lambda ") != std::string::npos);
  CHECK(consume_ptr->key.normalized_signature.find("ref(const [anon ") != std::string::npos);
  CHECK(consume_ptr->key.normalized_signature.find("ref(arr[2]([anon ") != std::string::npos);   // array by reference
  CHECK(consume_ptr->key.normalized_signature.find(")(ptr([anon ") != std::string::npos);           // array parameter decayed
  CHECK(consume_ptr->key.normalized_signature.find("fn[cc=cdecl]") != std::string::npos);
  const Symbol* use_memptr = find_symbol(f, "use_memptr");
  REQUIRE(use_memptr);
  CHECK(use_memptr->key.normalized_signature.find("memptr(Box<[lambda ") != std::string::npos);
  {
    const std::string sig = use_memptr->key.normalized_signature;
    const auto comma = sig.find(", ");
    REQUIRE(comma != std::string::npos);
    CHECK(sig.substr(1, comma - 1) != sig.substr(comma + 2, sig.size() - comma - 3));  // g1 and g2 classes differ
  }
  // Template argument kinds: 1 vs 1u, K{1} vs K{2} are four distinct parameter spellings.
  const Symbol* take_wrap = find_symbol(f, "take_wrap");
  REQUIRE(take_wrap);
  {
    const std::string sig = take_wrap->key.normalized_signature;
    std::vector<std::string> parts;
    std::size_t start = 1;
    int depth = 0;
    for (std::size_t i = 1; i < sig.size(); ++i) {
      if (sig[i] == '<' || sig[i] == '(' || sig[i] == '{') ++depth;
      if (sig[i] == '>' || sig[i] == ')' || sig[i] == '}') --depth;
      if ((sig[i] == ',' && depth == 0) || i + 1 == sig.size()) {
        parts.push_back(sig.substr(start, i - start));
        start = i + 2;
      }
    }
    std::set<std::string> distinct(parts.begin(), parts.end());
    CHECK_MESSAGE(distinct.size() == 4, sig);
    CHECK_FALSE(leaks_source_spelling(sig));
  }
  // Same-named local types in two blocks: distinct lambdas, distinct enums, distinct signatures.
  const Symbol* local_types = find_symbol(f, "local_types", "()");
  REQUIRE(local_types);
  const auto local_lambdas = lambdas_enclosed_by(f, local_types->id);
  REQUIRE(local_lambdas.size() == 2);
  CHECK(local_lambdas[0]->key.normalized_signature != local_lambdas[1]->key.normalized_signature);
  CHECK(local_lambdas[0]->key.normalized_signature.find("[local cm1:") != std::string::npos);
  CHECK(local_lambdas[0]->key.normalized_signature.find("Box<[local cm1:") != std::string::npos);
  const auto enums = symbols_named(f, "E", SymbolKind::enum_);
  REQUIRE(enums.size() == 2);
  CHECK(enums[0]->id != enums[1]->id);
  CHECK(std::get<LocalScope>(enums[0]->key.linkage).ordinal != std::get<LocalScope>(enums[1]->key.linkage).ordinal);
  const auto locals = symbols_named(f, "L", SymbolKind::struct_);
  REQUIRE(locals.size() == 2);
  // ABI attributes in local-type-containing function pointer parameters.
  const Symbol* abi = find_symbol(f, "abi");
  REQUIRE(abi);
  CHECK(abi->key.normalized_signature.find("fn[cc=cdecl](") != std::string::npos);
  CHECK(abi->key.normalized_signature.find("fn[cc=cdecl,noexcept](") != std::string::npos);
  CHECK(abi->key.normalized_signature.find("fn[cc=vectorcall](") != std::string::npos);
  // No identity or evidence text carries a path, line or body spelling.
  for (const auto& [id, s] : f.symbols()) {
    CHECK_FALSE(leaks_source_spelling(s.key.canonical_name));
    CHECK_FALSE(leaks_source_spelling(s.key.normalized_signature));
    CHECK_FALSE(leaks_source_spelling(s.key.template_arguments));
    for (const auto& o : s.key.owner_chain) CHECK_FALSE(leaks_source_spelling(o.normalized_signature));
  }
  for (const auto& [key, rel] : f.relations()) {
    for (const auto& e : rel.evidence) CHECK_FALSE(leaks_source_spelling(e.template_arguments));
  }

  // 6. Copied root with inserted lines: every identity is unchanged; three-run determinism is covered below.
  const fs::path copy_root = work / path_from_utf8("Copied Root");
  write_file(copy_root / "src/closures.cpp", "\n\n\n\n\n" + read_file(repo_root() / "src/closures.cpp"));
  const AnalysisResult copy = gate(copy_root, copy_root / "src/closures.cpp",
                                   {"clang-cl.exe", "/c", "/std:c++20", "/EHsc", "src/closures.cpp"});
  for (const auto& d : copy.diagnostics) MESSAGE(d.text());
  REQUIRE(copy.status == AnalysisStatus::ok);
  CHECK(id_set(copy.facts) == id_set(f));
  CHECK(copy.facts.symbols().size() == f.symbols().size());
  CHECK(copy.facts.relations().size() == f.relations().size());
  // Original-byte provenance still differs by five lines.
  const Symbol* copy_exercise = find_symbol(copy.facts, "exercise", "(Payload)");
  REQUIRE(copy_exercise);
  CHECK(copy_exercise->locations.front().location.span.begin_line == exercise->locations.front().location.span.begin_line + 5);

  // Same structural value written differently in two TUs: `K2{{0, 0}}` and `K2{}` are one identity.
  const fs::path tu_a = work / "tu_a";
  const fs::path tu_b = work / "tu_b";
  const std::string prelude = "struct K2 { int v[2]; };\ntemplate <K2 V> struct WrapArr { int w; };\n";
  write_file(tu_a / "a.cpp", prelude + "void take_arr(WrapArr<K2{{0, 0}}>) {}\n");
  write_file(tu_b / "a.cpp", prelude + "void take_arr(WrapArr<K2{}>) {}\n");
  const AnalysisResult ra = gate(tu_a, tu_a / "a.cpp", {"clang-cl.exe", "/c", "/std:c++20", "a.cpp"});
  const AnalysisResult rb = gate(tu_b, tu_b / "a.cpp", {"clang-cl.exe", "/c", "/std:c++20", "a.cpp"});
  REQUIRE(ra.status == AnalysisStatus::ok);
  REQUIRE(rb.status == AnalysisStatus::ok);
  const Symbol* take_a = find_symbol(ra.facts, "take_arr");
  const Symbol* take_b = find_symbol(rb.facts, "take_arr");
  REQUIRE(take_a);
  REQUIRE(take_b);
  CHECK(take_a->id == take_b->id);
  CHECK(take_a->key.normalized_signature == take_b->key.normalized_signature);
  CHECK(take_a->key.normalized_signature.find("arr[2]{0;0;}") != std::string::npos);

  // Depth exhaustion is an explicit limit, never a raw spelling: 40 pointer layers over a closure type.
  const fs::path tu_deep = work / "tu_deep";
  std::string deep = "auto base = [] { return 0; };\nusing P0 = decltype(base);\n";
  for (int i = 1; i <= 40; ++i) deep += "using P" + std::to_string(i) + " = P" + std::to_string(i - 1) + "*;\n";
  deep += "void deep_fn(P40) {}\nvoid shallow_fn(P3) {}\n";
  write_file(tu_deep / "deep.cpp", deep);
  const AnalysisResult rd = gate(tu_deep, tu_deep / "deep.cpp", {"clang-cl.exe", "/c", "/std:c++20", "deep.cpp"});
  REQUIRE(rd.status == AnalysisStatus::ok);
  CHECK(find_symbol(rd.facts, "deep_fn") == nullptr);
  CHECK(std::any_of(rd.limits.begin(), rd.limits.end(),
                    [](const std::string& l) { return l.find("unsupported form") != std::string::npos; }));
  const Symbol* shallow = find_symbol(rd.facts, "shallow_fn");
  REQUIRE(shallow);
  CHECK(shallow->key.normalized_signature.find("ptr(ptr(ptr([lambda ") != std::string::npos);
  for (const auto& [id, s] : rd.facts.symbols()) CHECK_FALSE(leaks_source_spelling(s.key.normalized_signature));

  std::error_code ec;
  fs::remove_all(work, ec);
}

TEST_CASE("Phase 1C identity correction: unsupported wrappers omit with limits; typed null and local-enum arguments encode") {
  const fs::path work = scratch_dir("closures-c");
  const std::vector<std::string> argv = {"clang-cl.exe", "/c", "/std:c++20", "unit.cpp"};

  // Blocker 1 (agent3): an unsupported wrapper over a local leaf must never reach the raw printer.
  const std::string atomic_src =
      "auto g = [] {};\n"
      "void atomic_fn(_Atomic(decltype(g))* value) { (void)value; }\n"
      "void ordinary_fn(decltype(g)* value) { (void)value; }\n"
      "void vector_fn(int __attribute__((vector_size(16))) v) { (void)v; }\n";
  const fs::path root_a = work / "atomic";
  const fs::path root_b = work / path_from_utf8("atomic copied");
  write_file(root_a / "unit.cpp", atomic_src);
  write_file(root_b / "unit.cpp", "\n\n\n\n\n" + atomic_src);
  const AnalysisResult a = gate(root_a, root_a / "unit.cpp", argv);
  const AnalysisResult b = gate(root_b, root_b / "unit.cpp", argv);
  for (const auto& d : a.diagnostics) MESSAGE(d.text());
  REQUIRE(a.status == AnalysisStatus::ok);
  REQUIRE(b.status == AnalysisStatus::ok);
  for (const AnalysisResult* r : {&a, &b}) {
    CHECK(find_symbol(r->facts, "atomic_fn") == nullptr);  // omitted, not spelled with a path
    CHECK(std::any_of(r->limits.begin(), r->limits.end(),
                      [](const std::string& l) { return l.find("unsupported form") != std::string::npos; }));
    for (const auto& [id, s] : r->facts.symbols()) CHECK_FALSE(leaks_source_spelling(s.key.normalized_signature));
    const Symbol* ordinary = find_symbol(r->facts, "ordinary_fn");
    REQUIRE(ordinary);
    CHECK(ordinary->key.normalized_signature.find("ptr([lambda ") != std::string::npos);
    const Symbol* vector_fn = find_symbol(r->facts, "vector_fn");  // non-local wrapper keeps the established spelling
    REQUIRE(vector_fn);
    CHECK_FALSE(leaks_source_spelling(vector_fn->key.normalized_signature));
  }
  CHECK(find_symbol(a.facts, "ordinary_fn")->id == find_symbol(b.facts, "ordinary_fn")->id);
  CHECK(find_symbol(a.facts, "vector_fn")->id == find_symbol(b.facts, "vector_fn")->id);
  CHECK(id_set(a.facts) == id_set(b.facts));
  // The wrapper itself is unsupported in this increment even when its contents are encodable: still an
  // explicit omission, never a raw or invented spelling (full type support remains later required work).
  const fs::path root_c = work / "atomic-ptr";
  write_file(root_c / "unit.cpp", "auto g = [] {};\nvoid atomic_ptr_fn(_Atomic(decltype(g)*) v) { (void)v; }\n");
  const AnalysisResult c = gate(root_c, root_c / "unit.cpp", argv);
  REQUIRE(c.status == AnalysisStatus::ok);
  CHECK(find_symbol(c.facts, "atomic_ptr_fn") == nullptr);
  CHECK(std::any_of(c.limits.begin(), c.limits.end(),
                    [](const std::string& l) { return l.find("unsupported form") != std::string::npos; }));
  for (const auto& [id, s] : c.facts.symbols()) CHECK_FALSE(leaks_source_spelling(s.key.normalized_signature));

  // R3 (coordinator): a dependent qualified name is not a leaf; its qualifier can carry a concrete local
  // leaf (`typename Box<decltype(g), T>::type*`). The pattern's signature must be omitted with a limit,
  // never spelled through the raw printer; a genuine template-parameter leaf stays supported.
  const fs::path root_d = work / "dependent";
  write_file(root_d / "unit.cpp",
             "auto g = [] {};\n"
             "template<class A, class B> struct Box {};\n"
             "template<class T> void deferred_fn(typename Box<decltype(g), T>::type*) {}\n"
             "template<class T> void plain_param(T, decltype(g)*) {}\n"
             "template<class T> void named_dependent(typename Box<int, T>::type*) {}\n");
  const AnalysisResult dep = gate(root_d, root_d / "unit.cpp", argv);
  for (const auto& diag : dep.diagnostics) MESSAGE(diag.text());
  REQUIRE(dep.status == AnalysisStatus::ok);
  CHECK(find_symbol(dep.facts, "deferred_fn", "", SymbolKind::function, TemplateRole::primary) == nullptr);
  CHECK(std::any_of(dep.limits.begin(), dep.limits.end(),
                    [](const std::string& l) { return l.find("unsupported form") != std::string::npos; }));
  const Symbol* plain_param = find_symbol(dep.facts, "plain_param", "", SymbolKind::function, TemplateRole::primary);
  REQUIRE(plain_param);
  CHECK(plain_param->key.normalized_signature.find("type-parameter-0-0, ptr([lambda ") != std::string::npos);
  const Symbol* named_dependent = find_symbol(dep.facts, "named_dependent", "", SymbolKind::function, TemplateRole::primary);
  REQUIRE(named_dependent);  // no local leaf anywhere: the established dependent spelling is kept
  CHECK(named_dependent->key.normalized_signature.find("Box<int, type-parameter-0-0>::type") != std::string::npos);
  for (const auto& [id, s] : dep.facts.symbols()) CHECK_FALSE(leaks_source_spelling(s.key.normalized_signature));

  // Classifier boundary (new reviewer + coordinator inputs). Before the fix each of these spelled
  // `(lambda at <absolute path>:1:10)` into the canonical key: a dependent EXPRESSION template
  // argument, a dependent ARRAY BOUND expression, a named type whose OWNER specialization carries a
  // local argument, and a DEPENDENT member-pointer class (no record decl, only a qualifier). Each is
  // an unsupported composite now: omitted with an explicit limit. The named controls must not change.
  const std::string classifier_src = R"cpp(auto g = [] {};
template<auto V> struct W {};
template<class A> struct Outer { struct Inner {}; };
template<class A, class B> struct Box {};
template<class T> void expr_local(W<sizeof(T) + sizeof(decltype(g))>*) {}
template<class T> void expr_named(W<sizeof(T)>*) {}
template<class T> void arr_local(int (*)[sizeof(T) + sizeof(decltype(g))]) {}
void nested_owner(Outer<decltype(g)>::Inner*) {}
void nested_memptr(int Outer<decltype(g)>::Inner::*) {}
void named_owner(Outer<int>::Inner*) {}
template<class T> void dependent_member(int Box<decltype(g), T>::*) {}
template<class T> void named_member(int Box<int, T>::*) {}
)cpp";
  const fs::path root_e = work / "classifier";
  const fs::path root_e_shift = work / path_from_utf8("classifier copied");
  write_file(root_e / "unit.cpp", classifier_src);
  write_file(root_e_shift / "unit.cpp", "\n\n\n\n\n" + classifier_src);
  const AnalysisResult cls = gate(root_e, root_e / "unit.cpp", argv);
  const AnalysisResult cls_shift = gate(root_e_shift, root_e_shift / "unit.cpp", argv);
  for (const auto& d : cls.diagnostics) MESSAGE(d.text());
  REQUIRE(cls.status == AnalysisStatus::ok);
  REQUIRE(cls_shift.status == AnalysisStatus::ok);
  for (const AnalysisResult* r : {&cls, &cls_shift}) {
    CHECK(find_symbol(r->facts, "expr_local", "", SymbolKind::function, TemplateRole::primary) == nullptr);
    CHECK(find_symbol(r->facts, "arr_local", "", SymbolKind::function, TemplateRole::primary) == nullptr);
    CHECK(find_symbol(r->facts, "dependent_member", "", SymbolKind::function, TemplateRole::primary) == nullptr);
    CHECK(find_symbol(r->facts, "nested_owner") == nullptr);
    CHECK(find_symbol(r->facts, "nested_memptr") == nullptr);
    CHECK(std::any_of(r->limits.begin(), r->limits.end(),
                      [](const std::string& l) { return l.find("unsupported form") != std::string::npos; }));
    const Symbol* expr_named = find_symbol(r->facts, "expr_named", "", SymbolKind::function, TemplateRole::primary);
    REQUIRE(expr_named);  // the expression mentions no local type: the established spelling stays
    CHECK(expr_named->key.normalized_signature == "(W<sizeof(type-parameter-0-0)> *)");
    const Symbol* named_owner = find_symbol(r->facts, "named_owner");
    REQUIRE(named_owner);  // named owner specialization: unchanged
    CHECK(named_owner->key.normalized_signature == "(Outer<int>::Inner *)");
    const Symbol* named_member = find_symbol(r->facts, "named_member", "", SymbolKind::function, TemplateRole::primary);
    REQUIRE(named_member);  // dependent member pointer with a wholly named qualifier: still supported
    CHECK_FALSE(named_member->key.normalized_signature.empty());
    for (const auto& [id, sym] : r->facts.symbols()) CHECK_FALSE(leaks_source_spelling(sym.key.normalized_signature));
  }
  CHECK(id_set(cls.facts) == id_set(cls_shift.facts));  // copied root + five inserted lines: same identities

  // H3/H4 (independent review, `build/reviewer-fresh/final-review.md`): `Declaration` non-type template
  // arguments. H3 - the referenced declaration's OWNER specialization was never classified, so
  // `&Holder<decltype(g)>::value` spelled the closure's absolute path and line into the key and the ID
  // changed on copy/line shift. H4 - the printer writes a declaration's OWN specialization arguments
  // nowhere, so `&target<int>`/`&target<double>` and two closure-typed specializations all printed
  // `&target` and collapsed pairs of overloads into one symbol with two definition locations; encoding
  // them through `symbol_id` would fold onto the pattern just the same. Both are omitted with their own
  // truthful limits (no identity is invented, no new spelling is introduced), while declarations whose
  // printed form is injective and path-free keep their established signatures and IDs.
  const std::string decl_src = R"cpp(auto g = [] {};
auto ga = [] {};
auto gb = [] {};
template<auto V> struct Wrap {};
template<class T> struct Holder { inline static int value = 0; };
template<class T> int target() { return 0; }
template<class T> int vt = 0;
int plain_object = 0;
int plain_fn() { return 0; }
void declaration_owner(Wrap<&Holder<decltype(g)>::value>) {}
void declaration_named(Wrap<&Holder<int>::value>) {}
void local_target(Wrap<&target<decltype(ga)>>) {}
void local_target(Wrap<&target<decltype(gb)>>) {}
void named_target(Wrap<&target<int>>) {}
void named_target(Wrap<&target<double>>) {}
void var_target(Wrap<&vt<int>>) {}
void var_target(Wrap<&vt<double>>) {}
void plain_decl(Wrap<&plain_object>) {}
void plain_func(Wrap<&plain_fn>) {}
void caller_a() { local_target(Wrap<&target<decltype(ga)>>{}); }
void caller_b() { local_target(Wrap<&target<decltype(gb)>>{}); }
)cpp";
  const fs::path decl_original = work / "declaration";
  const fs::path decl_copy = work / path_from_utf8("declaration copied");
  const fs::path decl_shift = work / path_from_utf8("declaration shifted");
  write_file(decl_original / "unit.cpp", decl_src);
  write_file(decl_copy / "unit.cpp", decl_src);
  write_file(decl_shift / "unit.cpp", "\n\n\n\n\n" + decl_src);
  const AnalysisResult d_orig = gate(decl_original, decl_original / "unit.cpp", argv);
  const AnalysisResult d_copy = gate(decl_copy, decl_copy / "unit.cpp", argv);
  const AnalysisResult d_shift = gate(decl_shift, decl_shift / "unit.cpp", argv);
  for (const auto& diag : d_orig.diagnostics) MESSAGE(diag.text());
  REQUIRE(d_orig.status == AnalysisStatus::ok);
  REQUIRE(d_copy.status == AnalysisStatus::ok);
  REQUIRE(d_shift.status == AnalysisStatus::ok);
  for (const AnalysisResult* r : {&d_orig, &d_copy, &d_shift}) {
    // H3: omitted, with the member-of-a-specialization limit in its own words.
    CHECK(find_symbol(r->facts, "declaration_owner") == nullptr);
    CHECK(std::any_of(r->limits.begin(), r->limits.end(), [](const std::string& l) {
      return l.find("declaration-valued template argument") != std::string::npos &&
             l.find("names a member of a template specialization") != std::string::npos;
    }));
    // H4: ZERO overload symbols for every specialization-valued argument, named ones included, with a
    // limit that is truthful for a wholly NAMED function specialization (no local-leaf claim).
    CHECK(symbols_named(r->facts, "local_target", SymbolKind::function).empty());
    CHECK(symbols_named(r->facts, "named_target", SymbolKind::function).empty());
    CHECK(symbols_named(r->facts, "var_target", SymbolKind::function).empty());
    const auto spec_limit = std::find_if(r->limits.begin(), r->limits.end(), [](const std::string& l) {
      return l.find("names a template specialization") != std::string::npos;
    });
    REQUIRE(spec_limit != r->limits.end());
    CHECK(spec_limit->find("declaration-valued template argument") != std::string::npos);
    CHECK(spec_limit->find("unsupported in this increment") != std::string::npos);
    CHECK(spec_limit->find("local") == std::string::npos);  // not a local-leaf message
    // Controls: an injective, path-free printed form keeps its established signature.
    const Symbol* declaration_named = find_symbol(r->facts, "declaration_named");
    REQUIRE(declaration_named);
    CHECK(declaration_named->key.normalized_signature == "(Wrap<&Holder<int>::value>)");
    const Symbol* plain_decl = find_symbol(r->facts, "plain_decl");
    REQUIRE(plain_decl);
    CHECK(plain_decl->key.normalized_signature == "(Wrap<&plain_object>)");
    const Symbol* plain_func = find_symbol(r->facts, "plain_func");
    REQUIRE(plain_func);
    CHECK(plain_func->key.normalized_signature == "(Wrap<&plain_fn>)");
    // No merged or bogus target survives: the callers keep no call edge to a collapsed symbol.
    const Symbol* caller_a = find_symbol(r->facts, "caller_a", "()");
    const Symbol* caller_b = find_symbol(r->facts, "caller_b", "()");
    REQUIRE(caller_a);
    REQUIRE(caller_b);
    for (const Symbol* caller : {caller_a, caller_b}) {
      for (const auto* rel : relations_from(r->facts, caller->id, RelationKind::calls)) {
        const Symbol* callee = r->facts.find_symbol(rel->key.target);
        REQUIRE(callee);
        CHECK(callee->key.canonical_name != "local_target");
      }
    }
    // The WHOLE canonical key of every symbol, not only the signature, is path/line free.
    for (const auto& [id, sym] : r->facts.symbols()) {
      CHECK_FALSE(leaks_source_spelling(serialize_canonical_key(sym.key)));
    }
  }
  // Identical bytes in another root and the same source shifted by five lines: one identity set.
  CHECK(id_set(d_orig.facts) == id_set(d_copy.facts));
  CHECK(id_set(d_orig.facts) == id_set(d_shift.facts));
  CHECK(find_symbol(d_orig.facts, "declaration_named")->id == find_symbol(d_shift.facts, "declaration_named")->id);

  // Blocker 2 (agent3): typed null and integral non-type arguments.
  const std::string null_src =
      "auto g1 = [] {};\n"
      "auto g2 = [] {};\n"
      "template <auto V> struct Wrap {};\n"
      "struct A {};\n"
      "struct B {};\n"
      "void consume_null(Wrap<static_cast<decltype(g1)*>(nullptr)>) {}\n"
      "void consume_null(Wrap<static_cast<decltype(g2)*>(nullptr)>) {}\n"
      "void consume_named_null(Wrap<static_cast<A*>(nullptr)>) {}\n"
      "void consume_named_null(Wrap<static_cast<B*>(nullptr)>) {}\n"
      "void consume_integral(Wrap<static_cast<short>(1)>) {}\n"
      "void consume_integral(Wrap<static_cast<int>(1)>) {}\n"
      "void consume_integral(Wrap<static_cast<long long>(1)>) {}\n"
      "void consume_unsigned(Wrap<1>) {}\n"
      "void consume_unsigned(Wrap<1u>) {}\n"
      "int call_nulls() {\n"
      "  consume_null(Wrap<static_cast<decltype(g1)*>(nullptr)>{});\n"
      "  consume_null(Wrap<static_cast<decltype(g2)*>(nullptr)>{});\n"
      "  consume_named_null(Wrap<static_cast<A*>(nullptr)>{});\n"
      "  consume_named_null(Wrap<static_cast<B*>(nullptr)>{});\n"
      "  consume_integral(Wrap<static_cast<short>(1)>{});\n"
      "  consume_integral(Wrap<static_cast<long long>(1)>{});\n"
      "  return 0;\n"
      "}\n"
      "int local_enum_nttp() {\n"
      "  enum E { X };\n"
      "  auto first = [](Wrap<X>) { return 1; };\n"
      "  {\n"
      "    enum E { X };\n"
      "    auto second = [](Wrap<X>) { return 2; };\n"
      "    return first({}) + second({});\n"
      "  }\n"
      "}\n";
  const fs::path root_n = work / "nulls";
  write_file(root_n / "unit.cpp", null_src);
  const AnalysisResult n = gate(root_n, root_n / "unit.cpp", argv);
  for (const auto& d : n.diagnostics) MESSAGE(d.text());
  REQUIRE(n.status == AnalysisStatus::ok);
  const FactSet& f = n.facts;
  const auto nulls = symbols_named(f, "consume_null", SymbolKind::function);
  REQUIRE(nulls.size() == 2);
  for (const auto* s : nulls) {
    CHECK(s->locations.size() == 1);
    CHECK(s->key.normalized_signature.find("nullptr(ptr([lambda ") != std::string::npos);
    CHECK_FALSE(leaks_source_spelling(s->key.normalized_signature));
  }
  CHECK(nulls[0]->key.normalized_signature != nulls[1]->key.normalized_signature);
  const auto named_nulls = symbols_named(f, "consume_named_null", SymbolKind::function);
  REQUIRE(named_nulls.size() == 2);
  for (const auto* s : named_nulls) CHECK(s->locations.size() == 1);
  {
    std::set<std::string> sigs;
    for (const auto* s : named_nulls) sigs.insert(s->key.normalized_signature);
    CHECK(sigs.count("(Wrap<nullptr(ptr(A))>)") == 1);
    CHECK(sigs.count("(Wrap<nullptr(ptr(B))>)") == 1);
  }
  const auto integrals = symbols_named(f, "consume_integral", SymbolKind::function);
  REQUIRE(integrals.size() == 3);  // short / int / long long keep their established raw spellings
  {
    std::set<std::string> sigs;
    for (const auto* s : integrals) sigs.insert(s->key.normalized_signature);
    CHECK(sigs.size() == 3);
    CHECK(sigs.count("(Wrap<1>)") == 1);  // int stays the plain numeric spelling
  }
  const auto unsigneds = symbols_named(f, "consume_unsigned", SymbolKind::function);
  REQUIRE(unsigneds.size() == 2);
  CHECK(unsigneds[0]->key.normalized_signature != unsigneds[1]->key.normalized_signature);
  // Exact call targets: the g1 null overload, the B named overload, the short integral overload.
  const Symbol* call_nulls = find_symbol(f, "call_nulls", "()");
  REQUIRE(call_nulls);
  const auto called = relations_from(f, call_nulls->id, RelationKind::calls);
  std::set<std::string> called_sigs;
  for (const auto* rel : called) {
    const Symbol* target = f.find_symbol(rel->key.target);
    REQUIRE(target);
    if (target->key.kind == SymbolKind::function) called_sigs.insert(target->key.canonical_name + target->key.normalized_signature);
  }
  CHECK(called_sigs.count("consume_named_null(Wrap<nullptr(ptr(A))>)") == 1);
  CHECK(called_sigs.count("consume_named_null(Wrap<nullptr(ptr(B))>)") == 1);
  const Symbol* g1_var = find_symbol(f, "g1", "", SymbolKind::variable);
  const Symbol* g2_var = find_symbol(f, "g2", "", SymbolKind::variable);
  REQUIRE(g1_var);
  REQUIRE(g2_var);
  const auto g1_lambda = lambdas_enclosed_by(f, g1_var->id);
  const auto g2_lambda = lambdas_enclosed_by(f, g2_var->id);
  REQUIRE(g1_lambda.size() == 1);
  REQUIRE(g2_lambda.size() == 1);
  CHECK(g1_lambda.front()->id.value != g2_lambda.front()->id.value);
  // Both sides of every overload pair are called and each call reaches its own distinct target.
  CHECK(called_sigs.count("consume_null(Wrap<nullptr(ptr([lambda " + g1_lambda.front()->id.value + "]))>)") == 1);
  CHECK(called_sigs.count("consume_null(Wrap<nullptr(ptr([lambda " + g2_lambda.front()->id.value + "]))>)") == 1);
  CHECK(std::count_if(called_sigs.begin(), called_sigs.end(),
                      [](const std::string& s) { return s.rfind("consume_null", 0) == 0; }) == 2);
  CHECK(std::count_if(called_sigs.begin(), called_sigs.end(),
                      [](const std::string& s) { return s.rfind("consume_named_null", 0) == 0; }) == 2);
  CHECK(std::count_if(called_sigs.begin(), called_sigs.end(),
                      [](const std::string& s) { return s.rfind("consume_integral", 0) == 0; }) == 2);
  // Copied root with five inserted lines: the typed-null and local-enum identities are unchanged.
  const fs::path root_n_shift = work / path_from_utf8("nulls copied");
  write_file(root_n_shift / "unit.cpp", "\n\n\n\n\n" + null_src);
  const AnalysisResult n_shift = gate(root_n_shift, root_n_shift / "unit.cpp", argv);
  REQUIRE(n_shift.status == AnalysisStatus::ok);
  CHECK(id_set(f) == id_set(n_shift.facts));
  // Local enums as integral arguments: each block's `E` is its own atom, so the two lambdas differ by type,
  // not only by ordinal, and each is called exactly once with the right target.
  const Symbol* local_enum_nttp = find_symbol(f, "local_enum_nttp", "()");
  REQUIRE(local_enum_nttp);
  const auto enum_lambdas = lambdas_enclosed_by(f, local_enum_nttp->id);
  REQUIRE(enum_lambdas.size() == 2);
  for (const auto* l : enum_lambdas) {
    CHECK(l->key.normalized_signature.find("Wrap<int([local cm1:") != std::string::npos);
    CHECK(evidence_list(f, local_enum_nttp->id, l->id).size() == 1);
  }
  CHECK(enum_lambdas[0]->key.normalized_signature != enum_lambdas[1]->key.normalized_signature);
  const auto enums = symbols_named(f, "E", SymbolKind::enum_);
  REQUIRE(enums.size() == 2);
  const bool first_names_an_enum_atom =
      enum_lambdas[0]->key.normalized_signature.find(enums[0]->id.value) != std::string::npos ||
      enum_lambdas[0]->key.normalized_signature.find(enums[1]->id.value) != std::string::npos;
  CHECK(first_names_an_enum_atom);
  for (const auto& [id, s] : f.symbols()) CHECK_FALSE(leaks_source_spelling(s.key.normalized_signature));

  std::error_code ec;
  fs::remove_all(work, ec);
}

// ---------------------------------------------------------------------------
// Direct-base / direct-include increment helpers.

const Relation* extends_relation(const FactSet& f, const StableId& derived, const StableId& base) {
  return f.find_relation(RelationKey{derived, base, RelationKind::extends, Confidence::confirmed});
}

std::vector<const DirectIncludeFact*> includes_of(const FactSet& f, const std::string& includer,
                                                  const std::string& operand) {
  std::vector<const DirectIncludeFact*> out;
  for (const auto& i : f.direct_includes()) {
    if (i.includer.generic == includer && i.operand_as_written == operand) out.push_back(&i);
  }
  return out;
}

const DirectIncludeFact* one_include(const FactSet& f, const std::string& includer, const std::string& operand) {
  const auto all = includes_of(f, includer, operand);
  return all.size() == 1 ? all.front() : nullptr;
}

std::string span_text(const std::string& bytes, const SourceSpan& span) {
  if (span.end_offset > bytes.size() || span.begin_offset > span.end_offset) return {};
  return bytes.substr(span.begin_offset, span.end_offset - span.begin_offset);
}

TEST_CASE("direct bases: effective access, written access, virtual, lexical order and original spans") {
  const fs::path work = scratch_dir("direct-bases");
  const fs::path root = work / "repo";
  const std::string bases_src = R"cpp(struct A {};
struct B : A {};
class C : A, protected virtual B {};
struct D : virtual B, private C {};
)cpp";
  write_file(root / "bases.h", bases_src);
  write_file(root / "one.cpp", "#include \"bases.h\"\n");
  write_file(root / "two.cpp", "#include \"bases.h\"\n");
  const std::vector<std::string> argv_one = {"clang-cl.exe", "/c", "/std:c++20", "one.cpp"};
  const std::vector<std::string> argv_two = {"clang-cl.exe", "/c", "/std:c++20", "two.cpp"};
  const AnalysisResult one = gate(root, root / "one.cpp", argv_one, "unit-one");
  const AnalysisResult two = gate(root, root / "two.cpp", argv_two, "unit-two");
  for (const auto& d : one.diagnostics) MESSAGE(d.text());
  REQUIRE(one.status == AnalysisStatus::ok);
  REQUIRE(two.status == AnalysisStatus::ok);

  const FactSet& f = one.facts;
  const Symbol* a = find_symbol(f, "A", "", SymbolKind::struct_);
  const Symbol* b = find_symbol(f, "B", "", SymbolKind::struct_);
  const Symbol* c = find_symbol(f, "C", "", SymbolKind::class_);
  const Symbol* d = find_symbol(f, "D", "", SymbolKind::struct_);
  REQUIRE(a);
  REQUIRE(b);
  REQUIRE(c);
  REQUIRE(d);

  const std::string bytes = read_file(root / "bases.h");
  struct Expect {
    const Symbol* derived;
    const Symbol* base;
    BaseAccess access;
    bool written;
    bool is_virtual;
    std::uint32_t ordinal;
    const char* text;
  };
  const std::vector<Expect> expected = {
      {b, a, BaseAccess::public_, false, false, 0, "A"},
      {c, a, BaseAccess::private_, false, false, 0, "A"},
      {c, b, BaseAccess::protected_, true, true, 1, "protected virtual B"},
      {d, b, BaseAccess::public_, false, true, 0, "virtual B"},
      {d, c, BaseAccess::private_, true, false, 1, "private C"},
  };
  for (const Expect& e : expected) {
    const Relation* rel = extends_relation(f, e.derived->id, e.base->id);
    REQUIRE(rel);
    REQUIRE(rel->evidence.size() == 1);
    const Evidence& ev = rel->evidence.front();
    REQUIRE(ev.base.has_value());
    CHECK(ev.base->effective_access == e.access);
    CHECK(ev.base->access_written == e.written);
    CHECK(ev.base->is_virtual == e.is_virtual);
    CHECK(ev.base->lexical_ordinal == e.ordinal);
    // The full written base specifier, reproducible from the original bytes.
    CHECK(span_text(bytes, ev.location.span) == e.text);
    REQUIRE(ev.location.span_hash.has_value());
    CHECK(ev.location.span_hash == hash_span(bytes, ev.location.span));
    CHECK(ev.location.file.generic == "bases.h");
  }
  // Only direct bases are stored; the transitive D -> A is a query, never a fact.
  CHECK(extends_relation(f, d->id, a->id) == nullptr);
  std::size_t extends_count = 0;
  for (const auto& [key, rel] : f.relations()) {
    if (key.kind == RelationKind::extends) ++extends_count;
  }
  CHECK(extends_count == 5);

  // Two units contribute the same base evidence with their own ownership.
  FactSet merged;
  REQUIRE(publish_contribution(merged, one).published);
  REQUIRE(publish_contribution(merged, two).published);
  const Symbol* mc = find_symbol(merged, "C", "", SymbolKind::class_);
  const Symbol* mb = find_symbol(merged, "B", "", SymbolKind::struct_);
  REQUIRE(mc);
  REQUIRE(mb);
  const Relation* merged_rel = extends_relation(merged, mc->id, mb->id);
  REQUIRE(merged_rel);
  REQUIRE(merged_rel->evidence.size() == 2);
  for (const auto& ev : merged_rel->evidence) {
    REQUIRE(ev.base.has_value());
    CHECK(ev.base->lexical_ordinal == 1);
    CHECK(ev.base->is_virtual);
  }
  // Removing one contribution leaves the other's evidence exactly, and the
  // survivor hash equals a freshly built survivor-only set.
  FactSet survivor_only;
  REQUIRE(publish_contribution(survivor_only, two).published);
  FactSet removed = merged;
  removed.remove_contribution(one.analysis_unit);
  CHECK(removed.fact_hash() == survivor_only.fact_hash());
  const Relation* survived = extends_relation(removed, mc->id, mb->id);
  REQUIRE(survived);
  REQUIRE(survived->evidence.size() == 1);
  CHECK(survived->evidence.front().analysis_unit == two.analysis_unit);
  REQUIRE(survived->evidence.front().base.has_value());
  CHECK(survived->evidence.front().base->effective_access == BaseAccess::protected_);

  // Base details take part in the fact hash: a changed detail changes it.
  FactSet tampered = survivor_only;
  {
    Evidence ev = survived->evidence.front();
    ev.base->lexical_ordinal = 7;
    tampered.add_relation_evidence(RelationKey{mc->id, mb->id, RelationKind::extends, Confidence::confirmed}, ev);
  }
  CHECK(tampered.fact_hash() != survivor_only.fact_hash());

  // Three runs are identical.
  const AnalysisResult again = gate(root, root / "one.cpp", argv_one, "unit-one");
  const AnalysisResult again2 = gate(root, root / "one.cpp", argv_one, "unit-one");
  CHECK(one.facts.fact_hash() == again.facts.fact_hash());
  CHECK(one.facts.fact_hash() == again2.facts.fact_hash());

  std::error_code ec;
  fs::remove_all(work, ec);
}

TEST_CASE("template bases fold to the selected pattern and keep their use-site arguments") {
  const fs::path work = scratch_dir("template-bases");
  const fs::path root = work / "repo";
  const std::string bases_src = R"cpp(struct Root {};
struct Plain {};
template <class T> struct Box { struct Inner {}; };
template <class T> struct G : Root {};
template <class U> struct G<U*> {};
template <> struct G<char> {};
template <class T> struct H {};
extern template struct H<int>;
template struct H<long>;

struct DPrimary : G<int> {};
struct DPartial : G<int*> {};
struct DTwo : G<int>, G<short> {};
struct DSpec : G<char> {};
struct DOrdinary : Plain {};
struct DExtDecl : H<int> {};
struct DExtDef : H<long> {};
template <class T> struct DDependent : G<T> {};

inline void local_use() {
  struct Local {};
  struct DLocal : G<Local> {};
  struct DUnsupported : G<Box<Local>::Inner> {};
  (void)sizeof(DLocal);
  (void)sizeof(DUnsupported);
}
)cpp";
  write_file(root / "bases.h", bases_src);
  write_file(root / "one.cpp", "#include \"bases.h\"\n");
  write_file(root / "two.cpp", "#include \"bases.h\"\n");
  const std::vector<std::string> argv_one = {"clang-cl.exe", "/c", "/std:c++20", "one.cpp"};
  const std::vector<std::string> argv_two = {"clang-cl.exe", "/c", "/std:c++20", "two.cpp"};
  const AnalysisResult one = gate(root, root / "one.cpp", argv_one, "unit-one");
  const AnalysisResult two = gate(root, root / "two.cpp", argv_two, "unit-two");
  for (const auto& d : one.diagnostics) MESSAGE(d.text());
  REQUIRE(one.status == AnalysisStatus::ok);
  REQUIRE(two.status == AnalysisStatus::ok);

  const FactSet& f = one.facts;
  const std::string bytes = read_file(root / "bases.h");

  // The instantiations are never public nodes: only the three written G
  // declarations exist, each with its own template role.
  const Symbol* primary = find_symbol(f, "G", "", SymbolKind::struct_, TemplateRole::primary);
  const Symbol* partial = find_symbol(f, "G", "", SymbolKind::struct_, TemplateRole::partial_specialization);
  const Symbol* spec = find_symbol(f, "G", "", SymbolKind::struct_, TemplateRole::explicit_specialization);
  REQUIRE(primary);
  REQUIRE(partial);
  REQUIRE(spec);
  CHECK(symbols_named(f, "G", SymbolKind::struct_).size() == 3);
  CHECK(symbols_named(f, "H", SymbolKind::struct_).size() == 1);

  const auto base_evidence = [&](const char* derived, const Symbol* target) {
    const Symbol* d = find_symbol(f, derived, "", SymbolKind::struct_);
    REQUIRE(d);
    const Relation* rel = extends_relation(f, d->id, target->id);
    REQUIRE(rel);
    return rel->evidence;
  };

  // Primary pattern target, use-site arguments on the evidence, original span.
  {
    const auto evidence = base_evidence("DPrimary", primary);
    REQUIRE(evidence.size() == 1);
    const Evidence& ev = evidence.front();
    CHECK(ev.template_use == TemplateUse::primary_implicit);
    CHECK(ev.template_arguments == "<int>");
    REQUIRE(ev.base.has_value());
    CHECK(ev.base->effective_access == BaseAccess::public_);
    CHECK(ev.base->lexical_ordinal == 0);
    CHECK(span_text(bytes, ev.location.span) == "G<int>");
    REQUIRE(ev.location.span_hash.has_value());
    CHECK(ev.location.span_hash == hash_span(bytes, ev.location.span));
  }
  // The selected partial specialization is the target, not the primary.
  {
    const auto evidence = base_evidence("DPartial", partial);
    REQUIRE(evidence.size() == 1);
    CHECK(evidence.front().template_use == TemplateUse::partial_implicit);
    CHECK(evidence.front().template_arguments == "<int *>");
    CHECK(span_text(bytes, evidence.front().location.span) == "G<int*>");
    const Symbol* d = find_symbol(f, "DPartial", "", SymbolKind::struct_);
    REQUIRE(d);
    CHECK(extends_relation(f, d->id, primary->id) == nullptr);
  }
  // Two bases selecting one pattern: one relation key, two distinct evidences.
  {
    const auto evidence = base_evidence("DTwo", primary);
    REQUIRE(evidence.size() == 2);
    std::vector<std::string> arguments;
    std::vector<std::uint32_t> ordinals;
    for (const auto& ev : evidence) {
      CHECK(ev.template_use == TemplateUse::primary_implicit);
      REQUIRE(ev.base.has_value());
      arguments.push_back(ev.template_arguments);
      ordinals.push_back(ev.base->lexical_ordinal);
    }
    std::sort(arguments.begin(), arguments.end());
    std::sort(ordinals.begin(), ordinals.end());
    CHECK(arguments == std::vector<std::string>{"<int>", "<short>"});
    CHECK(ordinals == std::vector<std::uint32_t>{0, 1});
  }
  // Explicit instantiation declaration and definition keep their own use value
  // and are never labelled implicit; the pattern kind is the target's role.
  {
    const Symbol* h = find_symbol(f, "H", "", SymbolKind::struct_, TemplateRole::primary);
    REQUIRE(h);
    const auto declared = base_evidence("DExtDecl", h);
    REQUIRE(declared.size() == 1);
    CHECK(declared.front().template_use == TemplateUse::explicit_instantiation_declaration);
    CHECK(declared.front().template_arguments == "<int>");
    const auto defined = base_evidence("DExtDef", h);
    REQUIRE(defined.size() == 1);
    CHECK(defined.front().template_use == TemplateUse::explicit_instantiation_definition);
    CHECK(defined.front().template_arguments == "<long>");
  }
  // Controls: an explicit specialization and an ordinary base are unchanged.
  {
    const auto evidence = base_evidence("DSpec", spec);
    REQUIRE(evidence.size() == 1);
    CHECK(evidence.front().template_use == TemplateUse::none);
    CHECK(evidence.front().template_arguments.empty());
    const Symbol* plain = find_symbol(f, "Plain", "", SymbolKind::struct_);
    REQUIRE(plain);
    const auto ordinary = base_evidence("DOrdinary", plain);
    REQUIRE(ordinary.size() == 1);
    CHECK(ordinary.front().template_use == TemplateUse::none);
    CHECK(ordinary.front().template_arguments.empty());
  }
  // A representable local argument is atomized, never spelled with a path.
  {
    const Symbol* d = find_symbol(f, "DLocal", "", SymbolKind::struct_);
    REQUIRE(d);
    const Relation* rel = extends_relation(f, d->id, primary->id);
    REQUIRE(rel);
    REQUIRE(rel->evidence.size() == 1);
    const std::string arguments = rel->evidence.front().template_arguments;
    CHECK_FALSE(arguments.empty());
    CHECK_FALSE(leaks_source_spelling(arguments));
    CHECK(arguments.find("cm1:") != std::string::npos);
  }
  // Negatives: an unrepresentable argument and a dependent base produce limits
  // and no edge; the transitive base of the pattern is never stored.
  {
    const Symbol* unsupported = find_symbol(f, "DUnsupported", "", SymbolKind::struct_);
    REQUIRE(unsupported);
    CHECK(extends_relation(f, unsupported->id, primary->id) == nullptr);
    const auto has_limit = [&](const char* text) {
      return std::any_of(one.limits.begin(), one.limits.end(),
                         [&](const std::string& l) { return l.find(text) != std::string::npos; });
    };
    CHECK(has_limit("base class template arguments could not be represented"));
    CHECK(has_limit("dependent or non-record base class was not recorded"));
    const Symbol* root_symbol = find_symbol(f, "Root", "", SymbolKind::struct_);
    const Symbol* d_primary = find_symbol(f, "DPrimary", "", SymbolKind::struct_);
    REQUIRE(root_symbol);
    REQUIRE(d_primary);
    CHECK(extends_relation(f, primary->id, root_symbol->id) != nullptr);  // the pattern's own base
    CHECK(extends_relation(f, d_primary->id, root_symbol->id) == nullptr);
  }

  // Contribution survival: two units observe the same folded base.
  FactSet merged;
  REQUIRE(publish_contribution(merged, one).published);
  REQUIRE(publish_contribution(merged, two).published);
  const Symbol* merged_primary =
      find_symbol(merged, "G", "", SymbolKind::struct_, TemplateRole::primary);
  const Symbol* merged_derived = find_symbol(merged, "DPrimary", "", SymbolKind::struct_);
  REQUIRE(merged_primary);
  REQUIRE(merged_derived);
  const Relation* merged_rel = extends_relation(merged, merged_derived->id, merged_primary->id);
  REQUIRE(merged_rel);
  REQUIRE(merged_rel->evidence.size() == 2);
  FactSet survivor_only;
  REQUIRE(publish_contribution(survivor_only, two).published);
  FactSet removed = merged;
  removed.remove_contribution(one.analysis_unit);
  CHECK(removed.fact_hash() == survivor_only.fact_hash());
  const Relation* survived = extends_relation(removed, merged_derived->id, merged_primary->id);
  REQUIRE(survived);
  REQUIRE(survived->evidence.size() == 1);
  CHECK(survived->evidence.front().template_arguments == "<int>");

  // The arguments take part in the fact hash.
  FactSet tampered = survivor_only;
  {
    Evidence ev = survived->evidence.front();
    ev.template_arguments = "<other>";
    tampered.add_relation_evidence(
        RelationKey{merged_derived->id, merged_primary->id, RelationKind::extends, Confidence::confirmed}, ev);
  }
  CHECK(tampered.fact_hash() != survivor_only.fact_hash());

  // Three runs are identical.
  const AnalysisResult again = gate(root, root / "one.cpp", argv_one, "unit-one");
  const AnalysisResult again2 = gate(root, root / "one.cpp", argv_one, "unit-one");
  CHECK(one.facts.fact_hash() == again.facts.fact_hash());
  CHECK(one.facts.fact_hash() == again2.facts.fact_hash());

  std::error_code ec2;
  fs::remove_all(work, ec2);
}

TEST_CASE("a template base whose pattern is outside the root stays an external placeholder") {
  const fs::path work = scratch_dir("template-base-external");
  const fs::path root = work / "repo";
  const fs::path outside = work / "sdk";
  fs::create_directories(root);
  fs::create_directories(outside);
  write_file(outside / "ext_tpl.hpp", "template <class T> struct E {};\n");
  write_file(root / "a.cpp", "#include \"ext_tpl.hpp\"\nstruct DExt : E<int> {};\n");
  const AnalysisResult r = gate(root, root / "a.cpp", {"clang-cl.exe", "/c", "/std:c++20", "/I../sdk", "a.cpp"});
  for (const auto& d : r.diagnostics) MESSAGE(d.text());
  REQUIRE(r.status == AnalysisStatus::ok);

  const FactSet& f = r.facts;
  const Symbol* pattern = find_symbol(f, "E", "", SymbolKind::struct_, TemplateRole::primary);
  REQUIRE(pattern);
  CHECK(pattern->presence == SymbolPresence::external_placeholder);
  CHECK(pattern->key.repository_member == "external");
  CHECK(pattern->locations.empty());
  CHECK(symbols_named(f, "E", SymbolKind::struct_).size() == 1);

  const Symbol* derived = find_symbol(f, "DExt", "", SymbolKind::struct_);
  REQUIRE(derived);
  const Relation* rel = extends_relation(f, derived->id, pattern->id);
  REQUIRE(rel);
  REQUIRE(rel->evidence.size() == 1);
  CHECK(rel->evidence.front().template_use == TemplateUse::primary_implicit);
  CHECK(rel->evidence.front().template_arguments == "<int>");

  std::error_code ec;
  fs::remove_all(work, ec);
}

TEST_CASE("direct includes: original bytes, structured conditions, activity, resolution and privacy") {
  const fs::path work = scratch_dir("direct-includes");
  const fs::path root = work / "repo";
  // CRLF throughout the main file so the continued macro directive keeps its
  // original backslash and CRLF bytes.
  const std::string main_src =
      std::string("#define HDR \"macro.hpp\"\r\n#include \\\r\n  HDR\r\n") +
      "\r\n"
      "#if OUTER\r\n"
      "# if A\r\n"
      "#  include \"a.hpp\"\r\n"
      "# elif B\r\n"
      "#  include \"b.hpp\"\r\n"
      "# else\r\n"
      "#  include \"c.hpp\"\r\n"
      "# endif\r\n"
      "#else\r\n"
      "# include \"outer_else.hpp\"\r\n"
      "#endif\r\n"
      "\r\n"
      "#if 0\r\n"
      "#include \"dead.hpp\"\r\n"
      "#endif\r\n"
      "\r\n"
      "#include \"guarded.hpp\"\r\n"
      "#include \"guarded.hpp\"\r\n"
      "\r\n"
      "#define TOGGLE 1\r\n"
      "#include \"toggle.hpp\"\r\n"
      "#undef TOGGLE\r\n"
      "#include \"toggle.hpp\"\r\n"
      "\r\n"
      "#include \"direct.hpp\"\r\n"
      "#include <ext_one.hpp>\r\n"
      "#include <ext_two.hpp>\r\n"
      "int anchor_symbol() { return 0; }\r\n";
  write_file(root / "main.cpp", main_src);
  for (const char* h : {"macro.hpp", "a.hpp", "b.hpp", "c.hpp", "outer_else.hpp", "dead.hpp", "leaf.hpp"}) {
    write_file(root / h, "\n");
  }
  write_file(root / "guarded.hpp", "#pragma once\n");
  write_file(root / "toggle.hpp",
             "#ifdef TOGGLE\n#include \"toggle_on.hpp\"\n#else\n#include \"toggle_off.hpp\"\n#endif\n");
  write_file(root / "toggle_on.hpp", "\n");
  write_file(root / "toggle_off.hpp", "\n");
  write_file(root / "direct.hpp", "#pragma once\n#include \"leaf.hpp\"\n");
  // Two external headers with IDENTICAL bytes: their opaque keys must differ.
  const fs::path ext = work / "outside";
  write_file(ext / "ext_one.hpp", "\n");
  write_file(ext / "ext_two.hpp", "\n");

  const std::vector<std::string> argv_on = {"clang-cl.exe", "/c", "/std:c++20", "/DOUTER=1", "/DA=0",
                                            "/DB=1",        "/I", ext.string(), "main.cpp"};
  const std::vector<std::string> argv_off = {"clang-cl.exe", "/c", "/std:c++20", "/DOUTER=0", "/DA=0",
                                             "/DB=0",        "/I", ext.string(), "main.cpp"};
  const AnalysisResult on = gate(root, root / "main.cpp", argv_on, "unit-on");
  const AnalysisResult off = gate(root, root / "main.cpp", argv_off, "unit-off");
  for (const auto& d : on.diagnostics) MESSAGE(d.text());
  REQUIRE(on.status == AnalysisStatus::ok);
  REQUIRE(off.status == AnalysisStatus::ok);
  const FactSet& f = on.facts;
  const std::string bytes = read_file(root / "main.cpp");

  // Macro-spelled, line-continued directive: both spellings, exact bytes.
  const DirectIncludeFact* macro_inc = one_include(f, "main.cpp", "HDR");
  REQUIRE(macro_inc);
  CHECK(macro_inc->operand_kind == IncludeOperandKind::macro_tokens);
  CHECK(macro_inc->expanded_spelling == std::optional<std::string>("macro.hpp"));
  CHECK(macro_inc->activity == IncludeActivity::active);
  CHECK(macro_inc->resolution == IncludeResolution::resolved_internal);
  REQUIRE(macro_inc->resolved_repo_target.has_value());
  CHECK(macro_inc->resolved_repo_target->generic == "macro.hpp");
  CHECK(macro_inc->directive_kind == IncludeDirectiveKind::include);
  // The original bytes keep the backslash and the CRLF continuation and stop
  // before the final line terminator.
  CHECK(span_text(bytes, macro_inc->directive_location.span) == "#include \\\r\n  HDR");
  REQUIRE(macro_inc->directive_location.span_hash.has_value());
  CHECK(macro_inc->directive_location.span_hash == hash_span(bytes, macro_inc->directive_location.span));
  CHECK(macro_inc->condition_path.empty());

  // Selected inner branch: active and resolved; the siblings are inactive with
  // no target and no guessing from the filesystem, although all files exist.
  const DirectIncludeFact* b_inc = one_include(f, "main.cpp", "\"b.hpp\"");
  REQUIRE(b_inc);
  CHECK(b_inc->activity == IncludeActivity::active);
  CHECK(b_inc->resolution == IncludeResolution::resolved_internal);
  REQUIRE(b_inc->condition_path.size() == 2);
  REQUIRE(b_inc->condition_path[0].own_condition.has_value());
  CHECK(b_inc->condition_path[0].own_condition->kind == ConditionTermKind::if_);
  CHECK(b_inc->condition_path[0].own_condition->expression_as_written == "OUTER");
  CHECK(b_inc->condition_path[0].preceding_branches.empty());
  REQUIRE(b_inc->condition_path[1].own_condition.has_value());
  CHECK(b_inc->condition_path[1].own_condition->kind == ConditionTermKind::elif);
  CHECK(b_inc->condition_path[1].own_condition->expression_as_written == "B");
  REQUIRE(b_inc->condition_path[1].preceding_branches.size() == 1);
  CHECK(b_inc->condition_path[1].preceding_branches[0].kind == ConditionTermKind::if_);
  CHECK(b_inc->condition_path[1].preceding_branches[0].expression_as_written == "A");
  // Condition terms keep their own original bytes.
  CHECK(span_text(bytes, b_inc->condition_path[1].own_condition->location.span) == "# elif B");
  CHECK(span_text(bytes, b_inc->condition_path[1].preceding_branches[0].location.span) == "# if A");

  for (const char* dead_operand : {"\"a.hpp\"", "\"c.hpp\"", "\"outer_else.hpp\"", "\"dead.hpp\""}) {
    const DirectIncludeFact* inc = one_include(f, "main.cpp", dead_operand);
    REQUIRE(inc);
    CHECK(inc->activity == IncludeActivity::inactive);
    CHECK(inc->resolution == IncludeResolution::not_evaluated);
    CHECK_FALSE(inc->resolved_repo_target.has_value());
    CHECK_FALSE(inc->expanded_spelling.has_value());
    CHECK(inc->external_dependency_key.empty());
  }
  // #else records every preceding sibling and no own condition.
  const DirectIncludeFact* c_inc = one_include(f, "main.cpp", "\"c.hpp\"");
  REQUIRE(c_inc);
  REQUIRE(c_inc->condition_path.size() == 2);
  CHECK(c_inc->condition_path[1].is_else);
  CHECK_FALSE(c_inc->condition_path[1].own_condition.has_value());
  REQUIRE(c_inc->condition_path[1].preceding_branches.size() == 2);
  CHECK(c_inc->condition_path[1].preceding_branches[0].expression_as_written == "A");
  CHECK(c_inc->condition_path[1].preceding_branches[1].expression_as_written == "B");
  const DirectIncludeFact* outer_else = one_include(f, "main.cpp", "\"outer_else.hpp\"");
  REQUIRE(outer_else);
  REQUIRE(outer_else->condition_path.size() == 1);
  CHECK(outer_else->condition_path[0].is_else);
  REQUIRE(outer_else->condition_path[0].preceding_branches.size() == 1);
  CHECK(outer_else->condition_path[0].preceding_branches[0].expression_as_written == "OUTER");
  // A literal #if 0 is an ordinary inactive branch, not a special case.
  const DirectIncludeFact* dead = one_include(f, "main.cpp", "\"dead.hpp\"");
  REQUIRE(dead);
  REQUIRE(dead->condition_path.size() == 1);
  CHECK(dead->condition_path[0].own_condition->expression_as_written == "0");

  // The other context selects the other branches; nothing is last-writer.
  const DirectIncludeFact* off_else = one_include(off.facts, "main.cpp", "\"outer_else.hpp\"");
  REQUIRE(off_else);
  CHECK(off_else->activity == IncludeActivity::active);
  CHECK(off_else->resolution == IncludeResolution::resolved_internal);
  for (const char* inner : {"\"a.hpp\"", "\"b.hpp\"", "\"c.hpp\""}) {
    const DirectIncludeFact* inc = one_include(off.facts, "main.cpp", inner);
    REQUIRE(inc);
    CHECK(inc->activity == IncludeActivity::inactive);
    CHECK_FALSE(inc->resolved_repo_target.has_value());
  }

  // Header-guard skipping is NOT conditional inactivity: both directives stay
  // active and resolved, with their own original spans.
  const auto guarded = includes_of(f, "main.cpp", "\"guarded.hpp\"");
  REQUIRE(guarded.size() == 2);
  CHECK(guarded[0]->directive_location.span.begin_offset != guarded[1]->directive_location.span.begin_offset);
  for (const auto* inc : guarded) {
    CHECK(inc->activity == IncludeActivity::active);
    CHECK(inc->resolution == IncludeResolution::resolved_internal);
    CHECK(inc->resolved_repo_target->generic == "guarded.hpp");
  }

  // The same unguarded header entered twice under toggled macros: both visits
  // survive with their own distinct targets, neither overwriting the other.
  const auto on_inc = includes_of(f, "toggle.hpp", "\"toggle_on.hpp\"");
  const auto off_inc = includes_of(f, "toggle.hpp", "\"toggle_off.hpp\"");
  REQUIRE(on_inc.size() == 2);
  REQUIRE(off_inc.size() == 2);
  const auto activities = [](const std::vector<const DirectIncludeFact*>& v) {
    std::set<IncludeActivity> out;
    for (const auto* i : v) out.insert(i->activity);
    return out;
  };
  CHECK(activities(on_inc) == std::set<IncludeActivity>{IncludeActivity::active, IncludeActivity::inactive});
  CHECK(activities(off_inc) == std::set<IncludeActivity>{IncludeActivity::active, IncludeActivity::inactive});
  for (const auto* i : on_inc) {
    if (i->activity == IncludeActivity::active) CHECK(i->resolved_repo_target->generic == "toggle_on.hpp");
  }
  for (const auto* i : off_inc) {
    if (i->activity == IncludeActivity::active) CHECK(i->resolved_repo_target->generic == "toggle_off.hpp");
  }

  // External resolution stays a private dependency: no repository path, an
  // opaque key, and distinct keys for distinct files with identical bytes.
  const DirectIncludeFact* ext_one = one_include(f, "main.cpp", "<ext_one.hpp>");
  const DirectIncludeFact* ext_two = one_include(f, "main.cpp", "<ext_two.hpp>");
  REQUIRE(ext_one);
  REQUIRE(ext_two);
  for (const auto* inc : {ext_one, ext_two}) {
    CHECK(inc->operand_kind == IncludeOperandKind::angled);
    CHECK(inc->activity == IncludeActivity::active);
    CHECK(inc->resolution == IncludeResolution::resolved_external);
    CHECK_FALSE(inc->resolved_repo_target.has_value());
    CHECK_FALSE(inc->external_dependency_key.empty());
    CHECK(inc->external_dependency_key.find('/') == std::string::npos);
    CHECK(inc->external_dependency_key.find('\\') == std::string::npos);
    CHECK(inc->external_dependency_key.find("outside") == std::string::npos);
  }
  CHECK(ext_one->external_dependency_key != ext_two->external_dependency_key);
  // The private observations still hold the resolved paths for invalidation.
  CHECK(std::any_of(on.files.begin(), on.files.end(),
                    [](const FileObservation& o) { return !o.in_root && o.path_utf8.find("ext_one") != std::string::npos; }));

  // Only direct edges: main -> direct.hpp and direct.hpp -> leaf.hpp exist,
  // main -> leaf.hpp does not, and no file symbol was invented for any of them.
  REQUIRE(one_include(f, "main.cpp", "\"direct.hpp\""));
  REQUIRE(one_include(f, "direct.hpp", "\"leaf.hpp\""));
  CHECK(one_include(f, "main.cpp", "\"leaf.hpp\"") == nullptr);
  for (const auto& [id, sym] : f.symbols()) {
    CHECK(sym.key.canonical_name.find(".hpp") == std::string::npos);
    CHECK(sym.key.canonical_name.find(".cpp") == std::string::npos);
  }
  std::size_t include_relations = 0;
  for (const auto& [key, rel] : f.relations()) {
    if (key.kind == RelationKind::includes) ++include_relations;
  }
  CHECK(include_relations == 0);  // include facts are not symbol relations in this increment

  // Determinism and contribution lifecycle.
  const AnalysisResult again = gate(root, root / "main.cpp", argv_on, "unit-on");
  const AnalysisResult again2 = gate(root, root / "main.cpp", argv_on, "unit-on");
  CHECK(f.fact_hash() == again.facts.fact_hash());
  CHECK(f.fact_hash() == again2.facts.fact_hash());
  FactSet merged;
  const PublishOutcome pub_on = publish_contribution(merged, on);
  REQUIRE(pub_on.published);
  CHECK(pub_on.direct_includes > 0);
  REQUIRE(publish_contribution(merged, off).published);
  CHECK(merged.direct_includes().size() > f.direct_includes().size());
  FactSet survivor_only;
  REQUIRE(publish_contribution(survivor_only, off).published);
  FactSet removed = merged;
  removed.remove_contribution(on.analysis_unit);
  CHECK(removed.fact_hash() == survivor_only.fact_hash());
  CHECK(std::none_of(removed.direct_includes().begin(), removed.direct_includes().end(),
                     [&](const DirectIncludeFact& i) { return i.analysis_unit == on.analysis_unit; }));
  CHECK(std::any_of(removed.direct_includes().begin(), removed.direct_includes().end(),
                    [&](const DirectIncludeFact& i) { return i.analysis_unit == off.analysis_unit; }));

  std::error_code ec;
  fs::remove_all(work, ec);
}

TEST_CASE("direct includes: missing target, post-error callbacks, and uncovered directive spellings") {
  const fs::path work = scratch_dir("include-boundaries");
  const std::vector<std::string> argv = {"clang-cl.exe", "/c", "/std:c++20", "main.cpp"};

  // A processed include the compiler could not find is `not_found`, NOT
  // inactive; and the observed callbacks of later directives still win, even
  // after a fatal missing include and an #error.
  const fs::path missing_root = work / "missing";
  write_file(missing_root / "main.cpp",
             "#include \"does_not_exist.hpp\"\n#include \"after.hpp\"\nint anchor() { return 0; }\n");
  write_file(missing_root / "after.hpp", "\n");
  const AnalysisResult missing = gate(missing_root, missing_root / "main.cpp", argv);
  CHECK(missing.status == AnalysisStatus::completed_with_errors);
  const DirectIncludeFact* absent = one_include(missing.facts, "main.cpp", "\"does_not_exist.hpp\"");
  REQUIRE(absent);
  CHECK(absent->activity == IncludeActivity::active);  // it WAS processed
  CHECK(absent->resolution == IncludeResolution::not_found);
  CHECK_FALSE(absent->resolved_repo_target.has_value());
  const DirectIncludeFact* after = one_include(missing.facts, "main.cpp", "\"after.hpp\"");
  REQUIRE(after);
  // An actually observed callback takes precedence: a directive after an error
  // is never blanket-downgraded to inactive or indeterminate.
  CHECK(after->activity == IncludeActivity::active);
  CHECK(after->resolution == IncludeResolution::resolved_internal);

  // A directive spelling the pinned dependency scanner does not report (the
  // digraph `%:` form) must never be published as "no includes" or, worse, as
  // an unconditional include that was actually inside a digraph conditional.
  const fs::path digraph_root = work / "digraph";
  write_file(digraph_root / "main.cpp", "%:include \"present.hpp\"\nint anchor() { return 0; }\n");
  write_file(digraph_root / "present.hpp", "\n");
  const AnalysisResult digraph = gate(digraph_root, digraph_root / "main.cpp", argv);
  REQUIRE(digraph.status == AnalysisStatus::ok);
  CHECK(digraph.facts.direct_includes().empty());
  CHECK(std::any_of(digraph.limits.begin(), digraph.limits.end(), [](const std::string& l) {
    return l.find("not covered by the dependency scanner") != std::string::npos;
  }));

  // Mixed: an ordinary #include nested inside a digraph conditional. Publishing
  // it would state an empty condition path, which is false, so the whole file's
  // include facts are withheld with the explicit limitation instead.
  const fs::path mixed_root = work / "mixed";
  write_file(mixed_root / "main.cpp",
             "%:if FLAG\n#include \"inner.hpp\"\n%:endif\n#include \"plain.hpp\"\nint anchor() { return 0; }\n");
  write_file(mixed_root / "inner.hpp", "\n");
  write_file(mixed_root / "plain.hpp", "\n");
  const AnalysisResult mixed = gate(mixed_root, mixed_root / "main.cpp", {"clang-cl.exe", "/c", "/std:c++20",
                                                                          "/DFLAG=1", "main.cpp"});
  REQUIRE(mixed.status == AnalysisStatus::ok);
  CHECK(includes_of(mixed.facts, "main.cpp", "\"inner.hpp\"").empty());
  CHECK(includes_of(mixed.facts, "main.cpp", "\"plain.hpp\"").empty());
  CHECK(std::any_of(mixed.limits.begin(), mixed.limits.end(), [](const std::string& l) {
    return l.find("not covered by the dependency scanner") != std::string::npos;
  }));
  // The private preprocessor observations are untouched by that withholding.
  CHECK(std::any_of(mixed.includes.begin(), mixed.includes.end(),
                    [](const IncludeObservation& o) { return o.spelling == "plain.hpp"; }));

  // Exact original bytes of a continued directive that also carries a trailing
  // comment: the span keeps the backslash, the intermediate CRLF and the
  // comment, and excludes only the final line terminator. The raw operand stops
  // at the last spelled token, so it holds neither the comment nor a newline.
  const fs::path comment_root = work / "comment";
  const std::string comment_src =
      std::string("#define HEADER \"target.hpp\"\r\n") + "#include \\\r\n  HEADER /* original trailing comment */\r\n" +
      "int anchor() { return 0; }\r\n";
  write_file(comment_root / "main.cpp", comment_src);
  write_file(comment_root / "target.hpp", "\n");
  const AnalysisResult commented = gate(comment_root, comment_root / "main.cpp", argv);
  REQUIRE(commented.status == AnalysisStatus::ok);
  const DirectIncludeFact* commented_inc = one_include(commented.facts, "main.cpp", "HEADER");
  REQUIRE(commented_inc);
  const std::string comment_bytes = read_file(comment_root / "main.cpp");
  CHECK(span_text(comment_bytes, commented_inc->directive_location.span) ==
        "#include \\\r\n  HEADER /* original trailing comment */");
  CHECK(commented_inc->directive_location.span_hash ==
        hash_span(comment_bytes, commented_inc->directive_location.span));
  CHECK(commented_inc->operand_as_written == "HEADER");
  CHECK(commented_inc->operand_kind == IncludeOperandKind::macro_tokens);
  CHECK(commented_inc->expanded_spelling == std::optional<std::string>("target.hpp"));
  CHECK(commented_inc->activity == IncludeActivity::active);

  // `#include_next` has a real callback on the pinned target and keeps its own
  // directive kind; `#import` is an unsupported Microsoft type-library import
  // here, so it is a diagnosed lexical directive with NO fabricated target.
  const fs::path kinds_root = work / "kinds";
  write_file(kinds_root / "main.cpp", "#include <next.hpp>\nint anchor() { return 0; }\n");
  write_file(kinds_root / "one/next.hpp", "#pragma once\n#include_next <next.hpp>\n");
  write_file(kinds_root / "two/next.hpp", "#pragma once\n");
  const AnalysisResult kinds =
      gate(kinds_root, kinds_root / "main.cpp",
           {"clang-cl.exe", "/c", "/std:c++20", "/I", (kinds_root / "one").string(), "/I",
            (kinds_root / "two").string(), "main.cpp"});
  REQUIRE(kinds.status == AnalysisStatus::ok);
  const DirectIncludeFact* next_inc = one_include(kinds.facts, "one/next.hpp", "<next.hpp>");
  REQUIRE(next_inc);
  CHECK(next_inc->directive_kind == IncludeDirectiveKind::include_next);
  CHECK(next_inc->activity == IncludeActivity::active);
  CHECK(next_inc->resolution == IncludeResolution::resolved_internal);
  REQUIRE(next_inc->resolved_repo_target.has_value());
  CHECK(next_inc->resolved_repo_target->generic == "two/next.hpp");

  const fs::path import_root = work / "import";
  write_file(import_root / "main.cpp", "#import \"lib.hpp\"\nint anchor() { return 0; }\n");
  write_file(import_root / "lib.hpp", "\n");
  const AnalysisResult imported = gate(import_root, import_root / "main.cpp", argv);
  const DirectIncludeFact* import_inc = one_include(imported.facts, "main.cpp", "\"lib.hpp\"");
  REQUIRE(import_inc);
  CHECK(import_inc->directive_kind == IncludeDirectiveKind::import);
  // No inclusion callback on this target: unproven, never inactive, and never a
  // resolved header target invented because the file happens to exist.
  CHECK(import_inc->activity == IncludeActivity::indeterminate);
  CHECK(import_inc->resolution == IncludeResolution::not_evaluated);
  CHECK_FALSE(import_inc->resolved_repo_target.has_value());
  CHECK(std::any_of(imported.limits.begin(), imported.limits.end(), [](const std::string& l) {
    return l.find("neither processed nor proven skipped") != std::string::npos;
  }));

  // A directive keyword split by an escaped newline cannot be matched literally
  // by any spelling comparison, so the coverage guard treats it as uncovered
  // rather than letting the file look complete.
  const fs::path spliced_root = work / "spliced";
  write_file(spliced_root / "main.cpp", "%:inc\\\nlude \"present.hpp\"\nint anchor() { return 0; }\n");
  write_file(spliced_root / "present.hpp", "\n");
  const AnalysisResult spliced = gate(spliced_root, spliced_root / "main.cpp", argv);
  CHECK(spliced.facts.direct_includes().empty());
  CHECK(std::any_of(spliced.limits.begin(), spliced.limits.end(), [](const std::string& l) {
    return l.find("not covered by the dependency scanner") != std::string::npos;
  }));

  // Mixed spliced digraph conditional plus an ordinary include: the ordinary
  // include must NOT be published as unconditional.
  const fs::path spliced_mixed_root = work / "spliced-mixed";
  write_file(spliced_mixed_root / "main.cpp",
             "%:i\\\nf FLAG\n#include \"inner.hpp\"\n%:endif\nint anchor() { return 0; }\n");
  write_file(spliced_mixed_root / "inner.hpp", "\n");
  const AnalysisResult spliced_mixed =
      gate(spliced_mixed_root, spliced_mixed_root / "main.cpp",
           {"clang-cl.exe", "/c", "/std:c++20", "/DFLAG=1", "main.cpp"});
  CHECK(includes_of(spliced_mixed.facts, "main.cpp", "\"inner.hpp\"").empty());
  CHECK(std::any_of(spliced_mixed.limits.begin(), spliced_mixed.limits.end(), [](const std::string& l) {
    return l.find("not covered by the dependency scanner") != std::string::npos;
  }));

  std::error_code ec;
  fs::remove_all(work, ec);
}

TEST_CASE("three independent runs over the same selection give identical fact hashes") {

  const fs::path work = scratch_dir("determinism");
  const CompilationDatabase db = load_fixture_database(repo_root(), work);
  std::vector<std::string> ids;
  for (const char* rel :
       {"src/shape_area.cpp", "src/shape_scaled.cpp", "src/use.cpp", "src/semantics.cpp", "src/callables.cpp",
        "src/closures.cpp"}) {
    ids.push_back(command_for(db, repo_root(), rel)->command_id);
  }
  ids.push_back(command_for(db, repo_root(), "src/feature.cpp", "/DFEATURE_X")->command_id);
  const UnitSelection sel = select_by_command_ids(db, ids);
  REQUIRE(sel.selected.size() == 7);

  std::vector<Sha256Digest> hashes;
  for (int run = 0; run < 3; ++run) {
    FactSet graph;
    for (const auto& r : analyze_selected(sel, options_for(repo_root()))) {
      REQUIRE(r.status == AnalysisStatus::ok);
      REQUIRE(publish_contribution(graph, r).published);
    }
    hashes.push_back(graph.fact_hash());
    CHECK_FALSE(graph.symbols().empty());
  }
  CHECK(hashes[0] == hashes[1]);
  CHECK(hashes[1] == hashes[2]);
  std::error_code ec;
  fs::remove_all(work, ec);
}

// --- stage 1 + 3 end to end: the real Temppal input shape --------------------
//
// A per-unit response file that names a shared nested one, carrying the exact
// two MSVC argument forms the Temppal command uses. The unit must analyse
// through the unmodified public contract, and every existing gate must still
// fire on what the expansion brings in.
TEST_CASE("native cl.exe response files expand and analyse; gates still run on the expansion") {
  const fs::path work = scratch_dir("rsp");
  const fs::path repo = work / "repo";
  write_file(repo / "inc/shared.h", "int shared_value();\n");
  write_file(repo / "src/a.cpp",
             "#include \"shared.h\"\n"
             "#ifndef FEATURE_ON\n"
             "#error FEATURE_ON must come from the nested response file\n"
             "#endif\n"
             "int a_fn() { return shared_value(); }\n");
  const fs::path a_cpp = repo / "src/a.cpp";
  // The nested file is named without a directory: it resolves against the
  // command directory, which is the measured native cl.exe rule.
  write_file(repo / "rsp/nested.rsp", "/DFEATURE_ON=1 /I \"inc\"\n");
  write_file(repo / "rsp/top.rsp",
             "/c /std:c++20\n"
             "@rsp/nested.rsp\n"
             "/experimental:log \"obj/a.sarif\"\n"
             "/d2ExtendedWarningInfo\n"
             "\"src/a.cpp\"\n");

  SUBCASE("the unit analyses and the replay identity covers the response bytes") {
    const CompileCommand raw = make_command(repo, a_cpp, {"cl.exe", "@rsp/top.rsp"});
    const AnalysisResult r = gate(repo, a_cpp, {"cl.exe", "@rsp/top.rsp"});
    for (const auto& d : r.diagnostics) MESSAGE(d.text());
    REQUIRE(r.status == AnalysisStatus::ok);
    CHECK(r.context_complete);
    CHECK(r.context_status == NormalizationStatus::ok);
    CHECK(r.rejected_arguments.empty());
    CHECK(find_symbol(r.facts, "a_fn", "()") != nullptr);

    REQUIRE(r.expansion.attempted);
    REQUIRE(r.expansion.ok);
    CHECK(r.expansion.files.size() == 2);
    // The raw command identity is untouched; the replay identity is separate.
    CHECK(r.command_id == raw.command_id);
    CHECK(r.replay_id != r.command_id);
    CHECK(r.replay_id == r.expansion.replay_id);
    // Dispositions are over the expanded argv, and each token still names the
    // raw argument and the file it came from.
    CHECK(r.context.dispositions.size() == r.expansion.arguments.size());
    CHECK(r.context.defines == std::vector<std::string>{"FEATURE_ON=1"});
    const auto define_at = std::find(r.expansion.arguments.begin(), r.expansion.arguments.end(), "/DFEATURE_ON=1");
    REQUIRE(define_at != r.expansion.arguments.end());
    const ExpandedTokenOrigin& origin =
        r.expansion.origins[static_cast<std::size_t>(define_at - r.expansion.arguments.begin())];
    CHECK(origin.from_response_file);
    CHECK(origin.raw_index == 1);  // the single `@rsp/top.rsp` argument
    CHECK(r.expansion.files[origin.snapshot].as_written == "rsp/nested.rsp");
    // Neither audited form reached the front end, and no SARIF file was made.
    CHECK(std::find(r.context.analyzer_arguments.begin(), r.context.analyzer_arguments.end(),
                    "/experimental:log") == r.context.analyzer_arguments.end());
    CHECK(std::find(r.context.analyzer_arguments.begin(), r.context.analyzer_arguments.end(),
                    "/d2ExtendedWarningInfo") == r.context.analyzer_arguments.end());
    CHECK_FALSE(fs::exists(repo / "obj/a.sarif"));
  }

  SUBCASE("a changed nested response file changes the replay identity, not the command identity") {
    const AnalysisResult before = gate(repo, a_cpp, {"cl.exe", "@rsp/top.rsp"});
    REQUIRE(before.status == AnalysisStatus::ok);
    write_file(repo / "rsp/nested.rsp", "/DFEATURE_ON=2 /I \"inc\"\n");
    const AnalysisResult after = gate(repo, a_cpp, {"cl.exe", "@rsp/top.rsp"});
    REQUIRE(after.status == AnalysisStatus::ok);
    CHECK(after.command_id == before.command_id);
    CHECK(after.replay_id != before.replay_id);
  }

  SUBCASE("a missing nested file rejects atomically; nothing partially expanded is analysed") {
    write_file(repo / "rsp/broken.rsp", "/c @rsp/absent.rsp \"src/a.cpp\"\n");
    const AnalysisResult r = gate(repo, a_cpp, {"cl.exe", "@rsp/broken.rsp"});
    CHECK(r.status == AnalysisStatus::rejected);
    CHECK_FALSE(r.expansion.ok);
    CHECK(r.expansion.arguments.empty());
    CHECK(r.replay_id == r.command_id);
    // The raw command still carries its `@` token, so the context is degraded
    // for the reason it always was.
    CHECK(r.context_status == NormalizationStatus::degraded);
    CHECK(r.context.unexpanded_response_files == std::vector<std::string>{"rsp/broken.rsp"});
    CHECK(std::any_of(r.diagnostics.begin(), r.diagnostics.end(), [](const AnalysisDiagnostic& d) {
      return d.message.find("absent.rsp") != std::string::npos;
    }));
    CHECK(r.files.empty());
    CHECK(r.facts.symbols().empty());
  }

  SUBCASE("expansion does not smuggle past the safety gate, extra sources or dangerous options") {
    struct Case {
      const char* name;
      const char* body;
    };
    for (const Case& k : {Case{"danger.rsp", "/c /std:c++20 /DFEATURE_ON=1 /I \"inc\" /clang:-load \"src/a.cpp\"\n"},
                          Case{"unknown.rsp", "/c /std:c++20 /DFEATURE_ON=1 /I \"inc\" /Qsomething \"src/a.cpp\"\n"},
                          Case{"second.rsp",
                               "/c /std:c++20 /DFEATURE_ON=1 /I \"inc\" \"src/a.cpp\" \"src/other.cpp\"\n"},
                          Case{"dupe.rsp", "/c /std:c++20 /DFEATURE_ON=1 /I \"inc\" \"src/a.cpp\" \"src/a.cpp\"\n"}}) {
      CAPTURE(k.name);
      write_file(repo / "src/other.cpp", "int other_fn() { return 3; }\n");
      write_file(repo / "rsp" / k.name, k.body);
      const AnalysisResult r = gate(repo, a_cpp, {"cl.exe", std::string("@rsp/") + k.name});
      CHECK(r.expansion.ok);  // expansion itself succeeded...
      CHECK(r.status == AnalysisStatus::rejected);  // ...and the existing gates still refused the unit
      CHECK(r.files.empty());
      CHECK(r.facts.symbols().empty());
    }
  }

  SUBCASE("no expansion is claimed for clang-cl: the same input still rejects unexpanded") {
    const AnalysisResult r = gate(repo, a_cpp, {"clang-cl.exe", "@rsp/top.rsp"});
    CHECK_FALSE(r.expansion.attempted);
    CHECK(r.status == AnalysisStatus::rejected);
    CHECK(r.context_status == NormalizationStatus::degraded);
    CHECK(r.context.unexpanded_response_files == std::vector<std::string>{"rsp/top.rsp"});
    CHECK(r.files.empty());
  }

  std::error_code ec;
  fs::remove_all(work, ec);
}

TEST_CASE("replay identity through the public analyzer path does not depend on the optional command_id") {
  const fs::path work = scratch_dir("rsp-identity");
  const fs::path repo = work / "repo";
  write_file(repo / "src/a.cpp", "int a_fn() { return 1; }\n");
  write_file(repo / "rsp/top.rsp", "/c /std:c++20 \"src/a.cpp\"\n");
  const fs::path a_cpp = repo / "src/a.cpp";

  // The public request contract allows an empty command_id; the analyzer
  // recomputes it. Expansion must use the recomputed identity, not the field.
  CompileCommand populated = make_command(repo, a_cpp, {"cl.exe", "@rsp/top.rsp"});
  CompileCommand empty_id = populated;
  empty_id.command_id.clear();

  auto analyse = [&](const CompileCommand& c) {
    AnalysisRequest req;
    req.command = &c;
    req.repository_root = repo;
    req.resource_dir = resource_dir();
    return analyze_translation_unit(req);
  };
  const AnalysisResult with_id = analyse(populated);
  const AnalysisResult without_id = analyse(empty_id);
  for (const auto& d : with_id.diagnostics) MESSAGE(d.text());

  REQUIRE(with_id.status == AnalysisStatus::ok);
  REQUIRE(without_id.status == AnalysisStatus::ok);
  REQUIRE(with_id.expansion.ok);
  REQUIRE(without_id.expansion.ok);
  CHECK(with_id.command_id == without_id.command_id);
  CHECK(with_id.replay_id == without_id.replay_id);

  // A stale, non-empty, wrong id is still refused: the recomputation inside
  // expansion must not weaken the analyzer's provenance gate.
  CompileCommand stale = populated;
  stale.command_id = "0123456789abcdef0123456789abcdef";
  const AnalysisResult r = analyse(stale);
  CHECK(r.status == AnalysisStatus::rejected);
  CHECK(std::any_of(r.diagnostics.begin(), r.diagnostics.end(), [](const AnalysisDiagnostic& d) {
    return d.message.find("does not match the command's content identity") != std::string::npos;
  }));

  std::error_code ec;
  fs::remove_all(work, ec);
}

// --- stage 2: producer-verified textual PCH replay ---------------------------

namespace {

// Builds a producer/consumer pair around one wrapper header, with a binary
// sidecar present next to the wrapper so the order guard is exercised.
struct PchFixture {
  fs::path work;
  fs::path repo;
  fs::path wrapper;
  fs::path main_cpp;

  explicit PchFixture(const char* name) : work(scratch_dir(name)), repo(work / "repo") {
    wrapper = repo / "pch/Wrapper.h";
    main_cpp = repo / "src/main.cpp";
    write_file(repo / "pch/shared_decl.h",
               "#pragma once\n"
               "struct FromPch { int value; };\n"
               "int pch_only_function();\n");
    write_file(wrapper,
               "#pragma once\n"
               "#define PCH_MACRO 41\n"
               "#include \"shared_decl.h\"\n"
               "#if defined(PCH_MACRO)\n"
               "#define PCH_CONDITIONAL 1\n"
               "#endif\n");
    // The MSVC artifact the consumer /Fp names. It is never consumed; its mere
    // presence beside the wrapper is what makes the Clang driver try to
    // substitute -include-pch, which the order guard prevents.
    write_file(repo / "pch/Wrapper.h.pch", "VCPCH0 not a clang pch at all");
    write_file(repo / "pch/Wrapper.cpp", "#include \"Wrapper.h\"\n");
    // The unit genuinely depends on a declaration, a macro and a conditional
    // include that only the PCH chain provides.
    write_file(main_cpp,
               "#ifndef PCH_CONDITIONAL\n"
               "#error the PCH prefix did not reach this translation unit\n"
               "#endif\n"
               "int uses_pch() { FromPch v{PCH_MACRO}; return v.value + pch_only_function(); }\n");
  }
  ~PchFixture() {
    std::error_code ec;
    fs::remove_all(work, ec);
  }
  PchFixture(const PchFixture&) = delete;
  PchFixture& operator=(const PchFixture&) = delete;

  CompileCommand producer(std::vector<std::string> extra = {}) const {
    std::vector<std::string> argv{"cl.exe", "/c", "/std:c++20", "/EHsc", "/TP"};
    argv.insert(argv.end(), extra.begin(), extra.end());
    argv.push_back("/YcWrapper.h");
    argv.push_back("/Fp" + path_to_utf8_generic(repo / "pch/Wrapper.h.pch"));
    argv.push_back(path_to_utf8_generic(repo / "pch/Wrapper.cpp"));
    return make_command(repo, repo / "pch/Wrapper.cpp", std::move(argv));
  }
  CompileCommand consumer(std::vector<std::string> extra = {}) const {
    std::vector<std::string> argv{"cl.exe", "/c", "/std:c++20", "/EHsc", "/TP"};
    argv.insert(argv.end(), extra.begin(), extra.end());
    argv.push_back("/FI" + path_to_utf8_generic(wrapper));
    argv.push_back("/Yu" + path_to_utf8_generic(wrapper));
    argv.push_back("/Fp" + path_to_utf8_generic(repo / "pch/Wrapper.h.pch"));
    argv.push_back(path_to_utf8_generic(main_cpp));
    return make_command(repo, main_cpp, std::move(argv));
  }
};

AnalysisResult analyze_pair(const fs::path& repo, const CompileCommand& consumer, const CompileCommand* producer) {
  AnalysisRequest req;
  req.command = &consumer;
  req.pch_producer = producer;
  req.repository_root = repo;
  req.resource_dir = resource_dir();
  return analyze_translation_unit(req);
}

}  // namespace

TEST_CASE("textual PCH replay accepts a verified producer and analyses the unit") {
  const PchFixture fx("pch-positive");
  const CompileCommand producer = fx.producer();
  const CompileCommand consumer = fx.consumer();

  SUBCASE("without a producer the unit still rejects") {
    const AnalysisResult r = analyze_pair(fx.repo, consumer, nullptr);
    CHECK(r.status == AnalysisStatus::rejected);
    CHECK(r.pch.mode == PchReplayMode::rejected);
    CHECK(r.pch.reject_reason.find("no producer compile command") != std::string::npos);
    CHECK(r.facts.symbols().empty());
  }

  SUBCASE("with a verified producer the unit analyses through the public contract") {
    const AnalysisResult r = analyze_pair(fx.repo, consumer, &producer);
    for (const auto& d : r.diagnostics) MESSAGE(d.text());
    REQUIRE(r.status == AnalysisStatus::ok);
    CHECK(r.context_complete);
    CHECK(r.rejected_arguments.empty());
    REQUIRE(r.pch.mode == PchReplayMode::textual_snapshot);
    CHECK(r.pch.mismatches.empty());
    CHECK(r.pch.producer.window_seen);
    CHECK(r.pch.consumer.window_seen);
    CHECK(r.pch.producer.events > 0);
    CHECK_FALSE(r.pch.prefix_identity.empty());
    // The guard is virtual: it must not exist on disk anywhere.
    CHECK_FALSE(fs::exists(path_from_utf8(r.pch.guard_path)));
    CHECK_FALSE(fs::exists(fx.repo / "pch" / path_from_utf8(r.pch.guard_path).filename()));
    // The unit really used the PCH-only declaration, macro and conditional.
    CHECK(find_symbol(r.facts, "uses_pch", "()") != nullptr);
    // The binary artifact was never consumed.
    CHECK(r.pch.pch_binary == path_to_utf8_generic(fx.repo / "pch/Wrapper.h.pch"));
  }
}

TEST_CASE("textual PCH replay fails closed on every unsupported shape") {
  const PchFixture fx("pch-negative");

  SUBCASE("a different /Fp artifact") {
    const CompileCommand producer = fx.producer();
    std::vector<std::string> argv{"cl.exe", "/c", "/std:c++20", "/EHsc", "/TP",
                                  "/FI" + path_to_utf8_generic(fx.wrapper),
                                  "/Yu" + path_to_utf8_generic(fx.wrapper),
                                  "/Fp" + path_to_utf8_generic(fx.repo / "pch/Other.pch"),
                                  path_to_utf8_generic(fx.main_cpp)};
    const CompileCommand consumer = make_command(fx.repo, fx.main_cpp, argv);
    const AnalysisResult r = analyze_pair(fx.repo, consumer, &producer);
    CHECK(r.status == AnalysisStatus::rejected);
    CHECK(r.pch.reject_reason.find("different /Fp") != std::string::npos);
  }

  SUBCASE("the /Yu header is not the first forced include") {
    write_file(fx.repo / "pch/Other.h", "#pragma once\n");
    const CompileCommand producer = fx.producer();
    std::vector<std::string> argv{"cl.exe", "/c", "/std:c++20", "/EHsc", "/TP",
                                  "/FI" + path_to_utf8_generic(fx.repo / "pch/Other.h"),
                                  "/FI" + path_to_utf8_generic(fx.wrapper),
                                  "/Yu" + path_to_utf8_generic(fx.wrapper),
                                  "/Fp" + path_to_utf8_generic(fx.repo / "pch/Wrapper.h.pch"),
                                  path_to_utf8_generic(fx.main_cpp)};
    const CompileCommand consumer = make_command(fx.repo, fx.main_cpp, argv);
    const AnalysisResult r = analyze_pair(fx.repo, consumer, &producer);
    CHECK(r.status == AnalysisStatus::rejected);
    CHECK(r.pch.reject_reason.find("first forced include") != std::string::npos);
  }

  SUBCASE("conflicting semantic options between producer and consumer") {
    const CompileCommand producer = fx.producer({"/DPRODUCER_ONLY=1"});
    const CompileCommand consumer = fx.consumer();
    const AnalysisResult r = analyze_pair(fx.repo, consumer, &producer);
    CHECK(r.status == AnalysisStatus::rejected);
    CHECK(r.pch.reject_reason.find("disagree on language, runtime, macro or semantic options") !=
          std::string::npos);
  }

  SUBCASE("a binary /Yu header is never replayed as text") {
    write_file(fx.repo / "pch/Binary.h", "VCPCH0 binary masquerading as a header");
    write_file(fx.repo / "pch/Binary.cpp", "\n");
    std::vector<std::string> pargv{"cl.exe", "/c", "/std:c++20", "/EHsc", "/TP", "/YcBinary.h",
                                   "/Fp" + path_to_utf8_generic(fx.repo / "pch/Binary.pch"),
                                   path_to_utf8_generic(fx.repo / "pch/Binary.cpp")};
    std::vector<std::string> cargv{"cl.exe", "/c", "/std:c++20", "/EHsc", "/TP",
                                   "/FI" + path_to_utf8_generic(fx.repo / "pch/Binary.h"),
                                   "/Yu" + path_to_utf8_generic(fx.repo / "pch/Binary.h"),
                                   "/Fp" + path_to_utf8_generic(fx.repo / "pch/Binary.pch"),
                                   path_to_utf8_generic(fx.main_cpp)};
    const CompileCommand producer = make_command(fx.repo, fx.repo / "pch/Binary.cpp", pargv);
    const CompileCommand consumer = make_command(fx.repo, fx.main_cpp, cargv);
    const AnalysisResult r = analyze_pair(fx.repo, consumer, &producer);
    CHECK(r.status == AnalysisStatus::rejected);
    CHECK(r.pch.reject_reason.find("binary precompiled header") != std::string::npos);
  }
}

TEST_CASE("textual PCH replay rejects a transient macro difference the final state hides") {
  // The counterexample. Producer and consumer differ only in an include
  // directory, which the contract allows. Each resolves <transient.h> to a file
  // that defines a macro, pushes it with #pragma push_macro, and undefines it
  // again. The FINAL macro table, the token stream and the pragma-producing
  // macro definition are identical in both; only the argument the push_macro
  // actually received differs, and only the ordered event digest, which carries
  // expansion arguments, can see it.
  const fs::path work = scratch_dir("pch-transient");
  const fs::path repo = work / "repo";
  write_file(repo / "pch/Wrapper.h",
             "#pragma once\n"
             "#define UE_PUSH_MACRO(name) __pragma(push_macro(name))\n"
             "#define VICTIM_A 1\n"
             "#define VICTIM_B 2\n"
             "#include <transient.h>\n"
             "#define PCH_CONDITIONAL 1\n");
  write_file(repo / "pch/Wrapper.cpp", "#include \"Wrapper.h\"\n");
  write_file(repo / "pch/Wrapper.h.pch", "VCPCH0 sidecar");
  write_file(repo / "p_inc/transient.h", "#define PICK \"VICTIM_A\"\nUE_PUSH_MACRO(PICK)\n#undef PICK\n");
  write_file(repo / "c_inc/transient.h", "#define PICK \"VICTIM_B\"\nUE_PUSH_MACRO(PICK)\n#undef PICK\n");
  write_file(repo / "src/main.cpp",
             "#ifndef PCH_CONDITIONAL\n#error missing prefix\n#endif\nint uses_pch() { return VICTIM_A; }\n");
  const fs::path wrapper = repo / "pch/Wrapper.h";
  const fs::path main_cpp = repo / "src/main.cpp";

  const CompileCommand producer =
      make_command(repo, repo / "pch/Wrapper.cpp",
                   {"cl.exe", "/c", "/std:c++20", "/EHsc", "/TP", "/I" + path_to_utf8_generic(repo / "p_inc"),
                    "/YcWrapper.h", "/Fp" + path_to_utf8_generic(repo / "pch/Wrapper.h.pch"),
                    path_to_utf8_generic(repo / "pch/Wrapper.cpp")});
  auto consumer_for = [&](const char* include_dir) {
    return make_command(repo, main_cpp,
                        {"cl.exe", "/c", "/std:c++20", "/EHsc", "/TP",
                         "/I" + path_to_utf8_generic(repo / include_dir),
                         "/FI" + path_to_utf8_generic(wrapper), "/Yu" + path_to_utf8_generic(wrapper),
                         "/Fp" + path_to_utf8_generic(repo / "pch/Wrapper.h.pch"),
                         path_to_utf8_generic(main_cpp)});
  };

  SUBCASE("the same include directory is accepted") {
    const CompileCommand consumer = consumer_for("p_inc");
    const AnalysisResult r = analyze_pair(repo, consumer, &producer);
    for (const auto& d : r.diagnostics) MESSAGE(d.text());
    REQUIRE(r.pch.mode == PchReplayMode::textual_snapshot);
    CHECK(r.status == AnalysisStatus::ok);
  }
  SUBCASE("a transient difference invisible in the final state is rejected") {
    const CompileCommand consumer = consumer_for("c_inc");
    const AnalysisResult r = analyze_pair(repo, consumer, &producer);
    CHECK(r.status == AnalysisStatus::rejected);
    REQUIRE(r.pch.mode == PchReplayMode::rejected);
    // The macro table at the boundary is identical: PICK is undefined in both
    // and VICTIM_A/VICTIM_B are untouched. Only the event stream differs.
    CHECK(r.pch.producer.macro_digest == r.pch.consumer.macro_digest);
    CHECK(std::find(r.pch.mismatches.begin(), r.pch.mismatches.end(), "preprocessor events") !=
          r.pch.mismatches.end());
  }

  std::error_code ec;
  fs::remove_all(work, ec);
}

// --- stage 2: the required acceptance matrix ---------------------------------
//
// Every case builds its own tree, so nothing depends on prepared scratch inputs.
// The wrapper is always named PLAINLY and found through `/I`, which is the shape
// MSVC and UE actually emit and the shape that must not be rejected.

namespace {

struct PchCase {
  fs::path work;
  fs::path repo;

  explicit PchCase(const char* name) : work(scratch_dir(name)), repo(work / "repo") {}
  ~PchCase() {
    std::error_code ec;
    fs::remove_all(work, ec);
  }
  PchCase(const PchCase&) = delete;
  PchCase& operator=(const PchCase&) = delete;

  void file(const std::string& rel, const std::string& text) const { write_file(repo / rel, text); }

  CompileCommand command(const std::string& source, std::vector<std::string> extra) const {
    std::vector<std::string> argv{"cl.exe", "/c", "/TP", "/std:c++20", "/EHsc"};
    argv.insert(argv.end(), extra.begin(), extra.end());
    argv.push_back(source);
    return make_command(repo, repo / path_from_utf8(source), std::move(argv));
  }
};

AnalysisResult run_pch(const fs::path& repo, const CompileCommand& consumer, const CompileCommand* producer) {
  AnalysisRequest req;
  req.command = &consumer;
  req.pch_producer = producer;
  req.repository_root = repo;
  req.resource_dir = resource_dir();
  return analyze_translation_unit(req);
}

// The standard shape: a wrapper reached through `/I include`, a producer whose
// source is exactly one include of it, and a consumer that needs a declaration,
// a macro and a conditional include the wrapper alone provides.
void write_standard_tree(const PchCase& c, const std::string& wrapper_extra = {}) {
  c.file("include/decl.hpp", "#pragma once\nstruct FromPch { int v; };\nint pch_fn();\n");
  c.file("include/wrapper.hpp",
         "#pragma once\n#define PCH_VALUE 17\n#include \"decl.hpp\"\n"
         "#if defined(PCH_VALUE)\n#define PCH_ON 1\n#endif\n" + wrapper_extra);
  c.file("producer.cpp", "#include \"wrapper.hpp\"\n");
  c.file("out/pch.pch", "VCPCH0 sidecar; never consumed");
  c.file("consumer.cpp",
         "#ifndef PCH_ON\n#error the PCH prefix did not reach this unit\n#endif\n"
         "int uses_pch() { FromPch x{PCH_VALUE}; return x.v + pch_fn(); }\n");
}

std::vector<std::string> standard_producer_args(const char* include_dir = "include") {
  return {std::string("/I") + include_dir, "/Ycwrapper.hpp", "/Fpout/pch.pch"};
}
std::vector<std::string> standard_consumer_args(const char* include_dir = "include") {
  return {std::string("/I") + include_dir, "/FIwrapper.hpp", "/Yuwrapper.hpp", "/Fpout/pch.pch"};
}

}  // namespace

TEST_CASE("PCH matrix: a relative /Yu found through include search is accepted") {
  // Regression for the shape MSVC and UE emit: `/Yu"wrapper.hpp"` resolved by
  // `/I"include"`. Resolving only against the working directory rejects it.
  PchCase c("pch-matching");
  write_standard_tree(c);
  const CompileCommand producer = c.command("producer.cpp", standard_producer_args());
  const CompileCommand consumer = c.command("consumer.cpp", standard_consumer_args());
  const AnalysisResult r = run_pch(c.repo, consumer, &producer);
  for (const auto& d : r.diagnostics) MESSAGE(d.text());
  REQUIRE(r.pch.mode == PchReplayMode::textual_snapshot);
  CHECK(r.status == AnalysisStatus::ok);
  CHECK(r.pch.mismatches.empty());
  CHECK(r.pch.final_drift.empty());
  CHECK(r.pch.final_parse.boundary_reached);
  CHECK(find_symbol(r.facts, "uses_pch", "()") != nullptr);
}

TEST_CASE("PCH matrix: different include lists that resolve the same files are accepted") {
  // The one difference the contract allows. Both commands carry decoy search
  // directories the other does not have; every header they actually resolve is
  // the same file.
  PchCase c("pch-include-equivalent");
  write_standard_tree(c);
  c.file("producer-only/decoy.hpp", "#error decoy must never be reached\n");
  c.file("consumer-only/decoy.hpp", "#error decoy must never be reached\n");
  const CompileCommand producer =
      c.command("producer.cpp", {"/Iproducer-only", "/Iinclude", "/Ycwrapper.hpp", "/Fpout/pch.pch"});
  const CompileCommand consumer = c.command(
      "consumer.cpp", {"/Iconsumer-only", "/Iinclude", "/FIwrapper.hpp", "/Yuwrapper.hpp", "/Fpout/pch.pch"});
  const AnalysisResult r = run_pch(c.repo, consumer, &producer);
  for (const auto& d : r.diagnostics) MESSAGE(d.text());
  REQUIRE(r.pch.mode == PchReplayMode::textual_snapshot);
  CHECK(r.status == AnalysisStatus::ok);
}

TEST_CASE("PCH matrix: the same spelling resolving to different files is rejected") {
  PchCase c("pch-conditional-target");
  write_standard_tree(c, "#include <selected.hpp>\n");
  c.file("p-sel/selected.hpp", "#pragma once\n#define SELECTED 1\n");
  c.file("c-sel/selected.hpp", "#pragma once\n#define SELECTED 2\n");
  const CompileCommand producer =
      c.command("producer.cpp", {"/Iinclude", "/Ip-sel", "/Ycwrapper.hpp", "/Fpout/pch.pch"});
  const CompileCommand consumer =
      c.command("consumer.cpp", {"/Iinclude", "/Ic-sel", "/FIwrapper.hpp", "/Yuwrapper.hpp", "/Fpout/pch.pch"});
  const AnalysisResult r = run_pch(c.repo, consumer, &producer);
  CHECK(r.status == AnalysisStatus::rejected);
  REQUIRE(r.pch.mode == PchReplayMode::rejected);
  CHECK_FALSE(r.pch.mismatches.empty());
}

TEST_CASE("PCH matrix: a counter consumed inside a conditional is caught") {
  // `__COUNTER__` used in an `#if` advances the counter while emitting nothing,
  // so the final macro table is identical and only the counter axis differs.
  PchCase c("pch-counter-if");
  write_standard_tree(c, "#include <counter.hpp>\n");
  c.file("p-cnt/counter.hpp", "#pragma once\n#if __COUNTER__ >= 0\n#define BRANCH 1\n#endif\n");
  c.file("c-cnt/counter.hpp", "#pragma once\n#if 1\n#define BRANCH 1\n#endif\n");
  const CompileCommand producer =
      c.command("producer.cpp", {"/Iinclude", "/Ip-cnt", "/Ycwrapper.hpp", "/Fpout/pch.pch"});
  const CompileCommand consumer =
      c.command("consumer.cpp", {"/Iinclude", "/Ic-cnt", "/FIwrapper.hpp", "/Yuwrapper.hpp", "/Fpout/pch.pch"});
  const AnalysisResult r = run_pch(c.repo, consumer, &producer);
  CHECK(r.status == AnalysisStatus::rejected);
  REQUIRE(r.pch.mode == PchReplayMode::rejected);
  CHECK(std::find(r.pch.mismatches.begin(), r.pch.mismatches.end(), "__COUNTER__") != r.pch.mismatches.end());
}

TEST_CASE("PCH matrix: include-once identity through different spellings is accepted") {
  // The wrapper reaches one header twice by two spellings. They are the same
  // physical file, so `#pragma once` must suppress the second entry in both
  // captures; without hardlink aliasing in the frozen snapshot it would not.
  PchCase c("pch-once-guard");
  write_standard_tree(c, "#include \"decl.hpp\"\n#include <decl.hpp>\n");
  const CompileCommand producer = c.command("producer.cpp", standard_producer_args());
  const CompileCommand consumer = c.command("consumer.cpp", standard_consumer_args());
  const AnalysisResult r = run_pch(c.repo, consumer, &producer);
  for (const auto& d : r.diagnostics) MESSAGE(d.text());
  REQUIRE(r.pch.mode == PchReplayMode::textual_snapshot);
  CHECK(r.status == AnalysisStatus::ok);
}

TEST_CASE("PCH matrix: a second forced include after the wrapper is outside the prefix") {
  // `Definitions.h` is forced AFTER the wrapper. It must not be part of the
  // compared prefix, must not make the capture fail, and must still reach the
  // translation unit.
  PchCase c("pch-post-prefix");
  write_standard_tree(c);
  c.file("include/definitions.hpp", "#pragma once\n#define SECOND_FI 1\n");
  c.file("consumer.cpp",
         "#ifndef PCH_ON\n#error prefix missing\n#endif\n"
         "#ifndef SECOND_FI\n#error second forced include missing\n#endif\n"
         "int uses_pch() { FromPch x{PCH_VALUE}; return x.v + pch_fn(); }\n");
  const CompileCommand producer = c.command("producer.cpp", standard_producer_args());
  const CompileCommand consumer = c.command(
      "consumer.cpp", {"/Iinclude", "/FIwrapper.hpp", "/FIdefinitions.hpp", "/Yuwrapper.hpp", "/Fpout/pch.pch"});
  const AnalysisResult r = run_pch(c.repo, consumer, &producer);
  for (const auto& d : r.diagnostics) MESSAGE(d.text());
  REQUIRE(r.pch.mode == PchReplayMode::textual_snapshot);
  CHECK(r.status == AnalysisStatus::ok);
  CHECK(r.pch.final_drift.empty());
  CHECK(find_symbol(r.facts, "uses_pch", "()") != nullptr);
}

TEST_CASE("PCH matrix: a user-written duplicate forced include survives deduplication") {
  // `/Yu` emulation appends ONE synthetic `/FI` of the wrapper. Only that exact
  // token may be removed: a forced include the user wrote after `/Yu` is a real
  // input and its position must not change.
  PchCase c("pch-duplicate-fi");
  write_standard_tree(c);
  const CompileCommand producer = c.command("producer.cpp", standard_producer_args());
  const CompileCommand consumer = c.command(
      "consumer.cpp", {"/Iinclude", "/FIwrapper.hpp", "/Yuwrapper.hpp", "/FIwrapper.hpp", "/Fpout/pch.pch"});
  const AnalysisResult r = run_pch(c.repo, consumer, &producer);
  for (const auto& d : r.diagnostics) MESSAGE(d.text());
  REQUIRE(r.pch.mode == PchReplayMode::textual_snapshot);
  CHECK(r.status == AnalysisStatus::ok);
  // Two forced includes of the wrapper were written; exactly one synthetic copy
  // is removed, so one written duplicate remains.
  std::size_t written = 0;
  std::size_t replayed = 0;
  for (const auto& argument : r.context.analyzer_arguments) {
    if (argument.rfind("/FI", 0) == 0 && argument.find("wrapper.hpp") != std::string::npos) ++written;
  }
  for (const auto& argument : r.pch.replay_arguments) {
    if (argument.rfind("/FI", 0) == 0 && argument.find("wrapper.hpp") != std::string::npos) ++replayed;
  }
  CHECK(written == 3);   // two the user wrote, plus the one /Yu emulation added
  CHECK(replayed == 2);  // exactly the synthetic one was removed
}

TEST_CASE("PCH matrix: producer shape and safety negatives all fail closed") {
  PchCase c("pch-producer-negatives");
  write_standard_tree(c);
  const CompileCommand consumer = c.command("consumer.cpp", standard_consumer_args());

  SUBCASE("a /Yc naming a different header") {
    c.file("include/other.hpp", "#pragma once\n");
    const CompileCommand producer =
        c.command("producer.cpp", {"/Iinclude", "/Ycother.hpp", "/Fpout/pch.pch"});
    const AnalysisResult r = run_pch(c.repo, consumer, &producer);
    CHECK(r.status == AnalysisStatus::rejected);
    CHECK(r.pch.reject_reason.find("did not build this precompiled header") != std::string::npos);
  }
  SUBCASE("a producer source with a pragma before the wrapper") {
    // Hidden pre-wrapper state: the capture window opens at the wrapper, so this
    // macro-stack mutation would never be compared.
    c.file("producer.cpp", "#pragma push_macro(\"HIDDEN\")\n#include \"wrapper.hpp\"\n");
    const CompileCommand producer = c.command("producer.cpp", standard_producer_args());
    const AnalysisResult r = run_pch(c.repo, consumer, &producer);
    CHECK(r.status == AnalysisStatus::rejected);
    CHECK(r.pch.reject_reason.find("#pragma") != std::string::npos);
  }
  SUBCASE("a producer source with code outside the include") {
    c.file("producer.cpp", "#include \"wrapper.hpp\"\nint extra = 1;\n");
    const CompileCommand producer = c.command("producer.cpp", standard_producer_args());
    const AnalysisResult r = run_pch(c.repo, consumer, &producer);
    CHECK(r.status == AnalysisStatus::rejected);
    CHECK(r.pch.reject_reason.find("outside its single wrapper include") != std::string::npos);
  }
  SUBCASE("a producer that forces includes of its own") {
    const CompileCommand producer =
        c.command("producer.cpp", {"/Iinclude", "/FIdecl.hpp", "/Ycwrapper.hpp", "/Fpout/pch.pch"});
    const AnalysisResult r = run_pch(c.repo, consumer, &producer);
    CHECK(r.status == AnalysisStatus::rejected);
    CHECK(r.pch.reject_reason.find("forced includes") != std::string::npos);
  }
  SUBCASE("two /Yc options") {
    const CompileCommand producer =
        c.command("producer.cpp", {"/Iinclude", "/Ycwrapper.hpp", "/Ycwrapper.hpp", "/Fpout/pch.pch"});
    const AnalysisResult r = run_pch(c.repo, consumer, &producer);
    CHECK(r.status == AnalysisStatus::rejected);
    CHECK(r.pch.reject_reason.find("exactly one /Yc") != std::string::npos);
  }
  SUBCASE("/Y- contradicting the PCH context") {
    const CompileCommand producer =
        c.command("producer.cpp", {"/Iinclude", "/Ycwrapper.hpp", "/Y-", "/Fpout/pch.pch"});
    const AnalysisResult r = run_pch(c.repo, consumer, &producer);
    CHECK(r.status == AnalysisStatus::rejected);
    CHECK(r.pch.reject_reason.find("/Y-") != std::string::npos);
  }
  SUBCASE("a stale producer command_id") {
    CompileCommand producer = c.command("producer.cpp", standard_producer_args());
    producer.command_id = "0123456789abcdef0123456789abcdef";
    const AnalysisResult r = run_pch(c.repo, consumer, &producer);
    CHECK(r.status == AnalysisStatus::rejected);
    CHECK(r.pch.reject_reason.find("command_id") != std::string::npos);
  }
  SUBCASE("an unsafe producer option rejects before any capture runs") {
    const CompileCommand producer =
        c.command("producer.cpp", {"/Iinclude", "/Ycwrapper.hpp", "/Fpout/pch.pch", "/clang:-load"});
    const AnalysisResult r = run_pch(c.repo, consumer, &producer);
    CHECK(r.status == AnalysisStatus::rejected);
    CHECK(r.pch.reject_reason.find("allowlist") != std::string::npos);
    // Nothing was executed: no capture reached a front end.
    CHECK(r.pch.producer.events == 0);
    CHECK(r.pch.consumer.events == 0);
  }
  SUBCASE("a missing producer keeps the unit rejected") {
    const AnalysisResult r = run_pch(c.repo, consumer, nullptr);
    CHECK(r.status == AnalysisStatus::rejected);
    CHECK(r.pch.reject_reason.find("no producer compile command") != std::string::npos);
  }
}

TEST_CASE("PCH matrix: a one-letter semantic option reaches both captures") {
  // `/J` makes plain `char` unsigned. Dropping short options from the captures
  // would validate a prefix that is not the prefix the final parse uses.
  PchCase c("pch-short-option");
  c.file("include/decl.hpp", "#pragma once\nstruct FromPch { int v; };\nint pch_fn();\n");
  c.file("include/wrapper.hpp",
         "#pragma once\n#define PCH_VALUE 17\n#include \"decl.hpp\"\n"
         "#if defined(PCH_VALUE)\n#define PCH_ON 1\n#endif\n"
         "#ifdef _CHAR_UNSIGNED\n#define SAW_J 1\n#endif\n");
  c.file("producer.cpp", "#include \"wrapper.hpp\"\n");
  c.file("out/pch.pch", "VCPCH0 sidecar");
  c.file("consumer.cpp",
         "#ifndef PCH_ON\n#error prefix missing\n#endif\n"
         "#ifndef SAW_J\n#error the capture did not see /J\n#endif\n"
         "int uses_pch() { FromPch x{PCH_VALUE}; return x.v + pch_fn(); }\n");
  const CompileCommand producer = c.command("producer.cpp", {"/J", "/Iinclude", "/Ycwrapper.hpp", "/Fpout/pch.pch"});
  const CompileCommand consumer =
      c.command("consumer.cpp", {"/J", "/Iinclude", "/FIwrapper.hpp", "/Yuwrapper.hpp", "/Fpout/pch.pch"});
  const AnalysisResult r = run_pch(c.repo, consumer, &producer);
  for (const auto& d : r.diagnostics) MESSAGE(d.text());
  REQUIRE(r.pch.mode == PchReplayMode::textual_snapshot);
  CHECK(r.status == AnalysisStatus::ok);
  // The prefix really observed `/J`: the wrapper's `_CHAR_UNSIGNED` branch is
  // part of the compared window, so an omitted `/J` would change its digests.
  CHECK(r.pch.producer.event_digest == r.pch.consumer.event_digest);
}

TEST_CASE("PCH matrix: case-distinct wrapper siblings are not conflated") {
  // The same invariant the response-file snapshot has: on a case-sensitive
  // directory `wrapper.hpp` and `WRAPPER.hpp` are DIFFERENT files. A folded key
  // would treat the decoy as the wrapper, open the prefix window on it, and
  // compare the wrong header.
  PchCase c("pch-case-distinct");
  // The flag only takes on a directory that is still empty, so it is set on a
  // dedicated include directory before anything is written into it.
  const fs::path cs_include = c.work / "cs-include";
  fs::create_directories(cs_include);
  if (!lcm::test_support::try_enable_case_sensitive_directory(cs_include)) {
    MESSAGE("skipped: per-directory case sensitivity is unavailable in this environment");
    return;
  }
  write_file(cs_include / "decl.hpp", "#pragma once\nstruct FromPch { int v; };\nint pch_fn();\n");
  write_file(cs_include / "wrapper.hpp",
             "#pragma once\n#define PCH_VALUE 17\n#include \"decl.hpp\"\n"
             "#if defined(PCH_VALUE)\n#define PCH_ON 1\n#endif\n");
  write_file(cs_include / "WRAPPER.hpp", "#pragma once\n#define WRONG_WRAPPER 1\n");
  c.file("producer.cpp", "#include \"wrapper.hpp\"\n");
  c.file("out/pch.pch", "VCPCH0 sidecar");
  c.file("consumer.cpp",
         "#ifndef PCH_ON\n#error prefix missing\n#endif\n"
         "int uses_pch() { FromPch x{PCH_VALUE}; return x.v + pch_fn(); }\n");
  REQUIRE(fs::exists(cs_include / "wrapper.hpp"));
  REQUIRE(fs::exists(cs_include / "WRAPPER.hpp"));
  REQUIRE(read_file(cs_include / "wrapper.hpp") != read_file(cs_include / "WRAPPER.hpp"));

  const std::string inc = "/I" + path_to_utf8_generic(cs_include);
  const CompileCommand producer = c.command("producer.cpp", {inc, "/Ycwrapper.hpp", "/Fpout/pch.pch"});

  SUBCASE("the real wrapper still resolves and is accepted") {
    const CompileCommand consumer =
        c.command("consumer.cpp", {inc, "/FIwrapper.hpp", "/Yuwrapper.hpp", "/Fpout/pch.pch"});
    const AnalysisResult r = run_pch(c.repo, consumer, &producer);
    for (const auto& d : r.diagnostics) MESSAGE(d.text());
    REQUIRE(r.pch.mode == PchReplayMode::textual_snapshot);
    CHECK(r.status == AnalysisStatus::ok);
    CHECK(r.pch.wrapper_header.find("cs-include/wrapper.hpp") != std::string::npos);
  }
  SUBCASE("a consumer naming the case-distinct sibling does not borrow the producer's PCH") {
    const CompileCommand consumer =
        c.command("consumer.cpp", {inc, "/FIWRAPPER.hpp", "/YuWRAPPER.hpp", "/Fpout/pch.pch"});
    const AnalysisResult r = run_pch(c.repo, consumer, &producer);
    CHECK(r.status == AnalysisStatus::rejected);
    REQUIRE(r.pch.mode == PchReplayMode::rejected);
    CHECK(r.pch.reject_reason.find("did not build this precompiled header") != std::string::npos);
  }
}

TEST_CASE("PCH replay: the frozen snapshot freezes existence answers, not only bytes") {
  // Immutable buffers alone do not make a prefix deterministic. A path that was
  // ABSENT when the prefix was validated can appear before the final parse and
  // flip a `__has_include` or an include search WITHOUT any new file being
  // entered, which a file-membership check cannot see. The snapshot therefore
  // replays existence answers too. Driven directly, because the effect is a
  // property of the snapshot rather than of any one analysis.
  const fs::path work = scratch_dir("frozen-queries");
  fs::create_directories(work);
  const fs::path present = work / "present.h";
  const fs::path appears_later = work / "appears_later.h";
  write_file(present, "#define PRESENT 1\n");

  llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> physical(llvm::vfs::createPhysicalFileSystem().release());
  auto frozen = llvm::makeIntrusiveRefCnt<lcm::analyzer::detail::FrozenFileSystem>(physical);

  REQUIRE(static_cast<bool>(frozen->openFileForRead(path_to_utf8_generic(present))));
  CHECK_FALSE(static_cast<bool>(frozen->status(path_to_utf8_generic(appears_later))));
  frozen->seal();

  // The world changes underneath: a new header appears and a frozen one is
  // rewritten.
  write_file(appears_later, "#define APPEARED 1\n");
  write_file(present, "#define PRESENT 999\n");

  CHECK_FALSE(static_cast<bool>(frozen->status(path_to_utf8_generic(appears_later))));
  CHECK_FALSE(static_cast<bool>(frozen->openFileForRead(path_to_utf8_generic(appears_later))));
  auto reopened = frozen->openFileForRead(path_to_utf8_generic(present));
  REQUIRE(static_cast<bool>(reopened));
  auto buffer = (*reopened)->getBuffer(path_to_utf8_generic(present));
  REQUIRE(static_cast<bool>(buffer));
  const std::string served((*buffer)->getBuffer().data(), (*buffer)->getBuffer().size());
  CHECK(served.find("PRESENT 1") != std::string::npos);
  CHECK(served.find("999") == std::string::npos);

  std::error_code ec;
  fs::remove_all(work, ec);
}

TEST_CASE("PCH replay produces the same facts and spans as an ordinary textual analysis") {
  // The honest baseline. The SAME unit is analysed twice: once through PCH
  // replay, and once as an ordinary textual compile with no `/Yu` and no `/Fp`
  // at all -- a command the product already accepts without any PCH machinery.
  // Every public fact must agree, down to byte spans; the only permitted
  // difference is the synthetic order guard, which is named rather than
  // quietly filtered out.
  PchCase c("pch-vs-baseline");
  c.file("include/decl.hpp",
         "#pragma once\n"
         "struct FromPch { int v; };\n"
         "int pch_fn();\n");
  c.file("include/wrapper.hpp",
         "#pragma once\n"
         "#define PCH_VALUE 17\n"
         "#include \"decl.hpp\"\n"
         "#if defined(PCH_VALUE)\n#define PCH_ON 1\n#endif\n");
  c.file("producer.cpp", "#include \"wrapper.hpp\"\n");
  c.file("out/pch.pch", "VCPCH0 sidecar; never consumed");
  c.file("include/base.hpp",
         "#pragma once\n"
         "struct Alpha { virtual ~Alpha() = default; };\n"
         "struct Beta { virtual ~Beta() = default; };\n");
  c.file("consumer.cpp",
         "#include \"base.hpp\"\n"
         "#ifndef PCH_ON\n#error prefix missing\n#endif\n"
         "struct Derived : public Alpha, public virtual Beta {\n"
         "  int seed();\n"
         "  int run();\n"
         "};\n"
         "int Derived::seed() { return PCH_VALUE; }\n"
         "int Derived::run() { return seed() + pch_fn(); }\n");

  const CompileCommand producer = c.command("producer.cpp", standard_producer_args());
  const CompileCommand replayed = c.command("consumer.cpp", standard_consumer_args());
  // The baseline forces the very same wrapper, but names no PCH at all, so it
  // never enters the replay path.
  const CompileCommand baseline = c.command("consumer.cpp", {"/Iinclude", "/FIwrapper.hpp"});

  const AnalysisResult with_pch = run_pch(c.repo, replayed, &producer);
  const AnalysisResult without_pch = run_pch(c.repo, baseline, nullptr);
  for (const auto& d : with_pch.diagnostics) MESSAGE(d.text());
  for (const auto& d : without_pch.diagnostics) MESSAGE(d.text());

  REQUIRE(with_pch.pch.mode == PchReplayMode::textual_snapshot);
  REQUIRE(with_pch.status == AnalysisStatus::ok);
  REQUIRE(without_pch.pch.mode == PchReplayMode::not_requested);
  REQUIRE(without_pch.status == AnalysisStatus::ok);

  // --- symbols, by qualified name and by definition span --------------------
  const auto symbol_spans = [](const AnalysisResult& r) {
    std::map<std::string, std::string> out;
    for (const auto& [id, symbol] : r.facts.symbols()) {
      for (const SymbolLocation& location : symbol.locations) {
        if (location.role != LocationRole::definition) continue;
        out[symbol.key.canonical_name + "|" + symbol.key.normalized_signature] =
            location.location.file.generic + ":" + std::to_string(location.location.span.begin_offset) + "-" +
            std::to_string(location.location.span.end_offset);
      }
    }
    return out;
  };
  CHECK(symbol_spans(with_pch) == symbol_spans(without_pch));

  // --- direct bases: order, access, virtuality and the evidence span --------
  const auto base_details = [](const AnalysisResult& r) {
    std::vector<std::string> out;
    for (const auto& [key, relation] : r.facts.relations()) {
      if (key.kind != RelationKind::extends) continue;
      const auto source = r.facts.symbols().find(key.source);
      const auto target = r.facts.symbols().find(key.target);
      if (source == r.facts.symbols().end() || target == r.facts.symbols().end()) continue;
      for (const Evidence& e : relation.evidence) {
        if (!e.base) continue;
        out.push_back(source->second.key.canonical_name + " -> " + target->second.key.canonical_name + " #" +
                      std::to_string(e.base->lexical_ordinal) + " " + std::string(to_string(e.base->effective_access)) +
                      (e.base->access_written ? "(written)" : "(implied)") +
                      (e.base->is_virtual ? " virtual" : "") + " @" + e.location.file.generic + ":" +
                      std::to_string(e.location.span.begin_offset) + "-" +
                      std::to_string(e.location.span.end_offset));
      }
    }
    std::sort(out.begin(), out.end());
    return out;
  };
  const std::vector<std::string> replay_bases = base_details(with_pch);
  CHECK(replay_bases == base_details(without_pch));
  CHECK(replay_bases.size() == 2);

  // --- calls, with their spans ----------------------------------------------
  const auto call_details = [](const AnalysisResult& r) {
    std::vector<std::string> out;
    for (const auto& [key, relation] : r.facts.relations()) {
      if (key.kind != RelationKind::calls) continue;
      const auto source = r.facts.symbols().find(key.source);
      const auto target = r.facts.symbols().find(key.target);
      if (source == r.facts.symbols().end() || target == r.facts.symbols().end()) continue;
      for (const Evidence& e : relation.evidence) {
        out.push_back(source->second.key.canonical_name + " -> " + target->second.key.canonical_name + " @" +
                      e.location.file.generic + ":" + std::to_string(e.location.span.begin_offset) + "-" +
                      std::to_string(e.location.span.end_offset));
      }
    }
    std::sort(out.begin(), out.end());
    return out;
  };
  CHECK(call_details(with_pch) == call_details(without_pch));

  // --- direct includes, with their directive spans --------------------------
  const auto include_details = [](const AnalysisResult& r) {
    std::vector<std::string> out;
    for (const DirectIncludeFact& include : r.facts.direct_includes()) {
      out.push_back(include.includer.generic + " " + include.operand_as_written + " @" +
                    std::to_string(include.directive_location.span.begin_offset) + "-" +
                    std::to_string(include.directive_location.span.end_offset));
    }
    std::sort(out.begin(), out.end());
    return out;
  };
  CHECK(include_details(with_pch) == include_details(without_pch));

  // --- the ONLY permitted difference: the synthetic order guard -------------
  // Compared by path identity: the front end reports observations with the
  // separators the platform gave it, which is not a semantic difference.
  const auto observed_files = [](const AnalysisResult& r) {
    std::vector<std::string> out;
    for (const FileObservation& f : r.files) out.push_back(path_key(path_from_utf8(f.path_utf8)));
    std::sort(out.begin(), out.end());
    return out;
  };
  std::vector<std::string> replay_files = observed_files(with_pch);
  const std::vector<std::string> baseline_files = observed_files(without_pch);
  const std::string guard = path_key(path_from_utf8(with_pch.pch.guard_path));
  REQUIRE_FALSE(guard.empty());
  const auto guard_at = std::find(replay_files.begin(), replay_files.end(), guard);
  CHECK(guard_at != replay_files.end());  // the guard is NAMED, not hidden from observations
  if (guard_at != replay_files.end()) replay_files.erase(guard_at);
  for (const std::string& f : replay_files) {
    if (std::find(baseline_files.begin(), baseline_files.end(), f) == baseline_files.end()) {
      MESSAGE("only in replay: " << f);
    }
  }
  for (const std::string& f : baseline_files) {
    if (std::find(replay_files.begin(), replay_files.end(), f) == replay_files.end()) {
      MESSAGE("only in baseline: " << f);
    }
  }
  // Once the one synthetic input is accounted for, the replayed unit observed
  // exactly what an ordinary textual compile of the same unit observed.
  CHECK(replay_files == baseline_files);
}

TEST_CASE("PCH matrix: an UNGUARDED wrapper repeated after a later /FI keeps its order and is accepted") {
  // Repeated forced includes are observable, so the wrapper here has no include
  // guard on purpose. The written order is
  //     /FIwrapper  /Yuwrapper  /FIflip  /FIwrapper
  // and the second wrapper must see what `flip` defined, changing the value the
  // unit asserts on. Only the synthetic `/FI` the `/Yu` emulation appended may
  // be removed.
  //
  // This is also the case that exposed a one-shot bug in the prefix observer:
  // the later wrapper re-entered the window, so post-boundary work was rolled
  // into the prefix measurement and reported as drift.
  PchCase c("pch-unguarded-repeat");
  c.file("include/wrapper.hpp",
         "#ifndef LCM_ORDER_VALUE\n"
         "#define LCM_ORDER_VALUE 1\n"
         "#else\n"
         "#undef LCM_ORDER_VALUE\n"
         "#define LCM_ORDER_VALUE 2\n"
         "#endif\n"
         "#define PCH_ON 1\n");
  c.file("include/flip.hpp", "#pragma once\n#define LCM_FLIPPED 1\n");
  c.file("producer.cpp", "#include \"wrapper.hpp\"\n");
  c.file("out/pch.pch", "VCPCH0 sidecar");
  c.file("consumer.cpp",
         "#ifndef PCH_ON\n#error prefix missing\n#endif\n"
         "#ifndef LCM_FLIPPED\n#error the later forced include was lost\n#endif\n"
         "static_assert(LCM_ORDER_VALUE == 2, \"the repeated wrapper must be observed\");\n"
         "int uses_pch() { return LCM_ORDER_VALUE; }\n");

  const CompileCommand producer = c.command("producer.cpp", {"/Iinclude", "/Ycwrapper.hpp", "/Fpout/pch.pch"});
  const CompileCommand consumer =
      c.command("consumer.cpp", {"/Iinclude", "/FIwrapper.hpp", "/Yuwrapper.hpp", "/FIflip.hpp",
                                 "/FIwrapper.hpp", "/Fpout/pch.pch"});

  const AnalysisResult r = run_pch(c.repo, consumer, &producer);
  for (const auto& d : r.diagnostics) MESSAGE(d.text());
  REQUIRE(r.pch.mode == PchReplayMode::textual_snapshot);
  CHECK(r.pch.final_drift.empty());
  CHECK(r.status == AnalysisStatus::ok);
  CHECK(find_symbol(r.facts, "uses_pch", "()") != nullptr);

  // The order the unit actually saw: guard, wrapper, flip, wrapper.
  std::vector<std::string> forced;
  for (const std::string& argument : r.pch.replay_arguments) {
    if (argument.rfind("/FI", 0) != 0) continue;
    const std::string value = argument.substr(3);
    if (value == r.pch.guard_path) {
      forced.push_back("guard");
    } else if (value.find("wrapper.hpp") != std::string::npos) {
      forced.push_back("wrapper");
    } else if (value.find("flip.hpp") != std::string::npos) {
      forced.push_back("flip");
    }
  }
  CHECK(forced == std::vector<std::string>{"guard", "wrapper", "flip", "wrapper"});
  // The prefix window is one-shot: it covers the FIRST wrapper only, so the
  // second one is ordinary post-prefix work.
  CHECK(r.pch.consumer.boundary_reached);
  CHECK(r.pch.final_parse.boundary_reached);
}

TEST_CASE("PCH matrix: parser-installed pragma handlers are present in the captures too") {
  // The regression for the measured capture-mode gap.
  //
  // `#pragma vtordisp` is registered by the PARSER, not by the preprocessor, and
  // its handler lexes its own arguments THROUGH the preprocessor. So a macro used
  // as its argument expands -- and fires MacroExpands -- only when a parser is
  // present. With preprocess-only captures the producer and consumer would miss
  // that event while the final parse has it, and this unit would be rejected for
  // a difference that is purely an artifact of how the prefix was measured.
  //
  // `#pragma pack` covers the other half: the parser replaces its tokens with a
  // single annotation token, which is why the token digest counts ordinary
  // tokens only. The template is there because `--driver-mode=cl` enables
  // delayed template parsing, which replays cached body tokens.
  PchCase c("pch-parser-aware");
  c.file("include/decl.hpp",
         "#pragma once\n"
         "struct FromPch { int v; };\n"
         "int pch_fn();\n");
  c.file("include/wrapper.hpp",
         "#pragma once\n"
         "#define PCH_VALUE 17\n"
         "#include \"decl.hpp\"\n"
         "#define VTOR_MODE push, 2\n"
         "#pragma vtordisp(VTOR_MODE)\n"
         "struct WithVirtual { virtual ~WithVirtual() = default; };\n"
         "#pragma vtordisp(pop)\n"
         "#pragma pack(push, 4)\n"
         "template <typename T> struct Packed { T v; T get() const { return v; } };\n"
         "#pragma pack(pop)\n"
         "#if defined(PCH_VALUE)\n#define PCH_ON 1\n#endif\n");
  c.file("producer.cpp", "#include \"wrapper.hpp\"\n");
  c.file("out/pch.pch", "VCPCH0 sidecar");
  c.file("consumer.cpp",
         "#ifndef PCH_ON\n#error prefix missing\n#endif\n"
         "int uses_pch() { Packed<int> p{PCH_VALUE}; FromPch f{p.get()}; return f.v + pch_fn(); }\n");

  const CompileCommand producer = c.command("producer.cpp", standard_producer_args());
  const CompileCommand consumer = c.command("consumer.cpp", standard_consumer_args());
  const AnalysisResult r = run_pch(c.repo, consumer, &producer);
  for (const auto& d : r.diagnostics) MESSAGE(d.text());

  REQUIRE(r.pch.mode == PchReplayMode::textual_snapshot);
  CHECK(r.status == AnalysisStatus::ok);
  CHECK(r.pch.mismatches.empty());
  CHECK(r.pch.final_drift.empty());

  // All three measurements are taken the same way, so all three agree -- this is
  // what preprocess-only captures could not deliver.
  REQUIRE(r.pch.producer.boundary_reached);
  REQUIRE(r.pch.consumer.boundary_reached);
  REQUIRE(r.pch.final_parse.boundary_reached);
  CHECK(r.pch.producer.event_digest == r.pch.consumer.event_digest);
  CHECK(r.pch.consumer.event_digest == r.pch.final_parse.event_digest);
  CHECK(r.pch.producer.token_digest == r.pch.consumer.token_digest);
  CHECK(r.pch.consumer.token_digest == r.pch.final_parse.token_digest);
  CHECK(r.pch.producer.macro_digest == r.pch.final_parse.macro_digest);
  CHECK(r.pch.producer.file_digest == r.pch.final_parse.file_digest);
  CHECK(r.pch.producer.counter == r.pch.final_parse.counter);
  // The pragma really is inside the compared window rather than incidental.
  CHECK(r.pch.consumer.events > 0);
  CHECK(find_symbol(r.facts, "uses_pch", "()") != nullptr);
}
