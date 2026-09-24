// Phase 1A ordinary-C++ analyzer: one explicitly selected compile command plus
// its normalized context -> a staged FactSet contribution for that unit
// (PRD FR-BLD-003/004/005, FR-GPH-001..005, FR-CPP-001..004 subset).
//
// Contract:
//   * Units are analysed only when explicitly selected (by command id, or by
//     file when the database has exactly one command for it). Nothing ever
//     analyses "all entries" implicitly and ambiguity is returned as choices.
//   * Each unit's facts are staged in its own AnalysisResult. Nothing mutates a
//     caller's FactSet; publishing is a separate explicit step that can refuse
//     units with errors or a degraded compile context.
//   * The front end runs in-process on syntax-only settings over a read-only
//     overlay of the real filesystem with the unit's working directory. Only
//     allowlisted argument forms reach it; everything else rejects the unit.
//   * Semantics the increment does not model are recorded as limits or
//     unresolved sites, never as invented confirmed facts.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "lcm/compile_context.hpp"
#include "lcm/compile_db.hpp"
#include "lcm/analyzer/pch_replay.hpp"
#include "lcm/facts.hpp"
#include "lcm/response_file.hpp"
#include "lcm/source.hpp"

namespace lcm::analyzer {

enum class AnalysisStatus : std::uint8_t {
  ok,                     // front end finished without errors; facts staged
  completed_with_errors,  // front end reported errors; facts staged but provisional (recovered
                          // constructs are recorded as unresolved, never as targets)
  rejected,               // nothing ran: unusable/incomplete context or unsafe arguments; no facts
};

struct AnalysisDiagnostic {
  enum class Level : std::uint8_t { note, remark, warning, error, fatal };
  Level level = Level::error;
  std::string message;
  std::optional<std::string> file;  // UTF-8 path as the front end reported it
  unsigned line = 0;
  unsigned column = 0;

  [[nodiscard]] std::string text() const;
};

// A file the preprocessor entered while analysing the unit: dependency input
// for invalidation (PRD FR-IDX-003). Out-of-root files are recorded by path
// and hash only; nothing of their content becomes a public fact.
struct FileObservation {
  std::string path_utf8;                      // absolute, as resolved by the front end
  std::optional<RepoRelativePath> repo_path;  // present when inside the repository root
  bool in_root = false;
  bool is_main_file = false;
  bool is_system = false;
  FileContentHash content_hash;               // of the bytes the compiler saw
};

// One direct #include as written (PRD FR-GPH-004). Transitive closure is
// computed at query time, not stored.
struct IncludeObservation {
  std::string includer_path_utf8;
  std::optional<RepoRelativePath> includer_repo_path;
  std::string spelling;  // text between the delimiters
  bool angled = false;
  std::optional<std::string> resolved_path_utf8;  // nullopt when the include was not found
  std::optional<RepoRelativePath> resolved_repo_path;
  bool resolved_in_root = false;
  unsigned line = 0;
};

struct AnalysisRequest {
  // The actual compile command. Its `command_id` must equal
  // compute_command_id(*command) (or be empty, in which case it is computed);
  // a mismatch rejects the unit. Normalization is performed internally from
  // this command, so no separately supplied context can be paired with a
  // different raw command.
  const CompileCommand* command = nullptr;
  // The command that BUILT the precompiled header this unit uses, supplied
  // explicitly by the caller and never discovered. Without it a unit whose
  // context is degraded by `/Yu` emulation keeps rejecting; with it, the
  // producer and consumer prefixes are compared and the unit is accepted only
  // when they are proven equivalent (see lcm/analyzer/pch_replay.hpp).
  const CompileCommand* pch_producer = nullptr;
  std::filesystem::path repository_root;  // absolute
  std::string repository_member = "repo";
  std::filesystem::path resource_dir;     // pinned Clang resource directory (lib/clang/<major>)
  // Optional logical contribution-unit label. It never replaces provenance:
  // AnalysisResult::command_id always carries the actual command identity.
  std::string analysis_unit;
};

struct AnalysisResult {
  AnalysisStatus status = AnalysisStatus::rejected;
  std::string command_id;     // actual compile-command identity (provenance)
  std::string analysis_unit;  // contribution label used in evidence (== command_id unless a label was given)
  std::vector<AnalysisDiagnostic> diagnostics;
  std::vector<std::string> rejected_arguments;  // safety gate hits (status == rejected)

