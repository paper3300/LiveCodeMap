// Stage 2: producer-verified TEXTUAL precompiled-header replay.
//
// What this is. An MSVC `/Yu` command consumes a binary `.pch` that no other
// front end can read. Instead of consuming it, this replays the PCH's content
// TEXTUALLY -- the same header the `/Yu` names is force-included and actually
// parsed -- but only after PROVING that the header produces the same
// preprocessor state under the consumer's context as it did under the
// producer's. Nothing is inferred from file text alone and no degraded check is
// removed to make a unit pass.
//
// What this is NOT. No claim is made that the replayed context equals the
// historical binary `.pch`, nor that the inputs on disk are current. The mode is
// an EXISTING-INPUT TEXTUAL SNAPSHOT: it proves equivalence between the producer
// and consumer prefixes as they are on disk right now, and says so.
//
// How equivalence is decided. Two in-process capture runs preprocess the SAME
// wrapper header, one under the producer's options and one under the consumer's,
// and the window bounded by that header's entry and exit is compared on five
// independent axes:
//
//   1. an ordered PREPROCESSOR EVENT digest -- macro definitions, undefinitions
//      and expansions WITH their actual argument tokens, conditional outcomes,
//      include resolutions and every pragma event. This is the primary
//      invariant. Macro names and definitions alone are not sufficient: a macro
//      can hold different values when a `#pragma push_macro` runs and be
//      undefined again before the boundary, leaving every after-the-fact
//      comparison identical while hidden preprocessor stacks differ;
//   2. the preprocessed token stream;
//   3. the macro table at the boundary, including parameter names, order and
//      variadic flags;
//   4. the `__COUNTER__` value, which `#if __COUNTER__` can advance invisibly;
//   5. the set of files entered, with content hashes.
//
// Because the event digest compares the ordered operations INCLUDING their
// effective arguments, the unreadable `pack` / `warning` / `push_macro` stacks
// are equal whenever it matches, so no stack needs to be read and no balance
// rule is imposed.
//
// Failure is closed: a missing producer, any ladder mismatch, any digest
// difference, an unsupported `/Yu` layout or a binary `/Yu` header all keep the
// unit rejected.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "lcm/compile_context.hpp"
#include "lcm/compile_db.hpp"
#include "lcm/response_file.hpp"

namespace lcm::analyzer {

enum class PchReplayMode : std::uint8_t {
  not_requested,     // the unit has no emulated PCH, or no producer was supplied
  rejected,          // a producer was supplied but equivalence was not established
  textual_snapshot,  // proven equivalent for the inputs as they are on disk now
};

[[nodiscard]] std::string_view to_string(PchReplayMode value);

// One capture run's observable prefix state.
struct PchPrefixCapture {
  std::string event_digest;    // ordered preprocessor events, incl. expansion arguments
  std::string token_digest;    // preprocessed token stream
  std::string macro_digest;    // macro table at the boundary (names, params, variadic, body)
  std::string file_digest;     // entered files and their content hashes
  std::uint32_t counter = 0;   // Preprocessor::getCounterValue()
  std::size_t events = 0;      // event count, for cost reporting
  std::size_t files = 0;       // files entered inside the window
  bool window_seen = false;      // the wrapper header was entered
  bool boundary_reached = false;  // ...and exited, so the state below is the PREFIX state
  std::vector<std::string> diagnostics;  // front-end errors during the capture
};

struct PchReplay {
  PchReplayMode mode = PchReplayMode::not_requested;
  std::string reject_reason;  // set when mode == rejected

  std::string producer_command_id;
  std::string consumer_command_id;
  std::string wrapper_header;   // the `/Yu` header, generic UTF-8
  std::string pch_binary;       // the `/Fp` path, recorded but never consumed
  std::string producer_replay_id;      // producer raw identity, or its response-expansion replay identity
  // The producer's response expansion EXACTLY AS CONSUMED by validation: the
  // files that were read, their bytes and encodings, and every expanded token
  // with its origin. Re-expanding later to report provenance would read the
  // filesystem a second time and could describe different bytes than the ones
  // the decision was made on.
  ResponseExpansion producer_expansion;
  std::string producer_wrapper_include;  // the single include spelling its source contains
  std::string guard_path;       // virtual, in-memory only; never written to disk
  std::string prefix_identity;  // digest over the frozen prefix inputs actually used
  std::vector<std::string> prefix_files;  // files entered inside the verified prefix window

  PchPrefixCapture producer;
  PchPrefixCapture consumer;
  // The same measurement taken during the FINAL parse. It must equal `consumer`
  // on every axis, or the prefix that was analysed is not the prefix that was
  // proven and the unit rejects.
  PchPrefixCapture final_parse;
  // Named explicitly rather than implied: which axes the final parse was held
  // to. The token stream is deliberately absent, because a parse and a
  // preprocess-only capture do not agree on what a token IS once the parser's
  // pragma handlers turn a pragma into one annotation token.
  std::vector<std::string> final_axes_compared;
  std::vector<std::string> final_drift;

  // The analyzer arguments the replay actually used: the order guard first, the
  // one synthetic `/Yu` forced include removed by origin, every other option and
  // every forced include the command itself wrote left in its original place.
  std::vector<std::string> replay_arguments;

  // Axes that differed, empty when mode == textual_snapshot.
  std::vector<std::string> mismatches;
};

}  // namespace lcm::analyzer
