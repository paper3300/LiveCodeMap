#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "lcm/compile_db.hpp"
#include "lcm/source.hpp"
#include "support/case_sensitive_dir.hpp"

using namespace lcm;
namespace fs = std::filesystem;

namespace {

const fs::path& fixture_dir() {
  static const fs::path dir = path_from_utf8(LCM_TEST_FIXTURE_DIR) / "compile_db";
  return dir;
}

std::vector<std::string> args(std::string_view command, CommandSyntax syntax) {
  return tokenize_command_line(command, syntax).arguments;
}

std::size_t count(const std::vector<CompileDbDiagnostic>& diagnostics, DiagnosticSeverity severity) {
  return static_cast<std::size_t>(
      std::count_if(diagnostics.begin(), diagnostics.end(), [&](const auto& d) { return d.severity == severity; }));
}

std::string generic(const fs::path& p) { return path_to_utf8_generic(p); }

}  // namespace

TEST_CASE("Windows command tokenization follows CommandLineToArgv rules") {
  using V = std::vector<std::string>;
  CHECK(args(R"(cl.exe /c "C:\path with space\a.cpp" /DFOO="bar baz")", CommandSyntax::windows) ==
        V{"cl.exe", "/c", R"(C:\path with space\a.cpp)", "/DFOO=bar baz"});
  CHECK(args(R"(a\b\c)", CommandSyntax::windows) == V{R"(a\b\c)"});          // backslashes literal
  CHECK(args(R"(x\\\"y)", CommandSyntax::windows) == V{R"(x\"y)"});           // 2n+1 backslashes + quote
  CHECK(args(R"(x\\"y z")", CommandSyntax::windows) == V{R"(x\y z)"});         // 2n backslashes + quote toggle
  CHECK(args(R"("a""b" c)", CommandSyntax::windows) == V{R"(a"b)", "c"});       // doubled quote inside quotes
  CHECK(args("  cl.exe\t/c   a.cpp  ", CommandSyntax::windows) == V{"cl.exe", "/c", "a.cpp"});
  CHECK(args(R"(-I"C:\inc\" /c)", CommandSyntax::windows) == V{R"(-IC:\inc" /c)"});  // classic trailing-backslash trap
  const auto unterminated = tokenize_command_line(R"(cl.exe "a.cpp)", CommandSyntax::windows);
  CHECK(unterminated.arguments == V{"cl.exe", "a.cpp"});
  CHECK(unterminated.unterminated_quote);
}

TEST_CASE("GNU command tokenization handles single/double quotes and backslash escapes") {
  using V = std::vector<std::string>;
  CHECK(args(R"(g++ -c 'a b.cpp' -DSTR=\"hi\" "-I/inc dir" -o a\ b.o)", CommandSyntax::gnu) ==
        V{"g++", "-c", "a b.cpp", R"(-DSTR="hi")", "-I/inc dir", "-o", "a b.o"});
  CHECK(args(R"("esc\"aped" 'lit\eral')", CommandSyntax::gnu) == V{R"(esc"aped)", R"(lit\eral)"});
  const auto unterminated = tokenize_command_line("g++ 'a.cpp", CommandSyntax::gnu);
  CHECK(unterminated.arguments == V{"g++", "a.cpp"});
  CHECK(unterminated.unterminated_quote);
}

TEST_CASE("auto-detect syntax matches the host like Clang does") {
  const auto tokens = args(R"(a\b "c d")", CommandSyntax::auto_detect);
#ifdef _WIN32
  CHECK(tokens == std::vector<std::string>{R"(a\b)", "c d"});
#else
  CHECK(tokens == std::vector<std::string>{"ab", "c d"});
#endif
}

TEST_CASE("command strings are argv data: shell operators are preserved as plain arguments") {
  const std::string command = R"(cl.exe /c a.cpp && del /q *.cpp | echo $(rm -rf /) ; %COMSPEC% `whoami`)";
  const auto tokens = args(command, CommandSyntax::windows);
  CHECK(std::find(tokens.begin(), tokens.end(), "&&") != tokens.end());
  CHECK(std::find(tokens.begin(), tokens.end(), "del") != tokens.end());
  CHECK(std::find(tokens.begin(), tokens.end(), "$(rm") != tokens.end());
  CHECK(std::find(tokens.begin(), tokens.end(), "%COMSPEC%") != tokens.end());
  CHECK(std::find(tokens.begin(), tokens.end(), "`whoami`") != tokens.end());
  CHECK(tokens.size() == 15);
}

TEST_CASE("entries resolve relative file and output against their directory and keep original spelling") {
  const std::string json = R"([
    {"directory": "C:/proj/build", "file": "../src/a.cpp",
     "arguments": ["cl.exe", "/c", "/I..\\include", "../src/a.cpp"], "output": "obj/a.obj"},
    {"directory": "C:/proj/build", "file": "C:/proj/src/b.cpp",
     "command": "cl.exe /c \"C:/proj/src/b.cpp\" /DNAME=\"a b\""}
  ])";
  CompileDbParseOptions options;
  options.syntax = CommandSyntax::windows;
  const auto result = parse_compilation_database(json, options);
  REQUIRE(result.database);
  CHECK(result.diagnostics.empty());
  const auto& commands = result.database->commands;
  REQUIRE(commands.size() == 2);

  const auto& a = commands[0];
  CHECK(a.entry_index == 0);
  CHECK(generic(a.directory) == "C:/proj/build");
  CHECK(generic(a.file) == "C:/proj/src/a.cpp");
  CHECK(a.file_as_written == "../src/a.cpp");
  REQUIRE(a.output);
  CHECK(generic(*a.output) == "C:/proj/build/obj/a.obj");
  CHECK(a.output_as_written == "obj/a.obj");
  CHECK(a.arguments == std::vector<std::string>{"cl.exe", "/c", "/I..\\include", "../src/a.cpp"});  // untouched
  CHECK_FALSE(a.command_as_written);
  CHECK(a.command_id.size() == 32);

  const auto& b = commands[1];
  CHECK(generic(b.file) == "C:/proj/src/b.cpp");
  REQUIRE(b.command_as_written);
  CHECK(*b.command_as_written == "cl.exe /c \"C:/proj/src/b.cpp\" /DNAME=\"a b\"");
  CHECK(b.arguments == std::vector<std::string>{"cl.exe", "/c", "C:/proj/src/b.cpp", "/DNAME=a b"});
  CHECK_FALSE(b.output);
  CHECK(b.command_id != a.command_id);
}

