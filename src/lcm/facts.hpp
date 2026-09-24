// Fact graph primitives: logical symbols with multiple locations, relations
// backed by evidence, unresolved sites and contribution removal
// (PRD FR-GPH-001, FR-GPH-005, FR-IDX-003, P-02, P-03).
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "lcm/identity.hpp"
#include "lcm/source.hpp"

namespace lcm {

enum class Confidence : std::uint8_t {
  confirmed,   // compiler/syntax/explicit configuration confirmed the target
  possible,    // executable but not a specific confirmed execution (virtual dispatch, runtime binding)
  unresolved,  // evidence exists but no target can be safely determined; never a relation
};

enum class RelationKind : std::uint8_t {
  contains,
  calls,
  references,
  includes,
  extends,
  overrides,
  aliases,
};

enum class EvidenceKind : std::uint8_t {
  compiler_semantic,  // resolved by the compiler front end
  syntax,             // structural/syntactic observation without semantic resolution
  build_config,       // build description (compilation database, module description)
};

enum class UnresolvedReason : std::uint8_t {
  dependent_expression,   // depends on template parameters
  ambiguous_overload,
  missing_declaration,
  indirect_callee,        // pointer/functor/callback without dataflow proof
  macro_expansion,
  analysis_failed,
  ignored_boundary,
  external_boundary,
};

// Orthogonal aspects of how a call/reference site reached its target. They
// carry compiler facts the plain relation kind cannot (PRD FR-CPP-002/003/004,
// FR-GPH-003) so default arguments, lambdas, virtual dispatch and template use
// are neither misattributed nor merged, and one aspect never overwrites another.

// Where the expression is evaluated relative to the attributed caller.
enum class EvaluationContext : std::uint8_t {
  body,                        // ordinary code of the caller
  default_argument,            // inside a default argument, evaluated at this call site
  default_member_initializer,  // inside a default member initializer used by this constructor
  init_capture_initializer,    // inside a lambda init-capture, evaluated by the enclosing callable
  mem_initializer,             // inside a written base/member/delegating initializer of this constructor
  implicit_mem_initializer,    // compiler-synthesized initializer (CXXCtorInitializer::isWritten()==false)
  capture_copy,                // compiler-inserted copy/move of an ordinary by-copy or *this capture
};

// Which subobject a constructor initializer or destructor step addresses
// (PRD FR-CPP-003: base/member initializer identity; destructor-owned
// subobject destruction). `subobject` on the evidence names it.
enum class SubobjectRole : std::uint8_t {
  none,
  base,
  member,
  delegating,
};

// Lifetime character of an implicit destructor call extracted from the
// compiler's control-flow elements. `none` for every explicitly written call.
enum class LifetimeKind : std::uint8_t {
  none,
  automatic_object,  // block-scope object destroyed at scope exit (any exit path; recorded once)
  temporary,         // discarded/bound temporary destroyed at the end of its full-expression
  parameter,         // by-value parameter destroyed by the callee (compiler ABI decision)
  subobject,         // base/member destroyed by the owning destructor
};

// How the compiler selected the callee.
enum class DispatchKind : std::uint8_t {
  static_target,  // direct/non-virtual or otherwise statically bound
  virtual_slot,   // virtual dispatch: target is the compile-time selected slot
  devirtualized,  // virtual method reached without dispatch (final class/method, object expression)
  qualified,      // explicitly qualified member call (Base::f() or Derived::f()): statically bound
};

// Template use at the site. A folded use keeps its concrete arguments on the
// evidence (see template_arguments); the instantiation itself is never a node.
enum class TemplateUse : std::uint8_t {
  none,
  primary_implicit,         // implicit instantiation folded to the primary pattern
  explicit_specialization,  // call to an explicit specialization
  partial_implicit,         // implicit instantiation folded to the selected partial specialization
  // Use site of an explicitly instantiated class template. Which pattern was
  // selected is the target's canonical template role; these values describe the
  // use only and claim nothing about indexing the instantiation declaration.
  explicit_instantiation_declaration,
  explicit_instantiation_definition,
};

// Direct-base details of an `extends` relation (PRD FR-CPP-001). They belong
// to contribution-owned evidence, never to the relation key: two units may
// observe the same base differently and removing one must leave the other.
enum class BaseAccess : std::uint8_t {
  public_,
  protected_,
  private_,
};

[[nodiscard]] std::string_view to_string(BaseAccess value);

struct BaseEdgeDetails {
  BaseAccess effective_access = BaseAccess::public_;  // compiler-effective, incl. class/struct defaults
  bool access_written = false;                        // an access token was actually spelled
  bool is_virtual = false;
  std::uint32_t lexical_ordinal = 0;  // zero-based index in the written base list

