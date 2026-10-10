# Query API, CLI and MCP server (Phase 3 increment)

Status: a read-only query layer over the TU contribution store ([storage.md](storage.md)), one
executable with CLI subcommands and an MCP stdio server. Four tools: `find_code`/`ask`,
`trace_calls`/`callers`, `file_api`/`skeleton` and `check_freshness`/`check`.

This is not the full PRD surface. Not included:

- ranking projection, BM25, graph rank or natural-language search;
- snippet bodies (locations only), class cards, `find_all`, `repo_map`, `blast`, `grep`, `viz`;
- `build`, `init` and `remove` (indexing stays with `lcm_index`).

| Path | Purpose |
| --- | --- |
| `src/lcm/query/` | `lcm_query`: engine, response model, tiers, traversal, file API, cursors, rendering, CLI, MCP session |
| `src/lcm/store/store.{hpp,cpp}` | Read side used here: `ReadOnlyStore`, `PublicView`, indexed `ReadSnapshot` row lookups, read-only `inspect_freshness` |
| `tools/livecodemap/` | `livecodemap.exe` (UTF-8 command line and stdio) |
| `tests/query/` | Fixture contract tests, MCP session tests, and the executable over pipes |

`lcm_query` and `livecodemap` depend on `lcm_store`, `lcm_core` and nlohmann JSON only. They build
and test with `LCM_WITH_CLANG=OFF`.

## Read-only database access

The query path never calls `Store::open`, never creates or migrates a schema, never touches
`<db>.writer.lock` and never writes a row. `ReadOnlyStore::open(db, expected_root)` works as
follows:

1. A missing path or a non-regular file is refused (`db_missing`).
2. Without WAL frames (`-wal` absent or empty), the 100-byte SQLite header is read with plain file
   I/O first: magic, application ID `LCMS` and schema version 3. A foreign or other-version file is
   refused (`db_foreign`, `db_schema_version`) before SQLite sees it.
3. A WAL-mode file whose `-wal` or `-shm` is absent has its workspace root checked through an
   `immutable=1` connection. That connection creates no file. A mismatch with `--root` is
   `db_root_mismatch`.
