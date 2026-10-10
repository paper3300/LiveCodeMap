# Selected-profile multi-TU indexing (Phase 2 increment)

Status: a small library interface that connects an explicitly selected set of compile actions of
one build profile to actual analyzer calls and one atomic store publication. It is not full Phase 2:
no CLI/MCP, no source or UBT action discovery, no UBT/UHT execution, no worktree seeding, no
retrieval/deep production and no real Unreal project validation. See [storage.md](storage.md) for the store.

| Path | Purpose |
| --- | --- |
| `src/lcm/analyzer/profile_index.{hpp,cpp}` | `index_profile(store, request)` (Clang build only) |
| `tests/analyzer/profile_index_tests.cpp` | executable pipeline tests on scratch copies of `tests/fixtures/cpp/store` |

## Request

`ProfileIndexRequest` names the profile (and its fixed UHT applicability), the explicit compile
database, the complete authoritative list of `SelectedAction`s, request-wide context inputs
(toolchain/configuration/descriptor files), caller role tags (generated, reflected, ignored), the
default build currency, the publish policy, the repository member and the pinned resource directory.
The workspace root is the store's.

A `SelectedAction` is a stable caller-supplied slot, its source (or synthetic unity file plus its
direct original members), an optional command ID (required when the database has several commands
for the source), an optional explicit PCH producer command ID, action-local context inputs (for
example the module's `Build.cs`) and an optional build currency overriding the request default.

## Selection (before any analysis)

The compile database bytes are read once, hashed and parsed from those bytes. The run is rejected,
nothing is analysed and nothing is retired when any of these hold:

- the database is missing, unreadable, not valid JSON or has an entry-level error;
- a slot is empty or duplicated; a source has no command, or several without an explicit ID; an
  explicit ID is unknown or names a command for another file; one command is selected twice;
- a file (source or unity member) is claimed by two slots; a member or ordinary source is outside
  the workspace root; a PCH producer is not in the database;
- a context input is missing, has a non-context role, two roles, or appears twice for one action;
  a role override is not canonical or not generated/reflected/ignored;
- the profile exists with another UHT applicability (it can never switch);
- the selection is empty without `empty_selection_intended`, or that flag is set with actions.

An intended empty selection with a valid database retires every committed slot (a new profile is
created empty). A declared source without a command is always an explicit error.

## Change detection (compiler-free)

A selected slot is `new` without a committed unit and `changed` when any of these differ from its
committed unit, otherwise `unchanged`:

- selected command ID; explicit PCH producer command ID;
- analysis settings (`clang` version, resource directory, repository member, publish policy);
- build currency (request default or the action's override);
- the set of `source`-role inputs (the source, or the unity file plus its members);
- the slot's context inputs: request-wide and action-local inputs as path, role and hash, and the
  compile database (`build_database`) by **path** only;
- the expected role of each committed header-like input under the current role tags;
- the bytes of every committed non-context input and response file (modified or deleted).

Committed slots absent from a valid selection are `retired`. All of this is stored as explicit
columns and input rows, so a reopened store supports the same decision.

The committed state is read set-based without include edges, diagnostics or limits
(`UnitDetail::inputs`). Each distinct recorded file is hashed at most once for change detection and
once more under the writer lock, however many units recorded it, the distinct files of a pass being
hashed by parallel workers first. An unchanged run of a private 551-action Unreal profile (1.32 million
input rows over 13,240 files) revalidates in about 2.5 s.

### Per-entry identity of the compile database

The database is read, parsed and validated on every run (see Selection); a deleted or malformed
database, a removed selected entry or new ambiguity rejects the run before analysis. Its bytes do not
schedule slots: a slot's compile context from the database is its own selected entry and, for a PCH
consumer, the producer entry — both covered by their command IDs, which are content identities of
directory, file and arguments — plus the response files those commands read (`response_file`
inputs, rehashed every run). Therefore:

- whitespace, entry order, and adding, removing or changing an **unselected** entry leave every
  selected command ID, the planned analyses, the generation and the facts unchanged;
- a changed selected entry reanalyses exactly its slot;
- a changed shared producer entry changes the producer command ID of, and reanalyses, every consumer
  naming it; a changed producer response file reanalyses the same consumers;
- a changed response file reanalyses exactly the actions that read it;
- a different database path is a changed context input for every slot.

A newly analysed slot records the hash of the database bytes it was analysed with (provenance); an
unchanged slot keeps the hash of its own analysis, because nothing is republished. **State after an
unrelated database edit:** `inspect_freshness` does not parse the database, so it sees the recorded
`build_database` hash differ and reports the compile context (and so the semantics) of the slots
analysed before the edit `stale`, with `livecodemap build` guidance, while `index_profile` keeps
reporting them `unchanged`. This is conservative: freshness never reports a compile context fresh
on the strength of an unchanged entry alone. It returns to fresh when the database bytes match the
recorded ones again or the slot is reanalysed.

## Atomicity

1. Context inputs are hashed before analysis; those hashes are what the contributions store.
2. New and changed slots are analysed without the writer lock, at most `jobs` at a time
   (`ProfileIndexRequest::jobs`, `lcm_index --jobs <n>`; default 1, one after the other in
   selection order). Every analysis has its own filesystem view and front end and only reads its
   own action. Clang's driver phase (building the compiler invocation, including MSVC toolchain
   detection) is not safe to run concurrently, so it is serialized by a process-wide lock from the
   start of each front-end run until the compiler invocation exists; the prefix captures, parsing
   and fact building run in parallel. After a failure no further analysis starts; analyses already
   running finish.
   Results are reported per slot in selection order: a failing slot is `failed`, a slot that was
   not started is `not_analyzed`, a good analysis of a failed run is `not_published`, and the run's
   message names the first failed slot in selection order. With one job the first failure stops
   analysis; with at least as many jobs as planned analyses every planned slot is analysed and
   reported. An exception inside an analysis is rethrown after every job has stopped.
   A finished contribution is validated and moved to a `ContributionSpool` next to the database at
   once; memory holds the analyses in flight and each slot's input list, not the finished facts.
   A contribution the store would refuse fails its slot like an analysis failure.
3. The writer lock is taken, also for an unchanged run. Under it the pipeline requires the fact
   generation and profile existence of the plan, rehashes every context input, every input the new
   analyses observed and the committed inputs of unchanged slots.
4. One `Store::publish` installs the new contributions from the spool, in selection order, and
   retires absent slots.

Any failure (analysis, mapping, PCH, revalidation, publication) publishes nothing: facts,
generation and last success are unchanged and a failed attempt is recorded for the involved slots.
A failing first run creates the empty profile (generation 0, no last success) to record it; an
empty profile key records nothing. A busy writer lock returns `busy` without publication (also
after parallel analyses; the busy lock means no attempt can be recorded). An unchanged run analyses
nothing, records no attempt and does not advance the generation.

## Unity actions and PCH

Every fact keeps its original file, span and hash; the fact label is the slot. The unity file's
direct includes, as observed by the analyzer, must be exactly the declared members. Members get the
`source` role. Direct-include facts written in the unity file are dropped (the include edges and
input stay as dependency metadata); any other fact located in the unity file fails the slot.

Members are classified by the analyzer's include observations, never by file names or by their
text alone. A member observed **directly including another translation-unit source** — an active,
resolved `#include` (macro operands included) of a `.cpp`, `.cc`, `.cxx` or `.c` file, which covers
`.gen.cpp` — is a nested/intermediate unity fragment: the action fails mapping validation and
nothing is published, whether or not the member has content of its own. A mapping naming the
originals behind a fragment fails the membership check. Every other member is an original,
including one that consists only of preprocessor lines (UE's `#include "X.h"` members, a macro
include such as `PerModuleInline.gen.cpp`, an empty or commented-out file): it keeps the `source`
role (generated members too; generated/reflected stay caller role tags), its bytes take part in
change detection, and it keeps its own direct-include facts (spelling, resolved target, condition
path, location). It has no symbol, relation, unresolved or capture facts of its own, because it
contains no code; facts of the headers it includes are located in those headers. An inactive
`#include` of a `.cpp` is not observed and does not make a fragment.

An explicit PCH producer is passed to the analyzer's textual replay. The unit must actually use it
(`textual_snapshot`); a producer supplied for a unit that needs none fails as an inconsistent
mapping, and a consumer without one is rejected by the analyzer.

## Known limitations

- After an unrelated compile-database edit, persisted freshness reports the compile context of
  earlier slots stale until they are reanalysed or the bytes match again (see above); there is no
  per-entry database row in the store. Module-level configuration belongs in action-local context
  inputs, which reanalyse only the naming actions.
- Parallel analysis multiplies memory: a real UE action peaks at about 2-3 GB, so choose `jobs`
  from memory as well as cores.
- The pipeline persists no freshness state and never promotes build currency; use
  `inspect_freshness`. A slot whose last attempt failed stays listed as failed after an unchanged
  run until a new attempt succeeds.
- The analyzer identity is the Clang version; changes of LiveCodeMap's own fact builder are not
  detected. The resource directory is compared by normalized spelling.
- Byte equality never verifies UBT/UHT/build currency; that remains caller evidence.