  friend bool operator==(const BaseEdgeDetails&, const BaseEdgeDetails&) = default;
  friend auto operator<=>(const BaseEdgeDetails&, const BaseEdgeDetails&) = default;
};

[[nodiscard]] std::string_view to_string(Confidence value);
[[nodiscard]] std::string_view to_string(RelationKind value);
[[nodiscard]] std::string_view to_string(EvidenceKind value);
[[nodiscard]] std::string_view to_string(UnresolvedReason value);
[[nodiscard]] std::string_view to_string(EvaluationContext value);
[[nodiscard]] std::string_view to_string(DispatchKind value);
[[nodiscard]] std::string_view to_string(TemplateUse value);
[[nodiscard]] std::string_view to_string(SubobjectRole value);
[[nodiscard]] std::string_view to_string(LifetimeKind value);

// Where something was observed, tied to the content hash of the file at
// analysis time so later snippet reads can be verified.
struct SourceLocation {
  RepoRelativePath file;
  FileContentHash content_hash;
  SourceSpan span;
  std::optional<Sha256Digest> span_hash;

  friend bool operator==(const SourceLocation&, const SourceLocation&) = default;
  friend auto operator<=>(const SourceLocation&, const SourceLocation&) = default;
};

struct Evidence {
  std::string analysis_unit;  // compile action / TU identifier that contributed this
  EvidenceKind kind = EvidenceKind::compiler_semantic;
  SourceLocation location;
  std::optional<std::string> provider_key;  // e.g. Clang USR; diagnostic only, never a public ID
  EvaluationContext evaluation = EvaluationContext::body;
  DispatchKind dispatch = DispatchKind::static_target;
  TemplateUse template_use = TemplateUse::none;
  bool immediately_invoked_lambda = false;  // enclosing callable invoking a lambda expression at its definition
  std::string template_arguments;           // for a folded template use: the concrete use-site arguments
  SubobjectRole subobject_role = SubobjectRole::none;
  std::string subobject;                    // base type spelling, member name or delegated class; empty otherwise
  LifetimeKind lifetime = LifetimeKind::none;
  // The compiler may elide this construction/destruction (NRVO candidate
  // variable, pre-C++17 elidable copy). Never a substitute for any other aspect.
  bool potentially_elided = false;
  // Present only on `extends` evidence produced from a written base specifier.
  std::optional<BaseEdgeDetails> base;

  friend bool operator==(const Evidence&, const Evidence&) = default;
  friend auto operator<=>(const Evidence&, const Evidence&) = default;
};

// Confidence is part of the relation identity: a `possible` relation and a
// `confirmed` relation between the same endpoints are distinct facts, so
// merging evidence can never promote an uncertain target to confirmed.
struct RelationKey {
  StableId source;
  StableId target;
  RelationKind kind = RelationKind::references;
  Confidence confidence = Confidence::confirmed;

  friend bool operator==(const RelationKey&, const RelationKey&) = default;
  friend auto operator<=>(const RelationKey&, const RelationKey&) = default;
};

struct Relation {
  RelationKey key;
  std::vector<Evidence> evidence;  // sorted, unique; never empty inside a FactSet
};

struct UnresolvedSite {
  StableId enclosing;      // callable/scope containing the site
  std::string expression;  // spelled callee/reference expression
  SourceLocation location;
  UnresolvedReason reason = UnresolvedReason::analysis_failed;
  std::string analysis_unit;

  friend bool operator==(const UnresolvedSite&, const UnresolvedSite&) = default;
  friend auto operator<=>(const UnresolvedSite&, const UnresolvedSite&) = default;
};

enum class LocationRole : std::uint8_t {
  declaration,
  definition,
  implicit_declaration,  // compiler-synthesized member: the anchor is the class name, there is no user span
};

[[nodiscard]] std::string_view to_string(LocationRole value);

// Per-declaration callable state as the compiler reports it for THIS
// declaration (PRD FR-CPP-003). Orthogonal on purpose: an explicitly
// defaulted member may also be implicitly deleted. Never part of identity.
struct CallableFlags {
  bool explicitly_defaulted = false;  // this declaration spells `= default`
  bool deleted_as_written = false;    // this declaration spells `= delete`
  bool implicitly_deleted = false;    // the compiler deleted a defaulted/implicit member
  bool trivial = false;               // special member is trivial

