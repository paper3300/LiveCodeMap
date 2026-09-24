# Foundation core (Phase 1 prerequisite)

Status: built and tested on Windows 11 with Visual Studio 2022 (MSVC 14.44.35207), CMake 3.30.0,
Windows SDK 10.0.26100.0. This is the prerequisite skeleton for PRD v0.3 Phase 1. It contains **no
semantic analyzer**: nothing here resolves C++ symbols, and the semantic-graph gates of the PRD are
not evaluated by this code. See `clang-sdk-acquisition.md` for the analyzer dependency.

## Layout

| Path | Purpose |
| --- | --- |
| `CMakeLists.txt`, `CMakePresets.json` | C++20 project, preset `vs2022-x64`, build presets `debug`/`release` |
| `cmake/Dependencies.cmake` | exact pinned third-party downloads into the build tree (never machine-wide) |
| `src/lcm/hash.*` | SHA-256 (in-house, tested against published vectors and hashlib answers) |
| `src/lcm/source.*` | repository-relative paths, UTF-8 path conversion, byte spans, content hashes, hash-verified snippets |
| `src/lcm/identity.*` | canonical key and `cm1:<hash>` stable IDs |
| `src/lcm/facts.*` | logical symbols with multiple locations, evidence-backed relations, unresolved sites, contribution removal, fact hash |
| `src/lcm/status.*` | per-layer availability/freshness/applicability and user status summary |
| `src/lcm/compile_db.*` | Clang JSON compilation database ingestion |
| `tests/` | doctest behavior tests; `tests/fixtures/compile_db/*.json` inputs |

## Pinned dependencies

| Dependency | Version | Source | SHA-256 |
| --- | --- | --- | --- |
| nlohmann/json (single header) | 3.12.0 | github.com/nlohmann/json release asset `json.hpp` | `aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63` |
| doctest (single header) | 2.4.12 | raw.githubusercontent.com/doctest/doctest/v2.4.12/doctest/doctest.h | `94029a7d32da24a56249658147dbd2b33ff0b9ed665295cbbaf19aafff5b0ced` |

Both are fetched by CMake `FetchContent` with `URL_HASH` verification into `build/<preset>/_deps` and
copied to `build/<preset>/third_party/include`. nlohmann/json is a private dependency of `lcm_core`
(only `compile_db.cpp` includes it); doctest is test-only.

## Build and test

```
cmake --preset vs2022-x64
cmake --build --preset debug           # everything on the static release CRT (/MT); Clang smoke when the SDK is present
ctest --preset debug
cmake --build --preset release          # core + Clang smoke when the SDK is present
ctest --preset release
cmake --build --preset relwithdebinfo   # same as release with debug info
ctest --preset relwithdebinfo
```

Bootstrap the SDK first (once) with `tools\bootstrap-clang-sdk.ps1`; without it the Clang-based
targets are absent and configuration says so.

Own code compiles with `/W4 /WX /permissive- /utf-8`.

### Clang SDK discovery (`cmake/ClangSdk.cmake`)

- `LCM_WITH_CLANG` (default ON) means "use the pinned SDK when it can be found". Discovery looks at
  `LCM_CLANG_SDK_DIR` and then at the bootstrap location `build/deps/<archive_root>` from
  `cmake/clang-sdk.json`. It imports `LLVMConfig.cmake`/`ClangConfig.cmake` with `NO_DEFAULT_PATH`
  and refuses any SDK whose version differs from the pin.
- When the SDK is missing, configuration prints an explicit STATUS diagnostic naming the searched
  paths and the bootstrap script, and the Clang-based targets and tests are simply absent. Nothing
  substitutes for them. `LCM_REQUIRE_CLANG=ON` turns the diagnostic into a configuration error for
  CI lanes that must have the analyzer.