TEST_CASE("UTF-8 directory and file spellings survive resolution") {
  const std::string json = R"([
    {"directory": "C:/프로젝트/build", "file": "../소스/파일.cpp",
     "arguments": ["cl.exe", "/c", "../소스/파일.cpp", "/D", "이름=값"]}
  ])";
  const auto result = parse_compilation_database(json, {});
  REQUIRE(result.database);
  REQUIRE(result.database->commands.size() == 1);
  const auto& c = result.database->commands.front();
  CHECK(generic(c.file) == "C:/프로젝트/소스/파일.cpp");
  CHECK(generic(c.directory) == "C:/프로젝트/build");
  CHECK(c.arguments.back() == "이름=값");
  CHECK(result.database->commands_for_file(path_from_utf8("C:/프로젝트/소스/파일.cpp")).size() == 1);
  CHECK(result.database->commands_for_file(path_from_utf8("../소스/파일.cpp"), c.directory).size() == 1);
}

TEST_CASE("ambiguous commands are reported, exact duplicates are not") {
  const auto result = load_compilation_database(fixture_dir() / "ambiguous.json", CommandSyntax::windows);
  REQUIRE(result.database);
  CHECK(count(result.diagnostics, DiagnosticSeverity::error) == 0);
  const auto& db = *result.database;
  REQUIRE(db.commands.size() == 6);

  SUBCASE("two different define contexts for a.cpp are ambiguous and both are returned") {
    const auto selection = db.select_unique(path_from_utf8("C:/proj/src/a.cpp"));
    CHECK(selection.kind == CommandSelection::Kind::ambiguous);
    CHECK(selection.command == nullptr);
    REQUIRE(selection.candidates.size() == 2);
    CHECK(selection.candidates[0]->arguments[2] == "/DPROFILE_A");
    CHECK(selection.candidates[1]->arguments[2] == "/DPROFILE_B");
    CHECK(selection.candidates[0]->command_id != selection.candidates[1]->command_id);
  }

  SUBCASE("identical argv reached via `command` and `arguments` with different path spelling is one identity") {
    const auto selection = db.select_unique(path_from_utf8("C:/proj/src/b.cpp"));
    CHECK(selection.kind == CommandSelection::Kind::unique);
    REQUIRE(selection.command);
    CHECK(selection.candidates.size() == 2);
    CHECK(selection.candidates[0]->command_id == selection.candidates[1]->command_id);
    CHECK(selection.command->entry_index == 2);
    // Lookup by the alternative spelling finds the same commands on Windows.
#ifdef _WIN32
    CHECK(db.commands_for_file(path_from_utf8("c:\\PROJ\\SRC\\b.cpp")).size() == 2);
#endif
  }

  SUBCASE("exact duplicate entries for c.cpp are unique") {
    const auto selection = db.select_unique(path_from_utf8("C:/proj/src/c.cpp"));
    CHECK(selection.kind == CommandSelection::Kind::unique);
    CHECK(selection.candidates.size() == 2);
  }

  SUBCASE("unknown file") {
    const auto selection = db.select_unique(path_from_utf8("C:/proj/src/none.cpp"));
    CHECK(selection.kind == CommandSelection::Kind::none);
    CHECK(selection.command == nullptr);
    CHECK(selection.candidates.empty());
  }
}