  friend bool operator==(const CallableFlags&, const CallableFlags&) = default;
  friend auto operator<=>(const CallableFlags&, const CallableFlags&) = default;
};

struct SymbolLocation {
  LocationRole role = LocationRole::declaration;
  SourceLocation location;
  std::optional<Sha256Digest> body_hash;  // definitions only
  std::string analysis_unit;
  CallableFlags callable;  // all false for non-callables

  friend bool operator==(const SymbolLocation&, const SymbolLocation&) = default;
  friend auto operator<=>(const SymbolLocation&, const SymbolLocation&) = default;
};

enum class SymbolPresence : std::uint8_t {
  indexed,               // at least one location contributed by an analysis unit
  external_placeholder,  // out-of-root symbol kept because it is referenced (PRD FR-GPH-004)
  endpoint_only,         // all contributed locations were removed, but surviving relations from
                         // other units still name it; kept truthfully without a location
  implicit_hidden,       // compiler-synthesized special member that a fact references; excluded from
                         // default search, carries `implicit_declaration` locations per contributing unit
};

// One lambda capture (PRD FR-CPP-004): descriptor of the captured entity
// without indexing locals as public symbols or expanding def-use.
enum class CaptureKind : std::uint8_t {
  by_copy,
  by_reference,
  this_pointer,
  star_this,
};

[[nodiscard]] std::string_view to_string(CaptureKind value);

struct CaptureFact {
  StableId lambda;
  CaptureKind kind = CaptureKind::by_copy;
  bool explicit_capture = false;
  bool init_capture = false;
  bool pack_expansion = false;
  std::string name;               // spelled variable name, or "this"
  std::string target_descriptor;  // hash of the captured declaration's file and span; empty for this/*this
  SourceLocation location;        // the capture spelling, or the introducer for implicit captures
  std::string analysis_unit;

  friend bool operator==(const CaptureFact&, const CaptureFact&) = default;
  friend auto operator<=>(const CaptureFact&, const CaptureFact&) = default;
};

[[nodiscard]] std::string_view to_string(SymbolPresence value);

// ---------------------------------------------------------------------------
// Direct includes (PRD FR-GPH-004). A contribution-owned file-level fact, not a
// symbol relation: an inactive or unresolved directive has no truthful target,
// and no file symbol is invented to host it. Transitive closure is never
// stored; it is a query.

enum class IncludeDirectiveKind : std::uint8_t {
  include,
  include_next,
  import,
};

enum class IncludeOperandKind : std::uint8_t {
  quoted,        // "header.hpp"
  angled,        // <header.hpp>
  macro_tokens,  // #include HDR: the operand is macro tokens, not a header name
};

enum class IncludeActivity : std::uint8_t {
  active,         // the preprocessor actually processed this directive
  inactive,       // the directive lies inside a range the preprocessor proved skipped
  indeterminate,  // neither proven: unvisited or aborted region. A missing callback alone is NOT inactivity.
};

enum class IncludeResolution : std::uint8_t {
  resolved_internal,  // the compiler resolved it to a file inside the repository root
  resolved_external,  // resolved outside the root: a private dependency, never a public path or edge
  not_found,          // actually processed, and the compiler did not find the file
  not_evaluated,      // never processed: inactive or indeterminate. Never guessed from the filesystem.
};

enum class ConditionTermKind : std::uint8_t {
  if_,
  ifdef,
  ifndef,
  elif,
  elifdef,
  elifndef,
  else_,
};

[[nodiscard]] std::string_view to_string(IncludeDirectiveKind value);
[[nodiscard]] std::string_view to_string(IncludeOperandKind value);
[[nodiscard]] std::string_view to_string(IncludeActivity value);
[[nodiscard]] std::string_view to_string(IncludeResolution value);
[[nodiscard]] std::string_view to_string(ConditionTermKind value);

// One conditional directive, with its original expression bytes. No condition
// is ever evaluated or normalized here.
struct ConditionTerm {
  ConditionTermKind kind = ConditionTermKind::if_;
  std::string expression_as_written;  // empty for #else
  SourceLocation location;            // the original directive bytes

  friend bool operator==(const ConditionTerm&, const ConditionTerm&) = default;
  friend auto operator<=>(const ConditionTerm&, const ConditionTerm&) = default;
};

// One nesting level. Preceding siblings are kept because #elif B means
// "no earlier branch taken AND B", and #else means "no earlier branch taken".
struct ConditionalBranch {
  std::vector<ConditionTerm> preceding_branches;  // earlier branches of the same group, in order
  std::optional<ConditionTerm> own_condition;     // absent for #else
  bool is_else = false;

