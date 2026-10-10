// Normalized SQLite storage of TU contributions with atomic replacement,
// attempt/generation state, an OS writer lock, committed-snapshot readers and
// bounded dependency freshness (PRD FR-IDX-001..005).
//
// Ownership model:
//   * One database belongs to one explicit workspace root. Every query and
//     mutation names a profile (a stable selected-build key); nothing chooses
//     among alternative commands.
//   * A logical unit is (profile, slot). The slot is a caller-supplied stable
//     identity of the selected compile action, independent of command, replay
//     or option hashes. Command/context/replay identities are provenance of the
//     committed contribution; installing a contribution under an existing slot
//     replaces that slot's previous contribution in the same transaction.
//   * Facts stay owned by their unit and keep their original repository-
//     relative file/span/hash. Shared symbols, relations and files are common
//     rows that are pruned only when the last owning contribution disappears.
//   * Storage never infers freshness from a successful analysis: dependency
//     roles, context inputs and build-action currency are caller evidence.
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "lcm/facts.hpp"
#include "lcm/source.hpp"
#include "lcm/status.hpp"

struct sqlite3;

namespace lcm::store {

inline constexpr int kSchemaVersion = 3;

// Raised for unusable databases: unsupported/newer schema, foreign file,
// workspace mismatch, SQLite errors outside a guarded publication.
class StoreError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

enum class JournalMode : std::uint8_t {
  wal,       // preferred; requires same-host shared memory
  rollback,  // DELETE journal; readers can make a commit wait (busy) but never see partial state
};

[[nodiscard]] std::string_view to_string(JournalMode value);

// What a dependency is to the unit that observed it. Public visibility is a
// separate input (see ReadSnapshot::load_facts); a role never hides a file.
enum class InputRole : std::uint8_t {
  source,               // the unit's main file
  header,
  inline_file,          // .inl
  generated,            // caller-tagged generated file
  reflected_header,     // caller-tagged UHT input
  ignored_dependency,   // caller-tagged ignored file that still affects semantics
  external_dependency,  // outside the repository root
  response_file,
  compiler,
  build_database,       // compile_commands.json and equivalents
  build_configuration,  // CMake cache/configure inputs, Build.cs, Target.cs
  project_descriptor,   // .uproject/.uplugin and equivalents
};

[[nodiscard]] std::string_view to_string(InputRole value);
// Compile-context inputs invalidate the compile context and dependent semantics.
[[nodiscard]] bool is_context_input(InputRole role);

// One file the unit depended on, with the hash of the bytes it depended on.
struct InputObservation {
  std::string path_utf8;                      // absolute
  std::optional<RepoRelativePath> repo_path;  // present when inside the workspace root
  InputRole role = InputRole::header;
  FileContentHash content_hash;
  bool is_system = false;
};

// One direct include as the preprocessor saw it. A command-forced include
// (/FI, -include) has no includer: `includer_path_utf8` is nullopt and no
// source file is made its owner.
struct IncludeEdge {
  std::optional<std::string> includer_path_utf8;
  std::string spelling;
  bool angled = false;
  std::optional<std::string> resolved_path_utf8;
  unsigned line = 0;

  friend bool operator==(const IncludeEdge&, const IncludeEdge&) = default;
};

struct UnitDiagnostic {
  std::string level;  // note|remark|warning|error|fatal
  std::string message;
  std::optional<std::string> file;
  unsigned line = 0;
  unsigned column = 0;

  friend bool operator==(const UnitDiagnostic&, const UnitDiagnostic&) = default;
};

enum class BuildCurrency : std::uint8_t {
  unknown,   // nobody verified that the selected command is the current build action
  verified,  // the caller verified it (never inferred from successful analysis)
};

[[nodiscard]] std::string_view to_string(BuildCurrency value);

struct Provenance {
  std::string command_id;                // raw compile command identity (required)
  std::optional<std::string> context_id;  // normalized/expanded context identity, when available
  std::string replay_id;                 // identity of the analysed context (required)
  std::optional<std::string> pch_producer_command_id;
  std::optional<std::string> pch_producer_replay_id;