  // The normalized context derived from the actual command -- from its EXPANDED
  // argv when `expansion.ok`, otherwise from the raw argv. In this bounded
  // increment only `ok` contexts reach the front end: `degraded` (a response
  // file that was not expanded, PCH emulation, unsupported or malformed
  // options) and `unusable` reject before invocation, so staged facts always
  // come from a complete normalized context. `context_complete` is kept
  // explicit for callers and for a later, narrower degraded-acceptance policy.
  // Disposition indices refer to whichever argv was normalized; use
  // `expansion.origins` to map them back to raw arguments.
  NormalizedCompileContext context;
  NormalizationStatus context_status = NormalizationStatus::unusable;
  bool context_complete = false;

  // Response-file expansion of the raw command. When it ran, `context` above
  // was normalized from the EXPANDED argv and every disposition index refers to
  // `expansion.arguments`; `expansion.origins` maps each of those back to the
  // raw argument and the response file it came from. `command_id` always stays
  // the raw command's identity.
  ResponseExpansion expansion;
  // Outcome of textual PCH replay. `not_requested` when the unit emulates no
  // PCH; `rejected` carries why equivalence was not established;
  // `textual_snapshot` means the producer and consumer prefixes were proven
  // equivalent for the inputs as they are on disk now -- never a claim about the
  // historical binary PCH or about input currency.
  PchReplay pch;
  // Identity of the context that was actually analysed: `expansion.replay_id`
  // when a response file was expanded (it covers the bytes of every file read),
  // otherwise `command_id`, since then the raw argv is the whole context.
  std::string replay_id;

  std::vector<FileObservation> files;
  std::vector<IncludeObservation> includes;
  std::vector<std::string> limits;  // semantics deliberately not modelled that this unit touched

  FactSet facts;  // staged contribution of this unit only; empty when rejected

  [[nodiscard]] bool has_errors() const;
};

// Analyses one translation unit. Never touches any caller-owned FactSet.
[[nodiscard]] AnalysisResult analyze_translation_unit(const AnalysisRequest& request);

enum class PublishPolicy : std::uint8_t {
  accepted_only,               // status ok AND context_complete
  allow_errors_and_degraded,   // caller explicitly accepts provisional facts
};

struct PublishOutcome {
  bool published = false;
  std::string reason;  // why nothing was published
  std::size_t symbol_locations = 0;
  std::size_t relation_evidence = 0;
  std::size_t unresolved_sites = 0;
  std::size_t placeholders = 0;
  std::size_t captures = 0;
  std::size_t direct_includes = 0;
};

// Copies a unit's staged facts into `target` through the public FactSet API.
// Rejected units never publish. Replacement of a previous contribution of the
// same unit is the storage layer's job (Phase 2); this only adds.
PublishOutcome publish_contribution(FactSet& target, const AnalysisResult& result,
                                    PublishPolicy policy = PublishPolicy::accepted_only);

// Explicit unit selection. Ambiguity is reported, never resolved silently.
struct UnitSelection {
  std::vector<const CompileCommand*> selected;  // in database order
  std::vector<std::string> unknown_command_ids;
  struct Ambiguity {
    std::filesystem::path file;
    std::vector<const CompileCommand*> candidates;
  };
  std::vector<Ambiguity> ambiguous;               // files with several distinct commands: not selected
  std::vector<std::filesystem::path> missing_files;  // files without any command
};

[[nodiscard]] UnitSelection select_by_command_ids(const CompilationDatabase& database,
                                                  const std::vector<std::string>& command_ids);

// Selects the unique command of each file; files with several distinct
// command identities are returned as `ambiguous` and left unselected.
[[nodiscard]] UnitSelection select_by_files(const CompilationDatabase& database,
                                            const std::vector<std::filesystem::path>& files);

struct DatabaseAnalysisOptions {
  std::filesystem::path repository_root;
  std::string repository_member = "repo";
  std::filesystem::path resource_dir;
};

// Analyses exactly the selected units, each independently, in order.
[[nodiscard]] std::vector<AnalysisResult> analyze_selected(const UnitSelection& selection,
                                                           const DatabaseAnalysisOptions& options);

}  // namespace lcm::analyzer