- CRT policy is project-wide: the root `CMakeLists.txt` sets `CMAKE_MSVC_RUNTIME_LIBRARY
  MultiThreaded` for every target and configuration (the SDK's `LLVMConfig.cmake` declares the same
  value, so the SDK-enabled build was already `/MT`; the setting makes it explicit and applies it to
  the Clang-disabled build too). `lcm_configure_clang_target()` adds the SDK-specific pieces: SYSTEM
  includes with `/external:W0` and `/wd4702`, LLVM definitions, `/GR-` when `LLVM_ENABLE_RTTI` is
  off, `/bigobj`. The Debug configuration builds own code unoptimized on the release static CRT
  (`_ITERATOR_DEBUG_LEVEL=0`, verified with `dumpbin /directives`) and runs every test. Core,
  smoke and future analyzer therefore share one CRT; the smoke library not linking `lcm_core` is a
  scope choice.
- Discovery trust: the bootstrapped SDK is accepted only with the provenance marker plus required
  paths; an `LCM_CLANG_SDK_DIR` override is flagged provenance-unverified, its declarations are
  checked when present, and a `/MT` compile/link/run probe against `clangBasic` is decisive (see
  `clang-sdk-acquisition.md`).
- `LCM_CLANG_SDK_SEARCH_DEFAULT=OFF` disables the bootstrap-location search so a lane can insist on
  an explicit `LCM_CLANG_SDK_DIR`; an explicit directory without `ClangConfig.cmake` is warned about
  and ignored.
- The SDK's exported `LLVMDebugInfoPDB` target bakes the LLVM build machine's DIA SDK path; the
  module remaps it to the local VS instance. See `clang-sdk-acquisition.md` for details.

### Clang smoke probe (`src/lcm/clang/smoke.*`, `tests/clang/smoke_tests.cpp`)

Not the analyzer. It runs the Clang front end in-process over an in-memory copy of
`tests/fixtures/cpp/smoke/overloads.cpp` (no includes, so no host include configuration is needed)
through an `ASTFrontendAction` + `RecursiveASTVisitor`, and records every `FunctionDecl`
redeclaration (qualified name, compiler-reported owner `DeclContext`, parameter-type signature,
`isThisDeclarationADefinition`) and every `CallExpr` with a `getDirectCallee()` that lies inside a
function **body** (caller contract). Calls in declarations outside any body, such as default
arguments, are listed separately as `calls_outside_bodies` and counted once even though every
redeclaration inheriting the default argument reaches the same expression. Tests assert that
declaration and definition are two redeclarations of one function under owner `geo::Shape`, that
three call sites spelled `print` resolve to three different functions (`print(int)`,
`print(double)`, `Other::print(int)`), that a comment and a string literal containing `print(99)`
produce no call, that a broken TU fails with compiler diagnostics, and that an indirect call
through a function pointer yields no invented target.

## Design decisions and invariants

### Stable IDs (`identity.hpp`)

- `cm1:` + first 32 hex characters (128 bits) of SHA-256 over a length-prefixed, tag-named
  serialization of the canonical key. Enum values are serialized as text tags so renumbering never
  changes IDs. A golden ID computed independently in Python pins the format.
- Key = repository member, language, symbol kind, owner chain (kind, name, callable signature),
  canonical name, normalized signature, linkage discriminator. Nothing else can enter: there is no
  field for paths, lines, body hashes, profiles or generated-file paths (PRD FR-GPH-002).
- Linkage discriminators: `ExternalLinkage`; `InternalLinkage{repository-relative file}` for
  `static`/anonymous-namespace symbols; `LocalScope{enclosing stable id, anchor, ordinal}` for
  lambdas, anonymous types and locals. Scoping locals by the enclosing *ID* (not just the owner
  chain) keeps identical lambdas under same-named static helpers in different files distinct.
