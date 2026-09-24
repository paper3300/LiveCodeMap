# First public release acceptance ledger

**2026-09-25 bounded replay expansion:** Herdr coordinator/Claude implementer/GPT-6-Sol-high reviewer completed the build diagnosis and Temppal replay stage 5. Host Release single/parallel-4 builds pass with node reuse disabled; Release CTest passes 5/5. The sandbox failure is MSBuild worker-pipe access denial; a separate reused-node Path/PATH environment collision is recorded. Product change is limited to 32 report-export lines in `tools/lcm_replay/main.cpp`. The task and reflected-class TUs each passed two measured replays, source oracles, actual-read hash audits and independent bounded acceptance; all JSON fields except elapsed time match within each pair. Generated-header directive-scanner limits, global unresolved sites, historical input/current unity differences and unproved UBT/UHT freshness remain explicit. No whole Phase 1, reflection, SQLite/search/MCP or release gate is accepted. Evidence: [execution record](2026-09-25-Next-Stage-Execution.md). Next contract: [TU storage and freshness](2026-09-25-Multi-TU-Index-Handoff.md).

**2026-09-22 resumption:** the submitted direct-base/include snapshot matched all 13 source/test/document hashes and both Release library hashes. Coordinator reruns passed Debug, Release and RelWithDebInfo CTest 5/5 each, plus Clang-disabled Release CTest 3/3. A new GPT-5.6-sol code-only audit reproduced a pre-existing implicit-template-base omission and a new empty include-owner invariant defect. Both bounded corrections are now independently accepted after fresh linking and adversarial probes; the coordinator's final probe rerun exits 0 with `failures=0`. Affected suites pass all three configurations and Clang-disabled core (core 106/2,482, analyzer 28/2,759, smoke 8/67). The template-base audit includes selected partials, explicit-instantiation kinds, distinct argument evidence, copied-root/line-shift local identities, external boundaries and contribution survival. Whole-family inheritance/template coverage, Phase 1 and release acceptance remain open. No implementation reports or explanations were given to the reviewer. Current checkpoint: [2026-09-22 recovery](2026-09-22-Progress-and-Resume.md).

**Fresh-session restart at the user's request, 2026-09-18 19:39 KST; status updated after the 20:36 review.** Coordinator `w8:p6` uses current files and the [progress and resume report](2026-09-18-Progress-and-Resume.md); new implementation/review agents are `lcm-impl-fresh` (`w8:p7`, Claude) and `lcm-review-fresh` (`w8:p8`, Codex). Prior conversation context is not reused and old agents remain idle. Bounded Phase 1C is accepted after fresh-link independent review; direct-base/include work is activated. No whole release gate has passed. Earlier entries below record their original evidence, not results for the current source.

Baseline: [LiveCodeMap PRD v0.3](LiveCodeMap-PRD.md). Coordinator-owned companion to the [execution plan](First-Release-Execution-Plan.md). The PRD remains authoritative, including every bullet within the sections indexed here. This ledger is not a reduced specification or a claim that a broad requirement is proved by a narrow test.

As of 2026-09-18: the foundation, pinned SDK prerequisite, bounded ordinary-C++ Phase 1A/1B/1C and adapter correction are accepted after independent reviews. All 50 explicitly selected fmt C++ contexts complete frontend diagnostics without compiler errors/timeouts; independent coverage review confirms retained profiles, warnings and limitations. The final Phase 1C Declaration correction passes 27 independent runs and 1,002/1,002 oracle checks with unchanged source/library hashes, plus coordinator reruns of all nine executables (core 102/2,438; analyzer 23/2,406; smoke 8/67 per configuration). The earlier core-only CTest 3/3 evidence remains valid for unchanged core sources. Direct-base/include implementation is now active. **No whole first-release gate below has passed.** No publication has occurred. Recheck current code, tests, artifacts and external state before changing a gate to passed.

## Functional requirement coverage

Fresh correction evidence at approximately 20:02: the coordinator checks the 11 hashes in `build/reports/phase1c-correction-handoff.md`, directly reruns all nine current executables (core 102/2,438; analyzer 23/2,295; smoke 8/67 in Debug, Release and RelWithDebInfo; zero failures/skips), and confirms unchanged source hashes during verification. The separately rebuilt Clang-disabled Debug tree passes CTest 3/3 and core 102/2,438. Evidence is preserved in `build/coordinator-fresh/phase1c-2000/`. Fresh-link independent review is active; this is not Phase 1C acceptance or a release-gate result.

All 55 FR sections and four SEC sections are indexed below. A family is not complete until every requirement in its listed PRD sections has direct evidence, including negative cases and the required environments.

