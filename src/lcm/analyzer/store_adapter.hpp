// Transfers an actual AnalysisResult into a store Contribution without making
// the store depend on the Clang SDK (PRD FR-IDX-001..003).
//
// The logical slot is supplied by the caller and never derived from the
// command, replay or file name. Facts are transferred unchanged: every fact
// keeps the result's `analysis_unit` label (pass the slot as
// AnalysisRequest::analysis_unit to make them equal), and the store refuses
// a label that another live slot owns. Dependency roles beyond what the
// compiler observed (generated, reflected, ignored) and compile-context
// inputs are caller evidence; nothing is inferred from a successful analysis.
#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "lcm/analyzer/analyzer.hpp"
#include "lcm/store/store.hpp"

namespace lcm::analyzer {

struct ContextInput {
  std::filesystem::path path;  // absolute
  store::InputRole role = store::InputRole::build_configuration;
  // Hash of the bytes captured before the analysis. When present it is stored
  // as is, so a later change is never stamped onto the older analysis; the
  // caller revalidates it before publication. When absent the file is hashed
  // at conversion.
  std::optional<FileContentHash> content_hash;
};

struct ContributionOptions {
  std::string slot;
  PublishPolicy policy = PublishPolicy::accepted_only;
  // Caller-tagged roles keyed by repository-relative generic path; applied to
  // in-root observations other than the main file.
  std::map<std::string, store::InputRole> role_overrides;
  // Compile-context inputs (compiler, build database, configuration,
  // descriptors). One without a captured hash is hashed now; a missing file
  // refuses the conversion.
  std::vector<ContextInput> context_inputs;
  store::BuildCurrency build_currency = store::BuildCurrency::unknown;
  // Settings of the AnalysisRequest that produced the result, recorded in
  // store::AnalysisSettings together with the front-end version and `policy`.
  // Left empty they are recorded as not known.
  std::string repository_member;
  std::filesystem::path resource_dir;
};

// Front-end identity recorded as store::AnalysisSettings::analyzer.
[[nodiscard]] std::string analyzer_identity();

[[nodiscard]] std::string_view to_string(PublishPolicy value);

struct ContributionConversion {
  std::optional<store::Contribution> contribution;
  std::string reason;  // why nothing was converted
};

// Rejected units, and units the policy refuses, convert to nothing; the
// caller records them with Store::record_rejected.
[[nodiscard]] ContributionConversion to_contribution(const AnalysisResult& result, const ContributionOptions& options);

}  // namespace lcm::analyzer
