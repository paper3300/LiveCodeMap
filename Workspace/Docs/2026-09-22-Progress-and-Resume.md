# 2026-09-22 orchestration recovery

Status: resumed work completed through the bounded direct-base/include audit and both corrections. Include ownership and template-base corrections are independently accepted. Full Phase 1 and first release remain incomplete.

The 2026-09-18 17:53 stop report is superseded by later evidence. Phase 1C was accepted in the fresh session, and the direct-base/include increment was implemented and submitted in `build/reports/base-include-handoff.md`. Recovery resumed from that six-point direct-base/include assignment, completed its audit and the two corrections described below. No complete Phase 1 or release gate has passed.

## Roles and ownership

- Agent1: GPT-6-astra / high coordinator in Herdr `w8:p9`, `lcm-coordinator`; own session turn metadata confirms the model. Owns orchestration, plan, ledger and recovery notes.
- Agent2: Claude Opus, `lcm-implementer`, `w8:pB`; owns product code, tests, implementation documentation and product builds. Startup UI reports Opus 5 / high effort.
- Agent3: GPT-5.6-sol / high, `lcm-reviewer`, `w8:pC`; new session for independent code-only review. Do not give it implementation handoffs, author explanations, or previous verdicts. Startup update dialog cleared externally; the coordinator verified the model and normal input screen, then assigned the blind review. Scratch: `build/reviewer-20260922/`.

All panes share `D:/Git/LiveCodeMap`. Product files remain untracked; preserve them and every existing diagnostic/build tree. No staging, commit, reset, cleanup, publication or external provider experiment is part of this recovery.

## Evidence recovered

All 13 product/test/document hashes and both Release library hashes in the direct-base/include handoff match the current files. This establishes snapshot identity, not correctness. Coordinator CTest reruns pass Debug 5/5, Release 5/5, RelWithDebInfo 5/5 and separate Clang-disabled Release 3/3. RelWithDebInfo's saved log confirms core 104 cases / 2,461 assertions, analyzer 26 / 2,663, smoke 8 / 67, with zero failures/skips. Evidence: `build/coordinator-20260922/relwithdebinfo-ctest.log`, `coreonly-ctest.log` and `frozen-source-hashes.json`. Product source stays frozen until the independent reviewer reports actionable findings or a bounded acceptance.

## Independent audit outcome

The reviewer freshly built the analyzer and core, inspected only code/test/build artifacts, and created `build/reviewer-20260922/probe/probe.cpp`. It found a pre-existing High template-base requirement gap (`struct D : G<int>` loses its resolved direct edge) and a new Medium storage invariant defect (`add_direct_include` accepts an empty contribution owner). The coordinator reran the same standalone executable in its own scratch: three failed assertions representing those two findings. Ordinary/macro/explicit-specialization base metadata and exercised generated-include semantic checks otherwise passed. Whole direct-base coverage and Phase 1 remain unaccepted.

The empty-owner correction is independently accepted: only `src/lcm/facts.cpp` and `tests/facts_tests.cpp` changed, rejection preserves empty/populated state and hashes, and valid owned removal still passes. Coordinator reruns pass the affected suites in all three configurations and Clang-disabled core. Current counts: core 105/2,469; analyzer 26/2,663; smoke 8/67. The freshly relinked independent probe retains only the separate template-base failure. JUnit results are under `build/coordinator-20260922/ownership-*.xml`; the accepted code snapshot is `ownership-accepted-source/`.

Agent2 submitted the bounded template-base correction recorded at the end of the execution plan: compiler-selected primary/partial pattern target, concrete argument evidence, truthful implicit/explicit-instantiation mode, no per-instantiation public nodes, and required regressions. The coordinator verified all submitted hashes and reran the affected suites in all configurations plus Clang-disabled core: core 106/2,482; analyzer 28/2,759; smoke 8/67. Snapshot and JUnit evidence: `build/coordinator-20260922/template-submitted-source/`, `template-submitted-hashes.json` and `template-*.xml`.