TEST_CASE("relative directory is resolved against the database location with a warning") {
  const fs::path db_path = fixture_dir() / "relative_directory.json";
  const auto result = load_compilation_database(db_path, CommandSyntax::gnu);
  REQUIRE(result.database);
  REQUIRE(result.database->commands.size() == 1);
  CHECK(count(result.diagnostics, DiagnosticSeverity::warning) == 1);
  CHECK(count(result.diagnostics, DiagnosticSeverity::error) == 0);
  const auto& c = result.database->commands.front();
  CHECK(path_key(c.directory) == path_key(fixture_dir() / "build"));
  CHECK(path_key(c.file) == path_key(fixture_dir() / "src" / "main.cpp"));
  REQUIRE(c.output);
  CHECK(path_key(*c.output) == path_key(fixture_dir() / "build" / "main.o"));

  SUBCASE("without a known database location the entry is rejected, not guessed") {
    const std::string json = R"([{"directory": "build", "file": "a.cpp", "arguments": ["cc", "a.cpp"]}])";
    const auto parsed = parse_compilation_database(json, {});
    REQUIRE(parsed.database);
    CHECK(parsed.database->commands.empty());
    CHECK(count(parsed.diagnostics, DiagnosticSeverity::error) == 1);
    CHECK(parsed.diagnostics.front().entry_index == 0);
  }
}

TEST_CASE("malformed entries are skipped individually with indexed diagnostics") {
  const auto result = load_compilation_database(fixture_dir() / "malformed_entries.json", CommandSyntax::windows);
  REQUIRE(result.database);
  const auto& commands = result.database->commands;
  REQUIRE(commands.size() == 2);
  CHECK(commands[0].file_as_written == "ok.cpp");
  CHECK(commands[1].file_as_written == "both.cpp");
  // `arguments` wins when both are present; the command text is kept as written.
  CHECK(commands[1].arguments == std::vector<std::string>{"cl.exe", "/c", "both.cpp"});
  REQUIRE(commands[1].command_as_written);
  CHECK(*commands[1].command_as_written == "cl.exe /c both.cpp /DIGNORED");

  CHECK(count(result.diagnostics, DiagnosticSeverity::error) == 4);
  CHECK(count(result.diagnostics, DiagnosticSeverity::warning) == 1);
  std::vector<std::size_t> error_indices;
  for (const auto& d : result.diagnostics) {
    if (d.severity == DiagnosticSeverity::error) error_indices.push_back(*d.entry_index);
  }
  CHECK(error_indices == std::vector<std::size_t>{1, 2, 3, 4});
}

