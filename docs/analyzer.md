# Phase 1A/1B analyzer: selected compile context to common facts

Status: bounded increments, implemented and tested on Windows with the pinned LLVM 22.1.0 SDK.
Ordinary C++ only. Phase 1A (selection, gate, symbols, direct calls, templates, dispatch) and
Phase 1B (callable state, hidden implicit members, initializer/lifetime facts, captures) are
integrated; it is not whole Phase 1, not Unreal, not SQLite, not CLI/MCP.

## Files

| Path | Purpose |
| --- | --- |
| `src/lcm/analyzer/analyzer.hpp/.cpp` | public API: explicit unit selection, per-unit staged analysis, explicit publish |
| `src/lcm/analyzer/safety.hpp/.cpp` | argument allowlist gate (per driver flavour) |
| `src/lcm/analyzer/facts_builder.hpp/.cpp` | front-end action: preprocessor observer + AST visitor producing facts |
| `tests/analyzer/analyzer_tests.cpp` | behaviour tests over a real fixture compilation database |
| `tests/fixtures/cpp/phase1a/**` | public synthetic fixture (repo + out-of-root `external_sdk`) |

Built only when `LCM_CLANG_FOUND` (see `cmake/ClangSdk.cmake`); links `lcm_core` and, since Phase
1B, `clangAnalysis` (the compiler's CFG) under the project-wide static CRT policy. The
Clang-disabled build has no analyzer and says so.

## Contract

- **Explicit selection.** `select_by_command_ids` and `select_by_files` return a `UnitSelection`;
  a file with several distinct command identities is listed under `ambiguous` with its candidates
  and is not selected. `analyze_selected` analyses exactly the selected units. Nothing analyses
  every database entry implicitly.
- **Provenance binding.** `AnalysisRequest` carries only the actual `CompileCommand`; the analyzer
  normalizes that command itself (`AnalysisResult::context`) and recomputes its content identity
  (`compute_command_id`, which uses the filesystem's canonical spelling for existing directory/file
  paths and a case-preserving lexical spelling for the output). A
  `command_id` that does not match the command's content rejects the unit, so a safe context can
  never be paired with a different raw command. An optional `analysis_unit` label may name the
  contribution, but `AnalysisResult::command_id` always holds the true identity and both are
  reported side by side.
- **Staged results.** `analyze_translation_unit` returns an `AnalysisResult` holding the unit's own
  `FactSet`, diagnostics, file and include observations and limits. It never touches a
  caller-owned graph. `publish_contribution(target, result, policy)` copies staged facts through the
  public `FactSet` API; the default policy `accepted_only` refuses units that completed with errors
  or whose context is not complete. Replacement of an earlier contribution of the same unit is the
  Phase 2 storage layer's job.
- **Invocation gate, before anything runs.** A unit is rejected (no facts, no files, raw command
  preserved, actionable note) when: the driver is unknown (wrappers such as `sccache.exe` are never
  guessed away); `source_identified` is false (the source was consumed as an operand, or is
  absent; the analyzer never appends it); the normalized context is `degraded` (a response file
  that was not expanded, PCH emulation, unsupported or malformed options, additional inputs); or any
  analyzer argument is outside the allowlist. The allowlist accepts only spelled-out forms: attached
  defines/undefines/include directories/forced includes, enumerated `/std:`, `/arch:`, `/fp:`
  values, validated `/EH*`, `/Zc:*`, `/Zp*`, `/vm*` shapes, exact MSVC runtime/conformance/charset
  flags, exact optimization spellings (`/O2`, `/Ob1`, `-O2`; `/OUT:` is not one), an exact list of
  GNU `-f` feature flags and `name=` options with a value, an exact list of `-m` machine flags
  (`-multi-lib-config=`, `-munknown-...` and `-mllvm` reject), and explicit two-token forms.
  Passthrough (`-Xclang`, `-Xpreprocessor`, `/clang:`, `-Wp,`), plugins, `--config`, `-ivfsoverlay`,
  module caches/files, outputs, response files, positional tokens and unknown options reject. No
  prefix ever stands in for a family of options. Output and dependency options are
  removed by the normalizer with recorded dispositions before the gate, so `-o`/`-MD`/`-MF`
  commands are accepted without producing files (tested). The gate also receives the ORIGINAL
  compiler's identity (`CompilerKind`): the clang-cl system include `/imsvc<dir>` (joined `-imsvc<dir>`
  or separated `-imsvc dir` in the raw command) is accepted for `clang-cl` only, so the same
  spelling in a native `cl.exe` command, where it is an unknown token, can never pass a shared
  MSVC-syntax allowlist.
