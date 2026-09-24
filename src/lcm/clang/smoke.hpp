// Clang LibTooling smoke probe.
//
// This is deliberately NOT the semantic analyzer. It exists to prove that the
// pinned prebuilt SDK links and runs, and that the facts it produces come from
// the compiler's AST: declaration/definition distinction, the compiler's
// qualified owner and the overload the compiler selected at each call site.
#pragma once

#include <string>
#include <vector>

namespace lcm::clang_smoke {

struct FunctionFact {
  std::string qualified_name;  // e.g. "geo::Shape::area"
  std::string owner;           // qualified DeclContext, e.g. "geo::Shape" or "geo"; empty for the global namespace
  std::string signature;       // "(<param types>)" plus " const" for const member functions
  bool is_definition = false;
  unsigned line = 0;           // 1-based line in the main file
};

struct CallFact {
  std::string caller_qualified;   // enclosing function definition
  std::string callee_qualified;   // FunctionDecl the compiler resolved (getDirectCallee)
  std::string callee_signature;   // same format as FunctionFact::signature
  bool member_call = false;       // CXXMemberCallExpr
  unsigned line = 0;              // line of the call expression
};

// A call that appears in a function declaration but outside any function
// body: default arguments, member initialisers of declarations without a
// body, and similar. These are NOT attributed to the declaring function as a
// caller (a default argument is evaluated at each call site, not by the
// declaring function); they are listed separately so nothing is invented.
struct OutsideBodyCall {
  std::string declaring_function;  // FunctionDecl whose declaration contains the expression
  std::string callee_qualified;
  std::string callee_signature;
  unsigned line = 0;
};

struct SmokeFacts {
  bool parsed_ok = false;
  std::vector<std::string> diagnostics;  // compiler errors/warnings, formatted
  std::vector<FunctionFact> functions;   // every FunctionDecl redeclaration in the main file
  std::vector<CallFact> calls;           // every CallExpr with a direct callee inside a function BODY
  std::vector<OutsideBodyCall> calls_outside_bodies;  // direct-callee CallExprs outside any body
};

// Parses `code` as file `file_name` with the given compiler arguments (no
// compiler executable is run; the front end runs in-process) and collects
// facts from the resulting AST.
[[nodiscard]] SmokeFacts collect_facts(const std::string& code, const std::string& file_name,
                                       const std::vector<std::string>& compiler_args);

// Version string of the Clang front end linked into this binary.
[[nodiscard]] std::string clang_version();

}  // namespace lcm::clang_smoke