4. The real connection is `file:...?mode=ro` with `SQLITE_OPEN_READONLY`. It sets connection-local
   pragmas only: `query_only=1`, `mmap_size=2147418112` (the build's 2 GiB cap) and
   `cache_size=-524288` (512 MiB). One long-lived connection per engine serves every call. Each
   call is one read transaction (snapshot), so the page cache stays warm across MCP calls.

**Sidecars.** For a valid, matching v3 WAL store whose `-wal`/`-shm` are absent, SQLite's read-only
open creates an empty `-wal` and a 32 KiB `-shm` next to the database and leaves them there. The
database bytes are never changed. A missing, foreign, other-version or root-mismatched input
creates nothing. The tests check this, and also that DB, WAL and SHM bytes (SHM by size) are
unchanged after every tool, with and without a live writer.

## Response model

The CLI with `--json` prints exactly the object that MCP returns as `structuredContent`:

```
{ "tool", "status", "project_kind", "profile", "fact_generation", "ranking_generation",
  "layers": [ {layer, applicable, availability, freshness, units{total,succeeded,failed,stale,ignored},
               last_success, last_attempt, reason} x6 ],
  "failed_slots": [...], "diagnostics": [ {code, message} ],
  "pagination": { complete, total, shown, returned_count, discovered, omitted, next_cursor, limit_reason },
  "result": { tool-specific } }
```

- **`project_kind`** is `unreal` when the profile is UHT-applicable, otherwise `cpp`. Layers are
  `ReadSnapshot::status` reported raw. Persisted `unknown` freshness stays `unknown`; no query tool
  runs an inspection.
- **Locations** give the repository-relative path, the span (lines, columns, offsets), the span
  hash and the file content hash with size. No source text is included.
- **Symbols** carry `id` (cm1 stable ID), `name`, `qualified_name` (owner names joined by `::`,
  `(anonymous)` for empty names), `kind`, `signature`, `presence` and `primary_location`.
  - The primary location is chosen from the visible locations: definition before declaration, then
    path, begin offset, end offset and unit label.
  - An implicit declaration is used only when nothing else is visible.
  - An external placeholder has `null`.
- **`status`** is the query outcome (contract R1), chosen in this order:
  1. `error`: the request is invalid or cannot be executed.
  2. `partial`: a search or traversal budget ended, or the profile has failed slots (diagnostic
     `failed_slots`).
  3. `stale`: a returned fact is owned by a unit whose persisted semantic state is `stale`
     (diagnostic `stale_result` with the fact IDs, also when `partial` wins).
  4. `success`: otherwise. A no-hit query is `success`.
- **Pagination** follows contract R3. Page truncation of a complete result is `complete: true`
  with `omitted > 0` and `limit_reason: page_limit`. An incomplete result has `total` and `omitted`
  set to `null` and always a `next_cursor`. The one exception is `check_freshness`, which has no
  continuation (see below).
- **Error codes.**

  | Area | Codes |
  | --- | --- |
  | Database | `db_missing`, `db_foreign`, `db_schema_version`, `db_root_mismatch`, `db_error` |
  | Profile | `profile_required`, `unknown_profile` |
  | Arguments | `empty_query`, `invalid_query`, `invalid_limit`, `invalid_depth`, `invalid_budget` |
  | Selectors | `empty_selector`, `malformed_stable_id`, `invalid_selector`, `unknown_selector`, `ambiguous_selector` |
  | Cursors | `invalid_cursor`, `cursor_expired` |
  | Paths | `empty_path`, `invalid_path`, `not_a_file`, `outside_workspace`, `file_not_indexed` |
  | Other | `internal_error` |

## Public projection

Every result equals the corresponding subset of `ReadSnapshot::load_facts(profile, Projection::public_)`
for the same snapshot and on-disk existence. The tests compare them for the baseline, an excluded
file, a deleted file and a retired owner (endpoint-only).

Each call captures one `PublicView`: the explicit exclusions plus every file row of the profile
missing under the workspace root, exactly `withheld_files()`. It then evaluates presence per
touched symbol with the `load_facts` rules:

- a visible real location makes the symbol `indexed`;
- only implicit visible locations make it `implicit_hidden`;
- no committed location plus a placeholder row makes it `external_placeholder`.

Withheld are:

- symbols whose locations are all withheld;
- location-less endpoint-only symbols;
- internal-linkage symbols scoped to a withheld file;
- locals whose enclosing symbol is withheld, recursively.

Relations need two public endpoints and at least one visible evidence site. Includes need a
visible includer, directive file and target. Withheld names, IDs, paths, counts, candidates,
callsites and owner links never appear. `implicit_hidden` symbols are graph endpoints, but not
search results.

## Cursors

`lcq1.<base64url(payload)>.<base64url(SHA-256("lcm-query-cursor-v1"‖payload)[0..16])>`. The payload is
canonical JSON. It binds:

- the tool;
- the database identity (normalized path and workspace root);
- the profile;
- the fact and ranking generations;
- a digest of the normalized binding arguments;
- the SHA-256 of the effective withheld-file set;
- the tool's position.

What the arguments bind, per tool:

| Tool | Bound arguments |
| --- | --- |
| `find_code` | trimmed query |
| `trace_calls` | resolved stable ID, direction, depth or `all` |
| `file_api` | repository-relative path |
| Ambiguous-candidate pages | the selector |

`limit` and budgets are not bound, so a page size can change or a budget be raised mid-chain.

**Validation:**

- A mismatch returns `status: error`, `cursor_expired`, no records and restart guidance. The
  withheld digest is recomputed for every call, so an exclusion change or a deleted file expires a
  cursor.
- A malformed or altered cursor, or one for another tool, is `invalid_cursor`.
- The checksum detects corruption and edits but is not a secret. A forged, self-consistent cursor
  can only select another position of the same public result.

The position holds counters only. Every call recomputes its result on its snapshot
deterministically, so continuation is gap- and duplicate-free.

## Tools

### `find_code` / `ask <identifier...>`

Default limits: MCP 5, CLI 8.

**Normalization.** Trim; an empty query is `empty_query`. For tiers 3 and 4 the text is split into
lower-cased components:

- every ASCII non-alphanumeric character separates components (`_ - :: / .` and spaces);
- inside a word, a boundary falls lower→Upper (`tryConnect`);
- a boundary falls before the last letter of an acronym followed by lower case (`XMLLoad` →
  `xml`, `load`);
- a boundary falls between letters and digits;
- UTF-8 bytes are word characters and never split.

The Unreal one-letter prefix stripping is deferred.

**Tiers.** Each public symbol appears once, at its highest tier:

1. **Exact identifier:** the case-sensitive canonical (unqualified) name.
2. **Qualified name:** applies to a query containing `::`. It matches the full qualified name, or a
   `::`-aligned suffix of it (`Shape::area` matches `geo::Shape::area`). A leading `::` requires
   the exact full name. A malformed name such as `geo::` is `invalid_query`.
3. **Identifier component:** the query's components form a contiguous run in the name's
   components. For a qualified query the run must occur in the qualified name's component sequence,
   prefiltered by the last segment.
4. **File path:** file records (not symbols) whose path components contain the query's run.

**Order.** Tiers 1-3 are ordered inside a tier by (qualified name, kind, signature, stable ID),
compared bytewise. Tier 4 is ordered by repository-relative path. External placeholders are
results with `primary_location: null`; `implicit_hidden` symbols are excluded.

**Budget.** Tiers are computed whole. The 2 s search budget is checked between tiers, never before
the tier after the ones an earlier page completed, so every continuation adds a tier. A stop is
`partial` with `limit_reason: time_budget`.

### `trace_calls` / `callers <selector>`

**Selector.** A well-formed stable ID (`cm1:` plus 32 hex digits), otherwise a name resolved over
the complete public tier-1/tier-2 match set (`implicit_hidden` is selectable by ID only).

- Several matches give `status: error`, `ambiguous_selector` and paginated candidates (ID,
  qualified name, kind, signature, primary location).
- A withheld symbol is `unknown_selector`, by name and by ID.

**Defaults.** Incoming (`--outgoing` for callees), depth 1 (maximum 5; `--all`/`all` lifts only
the depth limit), 20 records, a visited-node budget of 20,000 and a 2,000 ms time budget. Both
budgets can be overridden: nodes 1..1,000,000, time 1..60,000 ms.

**Traversal.** Level-synchronous BFS in discovery order:

- Levels come in hop order. Inside a level, the previous level's records come in their discovery
  order, then each node's slots, then each slot's relations in relation-row order.
- **Incoming slots** are the node itself, then its public `overrides` bases, breadth-first.
  Callers reaching the node only through a base slot are `possible`, and only for virtual-slot
  callsites.
- **Outgoing:** callees come in relation order. A callee whose visible evidence includes a
  virtual-slot call is followed by the public overriders of that slot, breadth-first, as possible
  branches.
- **Public relations only.** An `overrides` relation expands bases or overriders, in either
  direction, only when it is public, like every `calls` relation: both endpoints are public and at
  least one of its evidence sites is in a visible file. An override known only from an excluded or
  deleted file creates no possible branch, even when both methods stay public.

**Records.** A record is keyed by (direction, other symbol). It is sealed when discovered: all of
its public callsites into the previous level are collected at once, and it reports its shortest
hop.

- Callsites are keyed by (caller, slot, file, span, span hash, dispatch), with the units of
  repeated per-unit evidence grouped.
- A confirmed virtual-slot edge stays confirmed to its slot. Stored `possible` relations stay
  possible.
- `path_confidence` is `confirmed` only when the record has a confirmed callsite into a node whose
  own path is confirmed.
- Candidates reached by several override paths are one record. Cycles terminate.
- Outgoing results list the selected body's unresolved sites (first page only). They are never
  traversed.

**Counts.** `unique_callers` or `unique_callees`, `callsites`, `confirmed_callsites` and
`possible_callsites`, with `exact` true only for a complete traversal.

**Budget stops.** A budget stop is `partial` with `node_budget` or `time_budget`. The continuation
replays the traversal from the selected symbol. The node budget counts only records beyond the
furthest point an earlier page discovered, so every continuation advances.

### `file_api` / `skeleton <path>`

Default limit: 100 entries.

**Input.** A repository-relative or absolute path inside the workspace root. Repository-relative,
absolute and on-disk case aliases resolve to the same file through `make_repo_relative_on_disk`.

**Errors:**

| Code | When |
| --- | --- |
| `invalid_path` | any `..` segment |
| `not_a_file` | a directory |
| `outside_workspace` | a path outside the workspace root |
| `file_not_indexed` | a file the profile does not know, or one that is excluded or missing (one message, so a withheld file looks unknown) |

**Entries** are the public symbols with a declaration or definition location in the file;
implicit declarations are not API.

- Order: their first location here (begin line and column, end line and column), then stable ID.
- Each entry has `here` (per-unit copies merged, with a unit count) and `elsewhere`, the other
  visible declaration/definition locations (owner links).

**Includes** are the file's public direct includes in directive order. Each lists operand,
activity, resolution, target, condition path and unit count.

Per-unit observations of one directive are merged only when every rendered field is equal:

- the directive location (path, span, span hash, content hash);
- directive kind, operand kind and operand;
- expanded spelling, activity, resolution and target.

The condition path is lexical text of the same bytes, so equal content implies an equal path.
Observations that differ in any of these fields are separate entries with their own unit counts.
For example, a macro include expanded to `api.inl` in one unit and to `./api.inl` in another gives
two entries.

**Null fields.** `module` is `null` with `module_reason`. An `.inl` file has `inl_context: null`
with a reason.

**Scan.** Every call scans the file's locations completely (no partial mode).

### `check_freshness` / `check`

`ReadOnlyStore::inspect_freshness` runs as follows:

1. Read the profile's units and inputs from one snapshot.
2. Hash every distinct input twice within the budget (default 2 s; CLI `--budget`).
3. Re-read the fact generation.

It takes no writer lock and persists nothing, neither a fresh mark nor a downgrade.

**Fresh** requires all of these:

- both passes completed and equal the recorded hashes;
- the generation is unchanged;
- the unit's build-action currency is verified.

UHT never becomes fresh.

**Status from the outcome:**

| Outcome | `status` |
| --- | --- |
| `budget_exhausted` | `partial` (checked and unchecked counts, detected stale units; nothing fresh) |
| `generation_changed` | `partial` (re-run guidance) |
| Complete with any stale unit | `stale` |
| Complete with no stale unit | `success` |

**Pagination.** The records are the stale units.

- **Complete inspection:** the count is exact (`complete: true`, `total`, `omitted: 0`).
- **`budget_exhausted` or `generation_changed`:** unchecked inputs could still make more units
  stale. The response has `complete: false`, `total` and `omitted` set to `null`, and
  `limit_reason: "time_budget"` or `"generation_changed"`.
- **`next_cursor` stays `null`.** This is a documented exception to R3: a check has no
  continuation. Re-run it with a larger `--budget` (MCP `budget_ms`).
- **Every unit already detected stale:** the exact total stands, so the response keeps
  `complete: true`.

**Diagnostics** follow the page:

- `budget_exhausted`, incomplete count: the stale count is "not exact"; re-run with a larger
  `--budget`/`budget_ms`.
- `budget_exhausted`, all units stale: every unit is already detected stale, so the count is exact,
  although the inspection stopped early.
- `generation_changed`: a publication changed the index during the check. Re-run `check`, with a
  larger budget if it does not finish.

**Result.** The live overlay is `result.live`; the persisted state stays in `layers`. Changes are
listed with path, role and unit count. Non-fresh units come with `livecodemap build` guidance. It
never runs a compiler, a build, UBT or UHT.

`Store::inspect_freshness` (the writer path) keeps its lock, recheck, persistence and downgrade;
both paths share the snapshot loading, hashing and per-unit conclusion code.

## CLI

```
livecodemap [--db <index.sqlite>] [--profile <key>] [--root <workspace>] [--json] <command> ...
  ask <identifier...>   [--limit N] [--cursor C]
  callers <selector>    [--outgoing] [--depth N] [--all] [--limit N] [--node-budget N] [--time-budget MS] [--cursor C]
  skeleton <path>       [--limit N] [--cursor C]
  check                 [--budget MS]
  mcp
```

- `--db` defaults to `LIVECODEMAP_DB`.
- With one profile in the store it is used; otherwise `--profile` is required (`profile_required`
  lists them).
- `--root`, when given, must be the store's workspace root.
- Exit codes: 0 for success, partial and stale; 1 for an error response; 2 for a usage error
  (message on stderr).
- Arguments and output are UTF-8. Non-ASCII identifiers and paths are kept verbatim.

## MCP server

`livecodemap --db <index.sqlite> [--profile <key>] mcp` serves newline-delimited JSON-RPC 2.0 on
stdin/stdout:

- Streams are in binary mode with LF-terminated lines, one object per line.
- stdout carries protocol messages only.
- The database and profile are fixed at launch; each `tools/call` takes its own snapshot.

**Protocol revision.** `initialize` replies with the requested revision when it is one of
`2025-11-25`, `2025-06-18` or `2025-03-26`, otherwise with `2025-11-25`. The reply is
`capabilities: {tools: {}}` and `serverInfo: {name: "livecodemap", version}`.

**Requests.** Supported: `initialize`, `ping`, `tools/list` (exactly the four tools, object-root
`inputSchema`s with MCP defaults) and `tools/call`.

- Any message without an `id` is a notification and is never answered.
- Requests before `initialize` are served.

**`tools/call` result.** `content[0]` is the serialized response JSON, `content[1]` a human
rendering, `structuredContent` the response object, and `isError` is true exactly for
`status: error`. That covers bad selectors, ambiguity, expired cursors, a missing database and
any other tool-level failure.

**JSON-RPC errors:**

| Condition | Code |
| --- | --- |
| Invalid JSON | `-32700` |
| A non-object or invalid envelope, a batch array or a bad `id` type | `-32600` |
| An unknown method | `-32601` |
| A missing or unknown tool name, non-object `arguments`, a missing required argument, an unknown argument or a wrong argument type | `-32602` |

Out-of-range values (for example `limit: 0`) are tool errors. Request IDs are echoed verbatim.

## Known limitations

- **Existence scan.** The effective withheld set requires one `exists` stat per file row of the
  profile for every call. That is about 44 ms on the private 8,795-row Unreal profile.
- **Name list.** The component tier scans the profile's distinct names. It is cached per (profile,
  fact generation) for a long-lived engine (MCP); each CLI process pays it once (about 0.1 s on
  that profile).
- **`file_api` scan cost.** It scans every per-unit location row of the file, because there is no
  covering index (the schema stays v3). The largest header of that profile, a generated protobuf header
  seen by about 100 units with 1.5 M location rows, takes about 1.9 s from a new process.
- **Replay continuation.** Each page re-runs the traversal prefix, so the work over a long `--all`
  chain grows quadratically. Depth (≤5) and both budgets bound it.
- **Not implemented.** No snippets, ranking, natural-language search or class cards. The `.inl`
  use context and module/target metadata are not recorded. No UHT freshness evidence exists, and
  the read-only check is as slow as the writer inspection: the no-change Unreal profile needs about
  18 s, so the 2 s default ends `partial`.