- **Adapter compatibility (driver contracts).** The normalizer models the real end-of-options
  boundary for verified Clang-family executables (`clang-cl`, `clang`, `clang++`): a free `--` is a
  `structural` disposition and every later token is an input, so an option-shaped source such as
  `-dash.cc` is the entry file, while any non-selected input after it (`extra.cc`, `-DSTEALTH=1`,
  `/Iextra`, a second `--`) is `dropped_unsupported`, degrades the context and is never forwarded
  where a string allowlist could mistake it for an option. A second actual occurrence of the entry
  source itself (`main.cc main.cc`, `main.cc ./main.cc`, `main.cc -- main.cc`, `/Tpmain.cc main.cc`) is
  a second driver job and degrades the same way; operands that merely equal the file name (`/Imain.cc`,
  `/D main.cc`) are not occurrences. Native `cl.exe` ignores `--` (D9002) and
  the GNU aliases (`gcc`, `g++`, `cc`, `c++`) are not verified, so for them `--` stays an unknown
  option and the unit rejects. Operands are consumed before the delimiter is interpreted (`/I --`
  and `-imsvc --` are literal directories; only the free `--` delimits). Response tokens degrade
  before and after `--`, and a standalone `@file` consumed as a separated operand (`/I @x.rsp`,
  `-imsvc @x.rsp`, `-include @x.rsp`) is unexpanded response evidence that degrades the request,
  because the driver expands it before operand consumption; a joined `/I@literal` is one token and
  stays a literal path. `-imsvc` is recorded with `IncludeKind::internal_system` (after `-isystem`
  and `/external:I`, before the toolchain directories) in argv order. The exact token
  `-Wno-deprecated-declarations` in a clang-cl command is dropped as an audited diagnostic-only
  control; `/W...` spellings, other `-W...` tokens, `/clang:` forwarding and native-cl commands are
  not covered.
- **Front end.** `clang::tooling::ToolInvocation` over an overlay of an INDEPENDENT physical
  filesystem object (`llvm::vfs::createPhysicalFileSystem`) whose working directory is the compile
  command's directory; the host process working directory is never changed (tested across ok, error
  and rejected units with different directories), `--driver-mode=cl` + `/Zs` or
  `--driver-mode=g++` + `-fsyntax-only`, the pinned `-resource-dir`, the normalized arguments, a
  `--` delimiter and the entry file (our invocation is always Clang, so an option-shaped entry path
  is always an input; this decides nothing about the original compiler). Diagnostics are collected
  with file:line:column. Front-end errors yield
  `completed_with_errors` with provisional staged facts; recovered call sites become unresolved
  sites with reason `analysis_failed`, never targets.

## Facts produced

- **Symbols** (in-root declarations only): namespaces (anonymous ones as file-scoped nodes),
  classes/structs/unions, enums and enumerators, functions, methods, constructors, destructors,
  conversion functions, operators, namespace-scope and static member variables, fields, typedefs and
  aliases, lambdas (kind `lambda`, `LocalScope{enclosing id, "lambda", ordinal}`), local classes
  and their members (`LocalScope` scoped to the enclosing function or local class), and unnamed
  types. **Anchor choice for unnamed types (documented for review):** `struct {..} x;` at namespace
  or class scope is `anonymous_type` with `LocalScope{id of the declarator x, "anon-type", 0}`
  (variable or field); an unnamed member record without a declarator (anonymous union) is
  `LocalScope{id of the enclosing record, "anon-type", ordinal}` with a deterministic per-record
  ordinal; a typedef'd unnamed struct takes the typedef name and is a normal named type; members of
  an unnamed type are `LocalScope` under the type's id. No compiler path/line spelling is used.
  Unnamed types with no declarator and no enclosing record are skipped and listed in `limits`. Owner chain
  from `DeclContext` parents with callable signatures and template role/arguments; linkage from the
  compiler (`Linkage::Internal` or anonymous namespace membership -> file-scoped
  `InternalLinkage`); signature = canonical parameter types, cv/ref qualifiers, variadic marker.
  Every redeclaration in root becomes a `SymbolLocation` (declaration/definition, span over the
  compiler's bytes, content hash, span hash, body hash for definitions).
- **Template identity** (`TemplateRole`, `template_parameters`, `template_arguments` on keys and
  owner components): primary patterns carry a canonical parameter-kind signature (`<typename>`,
  `<int>`, `<typename...>`, `template<typename> class`), never parameter names, so
  `template<class T> f()` and `template<int N> f()` are distinct while renaming or redeclaring a
  parameter keeps the id. Explicit and partial specializations are distinct symbols; implicit
  instantiations are never public nodes. Calls into an implicit instantiation fold to the pattern
  with `TemplateUse::primary_implicit` and the use-site `template_arguments`; calls to an explicit
  specialization carry `TemplateUse::explicit_specialization`.
- **Relations** (all `confirmed`, evidence `compiler_semantic` with span and orthogonal aspects):
  `contains` (innermost owner to member), `calls` (direct callee; member calls; user-declared
  constructor calls; user operator calls; closure `operator()` folded to the lambda symbol),
  `references` (function addresses), `extends` (direct bases), `overrides` (compiler override set).
- **Call aspects** (orthogonal, all on every evidence): `EvaluationContext` (`body`,
  `default_argument`, `default_member_initializer`, `init_capture_initializer`), `DispatchKind`
  (`static_target`, `virtual_slot`, `devirtualized`, `qualified`), `TemplateUse` (`none`,
  `primary_implicit`, `explicit_specialization`) plus `immediately_invoked_lambda` and
  `template_arguments`. A default-argument virtual call keeps both its evaluation context and its
  selected slot; an init-capture implicit-template call keeps both its capture evaluation and its
  template folding. Default-argument evidence is attributed to the caller at the use site, never to
  the declaring function, once per use; init-capture calls belong to the enclosing callable while the
  lambda body's calls belong to the lambda. Virtual member calls record the compile-time selected
  slot unless the compiler devirtualizes (final class/method, object expression) or the call is
  explicitly qualified (`d.Base::run()` and `d.Derived::run()` are both `qualified`, each with its
  exact named target; neither is claimed to be a base call). An explicit qualifier is recorded
  whether or not the method is virtual: `p.Plain::run()` on a non-virtual method is `qualified`,
  while the ordinary `p.run()` is `static_target`. No confirmed edge to override
  implementations is stored; runtime candidates are query-time work.
- **Unresolved sites**: calls without a direct callee (`indirect_callee`), dependent callees
  (`dependent_expression`), recovery expressions (`analysis_failed`), with callee text and span.
  Since Phase 1B also: a by-value argument that the target ABI destroys inside an UNKNOWN callee
  (function pointer, callback) is an unresolved site of the caller with the argument text and
  reason `indirect_callee`; the caller never owns that destruction.

## Phase 1B: callables, hidden members, lifetime, captures

- **Callable state** (`SymbolLocation::callable`, per declaration, never identity): four orthogonal
  compiler flags, `explicitly_defaulted` (this declaration spells `= default`), `deleted_as_written`
  (`= delete`), `implicitly_deleted` (the compiler deleted a defaulted or implicit member) and
  `trivial`. An explicitly defaulted copy constructor whose member is not copyable carries both
  `explicitly_defaulted` and `implicitly_deleted`; an out-of-line `T::T() = default;` carries the flag
  on the definition location only, and that definition has no body hash (nothing user-written to
  hash). `user_provided` is derived, not stored.
- **Hidden implicit members** (`SymbolPresence::implicit_hidden`): a compiler-synthesized special
  member becomes a symbol only when a recorded fact names it (a construct expression, a destruction,
  a subobject step). It gets an `implicit_declaration` location per contributing unit, anchored at
  the class-name token the compiler itself uses, with the flags above and no body hash; the class
  `contains` it. Unused implicit members and never-destroyed implicit destructors are not nodes.
  Presence follows the surviving locations after removals: `indexed` while any real declaration
  remains, `implicit_hidden` when only synthesized anchors remain, `endpoint_only` when none remain
  but evidence still names the symbol. Referenced closure constructors, assignment and conversion
  functions are hidden members too (Phase 1C).
- **Initializers**: every `CXXCtorInitializer` of a constructor with a body, written or not, is
  traversed with evaluation context `mem_initializer` or `implicit_mem_initializer`; the construct
  expression it evaluates carries `SubobjectRole` (`base`, `member`, `delegating`) and the subobject
  name. Construction, copy, move, conversion functions (`CXXMemberCallExpr` under a user-defined
  conversion) and user operators are ordinary `calls`; built-in operators never become callables.
  The hidden implicit copy constructor of `Plain` owns its member copy; the outer caller does not.
- **Lifetime facts** are extracted from the compiler's CFG (`CFG::buildCFG` with implicit and
  temporary destructors and initializers) of every body, deduplicated by the destroyed object,
  temporary, field or base specifier, never by path count; blocks, edges and order are discarded.
  `LifetimeKind`: `automatic_object` (anchored at the declaration, once for any number of exits,
  arrays once with the element destructor), `temporary` (discarded temporaries; a guaranteed-elided
  prvalue initialisation yields none), `subobject` (`CFGMemberDtor`/`CFGBaseDtor` of a destructor
  body, owned by that destructor, including the CFG of a used implicit destructor's synthesized
  body), `parameter` (below). `potentially_elided` marks the destruction of an NRVO candidate
  variable and the construction of its returned value (`ReturnStmt::getNRVOCandidate`), and any
  `CXXConstructExpr::isElidable()` construction. Clang 22 does not resolve the destructor of a
  `CFGBaseDtor` element itself; the element's base type supplies the compiler-declared destructor.
  A body without a CFG or a used implicit destructor without a synthesized body is a `limits` entry,
  never a record-field guess. Dependent template patterns record no lifetime facts (limit).
