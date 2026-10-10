// Opaque continuation cursors (contract R2/R3, D3).
//
//   lcq1.<base64url(payload)>.<base64url(first 16 bytes of SHA-256("lcm-query-cursor-v1" || payload))>
//
// The payload is canonical JSON (sorted keys) binding the tool, the database
// identity, the profile, the fact and ranking generations, a digest of the
// normalized binding arguments and the digest of the effective withheld-file
// set, plus the tool's position. The checksum detects corruption and edits; it
// is not a secret: a self-consistent forged cursor can only select another
// position of the same public result, because every page re-applies
// visibility.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace lcm::query {

struct CursorBinding {
  std::string tool;
  std::string db;       // database identity digest
  std::string profile;
  std::int64_t fact_generation = 0;
  std::int64_t ranking_generation = 0;
  std::string args;     // digest of the normalized binding arguments
  std::string withheld; // digest of the effective withheld-file set

  friend bool operator==(const CursorBinding&, const CursorBinding&) = default;
};

[[nodiscard]] std::string encode_cursor(const CursorBinding& binding, const nlohmann::json& position);

struct DecodedCursor {
  CursorBinding binding;
  nlohmann::json position;
};

// nullopt for a malformed or altered cursor (`invalid_cursor`).
[[nodiscard]] std::optional<DecodedCursor> decode_cursor(std::string_view text);

// First 16 bytes of SHA-256 as lowercase hex.
[[nodiscard]] std::string short_digest(std::string_view text);

}  // namespace lcm::query