  friend bool operator==(const ConditionalBranch&, const ConditionalBranch&) = default;
  friend auto operator<=>(const ConditionalBranch&, const ConditionalBranch&) = default;
};

struct DirectIncludeFact {
  RepoRelativePath includer;
  IncludeDirectiveKind directive_kind = IncludeDirectiveKind::include;
  IncludeOperandKind operand_kind = IncludeOperandKind::quoted;
  std::string operand_as_written;                 // the literal operand tokens, delimiters included
  std::optional<std::string> expanded_spelling;   // compiler-expanded header name, when it processed the directive
  IncludeActivity activity = IncludeActivity::indeterminate;
  IncludeResolution resolution = IncludeResolution::not_evaluated;
  std::optional<RepoRelativePath> resolved_repo_target;  // only for resolved_internal
  // Opaque content-derived key of a resolved external file. It links to the
  // unit's private dependency observations; it is never a path and never ranks.
  std::string external_dependency_key;
  SourceLocation directive_location;             // the whole logical directive, original bytes
  std::vector<ConditionalBranch> condition_path;  // outermost first
  std::string analysis_unit;

  friend bool operator==(const DirectIncludeFact&, const DirectIncludeFact&) = default;
  friend auto operator<=>(const DirectIncludeFact&, const DirectIncludeFact&) = default;
};

// One logical symbol. Declarations and definitions from any file or TU
// attach here as locations; they are never separate public symbols.
struct Symbol {
  StableId id;
  CanonicalKey key;
  std::vector<SymbolLocation> locations;  // sorted, unique
  SymbolPresence presence = SymbolPresence::indexed;
};

class FactSet {
 public:
  // Attaches a location to the logical symbol identified by `key`, creating
  // the symbol on first sight. Returns the symbol's stable ID.
  StableId add_symbol_location(const CanonicalKey& key, SymbolLocation location);

  // Registers an out-of-root symbol that is referenced but not indexed.
  StableId add_external_placeholder(const CanonicalKey& key);

  // Attaches a contribution-owned `implicit_declaration` location to a
  // compiler-synthesized member referenced by a fact. The symbol stays
  // `implicit_hidden` (never `indexed`) unless a unit contributes a real
  // declaration. Throws std::invalid_argument for any other role.
  StableId add_hidden_member(const CanonicalKey& key, SymbolLocation location);

  void add_capture(CaptureFact capture);

  // Adds a direct-include fact. Independent of symbol lifetimes: no file
  // symbol, placeholder or relation is created for it.
  void add_direct_include(DirectIncludeFact include);

  // Adds evidence for a relation. Throws std::invalid_argument for
  // Confidence::unresolved: unresolved sites are not relations.
  void add_relation_evidence(const RelationKey& key, Evidence evidence);

  void add_unresolved_site(UnresolvedSite site);

  // Removes everything contributed by `analysis_unit`. Relations whose last
  // evidence disappears are removed; relations that still have evidence from
  // other units stay untouched. A symbol whose last location disappears is
  // removed unless surviving evidence still names it (as a relation endpoint
  // or as the enclosing symbol of an unresolved site), in which case it is
  // kept as `endpoint_only`. Placeholders and endpoint-only symbols that
  // nothing references any more are dropped. A surviving capture keeps its
  // lambda alive the same way.
  void remove_contribution(std::string_view analysis_unit);

  [[nodiscard]] const Symbol* find_symbol(const StableId& id) const;
  [[nodiscard]] const Relation* find_relation(const RelationKey& key) const;

  [[nodiscard]] const std::map<StableId, Symbol>& symbols() const { return symbols_; }
  [[nodiscard]] const std::map<RelationKey, Relation>& relations() const { return relations_; }
  [[nodiscard]] const std::vector<UnresolvedSite>& unresolved_sites() const { return unresolved_; }
  [[nodiscard]] const std::vector<CaptureFact>& captures() const { return captures_; }
  [[nodiscard]] const std::vector<DirectIncludeFact>& direct_includes() const { return direct_includes_; }

  // Hash over the canonical serialization of all facts; independent of
  // insertion order (PRD 11.2 determinism gate).
  [[nodiscard]] Sha256Digest fact_hash() const;

 private:
  std::map<StableId, Symbol> symbols_;
  std::map<RelationKey, Relation> relations_;
  std::vector<UnresolvedSite> unresolved_;  // kept sorted, unique
  std::vector<CaptureFact> captures_;       // kept sorted, unique
  std::vector<DirectIncludeFact> direct_includes_;  // kept sorted, unique
};

}  // namespace lcm