- **Parameter destruction follows the compiler's ABI decision**, `RecordDecl::isParamDestroyedInCallee()`,
  never the driver syntax or the availability of a definition. Both default Windows driver modes
  (`cl` and `g++`) target the MSVC ABI where the callee destroys: the callee owns `LifetimeKind::parameter`
  evidence from its definition's parameter and from every callsite argument (a declaration-only
  in-root callee is still the owner), and the ABI-agnostic caller-side CFG temporary is suppressed.
  Ownership is decided on the CONCRETE callee before template folding (`dependent_take<Tracker>`
  folds to the primary with `<Tracker>` arguments after `Tracker` decided), on declared parameters
  only (the object argument of a member operator call is skipped), and for a closure `operator()` it
  folds into the lambda. An unknown callee makes the destruction an unresolved site of the caller
  (above); an external callee is a boundary (limit). Under an explicit Itanium target
  (`-target x86_64-unknown-linux-gnu`) the caller destroys the argument temporaries and no callee
  owns anything. The CFG models a by-value parameter's destruction on the callee side irrespective
  of ABI and only on explicit-return paths, so `CFGAutomaticObjDtor` elements for parameters are
  ignored; the ABI decision is applied exactly once.
- **Captures** (`FactSet::captures()`, `CaptureFact`): per lambda capture the kind (`by_copy`,
  `by_reference`, `this`, `star_this`), explicit/implicit, init-capture, pack expansion, the spelled
  name and a `target_descriptor` (hash of the captured local's declaration file and span, empty for
  `this`). Locals are not indexed and no def-use is expanded. Init-capture initializers evaluate in
  the enclosing callable as `init_capture_initializer`; the compiler-inserted copy of an ordinary
  by-copy or `*this` capture of class type is a `calls` edge from the enclosing callable with
  `capture_copy`. Body calls belong to the lambda; nested lambdas are scoped to their enclosing lambda.
