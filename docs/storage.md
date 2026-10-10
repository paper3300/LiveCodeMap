# TU contribution store and dependency freshness (Phase 2 increment)

Status: storage plus dependency-freshness infrastructure for PRD FR-IDX-001..005. This is not full
Phase 2: there is no automatic structural/semantic incremental scheduling, no retrieval/deep
production, no search/CLI/MCP, no worktree seeding and no ignore-pattern engine. Reserved
fingerprint slots are not an implementation of retrieval/deep invalidation.

| Path | Purpose |
| --- | --- |
| `src/lcm/store/store.{hpp,cpp}` | `lcm_store`: schema, publication, writer lock, readers, freshness inspection, status |
| `src/lcm/analyzer/store_adapter.{hpp,cpp}` | `to_contribution()`: actual `AnalysisResult` -> `store::Contribution` (Clang build only) |
| `tests/store/` | Clang-free store tests and `lcm_store_child`, the cross-process writer used by them |
| `tests/analyzer/store_adapter_tests.cpp` | two real TUs (`tests/fixtures/cpp/store`) analysed and stored |
| `src/lcm/analyzer/profile_index.{hpp,cpp}` | selected-profile analyzer-to-store pipeline, see [profile-index.md](profile-index.md) |

`lcm_store` depends on `lcm_core` and SQLite only, so it builds and tests with `LCM_WITH_CLANG=OFF`.

## SQLite dependency

SQLite 3.53.4 amalgamation (`sqlite-amalgamation-3530400.zip`, SHA-256
`1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d`), fetched by
`cmake/Dependencies.cmake` with `URL_HASH`. The coordinator-verified archive at
`build/deps/downloads/` is used when present, otherwise `https://www.sqlite.org/2026/…`. It is
compiled as C into `lcm_sqlite3` (static CRT like everything else) with `SQLITE_OMIT_LOAD_EXTENSION`
and `SQLITE_DQS=0`; nothing is installed machine-wide.

## Ownership and identity

- One database belongs to one workspace root (recorded in `meta`; another root is refused). Every
  query and mutation names a profile (`ensure_profile`; UHT applicability is fixed at creation).
- A logical unit is `(profile, slot)`. The slot is supplied by the caller and never derived from a
  command ID, replay ID, file name or unity file. Command ID, normalized context ID, replay ID and
  PCH producer identities are stored as provenance columns of the committed unit.
- Every fact of a contribution must carry the contribution's `fact_label` as its analysis unit
  (the adapter uses `AnalysisResult::analysis_unit`; pass the slot as `AnalysisRequest::analysis_unit`).
  A label names one live slot per profile (`UNIQUE(profile_id, fact_label)` plus a pre-mutation
  check), so two slots can never collapse into one owner.
- Facts keep their original repository-relative path/span/hash and owning unit. Common rows
  (`symbol`, `symbol_owner`, `relation`, `file`) are shared; `unit_symbol` records which units name a
  symbol (and whether as an external placeholder). A common row is pruned only when its last owner
  disappears. Endpoints of relations, unresolved owners and capture lambdas must be symbols of the
  same contribution, which is what keeps every surviving unit's references alive.

## Schema (version 3, `PRAGMA application_id = 'LCMS'`, `user_version = 3`)

Normalized tables, frequently queried fields as columns, enums as the same stable text tags the fact
hash uses, SHA-256 digests as 32-byte BLOBs. No JSON and no source bodies.

- `profile` (fact generation, ranking generation, UHT applicability); `attempt` (layer, state,
  started/finished, message) and `slot_attempt` (install/retire/rejected per slot).
- `unit` (slot, label, provenance, analysis settings, analysis status, build currency, committed
  generation) with
  `unit_diagnostic`, `unit_limit`, `unit_fingerprint`, `unit_freshness`, `input` (the unit's
  dependency observations as references to shared `dependency` rows: path key, path, repo path,
  role, content hash, system flag) and `include_edge` (the unit's compiler include observations in
  order, as references to shared `include_site` rows: includer, spelling, angled, resolved, line; a
  forced include has a NULL includer).
- `file` (repo-relative generic path) and `file_version` (one observed state of a located file:
  `path_as_written`, content digest and size); every located row stores `file_id`, `version_id`,
  six span fields and the span hash.
- `symbol` (stable ID, serialized key for collision checks, key fields, linkage columns),
  `symbol_owner`, `unit_symbol`, `symbol_location` (role, body hash, callable flags).
- `relation` (source, target, kind, confidence) and `evidence` (unit, kind, location, provider key,
  all call aspects, base-edge details).
