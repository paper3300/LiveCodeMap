// Conservative compile-context normalization (PRD FR-BLD-003).
//
// Turns the argv it is given into the argument list an analyzer front end
// should receive, while keeping the CompileCommand untouched and recording one
// disposition per argument of `command.arguments`: what was kept, what was
// dropped and why, what was transformed into what.
//
// `@file` arguments are NOT expanded here; this function sees whatever argv it
// is handed. An unexpanded `@file` is reported as unsupported and degrades the
// context. Callers that support response files expand first (see
// lcm/response_file.hpp) and normalize the EXPANDED argv, in which case each
// disposition indexes that expanded argv and ResponseExpansion::origins maps it
// back to the raw argument and the file it came from.
//
// Scope and honesty: this recognises a documented subset of MSVC (cl.exe,
// clang-cl) and GNU-style (clang, clang++, gcc, g++, cc, c++) options. Options
// it does not know are KEPT and reported as `kept_unknown`; options it knows
// to be semantically irrelevant for parsing (outputs, dependency files,
// warnings, debug info, build-process flags, linker input) are dropped with a
// reason; options it knows it cannot honour (managed C++, precompiled-header
// binaries, unexpanded response files) are reported as unsupported and the
// context is marked `degraded`. Nothing is discarded silently and no claim of
// complete toolchain coverage is made.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "lcm/compile_db.hpp"

namespace lcm {

enum class DriverFlavor : std::uint8_t {
  unknown,
  msvc,  // cl.exe, clang-cl
  gnu,   // clang, clang++, gcc, g++, cc, c++
};

// The actual compiler executable named by argv[0]. The flavor says how the
// argument SYNTAX is parsed; the kind says whose verified driver semantics
// apply to raw syntax the flavors share only partly: native cl.exe ignores
// `--` (D9002) and has no -imsvc, clang-cl honours both; gcc/g++/cc/c++ are
// GNU-syntax aliases whose end-of-options behaviour is not verified here, so
// Clang-only raw syntax is never assumed for them.
enum class CompilerKind : std::uint8_t {
  unknown,
  native_cl,  // cl.exe
  clang_cl,   // clang-cl
  clang,      // clang, clang++ (verified Clang GNU-style driver)
  gnu_alias,  // gcc, g++, cc, c++
};

enum class OptionAction : std::uint8_t {
  compiler,            // argv[0]
  source_input,        // the entry's source file; supplied to the analyzer separately
  kept,                // recognised, semantically relevant, forwarded
  kept_unknown,        // not recognised; forwarded unchanged so nothing is lost silently
  transformed,         // replaced by an equivalent the analyzer can honour (see replacement)
  dropped,             // recognised as irrelevant to parsing; removed with a reason
  dropped_unsupported, // recognised but cannot be honoured; removed and context degraded
};

enum class OptionCategory : std::uint8_t {
  compiler,
  source,
  define,
  undefine,
  include,
  forced_include,
  language_standard,
  language,
  runtime_library,
  semantics,  // exceptions, RTTI, conformance, charset, target/arch, packing and similar
  optimization,
  output,
  dependency_generation,
  diagnostics,
  debug_info,
  build_process,
  codegen,
  precompiled_header,
  response_file,
  linker,
  structural,  // the `--` end-of-options delimiter of a verified Clang-family driver
  unknown,
};

// Include search group of a directory option, as the driver places it. One
// authoritative field; `is_system_include` derives the system predicate.
enum class IncludeKind : std::uint8_t {
  user,             // -I, /I
  quote,            // -iquote
  system,           // -isystem, /external:I
  internal_system,  // clang-cl -imsvc: as if part of %INCLUDE% (after -isystem, before the toolchain dirs)
  after,            // -idirafter
};

[[nodiscard]] constexpr bool is_system_include(IncludeKind kind) {
  return kind == IncludeKind::system || kind == IncludeKind::internal_system || kind == IncludeKind::after;
}

enum class NormalizationStatus : std::uint8_t {
  ok,
  degraded,  // some semantic input could not be honoured (see dispositions/notes)
  unusable,  // driver not recognised; nothing can be classified
};

struct OptionDisposition {
  std::size_t index = 0;  // position in the normalized command.arguments (expanded argv when the caller expanded)
  std::string raw;
  OptionAction action = OptionAction::kept;
  OptionCategory category = OptionCategory::unknown;
  std::string reason;                    // human-readable explanation
  std::vector<std::string> replacement;  // analyzer tokens this argument produced (kept/transformed)
};

struct IncludeDirectory {
  std::string as_written;
  std::filesystem::path resolved;  // against the command directory
  IncludeKind kind = IncludeKind::user;
};

struct NormalizedCompileContext {
  DriverFlavor driver = DriverFlavor::unknown;
  CompilerKind compiler_kind = CompilerKind::unknown;
  std::string compiler;                         // raw argv[0]
  std::vector<std::string> analyzer_arguments;  // normalized options; no compiler, no source file

  std::vector<std::string> defines;
  std::vector<std::string> undefines;
  std::vector<IncludeDirectory> includes;
  std::vector<std::string> forced_includes;
  std::optional<std::string> language_standard;  // e.g. "c++20"
  std::optional<std::string> language;           // e.g. "c++" from /TP or -x
  std::optional<std::string> runtime_library;    // MSVC: MD, MDd, MT, MTd, LD, LDd

  // True when exactly one argument was identified as the entry's source file
  // (by path or filesystem identity). When false the context is `degraded`
  // and an analyzer must not append the source itself: the argv would then
  // describe a compile context the database never contained.
  bool source_identified = false;

  bool pch_emulated = false;                          // /Yu turned into a forced include
  std::vector<std::string> unexpanded_response_files;  // @file arguments left unexpanded
  std::vector<OptionDisposition> dispositions;         // exactly one per element of command.arguments, in order
  NormalizationStatus status = NormalizationStatus::ok;
  std::vector<std::string> notes;
};

[[nodiscard]] std::string_view to_string(DriverFlavor value);
[[nodiscard]] std::string_view to_string(CompilerKind value);
[[nodiscard]] std::string_view to_string(OptionAction value);
[[nodiscard]] std::string_view to_string(OptionCategory value);
[[nodiscard]] std::string_view to_string(NormalizationStatus value);
[[nodiscard]] std::string_view to_string(IncludeKind value);

[[nodiscard]] DriverFlavor detect_driver(std::string_view compiler_argv0);
[[nodiscard]] CompilerKind detect_compiler(std::string_view compiler_argv0);

[[nodiscard]] NormalizedCompileContext normalize_compile_context(const CompileCommand& command);

}  // namespace lcm
