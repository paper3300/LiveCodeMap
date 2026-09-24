#include "lcm/identity.hpp"

#include <stdexcept>

#include "lcm/hash.hpp"

namespace lcm {
namespace {

// Field serialization: <tag>=<byte length>:<bytes>; unambiguous regardless
// of separators inside values. Tags are textual so enum renumbering cannot
// silently change IDs.
void put_field(std::string& out, std::string_view tag, std::string_view value) {
  out.append(tag);
  out.push_back('=');
  out.append(std::to_string(value.size()));
  out.push_back(':');
  out.append(value);
  out.push_back(';');
}

}  // namespace

std::string_view to_string(Language language) {
  switch (language) {
    case Language::cpp:
      return "cpp";
  }
  return "unknown";
}

std::string_view to_string(SymbolKind kind) {
  switch (kind) {
    case SymbolKind::namespace_:
      return "namespace";
    case SymbolKind::class_:
      return "class";
    case SymbolKind::struct_:
      return "struct";
    case SymbolKind::union_:
      return "union";
    case SymbolKind::enum_:
      return "enum";
    case SymbolKind::enum_constant:
      return "enum_constant";
    case SymbolKind::function:
      return "function";
    case SymbolKind::method:
      return "method";
    case SymbolKind::constructor:
      return "constructor";
    case SymbolKind::destructor:
      return "destructor";
    case SymbolKind::conversion_function:
      return "conversion_function";
    case SymbolKind::operator_function:
      return "operator_function";
    case SymbolKind::variable:
      return "variable";
    case SymbolKind::field:
      return "field";
    case SymbolKind::type_alias:
      return "type_alias";
    case SymbolKind::lambda:
      return "lambda";
    case SymbolKind::anonymous_type:
      return "anonymous_type";
    case SymbolKind::local_variable:
      return "local_variable";
    case SymbolKind::concept_:
      return "concept";
  }
  return "unknown";
}

namespace {

bool is_callable_kind(SymbolKind kind) {
  switch (kind) {
    case SymbolKind::function:
    case SymbolKind::method:
    case SymbolKind::constructor:
    case SymbolKind::destructor:
    case SymbolKind::conversion_function:
    case SymbolKind::operator_function:
    case SymbolKind::lambda:
      return true;
    default:
      return false;
  }
}

// Kinds that only exist inside a function body / enclosing symbol and are
// therefore identified through LocalScope.
bool is_local_only_kind(SymbolKind kind) {
  return kind == SymbolKind::lambda || kind == SymbolKind::anonymous_type || kind == SymbolKind::local_variable;
}

// Kinds that can appear in an owner chain (scopes that can contain symbols).
bool may_own(SymbolKind kind) {
  switch (kind) {
    case SymbolKind::namespace_:
    case SymbolKind::class_:
    case SymbolKind::struct_:
    case SymbolKind::union_:
    case SymbolKind::enum_:
    case SymbolKind::anonymous_type:
      return true;
    default:
      return is_callable_kind(kind);
  }
}

// Kinds whose canonical_name may be empty (unnamed entities).
bool may_be_unnamed(SymbolKind kind, const LinkageDiscriminator& linkage) {
  if (kind == SymbolKind::lambda || kind == SymbolKind::anonymous_type) return true;
  // The anonymous namespace itself: a file-scoped namespace node.
  return kind == SymbolKind::namespace_ && std::holds_alternative<InternalLinkage>(linkage);
}

}  // namespace

std::string_view to_string(TemplateRole role) {
  switch (role) {
    case TemplateRole::none:
      return "none";
    case TemplateRole::primary:
      return "primary";
    case TemplateRole::explicit_specialization:
      return "explicit_specialization";
    case TemplateRole::partial_specialization:
      return "partial_specialization";
  }
  return "unknown";
}

namespace {

std::optional<std::string> validate_template(TemplateRole role, const std::string& parameters,
                                             const std::string& arguments, std::string_view what) {
  const bool specialization =
      role == TemplateRole::explicit_specialization || role == TemplateRole::partial_specialization;
  const bool has_parameter_list = role == TemplateRole::primary || role == TemplateRole::partial_specialization;
  if (specialization && arguments.empty()) {
    return std::string(what) + ": a template specialization requires its template arguments";
  }
  if (!specialization && !arguments.empty()) {
    return std::string(what) + ": template arguments are only valid for specializations";
  }
  if (has_parameter_list && parameters.empty()) {
    return std::string(what) + ": a primary or partial template requires its canonical template-parameter signature";
  }
  if (!has_parameter_list && !parameters.empty()) {
    return std::string(what) + ": template parameters are only valid for primaries and partial specializations";
  }
  return std::nullopt;
}

}  // namespace

std::optional<std::string> validate_canonical_key(const CanonicalKey& key) {
  if (key.repository_member.empty()) return "repository_member must not be empty";
  if (looks_absolute(key.repository_member)) return "repository_member must not be an absolute path";
  if (auto error = validate_template(key.template_role, key.template_parameters, key.template_arguments, "symbol")) {
    return error;
  }
  for (const auto& owner : key.owner_chain) {
    if (auto error = validate_template(owner.template_role, owner.template_parameters, owner.template_arguments,
                                       "owner component")) {
      return error;
    }
  }

  // Kind / signature.
  if (is_callable_kind(key.kind)) {
    if (key.normalized_signature.empty()) {
      return std::string(to_string(key.kind)) + " requires a normalized signature (overloads are distinguished by it)";
    }
  } else if (!key.normalized_signature.empty()) {
    return std::string(to_string(key.kind)) + " must not carry a signature";
  }
  if (key.canonical_name.empty() && !may_be_unnamed(key.kind, key.linkage)) {
    return "canonical_name must not be empty for a " + std::string(to_string(key.kind));
  }

  // Owner chain.
  bool has_anonymous_namespace_owner = false;
  for (const auto& owner : key.owner_chain) {
    if (!may_own(owner.kind)) {
      return "owner component of kind " + std::string(to_string(owner.kind)) + " cannot own symbols";
    }
    if (is_callable_kind(owner.kind)) {
      if (owner.normalized_signature.empty()) {
        return "callable owner " + std::string(to_string(owner.kind)) + " requires its normalized signature";
      }
    } else if (!owner.normalized_signature.empty()) {
      return "owner component of kind " + std::string(to_string(owner.kind)) + " must not carry a signature";
    }
    if (owner.name.empty()) {
      if (owner.kind == SymbolKind::namespace_) {
        has_anonymous_namespace_owner = true;
      } else if (owner.kind != SymbolKind::lambda && owner.kind != SymbolKind::anonymous_type) {
        return "owner component of kind " + std::string(to_string(owner.kind)) + " must be named";
      }
    }
  }

  // A named namespace is one logical scope with external linkage, unless it is
  // nested inside an anonymous namespace, which makes it file-scoped.
  if (key.kind == SymbolKind::namespace_ && !key.canonical_name.empty() && !has_anonymous_namespace_owner &&
      !std::holds_alternative<ExternalLinkage>(key.linkage)) {
    return "a named namespace is a single logical scope with external linkage";
  }

  // Linkage discriminator.
  if (const auto* internal = std::get_if<InternalLinkage>(&key.linkage)) {
    if (is_local_only_kind(key.kind)) return std::string(to_string(key.kind)) + " must use LocalScope";
    if (internal->file_scope.empty()) return "internal linkage requires a file scope";
    if (!is_canonical_repo_relative(internal->file_scope.generic)) {
      return "internal linkage file scope must be a canonical repository-relative path: '" +
             internal->file_scope.generic + "'";
    }
  } else if (const auto* local = std::get_if<LocalScope>(&key.linkage)) {
    // Any block-scope entity (named local class/enum/alias and their members,
    // lambdas, anonymous types, local variables) is discriminated by its
    // enclosing symbol plus anchor/ordinal. Namespaces cannot be local.
    if (key.kind == SymbolKind::namespace_) return "a namespace cannot use LocalScope";
    // An unnamed type at namespace scope (`struct {..} x;`) has no semantic
    // owner but is anchored to its declarator through `enclosing`; likewise a
    // lambda in a namespace-scope declarator initializer, but only under the
    // dedicated anchor. Every other local entity must sit inside an owner.
    if (key.owner_chain.empty() && key.kind != SymbolKind::anonymous_type &&
        !(key.kind == SymbolKind::lambda && local->anchor == kLambdaDeclInitAnchor)) {
      return "local scope requires an enclosing owner";
    }
    if (!is_well_formed_stable_id(local->enclosing.value)) return "local scope requires a well-formed enclosing stable id";
    if (local->anchor.empty()) return "local scope requires an anchor";
  } else {
    if (is_local_only_kind(key.kind)) return std::string(to_string(key.kind)) + " must use LocalScope";
    if (has_anonymous_namespace_owner) {
      return "members of an anonymous namespace have internal linkage and require a file scope";
    }
  }
  return std::nullopt;
}

std::string serialize_canonical_key(const CanonicalKey& key) {
  std::string out;
  out.append(kStableIdSchema);
  out.push_back('|');
  put_field(out, "member", key.repository_member);
  put_field(out, "lang", to_string(key.language));
  put_field(out, "kind", to_string(key.kind));
  put_field(out, "owners", std::to_string(key.owner_chain.size()));
  for (const auto& owner : key.owner_chain) {
    put_field(out, "okind", to_string(owner.kind));
    put_field(out, "oname", owner.name);
    put_field(out, "osig", owner.normalized_signature);
    // Template fields are emitted only for templates so that non-template
    // identities (and their published IDs) are unchanged by this extension.
    if (owner.template_role != TemplateRole::none) {
      put_field(out, "otrole", to_string(owner.template_role));
      put_field(out, "otparams", owner.template_parameters);
      put_field(out, "otargs", owner.template_arguments);
    }
  }
  put_field(out, "name", key.canonical_name);
  put_field(out, "sig", key.normalized_signature);
  if (key.template_role != TemplateRole::none) {
    put_field(out, "trole", to_string(key.template_role));
    put_field(out, "tparams", key.template_parameters);
    put_field(out, "targs", key.template_arguments);
  }
  std::visit(
      [&out](const auto& linkage) {
        using T = std::decay_t<decltype(linkage)>;
        if constexpr (std::is_same_v<T, ExternalLinkage>) {
          put_field(out, "linkage", "external");
        } else if constexpr (std::is_same_v<T, InternalLinkage>) {
          put_field(out, "linkage", "internal");
          put_field(out, "file", linkage.file_scope.generic);
        } else {
          put_field(out, "linkage", "local");
          put_field(out, "enclosing", linkage.enclosing.value);
          put_field(out, "anchor", linkage.anchor);
          put_field(out, "ordinal", std::to_string(linkage.ordinal));
        }
      },
      key.linkage);
  return out;
}

StableId make_stable_id(const CanonicalKey& key) {
  if (const auto error = validate_canonical_key(key)) {
    throw std::invalid_argument("invalid canonical key: " + *error);
  }
  const std::string hex = sha256(serialize_canonical_key(key)).hex();
  std::string value;
  value.reserve(kStableIdSchema.size() + 1 + 32);
  value.append(kStableIdSchema);
  value.push_back(':');
  value.append(hex, 0, 32);  // 128-bit truncation of SHA-256
  return StableId{std::move(value)};
}

bool is_well_formed_stable_id(std::string_view id) {
  const std::string prefix = std::string(kStableIdSchema) + ":";
  if (id.size() != prefix.size() + 32) return false;
  if (id.compare(0, prefix.size(), prefix) != 0) return false;
  for (std::size_t i = prefix.size(); i < id.size(); ++i) {
    const char c = id[i];
    const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    if (!hex) return false;
  }
  return true;
}

}  // namespace lcm