  friend bool operator==(const Provenance&, const Provenance&) = default;
};

// Settings of the analysis that produced a contribution, beyond the command
// and its inputs, that change the facts produced: stored as explicit columns so
// a caller can tell whether a differently requested analysis is committed.
// Empty strings mean "not recorded".
struct AnalysisSettings {
  std::string analyzer;           // front-end identity, e.g. "clang 22.1.0"
  std::string resource_dir;       // pinned front-end resource directory (generic UTF-8)
  std::string repository_member;  // member named by in-root canonical keys
  std::string publish_policy;     // policy under which the facts were accepted

  friend bool operator==(const AnalysisSettings&, const AnalysisSettings&) = default;
};

// Fingerprint slots are typed and independent. Only slots actually derived
// are populated; the others are stored as explicitly unavailable.
enum class FingerprintKind : std::uint8_t {
  file_bytes,          // over the unit's (path, content hash) inputs
  declaration_api,     // over the declared stable IDs, their roles and their files
  symbol_body,         // over the definition body hashes
  compile_action,      // over command/context/replay identities and analysis settings
  retrieval_document,  // not produced by this increment
  deep_input,          // not produced by this increment
};

[[nodiscard]] std::string_view to_string(FingerprintKind value);

struct Fingerprint {
  FingerprintKind kind = FingerprintKind::file_bytes;
  std::optional<std::string> value;  // nullopt: unavailable
};

// The staged result of one unit, ready to replace the unit's previous
// contribution. Every fact must carry `fact_label` as its analysis unit.
struct Contribution {
  std::string slot;
  std::string fact_label;
  Provenance provenance;
  AnalysisSettings settings;
  std::string analysis_status;  // "ok" | "completed_with_errors"
  BuildCurrency build_currency = BuildCurrency::unknown;
  FactSet facts;
  std::vector<InputObservation> inputs;
  std::vector<IncludeEdge> includes;
  std::vector<UnitDiagnostic> diagnostics;
  std::vector<std::string> limits;
};

// Finished contributions held on disk until one publication installs them, so
// a run keeps only the analyses in flight in memory. Each contribution is
// validated as publish() would validate it when it is added, stored in a
// compact lossless form (contribution_codec.hpp) and checked against the digest
// of what was written when it is read back. The spool file is created in
// `directory`, held exclusively and deleted by the operating system when the
// spool is destroyed or its process dies; leftovers from a power loss are
// reclaimed by later spools and writer recovery once nobody holds them.
class ContributionSpool {
 public:
  explicit ContributionSpool(const std::filesystem::path& directory);
  ~ContributionSpool();
  ContributionSpool(const ContributionSpool&) = delete;
  ContributionSpool& operator=(const ContributionSpool&) = delete;

