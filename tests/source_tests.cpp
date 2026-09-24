#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include "lcm/source.hpp"
#include "support/case_sensitive_dir.hpp"

namespace fs = std::filesystem;

namespace {

struct TempTree {
  fs::path root;
  explicit TempTree(std::string_view utf8_name) {
    root = fs::temp_directory_path() / lcm::path_from_utf8(utf8_name);
    fs::remove_all(root);
    fs::create_directories(root);
  }
  ~TempTree() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }
  void write(std::string_view utf8_relative, std::string_view content) const {
    const fs::path p = root / lcm::path_from_utf8(utf8_relative);
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
  }
};

}  // namespace

TEST_CASE("looks_absolute recognises drive, UNC and rooted paths only") {
  CHECK(lcm::looks_absolute("C:/x"));
  CHECK(lcm::looks_absolute("c:\\x"));
  CHECK(lcm::looks_absolute("/usr/x"));
  CHECK(lcm::looks_absolute("\\\\server\\share"));
  CHECK_FALSE(lcm::looks_absolute("src/a.cpp"));
  CHECK_FALSE(lcm::looks_absolute("a.cpp"));
  CHECK_FALSE(lcm::looks_absolute(""));
}

TEST_CASE("make_repo_relative yields generic paths and rejects outsiders") {
  const fs::path root = lcm::path_from_utf8("C:/proj");
  auto rel = lcm::make_repo_relative(root, lcm::path_from_utf8("C:/proj/src/Widget.cpp"));
  REQUIRE(rel);
  CHECK(rel->generic == "src/Widget.cpp");

  SUBCASE("normalises dot segments and backslashes") {
    auto r = lcm::make_repo_relative(root, lcm::path_from_utf8("C:\\proj\\src\\.\\ui\\..\\Widget.cpp"));
    REQUIRE(r);
    CHECK(r->generic == "src/Widget.cpp");
  }
  SUBCASE("root itself is the empty relative path") {
    auto r = lcm::make_repo_relative(root, lcm::path_from_utf8("C:/proj/"));
    REQUIRE(r);
    CHECK(r->empty());
  }
  SUBCASE("sibling directory sharing a prefix is outside") {
    CHECK_FALSE(lcm::make_repo_relative(root, lcm::path_from_utf8("C:/project/src/a.cpp")));
    CHECK_FALSE(lcm::make_repo_relative(root, lcm::path_from_utf8("C:/other/a.cpp")));
    CHECK_FALSE(lcm::make_repo_relative(root, lcm::path_from_utf8("C:/proj/../other/a.cpp")));
  }
#ifdef _WIN32
  SUBCASE("drive letter and ASCII directory case differences still resolve, spelling preserved") {
    auto r = lcm::make_repo_relative(root, lcm::path_from_utf8("c:/PROJ/Src/Widget.cpp"));
    REQUIRE(r);
    CHECK(r->generic == "Src/Widget.cpp");
  }
#endif
}

TEST_CASE("path_key folds only ASCII case on Windows and keeps UTF-8 verbatim") {
  const std::string a = lcm::path_key(lcm::path_from_utf8("C:/Proj/Src/A.cpp"));
  const std::string b = lcm::path_key(lcm::path_from_utf8("c:\\proj\\src\\a.cpp"));
#ifdef _WIN32
  CHECK(a == b);
  CHECK(a == "c:/proj/src/a.cpp");
#else
  CHECK(a != b);
#endif
  const std::string korean = lcm::path_key(lcm::path_from_utf8("C:/프로젝트/소스/파일.cpp"));
  CHECK(korean == "c:/프로젝트/소스/파일.cpp");
  // Non-ASCII case is deliberately not folded; filesystem identity is asked instead.
  CHECK(lcm::path_key(lcm::path_from_utf8("C:/Ünïcode")) != lcm::path_key(lcm::path_from_utf8("C:/ÜNÏCODE")));
}

TEST_CASE("UTF-8 path conversions round-trip characters outside the ANSI code page") {
  // Hangul, Cyrillic, Latin with diacritics and a non-BMP emoji.
  const std::string utf8 = "C:/작업/тест/Ünïcode/😀/file.cpp";
  const fs::path p = lcm::path_from_utf8(utf8);
  CHECK(lcm::path_to_utf8_generic(p) == utf8);
  CHECK(p.filename() == lcm::path_from_utf8("file.cpp"));
  CHECK(lcm::path_to_utf8_generic(p.parent_path().filename()) == "😀");
}