- Template identity: `TemplateRole` (`none`, `primary`, `explicit_specialization`,
  `partial_specialization`) plus `template_parameters` (canonical parameter kinds for primaries and
  partial specializations, e.g. `<typename, int>`, never names) and `template_arguments` (for
  specializations) on the key and on owner components. These fields are serialized only when the
  role is not `none`, so every previously published non-template ID is unchanged; parameters are
  required for primaries/partials and forbidden otherwise, arguments are required for
  specializations and forbidden otherwise. An `anonymous_type` may use `LocalScope` with an empty
  owner chain (namespace-scope `struct {..} x;` anchored to its declarator), and so may a `lambda`
  under exactly the `lambda-decl-init` anchor (`auto g = [] {};` at translation-unit scope, anchored
  to the declarator); the validator inspects only the key's own fields.
- Local-type spellings inside signatures and template arguments are stable-ID atoms (`[lambda <id>]`,
  `[anon <id>]`, `[local <id>]`) inside a tagged canonical encoding of the type structure (see
  `docs/analyzer.md`, Phase 1C); named non-local types keep the established printer, so their IDs are
  unchanged. Unnamed-record and closure constructors are named `(ctor)`, destructors `~`.
- `validate_canonical_key` / `make_stable_id` (throws `std::invalid_argument`) enforce the
  kind/signature/linkage contract: callable kinds (function, method, constructor, destructor,
  conversion function, operator, lambda) require a normalized signature and non-callables must not
  carry one; callable owner components require their signature, non-callable owners must not; only
  scope-like kinds may own; unnamed symbols are only lambdas, anonymous types and the anonymous
  namespace node itself (kind `namespace`, empty name, `InternalLinkage`); a named namespace has
  external linkage unless an anonymous namespace ancestor makes it file-scoped
  (`namespace { namespace named { … } }`); members of an anonymous namespace require
  `InternalLinkage`; lambdas, anonymous types and local variables require `LocalScope`; any
  block-scope entity except a namespace may use `LocalScope` (named local classes/enums/aliases
  and their members, so same-named locals in different blocks differ by ordinal); `InternalLinkage`
  file scopes must satisfy `is_canonical_repo_relative` (no `.`/`..`/empty segments, no backslash,
  no trailing slash, relative), so `src/x/../a.cpp` is rejected rather than hashed into a second
  identity. The normalized signature is analyzer input: this layer stores it as data and does not
  attempt its own type normalization.

### Source evidence (`source.hpp`)

- All `std::string` paths are UTF-8. Conversions go through `path_from_utf8` /
  `path_to_utf8_generic` (`generic_u8string`), never the ANSI narrow conversion. Tested with Hangul,
  Cyrillic, diacritics and a non-BMP emoji in a real temporary directory tree.
- `RepoRelativePath` has two spellings: `generic` (canonical identity, the only field compared or
  hashed) and `as_written` (observed spelling for display). `make_repo_relative` is lexical:
  `generic` is normalised (dot segments removed) and must pass `is_canonical_repo_relative`;
  `as_written` keeps the raw tail. `make_repo_relative_on_disk` is filesystem-authoritative for
  existing paths: containment is decided by walking the file's ancestors until one is the same
  directory object as the root, never by spelling (a case-distinct sibling `caseroot` of root
  `CaseRoot` on a case-sensitive volume is outside), and `std::filesystem::canonical` supplies the
  on-disk spelling of every component for `generic`: `ÜNÏCODE/ФАЙЛ.CPP` and `Ünïcode/файл.cpp` for
  one existing NTFS file yield one identity while their `as_written` differ. Spellings that do not
  exist are never folded, and two distinct existing files keep distinct identities (verified with
  real Unicode and case-sensitive temp trees).
- `path_key` is a comparison key for paths that may not exist: lexically normal generic form,
  ASCII-only case fold on Windows. Non-ASCII case is not folded; `same_existing_file` asks the
  filesystem (`std::filesystem::equivalent`) and is the authority for existing files.
- `SourceSpan` is a half-open byte range plus 1-based line/byte-column ends. `SourceLocation`
  carries the `FileContentHash` (SHA-256 + size) of the bytes it was computed from and an optional
  span hash.
- `read_snippet` returns text only when the bytes actually read hash to the recorded hash;
  otherwise `source_changed` or `span_out_of_range`. A stale span is never applied to changed content
  (PRD FR-RET-004).