  // Validates and appends `contribution`; returns why it was refused (nothing
  // stored) or an empty string, and the entry index through `entry`. Several
  // threads may add at once.
  [[nodiscard]] std::string add(const Contribution& contribution, std::size_t* entry = nullptr);
  [[nodiscard]] std::size_t size() const;
  [[nodiscard]] std::string slot(std::size_t entry) const;
  [[nodiscard]] std::string fact_label(std::size_t entry) const;
  // Reads one entry back; throws StoreError when the bytes differ from those written.
  [[nodiscard]] Contribution load(std::size_t entry) const;
  [[nodiscard]] const std::filesystem::path& path() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Installs contributions (replacing the same slots) and retires slots in one
// transaction. A slot may appear once; installing and retiring the same slot
// is rejected as ambiguous.
struct Batch {
  std::string profile;
  std::vector<Contribution> install;
  std::vector<std::string> retire;
  // Further installs read one at a time from `spool` (entries in this order),
  // after `install`; the spool must outlive the publication.
  const ContributionSpool* spool = nullptr;
  std::vector<std::size_t> spooled;
};

enum class AttemptState : std::uint8_t {
  in_progress,
  succeeded,
  failed,
  interrupted,  // an in-progress attempt found by writer recovery; it has no completion time
};

[[nodiscard]] std::string_view to_string(AttemptState value);

struct AttemptInfo {
  std::int64_t id = 0;
  Layer layer = Layer::semantic_graph;
  AttemptState state = AttemptState::in_progress;
  Timestamp started{};
  std::optional<Timestamp> finished;
  std::string message;
};

enum class PublishOutcome : std::uint8_t {
  committed,  // generation advanced by exactly one
  no_op,      // nothing to install and nothing existing to retire; no attempt, no generation change
  rejected,   // validation refused the batch before mutation; failed attempt recorded
  failed,     // mutation failed and was rolled back; failed attempt recorded
};

[[nodiscard]] std::string_view to_string(PublishOutcome value);

struct PublishResult {
  PublishOutcome outcome = PublishOutcome::no_op;
  std::string message;
  std::int64_t fact_generation = 0;  // committed generation after the call
  std::optional<std::int64_t> attempt_id;
};

// Points inside a publication at which a test harness observer runs. An
// observer may throw to inject a failure or block/terminate the process.
enum class PublishPhase : std::uint8_t {
  attempt_recorded,  // in-progress attempt durable; replacement transaction not started
  old_removed,       // inside the transaction: previous contributions deleted, nothing inserted
  before_commit,     // inside the transaction: everything written, not committed
};

using PublishObserver = std::function<void(PublishPhase)>;

// Freshness inspection ---------------------------------------------------------

enum class InspectionOutcome : std::uint8_t {
  complete,            // both passes finished; results were published
  budget_exhausted,    // not published; unchecked units are unknown, detected changes stale
  lock_busy,           // another writer holds the lock; not published
  generation_changed,  // a publication happened between snapshot and lock; not published
};

[[nodiscard]] std::string_view to_string(InspectionOutcome value);

enum class ChangeKind : std::uint8_t {
  modified,
  deleted,
  changed_during_check,  // the two passes disagreed
};

[[nodiscard]] std::string_view to_string(ChangeKind value);

struct InputChange {
  std::string path_utf8;
  std::optional<RepoRelativePath> repo_path;
  InputRole role = InputRole::header;
  ChangeKind kind = ChangeKind::modified;
  std::vector<std::string> slots;  // dependent units in the profile
};

struct UnitFreshness {
  std::string slot;
  Freshness semantic = Freshness::unknown;
  Freshness compile_context = Freshness::unknown;
  std::optional<Freshness> uht;  // nullopt when UHT does not apply to the profile
  std::vector<std::string> reasons;
  // When the state was persisted by an inspection; nullopt for a state that
  // was never persisted (including every result of an incomplete inspection).
  std::optional<Timestamp> checked_at;
};

struct InspectRequest {
  std::string profile;
  std::chrono::milliseconds budget{2000};
  std::chrono::milliseconds lock_wait{250};
  // Test harness hooks. `before_probe` runs before each file of pass 1
  // (unlocked) and pass 2 (under the writer lock); `after_first_pass` runs
  // before lock acquisition; `before_publish` runs after the locked recheck,
  // before the freshness transaction.
  std::function<void(int pass, const std::string& path_utf8)> before_probe;
  std::function<void()> after_first_pass;
  std::function<void()> before_publish;
};

struct FreshnessReport {
  InspectionOutcome outcome = InspectionOutcome::complete;
  // A complete inspection persisted its per-unit states.
  bool published = false;
  // An incomplete inspection that could take the writer lock lowered every
  // persisted `fresh` state of the profile to `unknown` (and recorded the
  // stale units it detected), so no older verification is presented as current.
  bool persisted_downgrade = false;
  std::int64_t fact_generation = 0;
  std::size_t inputs_total = 0;    // distinct input files of the profile
  std::size_t inputs_checked = 0;  // distinct files checked in the last pass that ran
  std::vector<InputChange> changes;
  std::vector<UnitFreshness> units;
  std::string guidance;  // explicit `livecodemap build` guidance when anything is not fresh
  std::chrono::milliseconds elapsed{0};
};

// Read side -----------------------------------------------------------------

struct UnitRecord {
  std::string slot;
  std::string fact_label;
  Provenance provenance;
  AnalysisSettings settings;
  std::string analysis_status;
  BuildCurrency build_currency = BuildCurrency::unknown;
  std::int64_t committed_generation = 0;
  std::int64_t attempt_id = 0;
  std::vector<InputObservation> inputs;  // sorted by path key
  std::vector<IncludeEdge> includes;     // contribution order
  std::vector<UnitDiagnostic> diagnostics;
  std::vector<std::string> limits;
  std::vector<Fingerprint> fingerprints;  // one per FingerprintKind, in enum order
  UnitFreshness freshness;                // unknown until an inspection publishes
};

struct ProfileStatus {
  std::string profile;
  std::int64_t fact_generation = 0;
  std::int64_t ranking_generation = 0;
  std::optional<AttemptInfo> last_attempt;  // latest semantic-graph publication attempt
  std::optional<AttemptInfo> last_success;
  std::vector<std::string> failed_slots;    // latest attempt for the slot failed or was interrupted
  std::vector<LayerState> layers;           // one per Layer value, in enum order
};

enum class Projection : std::uint8_t {
  all,      // every committed fact, including withheld files (internal/invalidation use)
  // Withholds facts located in (or, for includes, naming) files that are
  // explicitly excluded or that do not exist under the workspace root at the
  // time of the call, whatever the last freshness inspection concluded. Only
  // symbols with a visible location and pure external placeholders are
  // public: a symbol whose committed locations are all withheld, a
  // location-less endpoint (its owners were retired or replaced), an
  // internal-linkage symbol scoped to a withheld file and a local symbol
  // under a withheld enclosing symbol are withheld entirely, together with
  // every incident relation, unresolved site and capture. Other units'
  // visible locations and evidence stay.
  public_,
};

class ReadSnapshot;
class WriterLock;

enum class UnitDetail : std::uint8_t {
  full,     // everything in UnitRecord
  inputs,   // no include edges, diagnostics, limits, fingerprints or freshness
  summary,  // freshness only: no inputs, include edges, diagnostics, limits or fingerprints
};

struct StoreOptions {
  JournalMode journal = JournalMode::wal;
  std::chrono::milliseconds busy_timeout{5000};
};

class Store {
 public:
  // Opens or creates the database at `db_path` for `workspace_root`. An
  // existing database with another schema identity/version or another
  // workspace root is refused (StoreError) and left untouched.
  static Store open(const std::filesystem::path& db_path, const std::filesystem::path& workspace_root,
                    const StoreOptions& options = {});