- **Closure lifetime**: a closure destructor the compiler uses (a capture with a non-trivial
  destructor) is a hidden destructor symbol under the lambda (`LocalScope{lambda id, "closure-dtor",
  0}`, name `~`), destroyed by the enclosing callable as `automatic_object`, and it owns the capture
  subobject destruction (`subobject` = capture name). Nothing about destruction folds into the
  lambda's body calls; only `operator()` does. Trivially destructible closures have no destructor node.
- **Contribution semantics**: flags, hidden locations, captures and every new evidence aspect are
  part of equality and of `fact_hash`; `publish_contribution` copies hidden locations and captures;
  `remove_contribution` removes them by unit and a surviving capture keeps its lambda alive as
  `endpoint_only`.
- **External boundary**: an out-of-root target becomes an `external_placeholder` with member
  `external`, no location; nothing from its file is indexed. Unreferenced external declarations
  are not retained.
- **Observations (not facts)**: every entered file with in-root flag, system flag and content hash;
  every direct `#include` with spelling, angled flag, resolved path and in-root flag.

## Phase 1C: closure members, unnamed constructors, local type atoms, namespace lambdas

- **Only the real call operator folds.** `is_closure_call_operator` compares the callee with the
  closure's call operator, or with its template for a generic lambda's specialization, and is applied
  to `x(...)` (`CXXOperatorCallExpr`) and `x.operator()(...)` (`CXXMemberCallExpr`) alike; the
  immediate-invocation flag needs a `LambdaExpr` object; a generic specialization carries
  `TemplateUse::primary_implicit` with its deduced arguments. `a = b` and `a.operator=(b)` on
  closures, and `int (*fp)() = closure`, are ordinary calls to hidden members and never execute the body.
- **Hidden closure members.** Referenced default/copy/move constructors (`(ctor)`), copy/move
  assignment (`operator=`), conversion functions and the destructor (`~`) are `implicit_hidden`
  symbols under the lambda: owner chain ends with the lambda component, `LocalScope{lambda id, anchor,
  0}` with anchors `closure-dtor` (unchanged identity), `closure-ctor`, `closure-assign`,
  `closure-conv`, `closure-member`; the signature distinguishes copy from move. Their synthesized bodies
  own their calls (the copy constructor's `Payload(const Payload&)` for capture `payload`, as
  `implicit_mem_initializer` / member). The static invoker `__invoke` is a stated boundary; calls through
  the converted pointer stay `indirect_callee`. Closure members are anchored to the lambda at namespace
  scope too, where `getParentFunctionOrMethod()` is null.
- **Unnamed-record constructors** use the fixed name `(ctor)`, anchored to the actual anonymous type
  (`LocalScope{type id, "(ctor)", 0}`), distinguished by signature (`()`, copy, move). A global
  `struct {..} a1;` references its default constructor from the variable's own initialization; aggregate
  `{}` initialization is not a constructor call (the semantic init-list form contributes only the actual
  member `CXXConstructExpr`, owned by the initializing callable).
- **Local type atoms.** Named, non-local types keep the established canonical printer, so every
  published ID is unchanged. A type whose leaves include a closure, an anonymous record or a block-scope
  class/enum/alias (scope policy: any declaration context that is a function, closure or unnamed
  record) is spelled with a tagged canonical encoding: leaves `[lambda <id>]`, `[anon <id>]`,
  `[local <id>]`; constructors `ptr(T)`, `ref(T)`, `rref(T)`, `memptr(C, T)` (the class through the same
  encoder, so `int Box<decltype(g1)>::*` keeps its arguments), `arr[N](T)`, `arr[](T)`,
  `fn[cc=<conv>(,noexcept|,throw(...))](R)(P...) [const|volatile|&|&&]`; qualifiers `const `,
  `volatile `, `restrict ` as prefixes; template specializations `Name<args>` with type arguments
  encoded, integral `int(T=v)`, declarations `decl(T=&<id>)`, `nullptr(T)`, packs `pack(...)`. Class-type
  non-type arguments (Clang 22 `TemplateParamObjectDecl`) and structural values are always encoded as
  `val(T=<value>)` with the compiler's `APValue` walked recursively (struct bases/fields in layout order,
  arrays fully expanded so `K2{{0, 0}}` and `K2{}` are one identity, unions, member pointers, lvalues to
  declarations by stable ID), never the raw printer. A depth budget (32) or an unsupported form
  (vector/atomic/complex/block/dependent types, lvalues with paths or offsets, arrays over 4096
  elements, template/expression arguments) makes the symbol's signature unavailable: the symbol is
  skipped with a precise `limits` entry, never given a raw or colliding spelling. Same-named local
  enums/aliases in separate blocks get structural ordinals like local classes.
- **Locality classification decides which printer runs.** The established canonical printer is used
  only for a type proven to carry no local leaf *anywhere the printer would spell it*; everything else
  is either encoded as an atom or skipped with a limit. Proven non-local leaves are builtins, bit-ints
  and template parameters (`type-parameter-N-M`). Everything else is classified recursively, including
  the parts the printer reaches indirectly: a tag's **enclosing** specialization arguments
  (`Outer<decltype(g)>::Inner`), a dependent member-pointer's **qualifier** when the class has no record
  declaration (`int Box<decltype(g), T>::*`), a dependent name's or unresolved-using type's qualifier
  chain (`typename Box<decltype(g), T>::type`), **expression** template arguments, dependent/variable
  array bounds and dependent exception specifications (`W<sizeof(decltype(g))>`,
  `int (*)[sizeof(decltype(g))]`) whose written expressions print their types canonically, and
  template-name arguments through their owner chain. Any argument kind or type form that is not
  classified is `unknown`, never "proven non-local": it reaches the encoder and, if unsupported, the
  limit path. These composites are unsupported in this increment — they are omitted with
  `type uses an unsupported form for identity; identity not invented`, not encoded —
  while the wholly named counterparts (`Outer<int>::Inner`, `int Box<int, T>::*`,
  `typename Box<int, T>::type`, `W<sizeof(T)>`) keep their established spellings and IDs.