- `unresolved`, `capture`, `direct_include`, `include_branch`, `include_term` (term ordinal -1 is a
  branch's own condition, 0.. its preceding branches).
- `excluded_file` (explicit visibility input).

An existing file with another application ID, another schema version or another workspace root is
refused with `StoreError` before anything writes to it; it is never migrated or deleted. Version 2
added the analysis-settings columns of `unit` (`analyzer`, `resource_dir`, `repository_member`,
`publish_policy`). Version 3 stores the same facts and dependency records more compactly: digests
as BLOBs, located rows referencing `file_version` instead of repeating spelling, digest and size,
and dependency observations and include edges shared between units that observed exactly the same
row (for example every consumer of one precompiled-header prefix). Readers reassemble exactly the
v2 values, and fact hashes are unchanged. Version 1 and 2 databases are refused like any other
version; rebuild them.

Shared rows follow the ownership rule of common rows: a `file_version`, `dependency` or
`include_site` row is removed in the same transaction as the last unit row that references it, so
a surviving unit keeps all of its records when another unit is replaced or retired.

`Contribution::settings` (`AnalysisSettings`) records what, beyond the command and its inputs,
determines the produced facts: the front-end identity (`clang <version>`), the pinned resource
directory, the repository member of in-root canonical keys and the publish policy. They are plain
columns, not folded into provenance IDs; empty values mean "not recorded".

## Publication

`Store::publish(lock, Batch)` installs contributions (replacing the same slots) and retires slots:

1. Validation before mutation: duplicate slots, a slot both installed and retired, facts labelled
   by another unit, endpoints outside the contribution, stable IDs that do not match their key,
   non-canonical paths, missing command/replay identity, a label owned by another live slot. A
   refused batch records a failed attempt and changes nothing else (`rejected`).
2. An empty batch, or one that only retires slots that do not exist, is a `no_op`: no attempt, no
   generation change. Re-publishing identical contributions is a real publication.
3. An `in_progress` attempt is committed on its own, so a terminated writer is identifiable.
4. One `BEGIN IMMEDIATE` transaction removes the old contributions, inserts the new ones, advances
   the fact generation by exactly one and marks the attempt succeeded. Any failure rolls back and
   the attempt becomes `failed`; committed facts, generation and last success are untouched.
5. The next writer that takes the lock marks leftover `in_progress` attempts `interrupted` without
   a completion time.

`record_rejected` records a failed attempt for slots whose analysis was rejected;
`record_auxiliary_failure` records a retrieval/deep failure. Neither touches facts or generations.
Nothing in this increment can mark retrieval or deep available, and the ranking generation is never
advanced by storage.

`PublishObserver` (phases `attempt_recorded`, `old_removed`, `before_commit`) is a harness hook for
fault injection and crash tests, not configuration.

### Spooled installs and the insert path

A `ContributionSpool` keeps finished contributions on disk until one publication installs them, so a
caller with many units holds none of them in memory. `add()` validates a contribution exactly as
`publish()` would (and refuses it, storing nothing, otherwise), encodes it losslessly
(`contribution_codec.hpp`: every field including display spellings, strings and digests interned per
contribution) and appends it to a file created next to the database; `load()` checks the bytes against
the digest taken when they were written. The spool's file is held exclusively by its owner and
deleted by the operating system when the owner closes it or dies, killed processes included (Windows:
the only handle, no sharing, `FILE_FLAG_DELETE_ON_CLOSE`; POSIX: locked and unlinked right after
creation). A leftover from a power loss is reclaimed when a new spool is created in that directory and
by writer recovery, and only when it can be opened exclusively, which a live owner's spool never can.
A `Batch` may name
spool entries (`spool`, `spooled`, installed in that order after `install`); the batch rules
(duplicate slots, install/retire overlap, fact labels owned by other live slots) cover them, and they
are installed inside the same single transaction, the next entry being decoded on another thread while
the current one is inserted (at most two in memory). Failure semantics are those of any batch.

Inside the transaction the rows shared between units (files, file versions, symbols with their
serialized keys for the collision check, relations, dependencies, include sites) are looked up once per
publication and kept for the rest of its insert phase, which never deletes; the per-row statements are
prepared once; and the connection's page cache is raised to 512 MiB for the transaction and restored
afterwards.

## Concurrency

- Writer: `acquire_writer(wait)` takes an exclusive `LockFileEx` byte-range lock on
  `<db>.writer.lock` (POSIX builds use `flock`), polling until `wait` expires; `nullopt` means busy.
  The OS releases it when the process exits; the file is never deleted. Take it only for the
  publication, not during analysis.
- Readers: `read()` opens a separate connection and a read transaction bound to one committed state;
  every call on the snapshot (facts, units, status, generation) sees that state.
- WAL is preferred (`synchronous=FULL`); `StoreOptions::journal = rollback` selects the DELETE
  journal, and a failed WAL switch falls back to it. In rollback mode an open reader makes a commit
  wait up to the busy timeout and then fail cleanly; partial state is never visible.
- The writer connection never leaves a statement active between operations (an active statement
  would pin an old WAL snapshot or hold a SHARED lock).

## Dependency freshness

Input roles are explicit: `source`, `header`, `inline_file`, `generated`, `reflected_header`,
`ignored_dependency`, `external_dependency`, and the compile-context roles `response_file`,
`compiler`, `build_database`, `build_configuration`, `project_descriptor`. The adapter assigns
source/header/inl/external from compiler observations, response files from the expansion snapshots
actually read, and takes generated/reflected/ignored roles and context inputs from the caller. The
in-memory order guard of a textual PCH replay is not a filesystem input and is not recorded (its
identity is part of the replay ID).
Build-action currency (`BuildCurrency::verified`) is caller evidence; it is never inferred.

`inspect_freshness(request)`:

1. Reads the profile's committed units and inputs in one snapshot, set-based: each distinct shared
   dependency row once, then every unit's references to it.
2. Pass 1 (unlocked): each distinct input file is hashed in 1 MiB chunks; the deadline is checked
   before every file and after every chunk.
3. Takes the writer lock (bounded by `lock_wait` and the remaining budget); gives up if the fact
   generation changed.
4. Pass 2 (locked): every input again. A file whose two passes disagree is `changed_during_check`.
5. Per unit: any changed/deleted dependency -> semantic stale; a context-role change -> compile
   context stale too; a reflected-header change -> UHT stale (when applicable). Otherwise the
   compile context is fresh only with verified build currency, and semantics only on a fresh
   compile context. UHT is never fresh: this increment has no UHT evidence.
6. Publishes per-unit states in one transaction, unless the deadline passed (checked again just
   before `COMMIT`).

An incomplete inspection (`budget_exhausted`, `lock_busy`, `generation_changed`) publishes nothing
fresh. If it holds or can immediately take the writer lock it lowers every persisted `fresh` state of
the profile to `unknown` and records the stale units it detected (`persisted_downgrade`). Its report
never contains a `fresh` unit or a `checked_at`. Anything not fresh comes with explicit
`livecodemap build` guidance. The inspection never runs a compiler, a project build, UBT/UHT or deep
analysis.

Public projection (`load_facts(profile, Projection::public_)`) withholds, at call time, facts located
in files that are explicitly excluded or do not exist under the workspace root; a direct include is
withheld when its includer or target is withheld. This does not depend on any inspection. Only
symbols with at least one visible location, and pure external placeholders (no committed location
at all), are public. Withheld entirely (key, name, stable ID) are: a symbol whose committed locations
are all in withheld files; a location-less `endpoint_only` symbol, because once its owners were
retired or replaced nothing committed proves its origin was visible (this also holds after a reopen
or after the withheld file becomes visible again); an internal-linkage symbol whose file scope is
withheld; and a local symbol whose enclosing symbol is withheld. Every relation incident to a
withheld symbol, and unresolved sites and captures owned by one, are withheld too (no boundary facts
are synthesized in this increment). A symbol that keeps a visible location from any unit stays, with
only its visible locations. `Projection::all` is unchanged: it returns everything committed,
including `endpoint_only` survivors, and dependency metadata is kept either way.

## Fingerprints

Per unit, typed and independent: `file_bytes` (inputs and their hashes), `declaration_api` (declared
stable IDs, presence, location roles and files; not spans or bodies), `symbol_body` (definition body
hashes), `compile_action` (command/context/replay/PCH identities and analysis settings).
`retrieval_document` and
`deep_input` are stored as explicitly unavailable. A body edit changes `symbol_body` but not
`declaration_api`; a signature edit changes the stable ID and `declaration_api`.

## Status

`ReadSnapshot::status(profile)` returns fact/ranking generations, the last semantic attempt and last
success, slots whose latest attempt failed or was interrupted, and one `LayerState` per layer:
structure `missing` (not produced here); compile context and semantic graph from committed units,
failed slots and persisted unit freshness (`unknown` until inspected); UHT `applicable=false` for
plain profiles, otherwise `missing` with unknown/stale freshness; retrieval/deep `missing`, or
`failed` after a recorded failure.

## Known limitations

- No mtime/size shortcut: every inspection hashes every distinct input twice, one file after the
  other (bounded by the budget). On a private 551-action Unreal profile (13,237 files, 179 MB) a no-change
  inspection takes about 17-19 s, nearly all of it opening and reading files; PRD 11.1's 100 ms
  no-change freshness is not met.
- `declaration_api` is per unit, not per file; per-file API invalidation and resolver buckets are
  later work.
- Deletion/exclusion suppression works on exact repository-relative files; no ignore patterns and no
  directory rules. Edges to withheld symbols are dropped from public output rather than rewritten
  into ignored/unresolved boundary facts. The rule for location-less endpoints is conservative: a
  visible caller's edge to a symbol whose visible defining unit was removed disappears from public
  output until a unit that locates the symbol is published again.
- Visibility changes and inspections do not advance any generation; cursors (later work) must bind
  the exclusion set separately.
- Freshness covers only committed units; commands without a committed unit, compile database entry
  changes that are not tracked as a `build_database` input and new sources are not discovered here
  (the selected-profile pipeline reports declared sources without a command explicitly).
- POSIX `flock` code path compiles but is untested (first release is Windows-only).