TEST_CASE("real Unicode directory tree: creation, relative paths and filesystem identity") {
  TempTree tree("lcm-테스트-😀-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  tree.write("Ünïcode/файл.cpp", "int main() {}\n");
  const fs::path file = tree.root / lcm::path_from_utf8("Ünïcode/файл.cpp");
  REQUIRE(fs::exists(file));

  auto rel = lcm::make_repo_relative(tree.root, file);
  REQUIRE(rel);
  CHECK(rel->generic == "Ünïcode/файл.cpp");

  // Round trip through UTF-8 and back reaches the same file.
  const fs::path again = lcm::path_from_utf8(lcm::path_to_utf8_generic(file));
  CHECK(fs::exists(again));
  CHECK(lcm::same_existing_file(file, again));

#ifdef _WIN32
  // NTFS folds non-ASCII case; the filesystem, not path_key, is the authority.
  const fs::path upper = tree.root / lcm::path_from_utf8("ÜNÏCODE/ФАЙЛ.CPP");
  CHECK(fs::exists(upper));
  CHECK(lcm::same_existing_file(file, upper));
  CHECK(lcm::path_key(file) != lcm::path_key(upper));

  SUBCASE("on-disk canonical identity unifies case aliases of one existing file, keeps display spelling") {
    const auto canonical_lower = lcm::make_repo_relative_on_disk(tree.root, file);
    const auto canonical_upper = lcm::make_repo_relative_on_disk(tree.root, upper);
    REQUIRE(canonical_lower);
    REQUIRE(canonical_upper);
    CHECK(canonical_lower->generic == "Ünïcode/файл.cpp");  // the spelling the filesystem stores
    CHECK(canonical_upper->generic == "Ünïcode/файл.cpp");
    CHECK(*canonical_lower == *canonical_upper);
    CHECK(canonical_upper->as_written == "ÜNÏCODE/ФАЙЛ.CPP");
    CHECK(canonical_lower->as_written == "Ünïcode/файл.cpp");
    // Purely lexical construction does not unify them; identity must go through the on-disk form.
    CHECK(lcm::make_repo_relative(tree.root, upper)->generic == "ÜNÏCODE/ФАЙЛ.CPP");
  }

  SUBCASE("a spelling that does not exist on disk is not folded into anything") {
    const fs::path ghost = tree.root / lcm::path_from_utf8("Ünïcode/ДРУГОЙ.cpp");
    const auto ghost_rel = lcm::make_repo_relative_on_disk(tree.root, ghost);
    REQUIRE(ghost_rel);
    CHECK(ghost_rel->generic == "Ünïcode/ДРУГОЙ.cpp");
    CHECK(*ghost_rel != *lcm::make_repo_relative_on_disk(tree.root, file));
  }

  SUBCASE("two genuinely distinct existing files keep distinct identities") {
    tree.write("Ünïcode/файл2.cpp", "int other;\n");
    const auto a = lcm::make_repo_relative_on_disk(tree.root, file);
    const auto b = lcm::make_repo_relative_on_disk(tree.root, tree.root / lcm::path_from_utf8("Ünïcode/файл2.cpp"));
    REQUIRE(a);
    REQUIRE(b);
    CHECK(*a != *b);
  }
#endif
  CHECK_FALSE(lcm::same_existing_file(file, tree.root / lcm::path_from_utf8("missing.cpp")));
}

#ifdef _WIN32
TEST_CASE("on-disk containment is decided by filesystem ancestry: a case-distinct sibling of the root is outside") {
  TempTree tree("lcm-containment-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  if (!lcm::test_support::try_enable_case_sensitive_directory(tree.root)) {
    MESSAGE("LIMITATION: per-directory case sensitivity unavailable; sibling-containment regression not executed");
    return;
  }
  const fs::path root = tree.root / "CaseRoot";
  const fs::path sibling = tree.root / "caseroot";
  fs::create_directories(root / "nested");
  fs::create_directories(sibling);
  REQUIRE_FALSE(fs::equivalent(root, sibling));
  tree.write("caseroot/external.hpp", "int external_api();\n");
  tree.write("CaseRoot/nested/inner.hpp", "int inner();\n");
  tree.write("CaseRoot/a.cpp", "int lower;\n");

  // The sibling shares the ASCII-folded prefix of the root but is a different directory object.
  CHECK(lcm::path_key(root) == lcm::path_key(sibling));
  CHECK_FALSE(lcm::make_repo_relative_on_disk(root, sibling / "external.hpp").has_value());
  // Genuine descendants, including nested ones, are inside with on-disk spelling.
  const auto nested = lcm::make_repo_relative_on_disk(root, root / "nested" / "inner.hpp");
  REQUIRE(nested);
  CHECK(nested->generic == "nested/inner.hpp");
  const auto direct = lcm::make_repo_relative_on_disk(root, root / "a.cpp");
  REQUIRE(direct);
  CHECK(direct->generic == "a.cpp");
  // The root itself maps to the empty relative path; a non-existing spelling falls back to lexical.
  REQUIRE(lcm::make_repo_relative_on_disk(root, root));
  CHECK(lcm::make_repo_relative_on_disk(root, root)->empty());
  const auto ghost = lcm::make_repo_relative_on_disk(root, root / "missing.cpp");
  REQUIRE(ghost);
  CHECK(ghost->generic == "missing.cpp");
}
#endif

TEST_CASE("is_canonical_repo_relative accepts only normalised relative spellings") {
  CHECK(lcm::is_canonical_repo_relative("src/a.cpp"));
  CHECK(lcm::is_canonical_repo_relative("a.cpp"));
  CHECK(lcm::is_canonical_repo_relative("소스/파일.cpp"));
  CHECK(lcm::is_canonical_repo_relative("src/.hidden/a.cpp"));
  CHECK(lcm::is_canonical_repo_relative("src/..a/b"));
  for (const char* bad : {"", ".", "..", "./a", "a/..", "a/../b", "a//b", "a\\b", "/a", "C:/a", "a/", "a/./b"}) {
    CAPTURE(bad);
    CHECK_FALSE(lcm::is_canonical_repo_relative(bad));
  }
}

TEST_CASE("make_repo_relative rejects dotted escapes and records display spelling") {
  const fs::path root = lcm::path_from_utf8("C:/proj");
  CHECK_FALSE(lcm::make_repo_relative(root, lcm::path_from_utf8("C:/proj/src/../../other.cpp")));
  const auto r = lcm::make_repo_relative(root, lcm::path_from_utf8("C:/proj/src/./x/../Widget.cpp"));
  REQUIRE(r);
  CHECK(r->generic == "src/Widget.cpp");
  CHECK(r->as_written == "src/./x/../Widget.cpp");
  CHECK(lcm::is_canonical_repo_relative(r->generic));
}

TEST_CASE("LineIndex maps offsets to 1-based line and byte column") {
  const std::string content = "ab\ncd\r\n\nxyz";
  lcm::LineIndex index(content);
  CHECK(index.line_count() == 4);
  CHECK(index.line_column(0) == std::pair<std::uint32_t, std::uint32_t>{1, 1});
  CHECK(index.line_column(2) == std::pair<std::uint32_t, std::uint32_t>{1, 3});  // the '\n'
  CHECK(index.line_column(3) == std::pair<std::uint32_t, std::uint32_t>{2, 1});
  CHECK(index.line_column(7) == std::pair<std::uint32_t, std::uint32_t>{3, 1});
  CHECK(index.line_column(8) == std::pair<std::uint32_t, std::uint32_t>{4, 1});
  CHECK(index.line_column(11) == std::pair<std::uint32_t, std::uint32_t>{4, 4});  // end of content

  auto span = index.span(3, 5);
  REQUIRE(span);
  CHECK(span->begin_line == 2);
  CHECK(span->begin_column == 1);
  CHECK(span->end_line == 2);
  CHECK(span->end_column == 3);
  CHECK(lcm::slice(content, *span) == std::string_view("cd"));

  CHECK_FALSE(index.span(5, 3));
  CHECK_FALSE(index.span(0, 12));
}

TEST_CASE("read_snippet returns text only when the file hash matches the recorded hash") {
  const std::string original = "int a();\nint area(int w, int h) {\n  return w * h;\n}\n";
  lcm::LineIndex index(original);
  const auto begin = original.find("int area");
  const auto end = original.find("}\n") + 1;
  const auto span = index.span(static_cast<std::uint32_t>(begin), static_cast<std::uint32_t>(end));
  REQUIRE(span);
  const auto recorded = lcm::hash_file_content(original);
  const auto span_hash = lcm::hash_span(original, *span);
  REQUIRE(span_hash);

  SUBCASE("unchanged file") {
    auto result = lcm::read_snippet(original, recorded, *span);
    auto* snippet = std::get_if<lcm::Snippet>(&result);
    REQUIRE(snippet);
    CHECK(snippet->text == "int area(int w, int h) {\n  return w * h;\n}");
    CHECK(snippet->source_hash == recorded);
    CHECK(lcm::sha256(snippet->text) == *span_hash);
  }

  SUBCASE("a line inserted before the symbol must not be quoted through the old span") {
    const std::string changed = "// new comment\n" + original;
    auto result = lcm::read_snippet(changed, recorded, *span);
    REQUIRE(std::holds_alternative<lcm::SnippetFailure>(result));
    CHECK(std::get<lcm::SnippetFailure>(result) == lcm::SnippetFailure::source_changed);
    // The old span over the new content would have quoted different code:
    CHECK(lcm::slice(changed, *span) != std::string_view("int area(int w, int h) {\n  return w * h;\n}"));
  }

  SUBCASE("same size but different bytes is still a change") {
    std::string changed = original;
    changed[changed.find("w * h")] = 'x';
    auto result = lcm::read_snippet(changed, recorded, *span);
    REQUIRE(std::holds_alternative<lcm::SnippetFailure>(result));
    CHECK(std::get<lcm::SnippetFailure>(result) == lcm::SnippetFailure::source_changed);
  }

  SUBCASE("span outside recorded content is reported, not clamped") {
    lcm::SourceSpan bad = *span;
    bad.end_offset = static_cast<std::uint32_t>(original.size() + 10);
    auto result = lcm::read_snippet(original, recorded, bad);
    REQUIRE(std::holds_alternative<lcm::SnippetFailure>(result));
    CHECK(std::get<lcm::SnippetFailure>(result) == lcm::SnippetFailure::span_out_of_range);
  }
}