### Facts (`facts.hpp`)

- One logical `Symbol` per stable ID; declarations and definitions from any TU attach as
  `SymbolLocation`s with their contributing analysis unit.
- `Evidence` carries orthogonal call aspects: `EvaluationContext` (incl. Phase 1B
  `mem_initializer`, `implicit_mem_initializer`, `capture_copy`), `DispatchKind`, `TemplateUse`,
  `immediately_invoked_lambda`, the `template_arguments` of the use site, and since Phase 1B
  `SubobjectRole` + `subobject` (base/member/delegating identity), `LifetimeKind`
  (`automatic_object`, `temporary`, `parameter`, `subobject`; `none` for written calls) and
  `potentially_elided`. Every aspect is part of evidence identity and of the fact hash, so one site
  can carry a default-argument evaluation, a virtual slot and a template use at once.
- `SymbolLocation` carries per-declaration `CallableFlags` (`explicitly_defaulted`,
  `deleted_as_written`, `implicitly_deleted`, `trivial`; orthogonal, never identity) and the role
  `implicit_declaration` for compiler-synthesized members. `SymbolPresence::implicit_hidden` marks
  such members (`add_hidden_member`); after `remove_contribution` presence follows the surviving
  locations (`indexed` while a real declaration remains, `implicit_hidden` when only synthesized
  anchors remain, `endpoint_only` when evidence alone names the symbol).
- `CaptureFact` (`add_capture`, `captures()`): lambda id, capture kind, explicit/init/pack flags,
  spelled name, hashed target descriptor, span and unit. Captures are hashed, removed per unit and
  keep their lambda alive as an endpoint.
- `RelationKey` = source, target, kind, **confidence**. `possible` and `confirmed` between the same
  endpoints are distinct facts, so accumulating evidence can never promote an uncertain target.
  `Confidence::unresolved` is rejected as a relation; `UnresolvedSite` keeps expression, location,
  reason and unit instead (PRD FR-GPH-005, P-03).
- `remove_contribution(unit)` deletes that unit's evidence, locations and unresolved sites. A
  relation survives while any other unit's evidence remains. A symbol whose last location vanished
  but is still named by surviving evidence (a relation endpoint or the `enclosing` of another unit's
  unresolved site) becomes `SymbolPresence::endpoint_only` (kept, no location, truthfully labelled);
  re-adding a location restores `indexed`. External placeholders and endpoint-only symbols disappear
  once nothing references them.
- `fact_hash()` hashes a sorted canonical serialization; equal content gives equal hashes
  regardless of insertion order (PRD 11.2 three-run determinism gate depends on this property).
- Clang USR has a slot as `Evidence::provider_key` only.

### Status (`status.hpp`)

- `LayerState` separates `applicable`, `availability` (missing/partial/ready/failed), `freshness`
  (unknown/fresh/stale), `last_success`, `last_attempt`, unit counts and a reason.
- `summarize_layer`: failed > missing > stale > partial > ready. **Unverified freshness (`unknown`)
  is reported as `stale`**; only a verified `fresh` layer can be `ready` or `partial`.
- `summarize_overall`: non-applicable layers never count (plain C++ `uht_context` is
  `applicable=false`). A failed/missing structure layer decides the result; otherwise any stale
  layer gives `stale`, any other non-ready layer gives `partial`, else `ready`. Missing optional
  `deep` therefore yields `partial`, never `missing`.

### Compilation database (`compile_db.hpp`)

- `arguments` is copied verbatim; `command` is tokenized into argv with either Windows
  (`CommandLineToArgv`/LLVM rules, including `""` inside quotes and backslash-quote counting) or GNU
  rules (backslash escapes, single/double quotes). `auto_detect` follows Clang: Windows on Windows
  hosts. Nothing is ever executed; shell operators simply become arguments.
- Relative `file`/`output` resolve against the entry `directory`. A relative `directory` (spec
  violation) resolves against the database's own absolute location with a warning, or is rejected
  when no location is known. `load_compilation_database` makes its own path absolute first.
