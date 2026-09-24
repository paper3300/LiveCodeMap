// Response-file (`@file`) expansion for a compile command (stage 1).
//
// Scope and honesty: expansion is enabled for ONE verified driver, native
// cl.exe, whose nesting, quoting and encoding behaviour was measured on this
// environment. Every other compiler keeps the previous contract, where an
// unexpanded `@file` degrades the normalized context and rejects the unit.
// No clang-cl support is claimed here.
//
// Measured native cl.exe rules this implements:
//   * a relative `@file` resolves against the PROCESS working directory, which
//     for a compilation-database entry is CompileCommand::directory -- never
//     against the directory of the response file that named it;
//   * referencing one file several times without recursion is legal;
//   * Windows quoting, including backslash-escaped quotes, round-trips;
//   * encodings that behave correctly are ASCII without a BOM, validated UTF-8
//     WITH a BOM, and validated UTF-16LE WITH a BOM. BOM-less non-ASCII bytes
//     MISDECODE on this compiler, so they are refused rather than guessed at.
//
// Failure is atomic: when anything is missing, malformed, cyclic or over a
// limit, no partially expanded argv is produced. The caller keeps the raw
// command, which still carries its `@` token and therefore still rejects.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "lcm/compile_db.hpp"

namespace lcm {

// Byte-level encodings accepted for a response file. Anything else is refused.
enum class ResponseEncoding : std::uint8_t {
  ascii,        // no BOM; every byte is in [0x01, 0x7F]
  utf8_bom,     // EF BB BF, remainder validated as UTF-8
  utf16le_bom,  // FF FE, remainder validated as UTF-16LE and converted to UTF-8
};

[[nodiscard]] std::string_view to_string(ResponseEncoding value);

// Finite bounds on the work one command may cause. Every one of them is a
// rejection when exceeded, never a truncation.
struct ResponseFileLimits {
  unsigned max_depth = 8;                     // nesting levels; a command-line `@file` is level 1
  std::size_t max_file_bytes = 8u << 20;      // one response file
  std::size_t max_total_bytes = 64u << 20;    // all distinct files read; a reused file counts once
  std::size_t max_tokens = 1u << 20;          // tokens in the expanded argv
  // Cumulative work budget: one unit per `@` reference AND one per token
  // examined, counted again on every traversal including snapshot cache hits.
  // The byte and token limits alone do not bound a diamond of empty response
  // files, where references multiply while nothing is read or emitted.
  std::size_t max_visited_tokens = 4u << 20;
};

// One response file that was read, snapshotted exactly once. A later reference
// to the same resolved path reuses this snapshot and its already-tokenized
// contents, so the bytes that were hashed are the bytes that were parsed.
struct ResponseFileSnapshot {
  std::string as_written;          // the `@` token's spelling, without the '@'
  std::filesystem::path resolved;  // absolute, lexically normal
  std::string resolved_utf8;       // generic spelling; participates in the replay identity
  // Other resolved spellings that reached this same file. Reuse is decided by
  // filesystem identity, so a case alias or a hardlink collapses onto one
  // snapshot; the spelling each reference actually used is kept as evidence and
  // participates in the replay identity.
  std::vector<std::string> alias_spellings;
  ResponseEncoding encoding = ResponseEncoding::ascii;
  std::size_t bytes = 0;           // size of the raw file content, before decoding
  std::string content_sha256;      // over those same raw bytes
  unsigned first_depth = 0;        // nesting level of the first reference
};

// Provenance of one token of the expanded argv.
struct ExpandedTokenOrigin {
  std::size_t raw_index = 0;        // index in CompileCommand::arguments this token descends from
  bool from_response_file = false;  // false: the raw argument itself, forwarded unchanged
  std::size_t snapshot = 0;         // index into ResponseExpansion::files (only when from_response_file)
  std::size_t index_in_file = 0;    // token position within that file's own token list
};

struct ResponseExpansion {
  // True when the command contains at least one `@` token AND the compiler is
  // one expansion is enabled for. False means "not applicable": the caller
  // uses the raw command unchanged.
  bool attempted = false;
  // True when every referenced file was read, decoded and tokenized within the
  // limits. `arguments`, `origins` and `replay_id` are populated only then.
  bool ok = false;

  std::vector<std::string> arguments;        // expanded argv, compiler first
  std::vector<ExpandedTokenOrigin> origins;  // exactly one per element of `arguments`
  std::vector<ResponseFileSnapshot> files;   // in first-read order
  std::string replay_id;                     // identity over snapshots, tokens and their origins
  std::vector<std::string> errors;           // why expansion was refused; non-empty iff attempted && !ok
};

// Expands `@file` arguments of `command`. Reads files; writes nothing.
[[nodiscard]] ResponseExpansion expand_response_files(const CompileCommand& command,
                                                      const ResponseFileLimits& limits = {});

}  // namespace lcm
