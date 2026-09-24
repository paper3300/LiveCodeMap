// Per-layer index state and user-facing status summary (PRD FR-IDX-005).
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace lcm {

enum class Layer : std::uint8_t {
  structure,
  compile_context,
  uht_context,
  semantic_graph,
  retrieval,
  deep,
};

enum class Availability : std::uint8_t {
  missing,  // never produced
  partial,  // some units failed or were skipped; committed results exist
  ready,    // complete committed result
  failed,   // last attempt failed and nothing committed is usable
};

enum class Freshness : std::uint8_t {
  unknown,
  fresh,
  stale,
};

enum class UserStatus : std::uint8_t {
  ready,
  partial,
  stale,
  missing,
  failed,
};

using Timestamp = std::chrono::sys_seconds;

struct UnitCounts {
  std::uint32_t total = 0;
  std::uint32_t succeeded = 0;
  std::uint32_t failed = 0;
  std::uint32_t stale = 0;
  std::uint32_t ignored = 0;

  friend bool operator==(const UnitCounts&, const UnitCounts&) = default;
};

struct LayerState {
  Layer layer = Layer::structure;
  bool applicable = true;  // false: layer does not apply to this project kind (e.g. UHT for plain C++)
  Availability availability = Availability::missing;
  Freshness freshness = Freshness::unknown;
  std::optional<Timestamp> last_success;
  std::optional<Timestamp> last_attempt;
  UnitCounts units;
  std::string reason;  // human-readable cause for non-ready states or non-applicability
};

[[nodiscard]] std::string_view to_string(Layer value);
[[nodiscard]] std::string_view to_string(Availability value);
[[nodiscard]] std::string_view to_string(Freshness value);
[[nodiscard]] std::string_view to_string(UserStatus value);

[[nodiscard]] LayerState not_applicable(Layer layer, std::string reason);

// User status of one applicable layer; nullopt when the layer is not
// applicable. Priority: failed > missing > stale > partial > ready.
// Freshness that has not been verified (`unknown`) is reported as `stale`:
// only a verified `fresh` layer may be presented as ready or partial.
[[nodiscard]] std::optional<UserStatus> summarize_layer(const LayerState& state);

// Overall status across layers. Non-applicable layers never count. A failed
// or missing structure layer decides the result; otherwise any stale layer
// gives `stale`, any other non-ready layer gives `partial`, else `ready`.
// With no applicable layers the result is `missing`.
[[nodiscard]] UserStatus summarize_overall(std::span<const LayerState> layers);

}  // namespace lcm