  Store(Store&&) noexcept;
  Store& operator=(Store&&) noexcept;
  ~Store();

  [[nodiscard]] JournalMode journal_mode() const;  // the mode actually in effect
  [[nodiscard]] const std::filesystem::path& path() const;
  // The workspace root this database belongs to (absolute, lexically normal).
  [[nodiscard]] std::filesystem::path workspace_root() const;

  // OS-backed exclusive writer lock (a byte-range lock on `<db>.writer.lock`;
  // the file is never deleted to release it). Waits at most `wait`; nullopt
  // when another writer holds it. On acquisition, in-progress attempts left
  // by a terminated writer are marked interrupted. Do not hold it during
  // analysis.
  [[nodiscard]] std::optional<WriterLock> acquire_writer(std::chrono::milliseconds wait);

  // Creates the profile if needed. `uht_applicable` is fixed at creation.
  void ensure_profile(const WriterLock& lock, std::string_view profile, bool uht_applicable = false);

  PublishResult publish(const WriterLock& lock, const Batch& batch, const PublishObserver& observer = {});

  // Records a failed attempt for slots whose analysis was rejected. Committed
  // facts, generation and last success are untouched.
  std::int64_t record_rejected(const WriterLock& lock, std::string_view profile, const std::vector<std::string>& slots,
                               std::string_view message);

  // Records a failed attempt of an auxiliary layer (retrieval or deep).
  // Committed facts, the fact generation and the ranking generation are never
  // touched. This increment produces no retrieval/deep results, so there is
  // no way to mark those layers available. Throws std::invalid_argument for
  // other layers.
  std::int64_t record_auxiliary_failure(const WriterLock& lock, std::string_view profile, Layer layer,
                                        std::string_view message);