At approximately 20:14 the fresh independent audit rejects that correction handoff for two additional Declaration NTTP defects: owner-specialization path/line leakage (H3) and function-specialization overload/target collisions (H4). `build/reviewer-fresh/final-review.md` records 608/615 passing oracle checks (seven failures are H3 observations), separate H4 confirmation and unchanged source/library hashes. A narrow explicit-limitation correction is activated; stable named controls remain required. Phase 1C is still unaccepted and direct-base/include remains inactive.

At approximately 20:30 the new Declaration correction is frozen. All 13 handoff hashes match, and the coordinator reruns all nine executables successfully (core 102/2,438; analyzer 23/2,406; smoke 8/67 in each configuration), with unchanged source hashes. Evidence: `build/coordinator-fresh/phase1c-2030/`. Fresh-link independent v2 review is active; no acceptance is inferred. Unsupported specialization-valued callees currently have explicit analysis limits and no confirmed edges, but lack per-call unresolved records; that broader requirement remains open.

| PRD sections | Required evidence beyond the accepted prerequisite/increment | Current assessment |
| --- | --- | --- |
| FR-BLD-001..005 | Explicit build/profile and real context priority; response/PCH/generated provenance; ambiguous selection; ordinary-C++ scope and no-context structure/text fallback without Unreal execution | Compilation DB selection/normalization and safe bounded invocation only; full contracts incomplete |
| FR-GPH-001..005 | Complete logical identities/template forms/aliases; original direct conditional includes and external boundaries; fact/candidate/unresolved separation and contribution survival | Bounded IR and analyzer evidence accepted; template/include/alias completeness and product-wide behavior incomplete |
| FR-CPP-001..005 | Direct-base access/virtual/order; runtime candidate query; full callable/lifetime/capture and wrapper semantics; exact boundaries against false targets | Phase 1A and bounded callable/lambda Phase 1B/1C accepted, including tested hidden closure and unnamed-constructor cases; direct-base increment active; full-family coverage incomplete |
| FR-UE-001..006 | Actual selected UBT/UHT profile; original-source mapping, reflection/event/RPC/interface/delegate/module/target; unity/PCH/inl/split evidence | Temppal selected by user; local UE 5.7.4 and existing TemppalEditor/Win64/Development/x64 receipt confirmed. Actual context freshness and analyzer verification remain open; see current recovery record |
| FR-IDX-001..007 | SQLite normalized facts; atomic committed generations and crash/cancel tests; full dependency/profile invalidation; budgeted freshness; layer applicability; ignore; independent worktrees/workspace catalog | In-memory facts/status prerequisites only; storage/product contracts incomplete |
| FR-RET-001..004 | Versioned lexical/ranking fields and graph projection; identifier decomposition; class cards and source-hash-verified snippets; development/untouched holdout evaluation | Not implemented or measured |
| FR-API-001..004 | All named commands/tools; shared semantic response and defaults; ambiguous selectors; generation-bound resumable pagination with no loss/duplication | Not implemented or verified |
| FR-DEEP-001..003 | Explicit provider choice and symbol windows; independent failures; compatible content cache; actual usage/cost/time; unchanged rerun makes zero provider calls | Not implemented or verified; provider/model/spend decision outstanding |
| FR-AGT-001..006 | Actual supported-host contracts; selected project-local init/dry-run/remove; preserved user config; ownership manifest; fail-open hooks, status and idempotency | Not implemented or verified; current host interfaces must be checked at implementation time |
| FR-VIZ-001..004 | Query-scoped viewer, confidence/freshness/pagination representation; loopback/authentication/security tests; bounded offline HTML without whole DB/graph | Not implemented or verified |
| FR-DST-001..006 | Lockstep versioned npm/ZIP/mcpb/source-build outputs; tag CI, OIDC/provenance, allowlist, schema/downgrade policy; license/contribution/third-party notices | Not implemented or verified; ownership/CI/publication prerequisites outstanding |
| SEC-001..004 | Common selected-input/ignore/deep boundaries; no asset graph; no required telemetry; actual usage only; private-source/derived-information exclusion from every public artifact | Local ignored diagnostic/public-corpus discipline observed; product/release-wide verification incomplete |

## Required surfaces and artifacts