TEST_CASE("empty compiler argument and malformed optional fields reject the entry with a diagnostic") {
  const std::string json = R"([
    {"directory": "C:/proj", "file": "a.cpp", "arguments": ["", "/c", "a.cpp"]},
    {"directory": "C:/proj", "file": "b.cpp", "command": ""},
    {"directory": "C:/proj", "file": "c.cpp", "command": "   "},
    {"directory": "C:/proj", "file": "d.cpp", "arguments": ["cl.exe", "/c", "d.cpp"], "output": 42},
    {"directory": "C:/proj", "file": "e.cpp", "arguments": ["cl.exe", "/c", "e.cpp"], "output": ""},
    {"directory": "", "file": "f.cpp", "arguments": ["cl.exe", "/c", "f.cpp"]},
    {"directory": "C:/proj", "file": "", "arguments": ["cl.exe", "/c", "g.cpp"]},
    {"directory": "C:/proj", "file": "ok.cpp", "arguments": ["cl.exe", "/c", "ok.cpp"], "output": "ok.obj"}
  ])";
  CompileDbParseOptions options;
  options.syntax = CommandSyntax::windows;
  const auto result = parse_compilation_database(json, options);
  REQUIRE(result.database);
  REQUIRE(result.database->commands.size() == 1);
  CHECK(result.database->commands.front().file_as_written == "ok.cpp");
  CHECK(count(result.diagnostics, DiagnosticSeverity::error) == 7);
  std::vector<std::size_t> indices;
  for (const auto& d : result.diagnostics) indices.push_back(*d.entry_index);
  CHECK(indices == std::vector<std::size_t>{0, 1, 2, 3, 4, 5, 6});
}

