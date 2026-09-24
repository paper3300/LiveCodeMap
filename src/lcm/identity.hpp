// Canonical symbol identity and public stable IDs (PRD FR-GPH-001..003).
//
// A stable ID is `cm1:<hash>` derived only from the canonical key. The key
// carries no absolute path, line number, body hash, active profile or
// generated-file path, so body/comment edits and file moves of external
// linkage symbols keep the ID while name/owner/signature changes produce a
// new one.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "lcm/source.hpp"

namespace lcm {

inline constexpr std::string_view kStableIdSchema = "cm1";

enum class Language : std::uint8_t {
  cpp,
};

enum class SymbolKind : std::uint8_t {
  namespace_,
  class_,
  struct_,
  union_,
  enum_,
  enum_constant,
  function,
  method,
  constructor,
  destructor,
  conversion_function,
  operator_function,
  variable,
  field,
  type_alias,
  lambda,
  anonymous_type,
  local_variable,
  concept_,
};

[[nodiscard]] std::string_view to_string(Language language);
[[nodiscard]] std::string_view to_string(SymbolKind kind);

// Template role of a symbol or owner (PRD FR-GPH-003). A primary template, an
// explicit specialization and a non-template entity with the same spelling
// are distinct identities; implicit instantiations are never public nodes.
enum class TemplateRole : std::uint8_t {
  none,
  primary,                 // the template pattern itself
  explicit_specialization, // template<> ... name<Args>
  partial_specialization,  // template<class U> ... name<U*>
};

[[nodiscard]] std::string_view to_string(TemplateRole role);

// One element of the semantic owner chain, outermost first. For callables
// acting as owners (e.g. the function enclosing a lambda) the normalized
// signature disambiguates overloads.
struct OwnerComponent {
  SymbolKind kind = SymbolKind::namespace_;
  std::string name;                 // canonical spelling; empty for anonymous namespace/type
  std::string normalized_signature; // only for callable owners; otherwise empty
  TemplateRole template_role = TemplateRole::none;
  std::string template_parameters; // canonical parameter kinds for primary/partial templates, e.g. "<typename, int>"
  std::string template_arguments;   // "<int>" for specializations; empty otherwise

  friend bool operator==(const OwnerComponent&, const OwnerComponent&) = default;
};

// Linkage discriminators. Exactly one applies to a symbol.
struct ExternalLinkage {
  friend bool operator==(const ExternalLinkage&, const ExternalLinkage&) = default;
};

// Internal linkage (`static` at namespace scope, anonymous namespace
// members): identity is scoped to the repository-relative file that owns
// the declaration.
struct InternalLinkage {
  RepoRelativePath file_scope;
  friend bool operator==(const InternalLinkage&, const InternalLinkage&) = default;
};

struct StableId {
  std::string value;  // "cm1:" + 32 lowercase hex characters

  friend bool operator==(const StableId&, const StableId&) = default;
  friend auto operator<=>(const StableId&, const StableId&) = default;
};

// Lambdas, anonymous types and local symbols: identity is scoped to the
// stable ID of the enclosing symbol plus a lexical anchor and ordinal. The
// enclosing ID already carries that symbol's own linkage (including internal
// file scope), so identical locals under same-named static helpers in
// different files stay distinct. Insertion of an earlier anonymous symbol may
// change the ordinal and thus the ID; the PRD allows this.
struct LocalScope {
  StableId enclosing;
  std::string anchor;  // e.g. "lambda", "anon-struct", or a local name
  std::uint32_t ordinal = 0;
  friend bool operator==(const LocalScope&, const LocalScope&) = default;
};

// The one anchor under which a lambda may have an EMPTY semantic owner chain:
// a closure written in the initializer of a namespace-scope declarator
// (`auto g = [] {};` at translation-unit scope). `enclosing` is then the
// declarator's stable ID. The validator checks only the anchor spelling; that
// `enclosing` really is such a declarator is the analyzer's responsibility.
inline constexpr std::string_view kLambdaDeclInitAnchor = "lambda-decl-init";

using LinkageDiscriminator = std::variant<ExternalLinkage, InternalLinkage, LocalScope>;

struct CanonicalKey {
  std::string repository_member;      // workspace member id (repo-relative; never an absolute path)
  Language language = Language::cpp;
  SymbolKind kind = SymbolKind::function;
  std::vector<OwnerComponent> owner_chain;  // outermost first, excluding the global namespace
  std::string canonical_name;         // unqualified canonical spelling
  std::string normalized_signature;   // analyzer-normalized; empty for non-callables
  LinkageDiscriminator linkage = ExternalLinkage{};
  TemplateRole template_role = TemplateRole::none;
  // Canonical template-parameter signature for primaries and partial
  // specializations: parameter kinds only ("<typename, int, typename...>"),
  // never parameter names, so renaming and redeclaration keep the ID while
  // `template<class T>` and `template<int N>` primaries stay distinct.
  std::string template_parameters;
  std::string template_arguments;     // required non-empty for specializations, empty otherwise

  friend bool operator==(const CanonicalKey&, const CanonicalKey&) = default;
};

// Returns a human-readable reason when the key violates the identity rules
// (absolute path in file scope or member, empty name for a named kind, ...),
// nullopt when the key is acceptable.
[[nodiscard]] std::optional<std::string> validate_canonical_key(const CanonicalKey& key);

// Deterministic, unambiguous serialization of the key (length-prefixed
// fields with textual tags). Exposed for diagnostics and tests.
[[nodiscard]] std::string serialize_canonical_key(const CanonicalKey& key);

// Throws std::invalid_argument when validate_canonical_key() reports an error.
[[nodiscard]] StableId make_stable_id(const CanonicalKey& key);

[[nodiscard]] bool is_well_formed_stable_id(std::string_view id);

}  // namespace lcm
