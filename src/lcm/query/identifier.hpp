// Identifier normalization and component splitting for the identifier tiers
// of find_code (PRD FR-RET-003, contract R9/D7).
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace lcm::query {

// Leading and trailing ASCII whitespace removed.
[[nodiscard]] std::string trim(std::string_view text);

// Lower-cased components of an identifier, qualified name or path. Every
// ASCII character that is not a letter or digit separates components (so
// `_`, `-`, `::`, `/`, `.` and spaces are boundaries). Inside a word a
// boundary falls between lower case and upper case (`tryConnect`), before the
// last upper-case letter of an acronym followed by lower case (`XMLLoad` ->
// `xml`, `load`) and between letters and digits (`Vec3d` -> `vec`, `3`, `d`).
// Bytes >= 0x80 (UTF-8) are kept verbatim as word characters and never split.
[[nodiscard]] std::vector<std::string> identifier_components(std::string_view text);

// True when `needle` is non-empty and occurs as a contiguous run in `haystack`.
[[nodiscard]] bool contains_run(const std::vector<std::string>& haystack, const std::vector<std::string>& needle);

}  // namespace lcm::query