  // Explicit visibility input: the exact repository-relative files excluded
  // from public projections (replaces the previous set). Dependency metadata
  // for them is kept for invalidation.
  void set_excluded_files(const WriterLock& lock, std::string_view profile,
                          const std::vector<RepoRelativePath>& files);

  // Stat/hash inspection of every committed input of the profile, a recheck
  // after the writer lock is acquired, then publication of per-unit
  // freshness. Never runs a compiler or build. Deleted-file suppression does
  // not depend on it: public projections check existence themselves.
  FreshnessReport inspect_freshness(const InspectRequest& request);

  // A read transaction bound to one committed state.
  [[nodiscard]] ReadSnapshot read() const;

 private:
  struct Impl;
  explicit Store(std::unique_ptr<Impl> impl);
  void require_lock(const WriterLock& lock) const;
  std::unique_ptr<Impl> impl_;
  friend class WriterLock;
};

class WriterLock {
 public:
  WriterLock(WriterLock&&) noexcept;
  WriterLock& operator=(WriterLock&&) noexcept;
  ~WriterLock();

  struct Handle;  // opaque OS lock handle

 private:
  friend class Store;
  WriterLock(std::unique_ptr<Handle> handle, const void* owner);
  std::unique_ptr<Handle> handle_;
  const void* owner_ = nullptr;
};

// Row lookups (ReadSnapshot). `row` fields are snapshot-local handles.
struct ProfileInfo {
  std::int64_t row = 0;
  std::string key;
  bool uht_applicable = false;
  std::int64_t fact_generation = 0;
  std::int64_t ranking_generation = 0;
};

struct SymbolRow {
  std::int64_t row = 0;
  StableId id;
  CanonicalKey key;  // complete, owner chain included
};

// One committed location; `location.analysis_unit` is the owning unit's label.
struct LocationRow {
  std::int64_t unit_row = 0;
  std::optional<std::int64_t> file_row;  // nullopt for an empty path
  SymbolLocation location;
};

struct FileRow {
  std::int64_t row = 0;
  std::string path;  // repository-relative generic path
};

struct RelationRow {
  std::int64_t row = 0;
  std::int64_t source_row = 0;
  std::int64_t target_row = 0;
  RelationKind kind = RelationKind::calls;
  Confidence confidence = Confidence::confirmed;
};

struct EvidenceRow {
  std::int64_t unit_row = 0;
  std::optional<std::int64_t> file_row;
  Evidence evidence;
};

// One evidence site without its location strings or digests (the hot path of
// traversals); resolve the located file state with file_version().
struct SiteRow {
  std::int64_t unit_row = 0;
  std::optional<std::int64_t> file_row;
  std::int64_t version_row = 0;
  SourceSpan span;
  std::optional<Sha256Digest> span_hash;
  DispatchKind dispatch = DispatchKind::static_target;
};

// One symbol location in a file without strings (file_api's full scan).
struct FileSiteRow {
  std::int64_t symbol_row = 0;
  std::int64_t unit_row = 0;
  std::int64_t version_row = 0;
  LocationRole role = LocationRole::declaration;
  SourceSpan span;
  std::optional<Sha256Digest> span_hash;
};

// One observed state of a located file.
struct VersionRow {
  std::string path;  // repository-relative generic path; empty for no file
  std::string path_as_written;
  FileContentHash content_hash;
};

struct IncludeRow {
  std::int64_t row = 0;
  std::int64_t unit_row = 0;
  std::optional<std::int64_t> includer_file_row;
  std::optional<std::int64_t> location_file_row;
  std::optional<std::int64_t> target_file_row;
  DirectIncludeFact include;  // condition path not loaded; see include_condition_path()
};

struct UnresolvedRow {
  std::int64_t unit_row = 0;
  std::optional<std::int64_t> file_row;
  UnresolvedSite site;
};

struct UnitSummaryRow {
  std::int64_t row = 0;
  std::string slot;
  std::string fact_label;
  Freshness semantic = Freshness::unknown;  // persisted; unknown until an inspection published
};

// Public-projection visibility captured once (one call of the query layer):
// the effective withheld-file set -- explicit exclusions plus file rows of the
// profile missing under the workspace root at capture time, exactly
// withheld_files() -- and per-symbol presence evaluated with the rules of
// load_facts(Projection::public_). Valid while its snapshot lives.
class PublicView {
 public:
  PublicView(PublicView&&) noexcept;
  PublicView& operator=(PublicView&&) noexcept;
  ~PublicView();