- Every entry keeps `*_as_written` spellings, the raw `command` string when used, the entry index and
  a `command_id` (SHA-256 of directory key, file key, argv, output key). Two entries with equal
  argv for the same file in different path spellings share an identity; a differing define or output
  does not. The output key is a case-PRESERVING lexical spelling (lexically normal, generic
  separators): it is independent of whether the artifact exists (identity is identical before the
  build creates `out.obj`, while it exists and after it is deleted), dot/separator variants of one
  output agree, and case variants (`out.obj`/`Out.obj`) are conservatively distinct identities on
  every volume, so they surface as an ambiguity and are never silently merged or picked first. On a
  case-sensitive directory those are in fact two artifacts (tested).
- `select_unique(file)` returns `unique` only when all commands for the file share one identity,
  `ambiguous` with all candidates otherwise, `none` when absent. The caller must choose explicitly
  (PRD FR-BLD-004). `commands_for_file` lets the filesystem decide when both the queried file and a
  command's file exist (aliases of one file match, case-distinct `a.cpp`/`A.cpp` on a case-sensitive
  volume never merge) and falls back to `path_key` only when no existing-file comparison is possible.
  `compute_command_id` is public and uses the filesystem's canonical spelling for existing paths
  (case-distinct sibling directories with identical arguments are different commands; aliases of one
  file agree), so hand-built commands carry their true content identity.
- Malformed entries (missing/empty `directory`/`file`, non-string or empty compiler argument,
  non-string or empty `output`, neither `arguments` nor `command`, a `command` with an unterminated
  quote) are skipped individually with an indexed error diagnostic, so a guessed argv can never
  acquire a command identity that merges with a valid neighbour. A document that is not a JSON array
  yields no database, and so does a relative `database_directory` passed to the direct parse API
  (the loader always passes an absolute one).

### Compile-context normalizer (`compile_context.hpp`)

`normalize_compile_context(command)` leaves `CompileCommand::arguments` untouched and produces
`analyzer_arguments` plus exactly one `OptionDisposition` per raw argument (`kept`, `kept_unknown`,
`transformed`, `dropped`, `dropped_unsupported`, `source_input`, `compiler`; a category; a reason;
the replacement tokens). Driver flavour comes from argv[0] (`cl`/`clang-cl` = MSVC; `clang`,
`clang++`, `gcc`, `g++`, `cc`, `c++`, with version suffixes = GNU; otherwise `unusable`). The
compiler executable itself is kept separately as `CompilerKind` (`native_cl`, `clang_cl`, `clang`,
`gnu_alias`, `unknown`): the flavour says how syntax is parsed, the kind says whose verified driver
semantics apply to raw syntax the flavours share only partly (native `cl.exe` ignores `--` and has
no `-imsvc`; clang-cl honours both; the GNU aliases are not verified for `--`).

- Structural: a free `--` of a verified Clang-family executable is `dropped` with category
  `structural`; every later token is an input. Non-selected inputs after it are
  `dropped_unsupported` (category `source`) and degrade the context, never forwarded. Operands are
  consumed first (`/I --` is a directory named `--`). A standalone `@file` consumed as a separated
  operand is recorded as an unexpanded response file (the driver expands it before operand
  consumption) and degrades the context; a joined `/I@literal` is a literal path.
- Kept: defines, undefines, include directories (`IncludeDirectory::kind`: `user`, `quote`,
  `system` for `-isystem`/`/external:I`, `internal_system` for clang-cl `-imsvc`, `after`;
  `is_system_include(kind)` derives the system predicate; resolved against the entry directory,
  argv order preserved), forced includes, `/std:` `-std=`, `/TP` `/TC` `-x`, MSVC runtime library flags, and
  flags that change predefined macros or front-end semantics (`/EH*`, `/GR*`, `/Zc:*`,
  `/permissive*`, charsets, `/arch:`, `/Zp`, `/J`, `/openmp`, `/fp:`, optimisation levels, `-f*`,
  `-m*`, `-O*`, targets, sysroots, `-Xclang`/`-Xpreprocessor`).