TEST_CASE("lookup falls back to filesystem identity for existing files with non-ASCII case differences") {
  const fs::path root = fs::temp_directory_path() /
                        path_from_utf8("lcm-cdb-Ünïcode-" +
                                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directories(root / path_from_utf8("Ünïcode"));
  const fs::path file = root / path_from_utf8("Ünïcode/файл.cpp");
  {
    std::ofstream out(file, std::ios::binary);
    out << "int x;\n";
  }
  REQUIRE(fs::exists(file));

  CompilationDatabase db;
  CompileCommand c;
  c.directory = root;
  c.file = file;
  c.arguments = {"cl.exe", "/c", "Ünïcode/файл.cpp"};
  c.command_id = "0123456789abcdef0123456789abcdef";
  db.commands.push_back(c);

  const fs::path other_case = root / path_from_utf8("ÜNÏCODE/ФАЙЛ.CPP");
  CHECK(path_key(other_case) != path_key(file));  // spelling alone does not match
#ifdef _WIN32
  CHECK(db.commands_for_file(other_case).size() == 1);  // NTFS says it is the same file
  CHECK(db.select_unique(other_case).kind == CommandSelection::Kind::unique);
#endif
  // A different, non-existing spelling is not conflated with anything.
  CHECK(db.commands_for_file(root / path_from_utf8("Ünïcode/другой.cpp")).empty());

  std::error_code ec;
  fs::remove_all(root, ec);
}

#ifdef _WIN32
TEST_CASE("existing case-distinct a.cpp and A.cpp select their own commands; aliases and profile ambiguity preserved") {
  const fs::path root = fs::temp_directory_path() /
                        path_from_utf8("lcm-cdb-case-" +
                                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directories(root);
  if (!lcm::test_support::try_enable_case_sensitive_directory(root)) {
    MESSAGE("LIMITATION: per-directory case sensitivity unavailable; case-distinct selection regression not executed");
    std::error_code ec;
    fs::remove_all(root, ec);
    return;
  }
  {
    std::ofstream l(root / "a.cpp", std::ios::binary);
    l << "int lower;\n";
    std::ofstream u(root / "A.cpp", std::ios::binary);
    u << "int upper;\n";
  }
  REQUIRE_FALSE(same_existing_file(root / "a.cpp", root / "A.cpp"));

  CompilationDatabase db;
  CompileCommand lower;
  lower.directory = root;
  lower.file = root / "a.cpp";
  lower.arguments = {"cl.exe", "/c", "a.cpp"};
  lower.command_id = compute_command_id(lower);
  CompileCommand upper = lower;
  upper.file = root / "A.cpp";
  upper.arguments = {"cl.exe", "/c", "A.cpp"};
  upper.command_id = compute_command_id(upper);
  CompileCommand upper_profile2 = upper;
  upper_profile2.arguments = {"cl.exe", "/c", "/DPROFILE2", "A.cpp"};
  upper_profile2.command_id = compute_command_id(upper_profile2);
  db.commands = {lower, upper, upper_profile2};

  const auto lower_sel = db.select_unique(root / "a.cpp");
  CHECK(lower_sel.kind == CommandSelection::Kind::unique);
  REQUIRE(lower_sel.candidates.size() == 1);
  CHECK(lower_sel.candidates.front()->arguments.back() == "a.cpp");

  // Case-distinct sibling directories with identical arguments are different commands.
  const fs::path case_root = root / "CaseRoot";
  const fs::path case_sibling = root / "caseroot";
  fs::create_directories(case_root);
  fs::create_directories(case_sibling);
  REQUIRE_FALSE(same_existing_file(case_root, case_sibling));
  {
    std::ofstream a(case_root / "a.cpp", std::ios::binary);
    a << "int one;\n";
    std::ofstream b(case_sibling / "a.cpp", std::ios::binary);
    b << "int two;\n";
  }
  CompileCommand in_root;
  in_root.directory = case_root;
  in_root.file = case_root / "a.cpp";
  in_root.arguments = {"cl.exe", "/c", "a.cpp"};
  CompileCommand in_sibling = in_root;
  in_sibling.directory = case_sibling;
  in_sibling.file = case_sibling / "a.cpp";
  CHECK(compute_command_id(in_root) != compute_command_id(in_sibling));
  in_root.command_id = compute_command_id(in_root);
  in_sibling.command_id = compute_command_id(in_sibling);
  CompilationDatabase siblings;
  siblings.commands = {in_root, in_sibling};
  CHECK(siblings.select_unique(case_root / "a.cpp").candidates.size() == 1);
  CHECK(siblings.select_unique(case_sibling / "a.cpp").candidates.size() == 1);
  // The legitimately ambiguous file keeps its two profiles, and only those.
  const auto upper_sel = db.select_unique(root / "A.cpp");
  CHECK(upper_sel.kind == CommandSelection::Kind::ambiguous);
  CHECK(upper_sel.candidates.size() == 2);
  // A non-existing spelling falls back to the lexical key (documented).
  CHECK(db.commands_for_file(root / "missing.cpp").empty());

  std::error_code ec;
  fs::remove_all(root, ec);
}
#endif

TEST_CASE("loading a database by relative path still yields absolute command paths") {
  const fs::path previous = fs::current_path();
  fs::current_path(fixture_dir());
  const auto result = load_compilation_database(path_from_utf8("relative_directory.json"), CommandSyntax::gnu);
  fs::current_path(previous);
  REQUIRE(result.database);
  REQUIRE(result.database->commands.size() == 1);
  const auto& c = result.database->commands.front();
  CHECK(c.directory.is_absolute());
  CHECK(c.file.is_absolute());
  CHECK(path_key(c.directory) == path_key(fixture_dir() / "build"));
  CHECK(path_key(c.file) == path_key(fixture_dir() / "src" / "main.cpp"));
}

TEST_CASE("an unterminated command quote rejects that entry and cannot merge with a valid neighbour") {
  CompileDbParseOptions options;
  options.syntax = CommandSyntax::windows;
  const auto result = parse_compilation_database(
      R"([{"directory":"C:/p","file":"a.cpp","command":"cl.exe /c \"a.cpp"},
          {"directory":"C:/p","file":"a.cpp","command":"cl.exe /c a.cpp"},
          {"directory":"C:/p","file":"b.cpp","arguments":["cl.exe","/c","b.cpp"]}])",
      options);
  REQUIRE(result.database);
  REQUIRE(result.database->commands.size() == 2);
  CHECK(result.database->commands[0].entry_index == 1);
  CHECK(result.database->commands[1].entry_index == 2);
  REQUIRE(result.diagnostics.size() == 1);
  CHECK(result.diagnostics.front().severity == DiagnosticSeverity::error);
  CHECK(result.diagnostics.front().entry_index == 0);
  CHECK(result.diagnostics.front().message.find("unterminated quote") != std::string::npos);

  const auto selection = result.database->select_unique(path_from_utf8("C:/p/a.cpp"));
  CHECK(selection.kind == CommandSelection::Kind::unique);
  CHECK(selection.candidates.size() == 1);
  CHECK(selection.command->entry_index == 1);
}

