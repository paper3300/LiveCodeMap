// Lossless compact binary form of a store::Contribution: every field,
// including display spellings (`as_written`), which fact hashes ignore. It is a
// private transfer format between analysis and publication inside one run
// (ContributionSpool); it is never kept across runs or read by another build.
#pragma once

#include <string>
#include <string_view>

#include "lcm/store/store.hpp"

namespace lcm::store {

// Strings and digests are interned per contribution, so a unit label, a file
// path or a content digest repeated on thousands of rows is stored once.
[[nodiscard]] std::string encode_contribution(const Contribution& contribution);

// Rebuilds the contribution through the public FactSet API; throws StoreError
// on truncated or malformed bytes.
[[nodiscard]] Contribution decode_contribution(std::string_view bytes);

}  // namespace lcm::store