- CLI: `build`, `ask`, `skeleton`, `grep`, `callers`, `map`, `blast`, `check`, `viz`, `init`, `remove`; installed executable name `livecodemap`.
- MCP: `find_code`, `find_all`, `file_api`, `trace_calls`, `repo_map`, `check_freshness`.
- Shared responses: project kind/applicability, profile and generations, per-layer freshness, status/confidence/diagnostics and truthful pagination. Partial exploration has unknown totals; completed exploration can still require more result pages. Cursor validity includes all applicable snapshot/ranking/ignore state.
- Defaults: `ask` 8; `find_code` 5; `callers` 1 hop/20 callers; `trace_calls` 1 hop/20 records; `blast` 2 hops/50 impacts; physical `file_api` 100 entries. `--all` never removes traversal budgets. Source snippets require current-byte hash agreement or verified remapping/withholding.
- Hosts: Claude Desktop, Claude Code CLI, Codex and Codex CLI. No unselected home/machine configuration writes; no Desktop project hooks. Hook failure must not block editing/turn completion or trigger forced continuation/build/deep.
- One `vX.Y.Z` source tag: `@livecodemap/cli`, exact-version `@livecodemap/win32-x64`, standalone Windows ZIP, Claude Desktop `.mcpb`, SHA-256 checksums, SBOM, `THIRD_PARTY_NOTICES`; documented source build with matching CLI/MCP version/behavior.
- No binary-download `postinstall`, silent executable replacement, query/hook version-network access, automatic incompatible-DB migration/deletion, or implicit wiring update on package upgrade. Publish requires explicit appropriate authority and all release gates; local implementation authorization is not publication authorization.

## Quality evidence required

| Area | Required release evidence | Status |
| --- | --- | --- |
| Public corpus/profile | Fixed Synthetic C++, Synthetic UE, fmt and Cesium revisions/profiles; separate no-UE Windows and real-UE Windows tracks; Cesium structural lane distinct from full semantic lane | Revisions resolved; fmt source/build/input snapshot prepared only |
| Graph (12.2) | Synthetic expected symbols/relations/states 100%; confirmed precision >=99%; direct recall >=95%; possible-target recall >=90%; zero false confirmed promotion, generated-internal leakage or deleted/ignored residue; three clean-build hashes identical | Not measured at release scope |
| Graph accounting | Freeze scope/gold before evaluation; retain failed/unresolved in-scope answers in denominators; micro and relation-specific metrics; possible-target precision/recall/candidate median/p95; exact synthetic candidate sets and comparable Graft/blind UE assessment | Not prepared/evaluated |
| Retrieval (12.3) | Untouched stratified holdout: exact identifier/path Top-1 >=98%, overall Hit@5 >=90%, identifier-free natural-language Hit@5 >=80%, unrelated test/helper Top-5 rate <=5%; fixed units/mappings and no-answer false positives | No frozen query/gold splits or measurements |
| Graft comparison | Pinned 0.18.0, same snapshot/query/scope/context budget/deep settings; common C++ quality and Recall@5 non-inferiority; separate unsupported UE functions; numerators/denominators/type breakdown/95% intervals | Commit resolved only |
| Mutation (12.4) | All listed source/header/reflection/profile/ignore/worktree/external/generated/context/toolchain/response/CMake mutations, snippet changes/races; expected invalidated TUs, replaced contributions, surviving evidence and freshness | Bounded in-memory regressions only; complete mutation harness absent |
| Traversal | High-fan-out/cycle budget interruption and resumed cursor union equals full gold set; no duplicate/omission/false totals or mixed generations | Not implemented/verified |
| Deep (12.5) | Real provider input/output/cache usage, cost and p50/p95; unchanged rerun token zero; failure/missing/cache accounting; local providers use same workload/output validation | Not run |
| Early A/B | Before retrieval tuning/deep, representative Navigation/Impact/Patch development tasks, baseline versus retrieval and applicable Graft; record correctness/tokens/time/omissions | Not run |
| Final A/B (12.6) | 12 public C++/UE tasks (4 Navigation, 4 Impact, 4 Patch), fixed gold/forbidden files/tests/rubric; baseline, retrieval, instruction and hook conditions plus applicable Graft; same model/settings/budgets; randomized independent clean sessions; >=3 repeats, 5 if disagreement/CV>20% | Not prepared/run |
| Agent value | All-run correctness non-regression including failures/timeouts; wrong-file/sibling omission not increased; paired-success actual input ratio task-median aggregate <=0.70; every task has a successful pair; actual usage includes schemas/instructions/hooks/cache correctly; task-level uncertainty and time/cost reported | Not measured; text length is not token evidence |

The fmt upstream build succeeded, but upstream CTest remains **19/21**, with `chrono_test.locale` and `unicode_test.legacy_locale` failing on this host. They are not skipped or patched, and the precise cause is unproven. Neither that upstream result nor the context-only survey proves LiveCodeMap graph or retrieval quality.

Initial context-only census: all 50 C++ commands parse/identify their source but are rejected by the accepted Phase 1A safety gate (`--`: 50; `-imsvc` and `-Wno-deprecated-declarations`: 46 each). This is a recorded compatibility gap, not a reason to exclude those inputs from an eventual in-scope recall denominator. No corpus frontend analysis has run.