TEST_CASE("direct parse API rejects a relative database base instead of returning relative paths") {
  CompileDbParseOptions options;
  options.database_directory = path_from_utf8("db");
  const auto result = parse_compilation_database(
      R"([{"directory":"build","file":"a.cpp","arguments":["cl.exe","a.cpp"]}])", options);
  CHECK_FALSE(result.database);
  REQUIRE(result.diagnostics.size() == 1);
  CHECK(result.diagnostics.front().severity == DiagnosticSeverity::error);
  CHECK_FALSE(result.diagnostics.front().entry_index);
  CHECK(result.diagnostics.front().message.find("absolute") != std::string::npos);

  // An absolute base produces absolute command paths.
  options.database_directory = path_from_utf8("C:/db");
  const auto ok = parse_compilation_database(
      R"([{"directory":"build","file":"a.cpp","arguments":["cl.exe","a.cpp"]}])", options);
  REQUIRE(ok.database);
  REQUIRE(ok.database->commands.size() == 1);
  CHECK(ok.database->commands.front().directory.is_absolute());
  CHECK(generic(ok.database->commands.front().file) == "C:/db/build/a.cpp");
}

TEST_CASE("unusable documents yield no database") {
  SUBCASE("not JSON") {
    const auto result = parse_compilation_database("this is not json", {});
    CHECK_FALSE(result.database);
    REQUIRE(result.diagnostics.size() == 1);
    CHECK_FALSE(result.diagnostics.front().entry_index);
  }
  SUBCASE("object instead of array") {
    const auto result = load_compilation_database(fixture_dir() / "not_an_array.json");
    CHECK_FALSE(result.database);
    CHECK(result.diagnostics.size() == 1);
  }
  SUBCASE("missing file") {
    const auto result = load_compilation_database(fixture_dir() / "does_not_exist.json");
    CHECK_FALSE(result.database);
    CHECK(result.diagnostics.size() == 1);
  }
  SUBCASE("UTF-8 BOM is tolerated") {
    const auto result = parse_compilation_database("\xEF\xBB\xBF[]", {});
    REQUIRE(result.database);
    CHECK(result.database->commands.empty());
  }
}

TEST_CASE("command identity: aliases of one existing file agree, argument differences do not") {
  // Real existing paths: the fixture directory spelled with different ASCII case is the same object
  // on the default case-insensitive volume, so identity uses the filesystem's canonical spelling.
  const fs::path real = fixture_dir() / "ambiguous.json";
  REQUIRE(fs::exists(real));
  std::string upper = path_to_utf8_generic(real);
  for (char& c : upper) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  }
  const fs::path alias = path_from_utf8(upper);
#ifdef _WIN32
  REQUIRE(same_existing_file(real, alias));
  CompileCommand a;
  a.directory = fixture_dir();
  a.file = real;
  a.arguments = {"cl.exe", "/c", "x.cpp"};
  CompileCommand b = a;
  b.directory = alias.parent_path();
  b.file = alias;
  CHECK(compute_command_id(a) == compute_command_id(b));
  b.arguments.push_back("/DX");
  CHECK(compute_command_id(a) != compute_command_id(b));
#endif
}

