// Selected-profile multi-TU indexing: an explicit, authoritative set of
// compile actions of one build profile -> actual analyses -> one atomic store
// publication (PRD FR-BLD-002..004, FR-IDX-002/003, FR-UE-006 subset).
//
// Contract:
//   * The caller supplies the complete selection: stable logical slots, the
//     source (or synthetic unity file plus its original member files) of each
//     slot and, where the database has several commands for a source, the
//     command identity. Nothing selects every database entry, discovers
//     sources or chooses among alternatives. Missing, ambiguous, duplicate or
//     inconsistent selection input rejects the whole run before any analysis
//     and never retires a committed slot. An empty selection retires every
//     slot only when it is declared intentional and the database is valid.
//   * Change detection is compiler-free: committed provenance, analysis
//     settings (front end, resource directory, repository member, policy),
//     source/member roles, compile-context inputs, input roles and build
//     currency are compared, and every committed dependency is rehashed. The
//     compile database is read, parsed and validated on every run but schedules
//     a slot only through its own selected entry and PCH producer entry (their
//     command IDs) and their response files, not through the file's bytes. Unchanged slots
//     are neither analysed nor republished; committed slots absent from a
//     valid selection are retired together with the replacements.
//   * Context inputs (the compile database and caller-supplied toolchain/
//     configuration/descriptor files) are hashed before analysis and those
//     hashes are stored. Analysis runs without the writer lock. Under the lock
//     the fact generation, the context inputs, every input observed by the new
//     analyses and the committed inputs of unchanged slots are revalidated;
//     any difference publishes nothing.
//   * All-or-nothing: a failed analysis, mapping check, revalidation or
//     publication leaves committed facts, generation and last success as they
//     were and records a failed attempt for the involved slots. A first run of
//     a new profile that fails creates the empty profile to record it.
//   * Build-action currency is caller evidence and never inferred; this
//     pipeline does not persist freshness states.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "lcm/analyzer/analyzer.hpp"
#include "lcm/analyzer/store_adapter.hpp"
#include "lcm/compile_db.hpp"
#include "lcm/store/store.hpp"

namespace lcm::analyzer {

// One explicitly selected compile action of the profile.
struct SelectedAction {
  std::string slot;  // stable logical identity; never derived from command, replay or file names
  // The command's file (relative paths resolve against the workspace root).
  // For a unity action this is the synthetic unity file.
  std::filesystem::path source;
  // Required when the database has several distinct commands for `source`;
  // when given it must name a command for `source`.
  std::optional<std::string> command_id;
  // Non-empty: `source` is a synthetic unity file and these are the original
  // files it includes directly, all inside the workspace root. The mapping is
  // checked against what the analyzer observed. Nested/intermediate unity
  // fragments are not supported: a member observed directly including another
  // translation-unit source (.cpp/.cc/.cxx/.c, also .gen.cpp), or a mapping that
  // differs from the unity file's direct includes, fails. A member consisting
  // only of includes is an original like any other.
  std::vector<std::filesystem::path> unity_members;
  // Explicit PCH producer (see AnalysisRequest::pch_producer). It must be in
  // the database and the unit must actually use it.
  std::optional<std::string> pch_producer_command_id;
  // Context inputs of this action only (e.g. its module's Build.cs), in
  // addition to the request's; a change reanalyses only the actions naming it.
  std::vector<ContextInput> context_inputs;
  // Overrides ProfileIndexRequest::build_currency for this action.
  std::optional<store::BuildCurrency> build_currency;
};

// Points at which a test harness observer runs; it may mutate files or throw.
enum class IndexPhase : std::uint8_t {
  planned,   // selection validated and changes detected; nothing analysed
  analyzed,  // every planned analysis finished (possibly none); writer lock not yet taken
  locked,    // writer lock held; revalidation not yet started (also for an unchanged run)
};

struct ProfileIndexRequest {
  std::string profile;
  bool uht_applicable = false;  // must equal an existing profile's applicability
  std::filesystem::path compile_database;  // explicit; tracked as a build_database input
  CommandSyntax syntax = CommandSyntax::auto_detect;
  // The complete authoritative selection of the profile.
  std::vector<SelectedAction> actions;
  // An empty `actions` is accepted (retiring every committed slot) only when
  // this is set; it must not be set with a non-empty selection.
  bool empty_selection_intended = false;
  // Toolchain, configuration and descriptor files (roles compiler,
  // build_configuration, project_descriptor); applied to every slot. The
  // compile database is added as a build_database input of every slot.
  std::vector<ContextInput> context_inputs;
  // Caller-tagged roles (generated, reflected, ignored) by repository-relative
  // generic path. Unity members always have the source role.
  std::map<std::string, store::InputRole> role_overrides;
  store::BuildCurrency build_currency = store::BuildCurrency::unknown;  // default for every action
  PublishPolicy policy = PublishPolicy::accepted_only;
  std::string repository_member = "repo";
  std::filesystem::path resource_dir;  // pinned Clang resource directory
  std::chrono::milliseconds lock_wait{5000};
  // Maximum number of analyses run at the same time (0 and 1: one after the
  // other). Each analysis has its own filesystem view and front end; results
  // are reported in selection order and published in one batch.
  std::size_t jobs = 1;
  // Test harness hooks, not configuration.
  std::function<void(IndexPhase)> observer;
  store::PublishObserver publish_observer;
};

enum class IndexOutcome : std::uint8_t {
  committed,  // installs and/or retirements published; fact generation advanced once
  // Every selected slot matches its committed unit, revalidated under the
  // writer lock; nothing analysed or published. An intended empty selection
  // of a new profile creates the (empty) profile.
  unchanged,
  rejected,   // selection, database or context input invalid; nothing analysed or published
  failed,     // analysis, mapping, revalidation or publication failed; nothing published
  busy,       // the writer lock was not obtained (also for an unchanged run); nothing published
};

[[nodiscard]] std::string_view to_string(IndexOutcome value);

enum class SlotPlan : std::uint8_t {
  new_action,  // no committed unit
  changed,     // committed unit differs (see reasons)
  unchanged,
  retired,     // committed, absent from the authoritative selection
};

[[nodiscard]] std::string_view to_string(SlotPlan value);

enum class SlotResult : std::uint8_t {
  installed,
  unchanged,
  retired,
  failed,         // this slot's analysis or mapping failed
  not_published,  // planned work was not published because the run failed
  not_analyzed,   // planned analysis not started because another slot failed
};

[[nodiscard]] std::string_view to_string(SlotResult value);

struct SlotReport {
  std::string slot;
  SlotPlan plan = SlotPlan::new_action;
  SlotResult result = SlotResult::not_published;
  std::optional<std::string> command_id;  // the selected command
  std::vector<std::string> reasons;       // why changed, or why it failed
};

struct ProfileIndexReport {
  IndexOutcome outcome = IndexOutcome::rejected;
  std::string message;
  std::vector<std::string> errors;  // selection/database/context problems (rejected)
  std::vector<SlotReport> slots;    // selection order, then retired slots
  std::size_t planned_analyses = 0;
  std::size_t actual_analyses = 0;
  std::int64_t generation_before = 0;
  std::int64_t generation_after = 0;
  std::optional<std::int64_t> attempt_id;  // publication or recorded failure
  bool failure_recorded = false;

  [[nodiscard]] const SlotReport* slot(std::string_view name) const;
};

// Runs one selected-profile indexing pass against `store` (whose workspace
// root is the repository root of every analysis).
[[nodiscard]] ProfileIndexReport index_profile(store::Store& store, const ProfileIndexRequest& request);

}  // namespace lcm::analyzer
