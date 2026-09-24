// Stage 1: response-file expansion for native cl.exe.
//
// The driver rules asserted here were established by an independent native
// cl.exe oracle on this environment: nested references resolve against the
// process working directory, repeated non-cyclic references are legal, Windows
// quoting round-trips, and the encodings that behave correctly are ASCII
// without a BOM, UTF-8 WITH a BOM and UTF-16LE WITH a BOM.
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "lcm/compile_db.hpp"
#include "lcm/hash.hpp"
#include "lcm/response_file.hpp"
#include "lcm/source.hpp"

#include "support/case_sensitive_dir.hpp"

using namespace lcm;
namespace fs = std::filesystem;

namespace {

int next_temp_id() {
  static int n = 0;
  return n++;
}

// One temporary directory per test case, removed on scope exit.
class TempDir {
 public:
  explicit TempDir(const std::string& name)
      : path_(fs::temp_directory_path() / ("lcm_rsp_" + name + "_" + std::to_string(next_temp_id()))) {
    fs::remove_all(path_);
    fs::create_directories(path_);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path_, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const fs::path& path() const { return path_; }

  // Writes exact bytes; no encoding conversion, no trailing newline.
  void write(const std::string& relative, std::string_view bytes) const {
    const fs::path target = path_ / path_from_utf8(relative);
    fs::create_directories(target.parent_path());
    std::ofstream out(target, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }

 private:
  fs::path path_;
};

std::string why(const ResponseExpansion& e) { return e.errors.empty() ? std::string("(no error recorded)") : e.errors.front(); }

CompileCommand command_for(const fs::path& directory, std::vector<std::string> argv,
                           const std::string& file = "main.cpp") {
  CompileCommand c;
  c.directory = directory.lexically_normal();
  c.file = (directory / path_from_utf8(file)).lexically_normal();
  c.directory_as_written = path_to_utf8_generic(c.directory);
  c.file_as_written = file;
  c.arguments = std::move(argv);
  c.command_id = compute_command_id(c);
  return c;
}

// UTF-16LE bytes (no BOM) for an ASCII-or-BMP string given as UTF-8.
std::string utf16le_of(std::string_view utf8) {
  std::string out;
  std::size_t i = 0;
  while (i < utf8.size()) {
    const auto b0 = static_cast<unsigned char>(utf8[i]);
    std::uint32_t cp = 0;
    std::size_t length = 1;
    if (b0 < 0x80) {
      cp = b0;
    } else if ((b0 & 0xE0) == 0xC0) {
      length = 2;
      cp = b0 & 0x1Fu;
    } else if ((b0 & 0xF0) == 0xE0) {
      length = 3;
      cp = b0 & 0x0Fu;
    } else {
      length = 4;
      cp = b0 & 0x07u;
    }
    for (std::size_t k = 1; k < length; ++k) cp = (cp << 6) | (static_cast<unsigned char>(utf8[i + k]) & 0x3Fu);
    i += length;
    if (cp < 0x10000) {
      out.push_back(static_cast<char>(cp & 0xFF));
      out.push_back(static_cast<char>((cp >> 8) & 0xFF));
    } else {
      const std::uint32_t v = cp - 0x10000;
      const std::uint32_t hi = 0xD800 + (v >> 10);
      const std::uint32_t lo = 0xDC00 + (v & 0x3FF);
      out.push_back(static_cast<char>(hi & 0xFF));
      out.push_back(static_cast<char>((hi >> 8) & 0xFF));
      out.push_back(static_cast<char>(lo & 0xFF));
      out.push_back(static_cast<char>((lo >> 8) & 0xFF));
    }
  }
  return out;
}

const std::string kUtf8Bom = "\xEF\xBB\xBF";
const std::string kUtf16LeBom = std::string("\xFF\xFE", 2);

}  // namespace

TEST_CASE("no response token or an unverified driver means no expansion at all") {
  TempDir dir("noop");
  dir.write("a.rsp", "/DX");

  const CompileCommand plain = command_for(dir.path(), {"cl.exe", "/c", "main.cpp"});
  const ResponseExpansion none = expand_response_files(plain);
  CHECK_FALSE(none.attempted);
  CHECK(none.arguments.empty());
  CHECK(none.errors.empty());

  // Expansion is enabled for the one driver whose behaviour was measured.
  // Every other compiler keeps the previous contract: the `@` token survives
  // into normalization, which degrades the context and rejects the unit.
  for (const char* compiler : {"clang-cl.exe", "clang++", "gcc", "unknown-cc"}) {
    CAPTURE(compiler);
    const ResponseExpansion other = expand_response_files(command_for(dir.path(), {compiler, "@a.rsp", "main.cpp"}));
    CHECK_FALSE(other.attempted);
    CHECK(other.arguments.empty());
  }
}

TEST_CASE("nested references resolve against the command directory, not the naming file") {
  TempDir dir("nested");
  // sub/inner.rsp names `top.rsp` with no directory. Resolving against the
  // naming file would look in sub/ and fail; the native cl.exe rule resolves
  // against the process working directory, which is the command directory.
  dir.write("top.rsp", "/DFROM_TOP");
  dir.write("sub/inner.rsp", "/DFROM_INNER @top.rsp");

  const CompileCommand c = command_for(dir.path(), {"cl.exe", "@sub/inner.rsp", "main.cpp"});
  const ResponseExpansion e = expand_response_files(c);
  REQUIRE(e.attempted);
  REQUIRE_MESSAGE(e.ok, why(e));
  CHECK(e.arguments == std::vector<std::string>{"cl.exe", "/DFROM_INNER", "/DFROM_TOP", "main.cpp"});
  CHECK(e.files.size() == 2);
  CHECK(e.files[0].as_written == "sub/inner.rsp");
  CHECK(e.files[1].as_written == "top.rsp");
  CHECK(e.files[1].resolved == (dir.path() / "top.rsp").lexically_normal());
}

TEST_CASE("every expanded token keeps its raw argument and originating file") {
  TempDir dir("origins");
  dir.write("a.rsp", "/DA1 /DA2");

  const CompileCommand c = command_for(dir.path(), {"cl.exe", "/c", "@a.rsp", "main.cpp"});
  const ResponseExpansion e = expand_response_files(c);
  REQUIRE(e.ok);
  REQUIRE(e.arguments.size() == e.origins.size());
  CHECK(e.arguments == std::vector<std::string>{"cl.exe", "/c", "/DA1", "/DA2", "main.cpp"});

  CHECK_FALSE(e.origins[0].from_response_file);  // cl.exe
  CHECK_FALSE(e.origins[1].from_response_file);  // /c, raw argument 1
  CHECK(e.origins[1].raw_index == 1);
  CHECK(e.origins[2].from_response_file);
  CHECK(e.origins[2].raw_index == 2);  // both came from raw argument 2, the @ token
  CHECK(e.origins[2].index_in_file == 0);
  CHECK(e.origins[3].raw_index == 2);
  CHECK(e.origins[3].index_in_file == 1);
  CHECK(e.files[e.origins[3].snapshot].as_written == "a.rsp");
  CHECK_FALSE(e.origins[4].from_response_file);  // main.cpp, raw argument 3
  CHECK(e.origins[4].raw_index == 3);
}

TEST_CASE("a file referenced several times without recursion is legal and snapshotted once") {
  TempDir dir("repeat");
  dir.write("common.rsp", "/DCOMMON");
  dir.write("left.rsp", "@common.rsp /DLEFT");
  dir.write("right.rsp", "@common.rsp /DRIGHT");

  const CompileCommand c = command_for(dir.path(), {"cl.exe", "@left.rsp", "@right.rsp", "@common.rsp", "main.cpp"});
  const ResponseExpansion e = expand_response_files(c);
  REQUIRE_MESSAGE(e.ok, why(e));
  // The options appear as often as they were referenced...
  CHECK(e.arguments == std::vector<std::string>{"cl.exe", "/DCOMMON", "/DLEFT", "/DCOMMON", "/DRIGHT", "/DCOMMON",
                                                "main.cpp"});
  // ...but the file was read and hashed exactly once.
  CHECK(e.files.size() == 3);
  std::size_t common_snapshots = 0;
  for (const auto& f : e.files) {
    if (f.as_written == "common.rsp") ++common_snapshots;
  }
  CHECK(common_snapshots == 1);
}

TEST_CASE("recursion is rejected but a diamond is not") {
  TempDir dir("cycle");
  SUBCASE("direct self-reference") {
    dir.write("self.rsp", "/DX @self.rsp");
    const ResponseExpansion e = expand_response_files(command_for(dir.path(), {"cl.exe", "@self.rsp", "main.cpp"}));
    REQUIRE(e.attempted);
    CHECK_FALSE(e.ok);
    CHECK(e.arguments.empty());
    REQUIRE(e.errors.size() == 1);
    CHECK(e.errors.front().find("cyclic") != std::string::npos);
  }
  SUBCASE("mutual recursion") {
    dir.write("a.rsp", "@b.rsp");
    dir.write("b.rsp", "@a.rsp");
    const ResponseExpansion e = expand_response_files(command_for(dir.path(), {"cl.exe", "@a.rsp", "main.cpp"}));
    CHECK_FALSE(e.ok);
    CHECK(e.errors.front().find("cyclic") != std::string::npos);
  }
  SUBCASE("a diamond repeats a file on two paths but never on one path") {
    dir.write("leaf.rsp", "/DLEAF");
    dir.write("l.rsp", "@leaf.rsp");
    dir.write("r.rsp", "@leaf.rsp");
    dir.write("top.rsp", "@l.rsp @r.rsp");
    const ResponseExpansion e = expand_response_files(command_for(dir.path(), {"cl.exe", "@top.rsp", "main.cpp"}));
    REQUIRE_MESSAGE(e.ok, why(e));
    CHECK(e.arguments == std::vector<std::string>{"cl.exe", "/DLEAF", "/DLEAF", "main.cpp"});
  }
}

TEST_CASE("an exponential diamond of empty response files is bounded by the work budget") {
  // Neither bytes nor emitted tokens grow here: every file is tiny and only
  // ever references others. Without a cumulative work budget this expands to
  // 2^depth references and never terminates in practice.
  TempDir dir("work");
  const int levels = 24;
  dir.write("f" + std::to_string(levels) + ".rsp", "");
  for (int i = levels - 1; i >= 0; --i) {
    const std::string next = "@f" + std::to_string(i + 1) + ".rsp";
    dir.write("f" + std::to_string(i) + ".rsp", next + " " + next);
  }
  const ResponseExpansion e = expand_response_files(command_for(dir.path(), {"cl.exe", "@f0.rsp", "main.cpp"}));
  REQUIRE(e.attempted);
  CHECK_FALSE(e.ok);
  CHECK(e.arguments.empty());
  REQUIRE_FALSE(e.errors.empty());
  // Depth stays within max_depth only for the first few levels; whichever
  // bound trips first, the refusal names a limit rather than hanging.
  const std::string& why = e.errors.front();
  CHECK((why.find("references and tokens") != std::string::npos || why.find("nesting deeper") != std::string::npos));
}

TEST_CASE("depth, per-file size, aggregate size and token count are finite and rejecting") {
  TempDir dir("limits");
  SUBCASE("nesting depth") {
    dir.write("d4.rsp", "/DDEEP");
    dir.write("d3.rsp", "@d4.rsp");
    dir.write("d2.rsp", "@d3.rsp");
    dir.write("d1.rsp", "@d2.rsp");
    ResponseFileLimits limits;
    limits.max_depth = 3;
    const ResponseExpansion e = expand_response_files(command_for(dir.path(), {"cl.exe", "@d1.rsp", "main.cpp"}), limits);
    CHECK_FALSE(e.ok);
    CHECK(e.errors.front().find("nesting deeper than 3") != std::string::npos);
  }
  SUBCASE("one file over the per-file limit") {
    dir.write("big.rsp", std::string(4096, 'x'));
    ResponseFileLimits limits;
    limits.max_file_bytes = 1024;
    const ResponseExpansion e = expand_response_files(command_for(dir.path(), {"cl.exe", "@big.rsp", "main.cpp"}), limits);
    CHECK_FALSE(e.ok);
    CHECK(e.errors.front().find("per-file limit") != std::string::npos);
  }
  SUBCASE("several files over the aggregate limit") {
    dir.write("p1.rsp", std::string(800, 'a'));
    dir.write("p2.rsp", std::string(800, 'b'));
    ResponseFileLimits limits;
    limits.max_file_bytes = 1024;
    limits.max_total_bytes = 1024;
    const ResponseExpansion e =
        expand_response_files(command_for(dir.path(), {"cl.exe", "@p1.rsp", "@p2.rsp", "main.cpp"}), limits);
    CHECK_FALSE(e.ok);
    CHECK(e.errors.front().find("aggregate limit") != std::string::npos);
  }
  SUBCASE("token count") {
    dir.write("many.rsp", "/DA /DB /DC /DD /DE");
    ResponseFileLimits limits;
    limits.max_tokens = 4;
    const ResponseExpansion e = expand_response_files(command_for(dir.path(), {"cl.exe", "@many.rsp", "main.cpp"}), limits);
    CHECK_FALSE(e.ok);
    CHECK(e.errors.front().find("limit of 4 tokens") != std::string::npos);
  }
}

TEST_CASE("missing, unreadable and unterminated response files reject atomically") {
  TempDir dir("missing");
  dir.write("good.rsp", "/DGOOD");

  SUBCASE("a missing file rejects and discards the tokens already collected") {
    const ResponseExpansion e =
        expand_response_files(command_for(dir.path(), {"cl.exe", "@good.rsp", "@absent.rsp", "main.cpp"}));
    REQUIRE(e.attempted);
    CHECK_FALSE(e.ok);
    CHECK(e.arguments.empty());  // atomic: no partially expanded argv escapes
    CHECK(e.origins.empty());
    CHECK(e.replay_id.empty());
    CHECK(e.errors.front().find("absent.rsp") != std::string::npos);
  }
  SUBCASE("a directory is not a response file") {
    fs::create_directories(dir.path() / "adir.rsp");
    const ResponseExpansion e = expand_response_files(command_for(dir.path(), {"cl.exe", "@adir.rsp", "main.cpp"}));
    CHECK_FALSE(e.ok);
    CHECK(e.errors.front().find("readable file") != std::string::npos);
  }
  SUBCASE("an unterminated quote is a refusal, not a best-effort parse") {
    dir.write("open.rsp", "/I \"C:/unterminated");
    const ResponseExpansion e = expand_response_files(command_for(dir.path(), {"cl.exe", "@open.rsp", "main.cpp"}));
    CHECK_FALSE(e.ok);
    CHECK(e.errors.front().find("quoted argument") != std::string::npos);
  }
}

TEST_CASE("Windows quoting round-trips, including spaces and escaped quotes") {
  TempDir dir("quoting");
  dir.write("q.rsp",
            "/I \"C:/Program Files/SDK/inc\"\r\n"
            // An embedded quote must itself be inside a quoted run to keep the
            // space: `\"a b\"` alone is two tokens, exactly as the driver reads it.
            "/DTEXT=\"\\\"hello world\\\"\"\r\n"
            "/DBARE=\\\"a b\\\"\n"
            "/I \"with\ttab\"\n"
            "/DPLAIN=1");
  const ResponseExpansion e = expand_response_files(command_for(dir.path(), {"cl.exe", "@q.rsp", "main.cpp"}));
  REQUIRE_MESSAGE(e.ok, why(e));
  CHECK(e.arguments == std::vector<std::string>{"cl.exe", "/I", "C:/Program Files/SDK/inc",
                                                "/DTEXT=\"hello world\"", "/DBARE=\"a", "b\"", "/I", "with\ttab",
                                                "/DPLAIN=1", "main.cpp"});
}

TEST_CASE("only the three verified encodings are accepted") {
  TempDir dir("encoding");
  const std::string unicode_option = "/I \"C:/\355\225\234 \352\270\200/inc(1)\"";  // "한 글" in UTF-8
  const std::vector<std::string> expected{"cl.exe", "/I", "C:/\355\225\234 \352\270\200/inc(1)", "main.cpp"};

  SUBCASE("ASCII without a BOM") {
    dir.write("a.rsp", "/I \"C:/plain ascii/inc\"");
    const ResponseExpansion e = expand_response_files(command_for(dir.path(), {"cl.exe", "@a.rsp", "main.cpp"}));
    REQUIRE(e.ok);
    CHECK(e.files.front().encoding == ResponseEncoding::ascii);
    CHECK(e.arguments == std::vector<std::string>{"cl.exe", "/I", "C:/plain ascii/inc", "main.cpp"});
  }
  SUBCASE("UTF-8 WITH a BOM carries Unicode and spaced paths") {
    dir.write("u8.rsp", kUtf8Bom + unicode_option);
    const ResponseExpansion e = expand_response_files(command_for(dir.path(), {"cl.exe", "@u8.rsp", "main.cpp"}));
    REQUIRE_MESSAGE(e.ok, why(e));
    CHECK(e.files.front().encoding == ResponseEncoding::utf8_bom);
    CHECK(e.arguments == expected);
  }
  SUBCASE("UTF-16LE WITH a BOM carries Unicode and spaced paths") {
    dir.write("u16.rsp", kUtf16LeBom + utf16le_of(unicode_option));
    const ResponseExpansion e = expand_response_files(command_for(dir.path(), {"cl.exe", "@u16.rsp", "main.cpp"}));
    REQUIRE_MESSAGE(e.ok, why(e));
    CHECK(e.files.front().encoding == ResponseEncoding::utf16le_bom);
    CHECK(e.arguments == expected);
    // The hash covers the raw bytes actually read, not the decoded text.
    CHECK(e.files.front().bytes == kUtf16LeBom.size() + utf16le_of(unicode_option).size());
  }
  SUBCASE("BOM-less non-ASCII is refused rather than guessed") {
    // The verified compiler misdecodes this exact shape, so treating it as
    // UTF-8 or as a code page would silently analyse a different context.
    dir.write("u8nobom.rsp", unicode_option);
    const ResponseExpansion e = expand_response_files(command_for(dir.path(), {"cl.exe", "@u8nobom.rsp", "main.cpp"}));
    CHECK_FALSE(e.ok);
    CHECK(e.errors.front().find("non-ASCII byte without a byte order mark") != std::string::npos);
  }
  SUBCASE("unsupported byte order marks are named, not silently skipped") {
    struct Case {
      const char* name;
      std::string bytes;
      const char* expected;
    };
    const std::vector<Case> cases{
        {"utf16be.rsp", std::string("\xFE\xFF", 2) + std::string("\x00/\x00D\x00X", 6), "UTF-16BE"},
        {"utf32le.rsp", std::string("\xFF\xFE\x00\x00", 4) + std::string("/\x00\x00\x00", 4), "UTF-32LE"},
        {"utf32be.rsp", std::string("\x00\x00\xFE\xFF", 4) + std::string("\x00\x00\x00/", 4), "UTF-32BE"},
    };
    for (const Case& k : cases) {
      CAPTURE(k.name);
      dir.write(k.name, k.bytes);
      const ResponseExpansion e =
          expand_response_files(command_for(dir.path(), {"cl.exe", std::string("@") + k.name, "main.cpp"}));
      CHECK_FALSE(e.ok);
      CHECK(e.errors.front().find(k.expected) != std::string::npos);
    }
  }
  SUBCASE("malformed UTF-8, surrogates and NUL are refused") {
    struct Case {
      const char* name;
      std::string bytes;
      const char* expected;
    };
    const std::vector<Case> cases{
        {"trunc.rsp", kUtf8Bom + "/D\xE0\xA4", "truncated"},
        {"lead.rsp", kUtf8Bom + "/D\xFE", "lead byte"},
        {"overlong.rsp", kUtf8Bom + std::string("/D\xC0\xAF"), "overlong"},
        {"surrogate8.rsp", kUtf8Bom + std::string("/D\xED\xA0\x80"), "surrogate"},
        {"nul8.rsp", kUtf8Bom + std::string("/DX\x00Y", 5), "NUL"},
        {"nulascii.rsp", std::string("/DX\x00Y", 5), "NUL"},
        {"odd16.rsp", kUtf16LeBom + std::string("/\x00D", 3), "odd number of bytes"},
        {"surrogate16.rsp", kUtf16LeBom + std::string("\x00\xD8\x00\x00", 4), "surrogate"},
    };
    for (const Case& k : cases) {
      CAPTURE(k.name);
      dir.write(k.name, k.bytes);
      const ResponseExpansion e =
          expand_response_files(command_for(dir.path(), {"cl.exe", std::string("@") + k.name, "main.cpp"}));
      CHECK_FALSE(e.ok);
      CHECK(e.errors.front().find(k.expected) != std::string::npos);
    }
  }
}

TEST_CASE("the replay identity tracks the bytes of every response file that was read") {
  TempDir dir("identity");
  dir.write("inner.rsp", "/DINNER=1");
  dir.write("outer.rsp", "@inner.rsp /DOUTER");
  const CompileCommand c = command_for(dir.path(), {"cl.exe", "@outer.rsp", "main.cpp"});

  const ResponseExpansion first = expand_response_files(c);
  REQUIRE(first.ok);
  CHECK_FALSE(first.replay_id.empty());
  CHECK(first.replay_id != c.command_id);

  SUBCASE("identical inputs give an identical replay identity") {
    CHECK(expand_response_files(c).replay_id == first.replay_id);
  }
  SUBCASE("a changed NESTED file changes the replay identity while the command identity is untouched") {
    dir.write("inner.rsp", "/DINNER=2");
    const ResponseExpansion second = expand_response_files(c);
    REQUIRE(second.ok);
    CHECK(second.replay_id != first.replay_id);
    CHECK(compute_command_id(c) == c.command_id);  // raw command identity is unchanged
  }
  SUBCASE("a change that leaves the token stream identical still changes the identity") {
    // Same tokens, different bytes: the hash is bound to the content actually
    // read, not only to the argv it produced.
    dir.write("inner.rsp", "/DINNER=1   ");
    const ResponseExpansion second = expand_response_files(c);
    REQUIRE(second.ok);
    CHECK(second.arguments == first.arguments);
    CHECK(second.replay_id != first.replay_id);
  }
  SUBCASE("the recorded hash is the hash of the bytes that were parsed") {
    const ResponseFileSnapshot& inner = first.files[0].as_written == "inner.rsp" ? first.files[0] : first.files[1];
    std::ifstream in(inner.resolved, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(inner.content_sha256 == sha256(bytes).hex());
    CHECK(inner.bytes == bytes.size());
  }
}

// --- review findings ---------------------------------------------------------

TEST_CASE("the replay identity never depends on the optional command_id field") {
  TempDir dir("optional-id");
  dir.write("a.rsp", "/DX");

  CompileCommand with_id = command_for(dir.path(), {"cl.exe", "@a.rsp", "main.cpp"});
  CompileCommand without_id = with_id;
  without_id.command_id.clear();  // the public request contract allows this

  const ResponseExpansion a = expand_response_files(with_id);
  const ResponseExpansion b = expand_response_files(without_id);
  REQUIRE(a.ok);
  REQUIRE(b.ok);
  CHECK(a.replay_id == b.replay_id);
}

TEST_CASE("commands that differ only in directory or file get distinct replay identities") {
  // The response file is named by ABSOLUTE path and carries a RELATIVE include,
  // so the expanded token sequence is byte-identical for both commands. Only the
  // command's own identity separates them, and their include lookup differs.
  TempDir dir("distinct-cwd");
  dir.write("shared.rsp", "/I \"inc\"");
  fs::create_directories(dir.path() / "one");
  fs::create_directories(dir.path() / "two");
  const std::string absolute = "@" + path_to_utf8_generic(dir.path() / "shared.rsp");

  auto make = [&](const char* sub) {
    CompileCommand c;
    c.directory = (dir.path() / sub).lexically_normal();
    c.file = (dir.path() / sub / "main.cpp").lexically_normal();
    c.arguments = {"cl.exe", absolute, "main.cpp"};
    c.command_id.clear();  // the empty-id path the finding used
    return c;
  };
  const ResponseExpansion one = expand_response_files(make("one"));
  const ResponseExpansion two = expand_response_files(make("two"));
  REQUIRE(one.ok);
  REQUIRE(two.ok);
  CHECK(one.arguments == two.arguments);  // identical expansion...
  CHECK(one.replay_id != two.replay_id);  // ...but not the same replay context
}

TEST_CASE("a repeated trailing reference changes the raw command, so it changes the replay identity") {
  TempDir dir("repeat-id");
  dir.write("empty.rsp", "");
  CompileCommand once = command_for(dir.path(), {"cl.exe", "@empty.rsp", "main.cpp"});
  CompileCommand twice = command_for(dir.path(), {"cl.exe", "@empty.rsp", "main.cpp", "@empty.rsp"});
  once.command_id.clear();
  twice.command_id.clear();
  const ResponseExpansion a = expand_response_files(once);
  const ResponseExpansion b = expand_response_files(twice);
  REQUIRE(a.ok);
  REQUIRE(b.ok);
  CHECK(a.arguments == b.arguments);  // the extra reference expands to nothing
  CHECK(a.replay_id != b.replay_id);  // but the raw invocation differs
}

TEST_CASE("max_tokens bounds the complete expanded argv, including the compiler token") {
  TempDir dir("zero-tokens");
  dir.write("empty.rsp", "");
  ResponseFileLimits limits;
  limits.max_tokens = 0;
  const ResponseExpansion e = expand_response_files(command_for(dir.path(), {"cl.exe", "@empty.rsp"}), limits);
  REQUIRE(e.attempted);
  CHECK_FALSE(e.ok);
  CHECK(e.arguments.empty());
  CHECK(e.errors.front().find("limit of 0 tokens") != std::string::npos);

  limits.max_tokens = 1;  // room for the compiler token only
  const ResponseExpansion one = expand_response_files(command_for(dir.path(), {"cl.exe", "@empty.rsp"}), limits);
  CHECK(one.ok);
  CHECK(one.arguments == std::vector<std::string>{"cl.exe"});
}

TEST_CASE("response-file reuse follows filesystem identity, not a lowercased path") {
  TempDir dir("case-identity");
  SUBCASE("a case alias of one file is one snapshot, and both spellings are recorded") {
    dir.write("Alias.rsp", "/DONCE");
    const ResponseExpansion e =
        expand_response_files(command_for(dir.path(), {"cl.exe", "@Alias.rsp", "@alias.rsp", "main.cpp"}));
    REQUIRE_MESSAGE(e.ok, why(e));
    CHECK(e.arguments == std::vector<std::string>{"cl.exe", "/DONCE", "/DONCE", "main.cpp"});
    REQUIRE(e.files.size() == 1);
    CHECK(e.files.front().alias_spellings.size() == 1);  // the second spelling kept as evidence
  }
  SUBCASE("case-distinct files in a case-sensitive directory are distinct snapshots") {
    const fs::path sub = dir.path() / "cs";
    fs::create_directories(sub);
    if (!lcm::test_support::try_enable_case_sensitive_directory(sub)) {
      MESSAGE("skipped: per-directory case sensitivity is unavailable in this environment");
      return;
    }
    // Two genuinely different files whose lowercased paths are equal.
    dir.write("cs/case.rsp", "/DLOWER");
    dir.write("cs/CASE.rsp", "/DUPPER");
    const ResponseExpansion e =
        expand_response_files(command_for(dir.path(), {"cl.exe", "@cs/case.rsp", "@cs/CASE.rsp", "main.cpp"}));
    REQUIRE_MESSAGE(e.ok, why(e));
    // A lexical key would treat the second as a cache hit and never read its
    // bytes, silently analysing the wrong content.
    CHECK(e.arguments == std::vector<std::string>{"cl.exe", "/DLOWER", "/DUPPER", "main.cpp"});
    CHECK(e.files.size() == 2);
    CHECK(e.files[0].content_sha256 != e.files[1].content_sha256);
  }
}