- **`Declaration` non-type arguments are kept on the established printer only where it is injective
  and path-free.** `&plain_object`, `&plain_fn` and `&Holder<int>::value` print their full owner chain,
  so they keep their established signatures and IDs. Two cases do not and are omitted with their own
  limits rather than given an identity: a declaration reached through a specialization that carries a
  local leaf (`&Holder<decltype(g)>::value`), which the printer spells with the closure's path and
  line; and a declaration that is itself a template specialization (`&target<int>`, `&vt<int>`), whose
  own arguments the printer writes nowhere — every such argument prints `&target`, collapsing distinct
  overloads. Encoding the latter through the stable ID is not an option either: an implicit function
  specialization records no arguments in its canonical key and instantiations are never published as
  nodes, so `decl(T=&<id>)` can fold onto the pattern in the same way; an explicit specialization does
  record its arguments, but a declaration-valued specialization identity is unsupported here either
  way, so the whole shape is refused rather than partly encoded. `atom_template_arg` therefore
  never encodes a folded ID, including when a sibling local argument forces atom encoding. The
  class-type `TemplateParamObjectDecl` short-circuit to `val(T=<value>)` is unaffected. Faithful
  identities for specialization-valued arguments remain later work.
- **Namespace-scope declarator lambdas.** `auto g = [] {};` at translation-unit scope has no semantic
  owner; its anchor is `lambda-decl-init` with `enclosing` = the declarator's stable ID (per-declarator
  ordinals: `auto p = ([]{}, []{});` has ordinals 0 and 1 and `p()` calls the second). The validator
  accepts an empty owner chain with `LocalScope` only for `anonymous_type` and for `lambda` under
  exactly this anchor; it checks the key's own fields only, and the analyzer constructs the anchor only
  from an actual declarator. Lambdas with an owner (`ns::g3`, static members, block scope) keep the
  established `lambda` anchor and IDs.

## Direct bases and direct includes

- **Direct-base details (PRD FR-CPP-001).** Every `extends` relation carries `BaseEdgeDetails` on its
  *evidence*, never on the relation key: compiler-effective access (`class` defaults to private,
  `struct` to public), whether an access token was actually written, the `virtual` flag and the
  zero-based lexical ordinal counted over every written base. The evidence location is the full written
  base specifier, so `protected virtual B` and `private C` reproduce from the original bytes with a
  matching span hash. The details take part in evidence equality, ordering and `fact_hash`, so two units
  observing the same base under different macro state keep separate evidence and removing one leaves the
  other intact. Only direct, compiler-resolved bases are stored: a transitive `D -> A` is a query, never
  a fact, and nothing classifies interfaces by name, prefix or pure-virtual shape.
- **Template bases fold to the selected pattern.** An instantiated base targets the pattern the
  compiler selected — the primary, or the partial specialization it chose — and never creates a node
  for the instantiation. The concrete use-site arguments stay on the evidence in `template_arguments`,
  so `struct D : G<int>, G<short>` is one relation key with two evidences distinguished by their
  arguments, lexical ordinals, access/virtual details and original spans. The use is described by
  `TemplateUse`: `primary_implicit` or `partial_implicit` for an implicit instantiation, and
  `explicit_instantiation_declaration` / `explicit_instantiation_definition` for a base on an
  explicitly instantiated template. For those two, primary versus partial is the target symbol's
  canonical template role; they describe the base use only and claim nothing about indexing the
  instantiation declaration. The classification reflects Clang's **final AST state for the translation
  unit**, not the chronological source-order state at the base token: an explicit instantiation written
  after the base still classifies that base's use, exactly as one written before it does.
  Explicit-specialization and ordinary base identities are unchanged.
- **Direct includes (PRD FR-GPH-004).** `DirectIncludeFact` is a contribution-owned file-level
  collection on `FactSet` with `add_direct_include`, publication, `analysis_unit` removal and hashing.
  It is deliberately *not* a symbol relation: an inactive or unresolved directive has no truthful
  target, and no file symbol or placeholder is invented to host one. Each fact keeps the directive kind
  (`include`, `include_next`, `import`), the literal operand tokens with their kind (quoted, angled or
  macro tokens), the compiler-expanded spelling when the directive was processed, the activity
  (`active`, `inactive`, `indeterminate`), the resolution (`resolved_internal`, `resolved_external`,
  `not_found`, `not_evaluated`), the in-root target only when the compiler resolved one, the original
  directive location and the structured outer-to-inner conditional branch path. Transitive closure is
  never stored: `main -> direct.hpp` and `direct.hpp -> leaf.hpp` exist, `main -> leaf.hpp` does not.