Agent3 accepted the frozen template-base increment after fresh linking against its independent build and compiler-only oracle: `template_base_edges=1`, `template_base_limit=0`, `failures=0`. The coordinator reran the final expanded probe in a separate runtime directory with the same results. Evidence: `template-final-review-terminal.txt` and `template-final-v2-probe.log` in the coordinator directory. Coverage includes selected partials, distinct same-pattern bases, exact spans, external patterns, contribution survival, block-local/lambda arguments across copied roots and five-line shifts, and explicit instantiations both before and after base use. Instantiation classification reflects Clang's final TU AST, not source-order timing. Deliberate malformed/include-error fixtures explain stderr diagnostics; the probe exits 0. No implementation explanation/report was sent to the reviewer. Unsupported template argument coverage is not exhaustive and broader Phase 1 requirements remain open.

The skipped-callee gap also has compiler-valid direct-call and constructor reproductions in implementer-owned scratch, but no implementation is authorized for it yet. The user has now selected Temppal as the first real UE test project and product motivation; see the confirmed environment below.

## First real Unreal project: Temppal

On 2026-09-22 the user selected `D:/ProjectT_Main/Program/Client/Temppal`. Treat actual navigation, impact analysis and source-evidence needs in this project as the initial product use cases. This is the user's project, not a public benchmark corpus; existing public release evidence requirements remain separate.

- Project descriptor: `Temppal.uproject`; runtime module `Temppal`, editor module `TemppalEditor`. `EngineAssociation` is empty, so it does not independently establish an engine version.
- Engine root: `D:/ProjectT_Main/Program/Client/Engine` (source-tree root: its parent). Local `Engine/Build/Build.version` reports **5.7.4**, compatible changelist `47537391`, branch `UE5`. The Windows registered source build points to the same parent directory.
- Initial coordinator-selected profile: **TemppalEditor / Win64 / Development / x64**. Existing `Binaries/Win64/TemppalEditor.target` confirms that exact tuple and matching engine version/BuildId. `Source/TemppalEditor.Target.cs` and `Source/Temppal.Target.cs` define editor/game targets. DebugGame editor artifacts also exist; they are a separate profile.
- Initial inspection is read-only. Existing response files and generated project files are inputs to readiness assessment, not proof of current freshness or successful LiveCodeMap analysis. No UBT/UHT/build or project/engine modification has run as part of this selection.
- Next UE verification must freeze selected source/build-context hashes, confirm generated-header and response/PCH/unity provenance, and compare analyzer facts with original project source. Record unsupported context explicitly. Synthetic tests and existing build receipts cannot satisfy that gate by themselves.

Agent2's bounded read-only survey is preserved in `build/implementer-resume-20260922/temppal-readiness.md` (worker observations, not independent acceptance). The coordinator separately counted 604 Development module response files and 1,322 generated headers in the Temppal UHT directory. Existing profiles have differing timestamps; freshness is unproved. The survey identifies nested response files, forced includes, MSVC PCH, engine-relative include paths and unity/generated-source mapping as context work to resolve before replay. No ready compilation database was identified. The first candidate is `Source/Temppal/Game/System/Guild/TPGuildSystem.cpp` for multiple/virtual inheritance; task dispatch and reflected UI classes follow as separate cases. No analyzer run or UE acceptance occurred. No product code changed, so the code-only reviewer was not given the worker survey or assigned a new code verdict.

## Next actions

1. Recheck current Herdr sessions, source hashes and workspace changes before the next assignment; keep the accepted code frozen until a bounded task is activated.
2. Next prepared task: skipped-callee unresolved-site correction for measured direct calls and constructors. Reproductions are in `build/implementer-resume-20260922/measured-evidence.md`; reviewer must receive actual code and coordinator acceptance requirements, not this worker explanation. Add a truthful unsupported-identity reason, retain original compiler evidence locations, and do not invent targets. Implicit destruction/argument-owner paths need separate proof and are not silently included.
3. Other ordinary-C++ Phase 1 gaps include broader template/alias/type-reference/allocation and missing-context behavior. Temppal is now selected for the first real UE test; context readiness and actual analysis remain unverified. Consult the PRD and ledger; neither this checkpoint nor the bounded approvals waive those requirements.
4. Continue ownership discipline: Agent2 changes product/tests/implementation docs, Agent3 reviews actual frozen code only, coordinator owns plan/ledger/recovery. Preserve all untracked files and previous evidence; no commits or publication occurred.
