#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "lcm/clang/smoke.hpp"

using lcm::clang_smoke::CallFact;
using lcm::clang_smoke::FunctionFact;
using lcm::clang_smoke::SmokeFacts;

namespace {

std::string read_fixture(const char* relative) {
  const std::filesystem::path path = std::filesystem::path(LCM_TEST_FIXTURE_DIR) / relative;
  std::ifstream in(path, std::ios::binary);
  REQUIRE_MESSAGE(in.good(), "fixture missing: " << path.string());
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

const std::vector<std::string> kArgs = {"-std=c++20"};

std::vector<const FunctionFact*> functions_named(const SmokeFacts& facts, const std::string& qualified) {
  std::vector<const FunctionFact*> out;
  for (const auto& f : facts.functions) {
    if (f.qualified_name == qualified) out.push_back(&f);
  }
  return out;
}

std::vector<const CallFact*> calls_from(const SmokeFacts& facts, const std::string& caller) {
  std::vector<const CallFact*> out;
  for (const auto& c : facts.calls) {
    if (c.caller_qualified == caller) out.push_back(&c);
  }
  return out;
}

bool has_call(const std::vector<const CallFact*>& calls, const std::string& callee, const std::string& signature,
              bool member) {
  return std::any_of(calls.begin(), calls.end(), [&](const CallFact* c) {
    return c->callee_qualified == callee && c->callee_signature == signature && c->member_call == member;
  });
}

}  // namespace

TEST_CASE("the linked Clang front end reports the pinned major version") {
  const std::string version = lcm::clang_smoke::clang_version();
  MESSAGE(version);
  CHECK(version.find("22.1.0") != std::string::npos);
}

TEST_CASE("smoke fixture parses without errors through the in-process front end") {
  const SmokeFacts facts = lcm::clang_smoke::collect_facts(read_fixture("cpp/smoke/overloads.cpp"), "overloads.cpp", kArgs);
  for (const auto& d : facts.diagnostics) MESSAGE(d);
  REQUIRE(facts.parsed_ok);
  CHECK(facts.diagnostics.empty());
  CHECK_FALSE(facts.functions.empty());
  CHECK_FALSE(facts.calls.empty());
}

TEST_CASE("declaration and definition of one function are distinct redeclarations with the compiler's owner") {
  const SmokeFacts facts = lcm::clang_smoke::collect_facts(read_fixture("cpp/smoke/overloads.cpp"), "overloads.cpp", kArgs);
  REQUIRE(facts.parsed_ok);

  const auto area = functions_named(facts, "geo::Shape::area");
  REQUIRE(area.size() == 2);
  const auto* declaration = area[0]->is_definition ? area[1] : area[0];
  const auto* definition = area[0]->is_definition ? area[0] : area[1];
  CHECK_FALSE(declaration->is_definition);
  CHECK(definition->is_definition);
  CHECK(declaration->line < definition->line);
  CHECK(declaration->owner == "geo::Shape");
  CHECK(definition->owner == "geo::Shape");
  CHECK(declaration->signature == "() const");
  CHECK(definition->signature == "() const");

  // Free-function overloads: two declarations + two definitions, owner is the global namespace.
  const auto prints = functions_named(facts, "print");
  REQUIRE(prints.size() == 4);
  CHECK(std::count_if(prints.begin(), prints.end(), [](const FunctionFact* f) { return f->signature == "(int)"; }) == 2);
  CHECK(std::count_if(prints.begin(), prints.end(), [](const FunctionFact* f) { return f->signature == "(double)"; }) == 2);
  CHECK(std::count_if(prints.begin(), prints.end(), [](const FunctionFact* f) { return f->is_definition; }) == 2);
  CHECK(std::all_of(prints.begin(), prints.end(), [](const FunctionFact* f) { return f->owner.empty(); }));

  // Same spelling, different owner: the compiler places it under Other, not the global namespace.
  const auto other_print = functions_named(facts, "Other::print");
  REQUIRE(other_print.size() == 2);
  CHECK(std::all_of(other_print.begin(), other_print.end(), [](const FunctionFact* f) { return f->owner == "Other"; }));
  CHECK(std::all_of(other_print.begin(), other_print.end(), [](const FunctionFact* f) { return f->signature == "(int)"; }));
}

TEST_CASE("call sites resolve to the overload the compiler selected, not to a name") {
  const SmokeFacts facts = lcm::clang_smoke::collect_facts(read_fixture("cpp/smoke/overloads.cpp"), "overloads.cpp", kArgs);
  REQUIRE(facts.parsed_ok);

  const auto from_use = calls_from(facts, "use");
  REQUIRE(from_use.size() == 4);
  CHECK(has_call(from_use, "print", "(int)", false));
  CHECK(has_call(from_use, "print", "(double)", false));
  CHECK(has_call(from_use, "Other::print", "(int)", true));
  CHECK(has_call(from_use, "geo::Shape::area", "() const", true));

  // Three call sites spell "print", but each resolves to a different function.
  const auto spelled_print = std::count_if(from_use.begin(), from_use.end(), [](const CallFact* c) {
    return c->callee_qualified == "print" || c->callee_qualified == "Other::print";
  });
  CHECK(spelled_print == 3);
  std::vector<std::string> distinct;
  for (const auto* c : from_use) distinct.push_back(c->callee_qualified + c->callee_signature);
  std::sort(distinct.begin(), distinct.end());
  CHECK(std::unique(distinct.begin(), distinct.end()) == distinct.end());

  // The member call inside geo::Shape::scaled is attributed to that method, not to `use`.
  const auto from_scaled = calls_from(facts, "geo::Shape::scaled");
  REQUIRE(from_scaled.size() == 1);
  CHECK(from_scaled.front()->callee_qualified == "geo::Shape::area");
  CHECK(from_scaled.front()->member_call);

  CHECK(facts.calls.size() == 5);
}

TEST_CASE("text that merely looks like a call is not a call") {
  const std::string code = read_fixture("cpp/smoke/overloads.cpp");
  const SmokeFacts facts = lcm::clang_smoke::collect_facts(code, "overloads.cpp", kArgs);
  REQUIRE(facts.parsed_ok);

  // The fixture mentions print(99) in a comment and in a string literal: count them to prove they exist.
  std::size_t textual = 0;
  for (std::size_t pos = code.find("print(99)"); pos != std::string::npos; pos = code.find("print(99)", pos + 1)) ++textual;
  CHECK(textual == 2);
  CHECK(std::none_of(facts.calls.begin(), facts.calls.end(),
                     [](const CallFact& c) { return c.callee_signature == "(int)" && c.line == 0; }));
  // No call fact points at the comment line or the string literal line.
  std::size_t comment_line = 0, string_line = 0, line = 1;
  for (std::size_t i = 0; i < code.size(); ++i) {
    if (code.compare(i, 4, "// \"") == 0 && comment_line == 0) comment_line = static_cast<std::size_t>(line);
    if (code.compare(i, 12, "const char* ") == 0) string_line = static_cast<std::size_t>(line);
    if (code[i] == '\n') ++line;
  }
  REQUIRE(comment_line > 0);
  REQUIRE(string_line > 0);
  for (const auto& c : facts.calls) {
    CHECK(c.line != comment_line);
    CHECK(c.line != string_line);
  }
}

TEST_CASE("calls in default arguments are not attributed to the declaring function and are counted once") {
  const SmokeFacts facts = lcm::clang_smoke::collect_facts(
      "int helper();\nint f(int x = helper());\nint f(int x) { return x; }\nint g() { return f(); }\n", "defaults.cpp",
      kArgs);
  REQUIRE(facts.parsed_ok);
  // Two redeclarations of f share the default argument; the declaring function made no call.
  CHECK(calls_from(facts, "f").empty());
  REQUIRE(facts.calls_outside_bodies.size() == 1);
  CHECK(facts.calls_outside_bodies.front().declaring_function == "f");
  CHECK(facts.calls_outside_bodies.front().callee_qualified == "helper");
  CHECK(facts.calls_outside_bodies.front().line == 2);
  // The body call g -> f is a real call; the default argument's helper() is not recorded as g's call
  // either (the AST places the default argument expression in the declaration, not at the call site;
  // attributing it to the call site would need CXXDefaultArgExpr handling, which is a stated limit).
  const auto from_g = calls_from(facts, "g");
  REQUIRE(from_g.size() == 1);
  CHECK(from_g.front()->callee_qualified == "f");
  CHECK(facts.calls.size() == 1);
}

TEST_CASE("a broken translation unit is reported as failed with compiler diagnostics") {
  const SmokeFacts facts =
      lcm::clang_smoke::collect_facts("int f(int);\nint g() { return f(1, 2); }\n", "broken.cpp", kArgs);
  CHECK_FALSE(facts.parsed_ok);
  REQUIRE_FALSE(facts.diagnostics.empty());
  CHECK(facts.diagnostics.front().find("error:") == 0);
}

TEST_CASE("an unresolvable callee yields no invented target") {
  const SmokeFacts facts = lcm::clang_smoke::collect_facts(
      "int a(int); int b(int);\nint pick(bool c) { int (*fp)(int) = c ? a : b; return fp(1); }\n", "indirect.cpp", kArgs);
  REQUIRE(facts.parsed_ok);
  // fp(1) has no direct callee; the only recorded calls must be direct ones (there are none here).
  CHECK(facts.calls.empty());
}