- **Two correlated compiler sources.** The pinned dependency-directive scanner supplies the original
  bytes of every directive, including directives inside skipped branches; `PPCallbacks` supply what the
  preprocessor actually processed and resolved. An actually observed callback always wins: a directive
  after a fatal missing include or an `#error` is still active, and is never blanket-downgraded.
  A directive with no callback is `inactive` only when it lies inside a range the preprocessor proved
  skipped; otherwise it stays `indeterminate` with a limitation, because a missing callback is not proof
  of inactivity. Correlation is per VISIT (`FileID` plus the `#` offset), never by physical path alone,
  so the same unguarded header entered twice under toggled macros keeps both outcomes and their
  different targets. Identical complete observations deduplicate; different outcomes survive.
  Header-guard skipping is not conditional inactivity: both directives of a doubly included guarded
  header stay active and resolved.
- **Original bytes.** The directive span is the whole logical directive and excludes only its final line
  terminator: a continued directive keeps its backslash and the intervening CRLF, and a trailing comment
  stays inside the span. The raw operand and each condition expression stop at the last spelled token,
  so neither absorbs the terminator or that comment. Nothing is hashed from macro-expanded or
  reconstructed text. Conditions are stored structurally with their preceding siblings (`#elif B` keeps
  `A`; `#else` keeps every earlier branch) and are never evaluated or flattened to a display string.
- **Scanner coverage is verified, never assumed.** Scanner success does not prove complete lexical
  coverage: on the pinned SDK a legal digraph `%:include` is silently invisible to it, and it also
  succeeds on input the real front end diagnoses. A raw compiler lex of the same bytes finds every
  line-leading `#` (including its `%:` spelling); any modelled directive the scanner did not report, and
  any directive keyword split by an escaped newline that no literal comparison could match, marks the
  file's coverage incomplete. Such a file publishes **no** direct-include facts at all, with an explicit
  limitation, because a missed conditional would leave the surrounding ordinary includes looking
  unconditional. The unit's private preprocessor observations are untouched by that withholding.
- **External boundary.** A resolved include outside the repository root is `resolved_external` with no
  public path, no repository target and no ranking edge. It carries an opaque `external_dependency_key`
  derived from the resolved locator and the file bytes, so two distinct external files stay distinct
  even with identical content, and the unit's private `FileObservation` list can recompute the key to
  link the dependency back to its path. Nothing of an external file's body or API is indexed.
- **Platform boundary.** On the pinned Windows target `#import` is an unsupported Microsoft
  type-library import: it is diagnosed and produces no inclusion callback, so its lexical directive kind
  is preserved as `indeterminate`/`not_evaluated` and no header target is invented from the file merely
  existing on disk. `#include_next` has a real callback and resolves normally.

## Fixture and tests (`lcm_analyzer_tests`, 28 cases)

Real `compile_commands.json` generated at test time with absolute directories into
`tests/fixtures/cpp/phase1a/repo`: two TUs sharing `include/geo.h` with a split method
implementation, overload/owner disambiguation, static and anonymous-namespace same-name helpers,
`feature.cpp` compiled twice with and without `/DFEATURE_X`, `use.cpp` (comment/string decoys,
function pointer call, lambda, function address, virtual call, out-of-root `ext.h`, `<cstdint>`),
`semantics.cpp` (the four-case matrix plus same-named local classes), a missing include TU and a
broken TU (GNU flavour). Tests cover selection semantics, the allowlist gate, the eight-case
invocation-safety matrix (wrapper, `/clang:` passthrough, literal include operands, quoted
Unicode/punctuation paths, injection/cache switches, audited output removal, additional source,
consumed source), shared-header merging and split implementation, overloads/owners/virtual
slot/overrides/extends/references/lambda/unresolved/decoys/external boundary, define variants as
separate units, the semantic matrix, error units leaving the accepted graph untouched, file move +
body edit identity, and three-run determinism. The audit-correction cases add: provenance binding
(label vs true `command_id`, forged id refused), host CWD unchanged plus per-unit include
resolution, the semantic-matrix audit boundaries (collision primaries, aspect orthogonality, five
anchored anonymous types, same-class qualified calls) and the case-sensitive sibling boundary.
Phase 1B adds the header-free `src/callables.cpp` (Policy/Wrap/Plain flags and hidden members;
Box/Part/Tracker initializers, NRVO, elision, arrays, nested exits, `Owner::~Owner`, used implicit
`~Implicit`; `take_def`/`take_decl`/`call_both`; the closure, member/free operator, template,
dependent-parameter and function-pointer argument cross-composition; `Worker::run` captures) and
five cases: callable state and hidden members; initializers/construction/conversion/operators/CFG
lifetime; parameter ABI ownership under default `cl`, default `g++` and an explicit Itanium target;
captures, closure lifetime, nested lambdas and `(*&target)(3)`; and mixed-unit publication, removal
and endpoint liveness of hidden members and captures. The determinism run covers six units.

## Filesystem authority

`make_repo_relative_on_disk` decides containment of existing paths by walking the file's ancestors
until one is the same directory object as the root (`std::filesystem::equivalent`), never by
spelling: a case-distinct sibling of the root on a case-sensitive volume (`CaseRoot`/`caseroot`) is
external, Unicode/case aliases of the root or of components stay inside, and a symlink resolving
outside is outside. Compile-command selection, entry-source identification and command identity
likewise let the filesystem decide when the paths exist and use the lexical key only as fallback.

## Limits (explicit)

- Type references (fields, parameters, wrappers) are not recorded; `new`/`delete` operator calls and
  the destructor calls of delete-expressions (`CFGDeleteDtor`) are not recorded.