- Dropped with reason: outputs (`/Fo` `/Fd` … `-o`), dependency generation (`/showIncludes`, `-MD`
  `-MF` …), warnings and diagnostics presentation, debug info, build-process flags (`/nologo`, `/c`,
  `/MP`, `-c`, `-pipe` …), codegen-only flags without preprocessor impact, linker input (`/link …`,
  `-l`, `-L`, `-Wl,`), and for clang-cl only the exact audited token `-Wno-deprecated-declarations`
  (diagnostic-only; `/W...` spellings, other `-W...` tokens and native-cl commands are not covered).
- Transformed: `/Yu<hdr>` becomes `/FI<hdr>` (`pch_emulated`, status `degraded`); `/Tp`/`/Tc`
  identify the entry source and forward `/TP`/`/TC` so the language override reaches the
  invocation; `-Wp,-D…/-U…/-I…` is unpacked into direct options.
- Unsupported, reported and `degraded`: `/clr`, `/ZW`, `/kernel`, `-include-pch`, response files
  (`@file`, left unexpanded and listed), `-Wp,` with anything else, options whose value is missing,
  MSVC attached-value options given with a separated value (`/Fo out.obj`: both tokens reported, the
  value is neither forwarded nor treated as input).
- Everything unrecognised is `kept_unknown` and forwarded, never silently discarded.
- Operand consumption mirrors the driver and is never reinterpreted from spelling: `-I -DNAME` is
  an include directory literally named `-DNAME`, `-D src/a.cpp` is a macro definition. No lookahead
  intent-guessing. `-Wp,` payloads with an empty or option-shaped value (`-Wp,-D,`, `-Wp,-D`,
  `-Wp,-I,-DX`) are `dropped_unsupported`, so no bare value-taking flag can reach the analyzer and
  swallow an appended source.
- `source_identified` is validated once after parsing: true when one argument was recognised as
  the entry's source. When both the spelled path and the entry file exist, filesystem identity
  decides (NTFS case aliases of a Unicode name match; distinct `a.cpp`/`A.cpp` in a case-sensitive
  directory do not, even though their ASCII path keys are equal); the path key is only the fallback
  when no existing-file comparison is possible. A source swallowed as an operand therefore leaves the
  context `degraded` with a note, and an analyzer must not append the source itself. A second actual
  occurrence of the source (same spelling, `./main.cc`, a filesystem alias, before or after `--`, via
  `/Tp`/`/Tc` or positional) is a second compilation job for the driver: it is `dropped_unsupported`
  and degrades the context, never forwarded; include/define operands that merely equal the file name
  are consumed as values first and are not occurrences. Genuinely different existing files and
  non-existing spellings are never treated as the source.

This is a documented subset, not full toolchain coverage.

## Known limitations of this foundation

- No Clang semantic analyzer beyond the smoke probe, no Unreal adapter, no SQLite store, no CLI/MCP.
  The PRD's semantic-graph, retrieval, freshness and agent gates are untouched.
- The normalizer covers a documented subset of MSVC/GNU options; it never expands response files
  itself (callers expand first, see `lcm/response_file.hpp`, which supports native `cl.exe` only);
  the analyzer arguments have not yet been fed to the Clang front end.
- Named local classes with `ExternalLinkage`/`InternalLinkage` are accepted because the key cannot
  tell them from namespace-scope classes; the analyzer must emit `LocalScope` for block-scope
  entities.
- Compilation database discovery/priority (explicit path, unique candidate under root/build dir,
  structural fallback) is not implemented; only parsing and selection for a parsed database is.
- `path_key` folds ASCII case only. Case-insensitive matching of non-ASCII path spellings relies on
  the filesystem (`make_repo_relative_on_disk`, `same_existing_file`) and therefore on the file
  existing.
- Non-Windows builds are not exercised (PRD first release is Windows-only); `#ifdef _WIN32` paths
  in tests are only compiled on Windows.
