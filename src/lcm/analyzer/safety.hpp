// Argument safety gate for the in-process front end: a conservative ALLOWLIST.
//
// Only argument forms whose effect on the front end is understood are
// forwarded. Everything else (passthrough wrappers such as -Xclang or
// /clang:, plugin/config/VFS/module-cache/output switches, response files,
// positional tokens, unknown options) rejects the unit with an actionable
// reason while the raw input stays preserved on the CompileCommand. Running
// with a syntax-only action is not treated as a guarantee by itself.
#pragma once

#include <string>
#include <vector>

#include "lcm/compile_context.hpp"

namespace lcm::analyzer {

struct SafetyVerdict {
  bool safe = true;
  std::vector<std::string> rejected;  // "argument: reason"
};

// `compiler` is the actual executable of the ORIGINAL command: an allowlist
// entry that only one compiler of a flavor understands (clang-cl `/imsvc`) is
// accepted for that compiler alone, so a native cl.exe command whose unknown
// token happens to spell like it can never pass.
[[nodiscard]] SafetyVerdict check_analyzer_arguments(const std::vector<std::string>& analyzer_arguments,
                                                     DriverFlavor driver, CompilerKind compiler);

}  // namespace lcm::analyzer