- Lifetime facts are not recorded for dependent template patterns, for bodies without a CFG, or
  for a used implicit destructor without a synthesized body (each a `limits` entry). Base
  construction and destruction edges are recorded for every base the compiler initializes or
  destroys, virtual bases included; what is not represented is the virtual-base distinction (no
  metadata marks a base step as virtual or records most-derived-class responsibility) and the
  execution order of subobject steps.
- Unknown callees: a call through a function pointer or a pointer to member function (`.*`, `->*`)
  takes its parameter prototype from the pointer type (for member pointers, the pointee of the RHS
  `MemberPointerType`); the target is never inferred from the variable. Where the ABI destroys a
  by-value argument in the callee, that destruction is an unresolved site of the caller.
- A by-value argument destroyed inside an external callee (MSVC ABI) is a boundary: no destructor
  call is recorded and a `limits` entry says so. The closure static invoker `__invoke` is never a node.
- Type atoms: unsupported type/value forms (see Phase 1C) skip the symbol with a limit; the remaining
  forms are recorded there for the later full template/type-reference work.
- Capture descriptors hash the captured declaration's file and span; an edit that moves the local
  changes the descriptor (locals are deliberately not stable public symbols).
- Default member initializers used by a constructor are attributed to that constructor with
  evaluation context `default_member_initializer` at the use site (and to the field itself at its
  declaration); default arguments are attributed to the caller at the use site with
  `default_argument`, matching evaluation semantics.
- Templates: dependent calls in patterns are unresolved; class template members of implicit
  instantiations fold to the pattern; partial specialization arguments use the compiler's printed
  form. Variable templates, alias templates, concepts, using-declarations and friends are skipped
  and listed in `limits`.
- Template bases: a base whose arguments cannot be represented, whose pattern is unreachable, or whose
  specialization kind is unclassified records no `extends` edge and says so in `limits` rather than
  publishing an argumentless edge. Dependent and non-record bases stay unrecorded with their own limit.
- Internal-linkage declarations outside the root get a pseudo file scope
  (`external/<hash of path>`) inside the `external` member.
- Degraded contexts always reject in this increment; a narrower accepted-degradation policy is
  future work. `completed_with_errors` facts are staged but not published by default.
- No incremental replacement, no storage, no query-time possible-target expansion, no Unreal.
- The front end prints a one-line error summary to stderr through `CompilerInstance` when a unit
  has errors; it is cosmetic and not part of the result.

## Response-file expansion (native cl.exe only)

`analyze_translation_unit` expands `@file` arguments before normalization, so the source,
duplicate-input, additional-input and safety gates all see everything the original driver would have
seen. Expansion is enabled for `CompilerKind::native_cl` only, whose nesting, quoting and encoding
behaviour was measured on this environment; for every other compiler an `@file` is still left
unexpanded, degrades the context and rejects the unit. No clang-cl support is claimed.

Rules, all of them rejections rather than best-effort parses: a relative `@file` resolves against
`CompileCommand::directory` (the process working directory), never against the naming file's
directory; repeated non-cyclic references are legal and reuse one snapshot, while a reference to a
file already on the active resolution path is a cycle; reuse is decided by filesystem identity, so
case aliases and hardlinks of one file collapse onto one snapshot while case-distinct siblings in a
case-sensitive directory stay separate, and every distinct resolved spelling is kept as evidence;
accepted encodings are ASCII without a BOM, validated UTF-8 with a BOM and validated UTF-16LE with a
BOM, with BOM-less non-ASCII bytes refused rather than guessed; depth, per-file bytes, aggregate
bytes, output tokens and a cumulative work budget are all finite.

Failure is atomic: nothing partially expanded is used, the raw command is normalized instead, and the
unit rejects with the expansion errors recorded as diagnostics.

`AnalysisResult::command_id` remains the raw command's identity. `AnalysisResult::replay_id`
identifies the context actually analysed: for an expanded unit it hashes the recomputed raw command
identity, every response file read (resolved path, alias spellings, encoding, size, content hash) and
every expanded token with its origin; otherwise it equals `command_id`. Dispositions index the
expanded argv, and `expansion.origins` maps each back to its raw argument and source file.

## Textual precompiled-header replay (stage 2)

An MSVC `/Yu` command consumes a binary `.pch` no other front end can read, so the normalizer emulates
it as a forced include and marks the context `degraded`, which rejects. That rejection stands unless
the caller supplies, explicitly, the **producer** command that built the PCH
(`AnalysisRequest::pch_producer`). Nothing is discovered: no producer, no replay.

### The ladder

Everything cheap and structural is checked first, and every rung rejects with an actionable reason.

- Both commands are native `cl.exe`, name the same executable, and have the same working directory,
  compared by **filesystem identity** rather than by a folded path.
- The producer expands its own response files and must match its own content identity; its expansion
  is retained as evidence rather than reconstructed later.
- **Both** argument lists pass the safety allowlist **before any front end runs**, so an unknown or
  dangerous option can never reach a capture.
- `/Yu`, `/Yc` and `/Fp` appear exactly once each; the consumer carries no `/Yc`, the producer no
  `/Yu`, neither a `/Y-`, and the producer no forced includes of its own.
- `/Yu`, `/Yc` and the consumer's first `/FI` are resolved **through the command's own include
  search** — quote directory, working directory, then each `/I` in order — because the ordinary MSVC
  and UE shape names the wrapper plainly and finds it through `/I`. They are then compared as files,
  not as spellings.