TEST_CASE("output identity is case-preserving, lexical and independent of artifact existence") {
  // Ordinary (case-insensitive) temp directory: the artifact may or may not exist.
  const fs::path root = fs::temp_directory_path() /
                        path_from_utf8("lcm-cdb-out-" +
                                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directories(root);
  {
    std::ofstream src(root / "a.cpp", std::ios::binary);
    src << "int a;\n";
  }
  CompileCommand base;
  base.directory = root;
  base.file = root / "a.cpp";
  base.arguments = {"cl.exe", "/c", "a.cpp"};
  base.output = root / "out.obj";
  const std::string before = compute_command_id(base);
  {
    std::ofstream obj(root / "out.obj", std::ios::binary);
    obj << "artifact";
  }
  REQUIRE(fs::exists(root / "out.obj"));
  CHECK(compute_command_id(base) == before);  // creation does not change identity
  fs::remove(root / "out.obj");
  CHECK(compute_command_id(base) == before);  // deletion does not change identity

  // Dot/separator variants of one output normalize to the same identity.
  for (const char* variant : {"./out.obj", "sub/../out.obj", "obj/./../out.obj", ".\\out.obj", "sub\\..\\out.obj"}) {
    CompileCommand same = base;
    same.output = root / path_from_utf8(variant);
    CHECK_MESSAGE(compute_command_id(same) == before, variant);
  }
  // Output spelling matters: a different artifact is a different command; no output at all differs too.
  CompileCommand other = base;
  other.output = root / "other.obj";
  CHECK(compute_command_id(other) != before);
  CompileCommand none = base;
  none.output.reset();
  CHECK(compute_command_id(none) != before);
  // Conservative rule (documented): a case alias of the output is kept as a distinct identity even on
  // this case-insensitive volume, because the volume's rule cannot be asked for an artifact that may not
  // exist yet. Such entries surface as an ambiguity, never as a silent merge.
  CompileCommand alias = base;
  alias.output = root / "Out.obj";
  CHECK(compute_command_id(alias) != before);
  CompilationDatabase db;
  base.command_id = before;
  alias.command_id = compute_command_id(alias);
  db.commands = {base, alias};
  const auto sel = db.select_unique(root / "a.cpp");
  CHECK(sel.kind == CommandSelection::Kind::ambiguous);
  CHECK(sel.candidates.size() == 2);

#ifdef _WIN32
  // Real case-sensitive directory: out.obj and Out.obj are two artifacts. Identities stay distinct and
  // stable before creation, while both exist, and after deletion.
  const fs::path cs_root = root / "cs";
  fs::create_directories(cs_root);
  if (lcm::test_support::try_enable_case_sensitive_directory(cs_root)) {
    CompileCommand lower = base;
    lower.output = cs_root / "out.obj";
    CompileCommand upper = base;
    upper.output = cs_root / "Out.obj";
    const std::string lower_id = compute_command_id(lower);
    const std::string upper_id = compute_command_id(upper);
    CHECK(lower_id != upper_id);
    {
      std::ofstream l(cs_root / "out.obj", std::ios::binary);
      l << "lower";
      std::ofstream u(cs_root / "Out.obj", std::ios::binary);
      u << "upper";
    }
    REQUIRE_FALSE(same_existing_file(cs_root / "out.obj", cs_root / "Out.obj"));
    CHECK(compute_command_id(lower) == lower_id);
    CHECK(compute_command_id(upper) == upper_id);
    fs::remove(cs_root / "out.obj");
    fs::remove(cs_root / "Out.obj");
    CHECK(compute_command_id(lower) == lower_id);
    CHECK(compute_command_id(upper) == upper_id);
    lower.command_id = lower_id;
    upper.command_id = upper_id;
    CompilationDatabase cs_db;
    cs_db.commands = {lower, upper};
    const auto cs_sel = cs_db.select_unique(root / "a.cpp");
    CHECK(cs_sel.kind == CommandSelection::Kind::ambiguous);  // never silently the first one
    CHECK(cs_sel.candidates.size() == 2);
  } else {
    MESSAGE("LIMITATION: per-directory case sensitivity unavailable; case-sensitive output regression not executed");
  }
#endif

  std::error_code ec;
  fs::remove_all(root, ec);
}

TEST_CASE("command identity ignores path spelling differences but not argument differences") {
  const std::string json = R"([
    {"directory": "C:/proj", "file": "a.cpp", "arguments": ["cl.exe", "/c", "a.cpp"]},
    {"directory": "c:\\PROJ\\", "file": "A.CPP", "arguments": ["cl.exe", "/c", "a.cpp"]},
    {"directory": "C:/proj", "file": "a.cpp", "arguments": ["cl.exe", "/c", "a.cpp", "/O2"]},
    {"directory": "C:/proj", "file": "a.cpp", "arguments": ["cl.exe", "/c", "a.cpp"], "output": "x.obj"}
  ])";
  const auto result = parse_compilation_database(json, {});
  REQUIRE(result.database);
  const auto& c = result.database->commands;
  REQUIRE(c.size() == 4);
#ifdef _WIN32
  CHECK(c[0].command_id == c[1].command_id);
#endif
  CHECK(c[0].command_id != c[2].command_id);
  CHECK(c[0].command_id != c[3].command_id);
}