The adapter correction is now handed off and frozen for independent audit. Coordinator reruns pass core 100/1,998, analyzer 20/1,510 and smoke 8/67 in Debug, Release and RelWithDebInfo; rebuilt Clang-disabled core passes 100/1,998. Agent2 reports 50/50 gate-ready C++ contexts with all identities/alternatives preserved, but a fresh independent census and negative-input boundary review are still underway. These are adapter checks, not frontend or release-quality measurements.

Independent follow-up verifies that 50/50 census and zero mismatches against all 51 frozen input records. Adapter acceptance remains withheld for a reproduced duplicate-selected-input defect: same-file/alias occurrences produce multiple Clang jobs but normalization drops the repeat as safe. The narrow correction and re-review are required; no corpus frontend or release-quality result is claimed.

Update at 16:40: the duplicate-input correction is independently accepted; freshly linked negative probes have zero failures and the census remains 50/50. Coordinator reruns pass core 101/2,428, analyzer 20/1,552 and smoke 8/67 in all three configurations and rebuilt core-only 101/2,428. Actual frontend-readiness diagnostics now begin with explicitly selected isolated units. The first `src/format.cc` command exits 0 without compiler errors but has 22 limits and 812 unresolved sites; this is not graph-quality or full-feature acceptance. Phase 1C callable/local-type implementation is separately activated.

The final readiness survey completes all 50 C++ IDs with no compiler errors/timeouts, preserving every alternative profile. Independent review at 16:47 confirms exact log/ID coverage and frozen executable identity. Ten warnings, 29 limitation categories and unresolved evidence remain; no quality denominator or precision/recall claim is derived from these counts. The source-hash snapshot and database are unchanged. Detailed evidence is in `build/reports/fmt-frontend-readiness.md`; the single diagnostic run does not pass any whole release gate.

## Performance, resilience and usability

PRD 11.1 requires Windows 11, >=16 logical cores, >=32 GiB RAM and NVMe, actual toolchain/cache-state records, and both public tracks. This workstation meets the hardware floor; it is not a substitute for the clean no-UE and selected-UE lanes.

| Measurement | Required target; all currently unmeasured at product scope |
| --- | --- |
| No-change freshness p95 | <=100 ms |
| Warm search p95 | <=500 ms |
| Complete default 1–2 hop traversal p95 | <=1 s, with first-page timing separately reported |
| Single-file structural/retrieval update response p95 | <=2 s |
| Single-cpp semantic update p95 | <=10 s |
| Large-header stale response | <=2 s |
| Worktree seed plus validation | <=1 min |
| 10K/50K/100K synthetic-symbol build throughput | Degradation <=30%, without deleting facts to meet targets |

Include CLI startup/freshness in query latency; distinguish cold indexing and required project build costs; never count budget-truncated traversal as completed latency. Verify crash/cancel isolation, partial/deep/ranking failure survival, fail-open hooks, next-action diagnostics, bounded truthful output and no public absolute-path disclosure (11.2–11.3).

## Release gate disposition (PRD 13)

| Gate | Authoritative evidence needed for a passed decision | Current disposition |
| --- | --- | --- |
| 13.1 Functional | Full surfaces, both real Windows lanes including fallback and UE-only facts, idempotent preserved host wiring, secure viewer/offline HTML, provider-independent structural product | Incomplete |
| 13.2 Quality | All required 12.2–12.6 evidence, comparable Graft results, release-scope three-clean-build hashes and zero false confirmed promotion | Missing/incomplete |
| 13.3 Agent value | Fixed full A/B all-run correctness/omissions, paired actual token threshold and evidence-based default configuration decision | Missing |
| 13.4 Distribution | npm organization ownership; tag CI build/test/clean install/version/MCP/init/remove; provenance/checksums/SBOM/notices; allowlist/private-data/credential/path scans; Apache-2.0 and contribution policy | Missing/incomplete |

The user selected local Temppal for the first real UE test on 2026-09-22; engine/version and the initial editor profile are recorded in the current recovery record. Real UE context/runner validation and separate public-corpus evidence remain open. Other requested external decisions remain open: A/B/deep provider/model and usage budget; npm organization/release CI ownership and eventual publication authorization. Continue independent local work while these are not the immediate dependency; do not invent their evidence or silently replace them with mocks.

Before final completion, expand each applicable PRD bullet into a checked evidence item, inspect current artifacts/results directly, and record any missing or contradicted evidence. Passing Phase 1A or any later bounded increment cannot close a family or release gate by implication.