  [[nodiscard]] const std::vector<RepoRelativePath>& withheld() const;  // sorted
  // SHA-256 over the sorted withheld generic paths, each followed by '\n'.
  [[nodiscard]] const Sha256Digest& digest() const;
  [[nodiscard]] bool path_withheld(std::string_view generic) const;
  // An empty path (no file row) is visible, as in load_facts.
  [[nodiscard]] bool file_withheld(const std::optional<std::int64_t>& file_row) const;
  // Presence the symbol has in load_facts(public_): indexed, implicit_hidden
  // or external_placeholder; nullopt when it is withheld (including
  // location-less endpoint_only symbols).
  [[nodiscard]] std::optional<SymbolPresence> presence(std::int64_t symbol_row) const;

 private:
  friend class ReadSnapshot;
  struct Impl;
  explicit PublicView(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

class ReadSnapshot {
 public:
  ReadSnapshot(ReadSnapshot&&) noexcept;
  ReadSnapshot& operator=(ReadSnapshot&&) noexcept;
  ~ReadSnapshot();

  [[nodiscard]] std::vector<std::string> profiles() const;
  [[nodiscard]] std::int64_t fact_generation(std::string_view profile) const;
  [[nodiscard]] FactSet load_facts(std::string_view profile, Projection projection = Projection::public_) const;
  // `detail` = inputs reads identity, provenance, settings, status, currency,
  // generation and inputs only (change detection); include edges, diagnostics,
  // limits, fingerprints and freshness stay empty. `summary` reads the same
  // identity fields and the persisted freshness, and no dependency rows.
  [[nodiscard]] std::vector<UnitRecord> units(std::string_view profile, UnitDetail detail = UnitDetail::full) const;
  [[nodiscard]] std::optional<UnitRecord> unit(std::string_view profile, std::string_view slot) const;
  [[nodiscard]] ProfileStatus status(std::string_view profile) const;
  // Files the public projection withholds right now (excluded or missing).
  [[nodiscard]] std::vector<RepoRelativePath> withheld_files(std::string_view profile) const;

  // Indexed row lookups for the query layer. Rows are the committed facts as
  // stored (no projection); `row` values are handles valid within this
  // snapshot only. Apply a PublicView for public results.
  [[nodiscard]] std::optional<ProfileInfo> profile_info(std::string_view profile) const;
  // Captures the effective withheld-file set once (see PublicView).
  [[nodiscard]] PublicView public_view(std::string_view profile) const;
  [[nodiscard]] std::vector<std::string> distinct_symbol_names(std::string_view profile) const;  // sorted
  [[nodiscard]] std::vector<SymbolRow> symbols_named(std::string_view profile, std::string_view canonical_name) const;
  [[nodiscard]] std::optional<SymbolRow> symbol_by_stable_id(std::string_view profile, std::string_view stable_id) const;
  [[nodiscard]] SymbolRow symbol(std::int64_t symbol_row) const;
  [[nodiscard]] std::vector<LocationRow> symbol_locations(std::int64_t symbol_row) const;
  [[nodiscard]] std::vector<FileRow> files(std::string_view profile) const;  // sorted by path
  [[nodiscard]] std::optional<FileRow> file_by_path(std::string_view profile, std::string_view generic_path) const;
  // Every symbol location in the file, in index order, without materializing them.
  void for_each_location_site_in_file(std::int64_t file_row, const std::function<void(const FileSiteRow&)>& visit) const;
  [[nodiscard]] std::vector<IncludeRow> includes_from(std::int64_t file_row) const;
  [[nodiscard]] std::vector<ConditionalBranch> include_condition_path(std::int64_t include_row) const;
  // Relations incident to a symbol, in relation-row order.
  [[nodiscard]] std::vector<RelationRow> relations_to(std::int64_t symbol_row, RelationKind kind) const;
  [[nodiscard]] std::vector<RelationRow> relations_from(std::int64_t symbol_row, RelationKind kind) const;
  // Relations from `source_row` to `target_row` (one per confidence).
  [[nodiscard]] std::vector<RelationRow> relations_between(std::int64_t profile_row, std::int64_t source_row,
                                                           std::int64_t target_row, RelationKind kind) const;
  [[nodiscard]] std::vector<EvidenceRow> evidence(std::int64_t relation_row) const;
  [[nodiscard]] std::vector<SiteRow> evidence_sites(std::int64_t relation_row) const;
  [[nodiscard]] VersionRow file_version(std::int64_t version_row) const;
  [[nodiscard]] std::vector<UnresolvedRow> unresolved_in(std::int64_t symbol_row) const;
  [[nodiscard]] std::vector<UnitSummaryRow> unit_summaries(std::string_view profile) const;  // by slot

 private:
  friend class Store;
  friend class ReadOnlyStore;
  struct Cache;  // prepared statements of this snapshot
  ReadSnapshot(sqlite3* db, std::filesystem::path workspace_root, bool owns_connection = true);
  sqlite3* db_ = nullptr;
  std::filesystem::path workspace_root_;
  bool owns_ = true;
  std::unique_ptr<Cache> cache_;
};

// Read-only access to an existing store (query tools). It never creates or
// migrates a schema, never takes or touches the writer lock and never writes a
// row: the connection is SQLITE_OPEN_READONLY with `query_only`. A missing
// path, a foreign or other-version file and a workspace-root mismatch are
// refused before any connection that could create files is opened. A valid
// WAL store whose `-wal`/`-shm` are absent gets them created by SQLite on the
// read-only open (an empty WAL and a shared-memory index); the database bytes
// are never changed.
enum class ReadOnlyFailure : std::uint8_t {
  missing,         // no regular file at the path
  foreign,         // not a LiveCodeMap store
  schema_version,  // another schema version
  root_mismatch,   // belongs to another workspace root than the expected one
};

[[nodiscard]] std::string_view to_string(ReadOnlyFailure value);

class ReadOnlyStoreError : public StoreError {
 public:
  ReadOnlyStoreError(ReadOnlyFailure failure, const std::string& message) : StoreError(message), failure_(failure) {}
  [[nodiscard]] ReadOnlyFailure failure() const { return failure_; }

 private:
  ReadOnlyFailure failure_;
};

class ReadOnlyStore {
 public:
  // `expected_root`, when given, must be the store's workspace root.
  static ReadOnlyStore open(const std::filesystem::path& db_path,
                            const std::optional<std::filesystem::path>& expected_root = std::nullopt);

  ReadOnlyStore(ReadOnlyStore&&) noexcept;
  ReadOnlyStore& operator=(ReadOnlyStore&&) noexcept;
  ~ReadOnlyStore();

  [[nodiscard]] const std::filesystem::path& path() const;
  [[nodiscard]] std::filesystem::path workspace_root() const;  // from the store's meta

  // A read transaction on the store's one long-lived connection, so its page
  // cache stays warm across calls. One live snapshot at a time.
  [[nodiscard]] ReadSnapshot read() const;

  // Read-only inspection over one captured input/generation set: two hashing
  // passes, no writer lock, no persistence and no downgrade in any outcome.
  // A unit is fresh only when both passes completed and equal the recorded
  // hashes, the fact generation is unchanged at the end and its build-action
  // currency is verified; UHT never becomes fresh. `complete` and
  // `generation_changed` keep their meaning; `lock_busy` never occurs.
  [[nodiscard]] FreshnessReport inspect_freshness(const InspectRequest& request) const;

 private:
  struct Impl;
  explicit ReadOnlyStore(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace lcm::store