- The producer's `/Yc` must resolve to the **same file** as the consumer's `/Yu`. A producer that
  precompiled a different header is refused even when everything else lines up.
- The producer's source must be a wrapper translation unit: comments, whitespace and **exactly one**
  `#include`, with no pragma exemption, because a `#pragma push_macro` before the wrapper mutates
  state the capture window never sees. Its bytes come from the frozen snapshot, and the single
  include must itself resolve to the wrapper.
- Semantic options must agree **in order**: `/DX=1 /DX=2` is not `/DX=2 /DX=1`, and later switches
  override earlier ones.

### The proof

Two capture runs then process the wrapper, one under each context, sharing one set of frozen buffers.
The window is bounded by the wrapper's entry and its exit — detected by the documented `PrevFID` — and
it is **one-shot**: repeated forced includes are observable, so an unguarded wrapper can legitimately
be entered again later, and that later entry is ordinary post-prefix work rather than a second
prefix. The macro table and `__COUNTER__` are snapshotted **at the boundary**, never at end of
translation, so the unit's own later state is not mistaken for the prefix.

The captures **parse**, exactly as the final analysis does. That matters, and it was measured rather
than assumed: the Parser registers pragma handlers the preprocessor does not have on its own
(`intrinsic`, `optimize`, `vtordisp`, `float_control`, `clang loop`, `comment`), and those handlers
lex their own arguments *through* the preprocessor. A macro used as a pragma argument therefore
expands — and fires a macro-expansion event — only when a parser is present. A preprocess-only
capture would miss those events while the final parse has them, and a valid unit would be rejected for
a difference that is an artifact of how its prefix was measured. Aligning the measurement is the fix;
excluding the axis would be a weaker contract dressed up as a technical limitation.

For the same reason the token digest is **canonical**: it counts ordinary tokens only, never the
annotation token the parser substitutes for a pragma. The pragma itself is still compared, as an
event.

Five axes must agree: the ordered preprocessor event digest (definitions, undefinitions, expansions
**with their actual argument tokens**, condition and include outcomes, pragma events), the canonical
token stream, the macro table including parameter names, order and variadic flags, `__COUNTER__`, and
the entered files identified by **content**. Include search lists may differ; that is the one
difference the capture exists to adjudicate, and it is adjudicated by what the search resolved to.

The event digest is the load-bearing axis. Equal final values do not prove equal hidden state: a
macro can hold different values when `#pragma push_macro` runs and be undefined again before the
boundary, leaving the final macro table, the token stream and the pragma-producing macro's own
definition identical while the unreadable push-macro stack differs. Because the digest compares the
ordered operations including their arguments, those stacks are equal whenever it matches, so none of
them needs to be read and no balance rule is imposed.

### The final parse

An accepted unit parses through the **same frozen buffers** the validation read, and the same recorder
measures its prefix. It is held to **every axis the prefix was proven on** — events, canonical tokens,
macro table, entered files, `__COUNTER__` and the boundary — or the unit rejects; the report names the
axes it compared. This is possible precisely because all three runs measure the same way. Checking
only which files were entered would be strictly weaker, because a file appearing between validation
and the final parse can flip a `__has_include` or a conditional without any new file being entered.
Filesystem **query outcomes** are frozen too — a path absent during validation stays absent — so that
class of change is prevented as well as detected.

The consumer capture runs on the **exact replay argument list**, so what is proven is literally what
is replayed. Implicit binary-PCH substitution is suppressed by an empty, deterministic, **in-memory**
order guard whose path and would-be sidecar are both collision-checked against the real filesystem;
nothing is written to disk. Only the one synthetic `/FI` that `/Yu` emulation appended is removed,
identified by its originating disposition, so forced includes the command itself wrote keep their
positions — including a repeat of the wrapper after a later `/FI`.

### What this is not

Acceptance is an **existing-input textual snapshot**: the producer and consumer prefixes are
equivalent for the files as they are on disk now. It is not a claim that the replayed context equals
the historical binary `.pch`, and not a claim that the inputs are current. For the Temppal profile
specifically, the recorded build command references 322 include directories while the shared response
file on disk now carries 329, so those inputs are known to have drifted since the object was produced;
that limitation stands independently of any replay result.

## Single-TU replay tool (stage 4)

`tools/lcm_replay` runs one translation unit through the public API and writes a durable JSON report
(`lcm.replay-report.v1`): the raw command verbatim, **both** the consumer's and the producer's
response expansions exactly as consumed — files, encodings, content hashes and every expanded token
mapped back to its raw argument and origin — the per-argument dispositions, all identities, the PCH
verdict with the producer, consumer and final-parse capture digests, the replay arguments actually
used, status, diagnostics, limits and timing.

It also exports generic **fact evidence** for the unit's own main file so results can be checked
against the original source without the tool knowing any expected fact: direct bases ordered by
written position with effective access and whether access was spelled, full spans (line, column and
byte offset) plus content hashes everywhere, calls with each callee's definition locations, direct
includes in source order with their directive spans, unresolved sites, and the order guard named
explicitly as the only synthetic input. `--evidence-limit` bounds the lists and `truncated` says when
it clipped anything; `0` means unlimited.

It repairs nothing: no token is deleted and no compiler name is substituted, so a rejection is the
product contract speaking. Failures use distinct non-zero exits — 2 usage, 3 bad input, 4 context
rejected, 5 PCH replay not established, 6 completed with errors, 7 structural mismatch, 8 report
write failed. An incomplete producer pair is a usage error, never a silent downgrade to "no producer".
