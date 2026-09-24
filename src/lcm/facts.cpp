#include "lcm/facts.hpp"

#include <algorithm>
#include <stdexcept>

#include "lcm/hash.hpp"

namespace lcm {
namespace {

template <typename T>
void insert_sorted_unique(std::vector<T>& items, T value) {
  auto it = std::lower_bound(items.begin(), items.end(), value);
  if (it != items.end() && *it == value) return;
  items.insert(it, std::move(value));
}

void put(std::string& out, std::string_view tag, std::string_view value) {
  out.append(tag);
  out.push_back('=');
  out.append(std::to_string(value.size()));
  out.push_back(':');
  out.append(value);
  out.push_back(';');
}

void put_span(std::string& out, const SourceSpan& span) {
  put(out, "span",
      std::to_string(span.begin_offset) + "," + std::to_string(span.end_offset) + "," +
          std::to_string(span.begin_line) + "," + std::to_string(span.begin_column) + "," +
          std::to_string(span.end_line) + "," + std::to_string(span.end_column));
}

void put_location(std::string& out, const SourceLocation& location) {
  put(out, "file", location.file.generic);
  put(out, "content", location.content_hash.digest.hex() + ":" + std::to_string(location.content_hash.size));
  put_span(out, location.span);
  put(out, "spanhash", location.span_hash ? location.span_hash->hex() : std::string{});
}

}  // namespace

std::string_view to_string(IncludeDirectiveKind value) {
  switch (value) {
    case IncludeDirectiveKind::include_next:
      return "include_next";
    case IncludeDirectiveKind::import:
      return "import";
    default:
      return "include";
  }
}

std::string_view to_string(IncludeOperandKind value) {
  switch (value) {
    case IncludeOperandKind::angled:
      return "angled";
    case IncludeOperandKind::macro_tokens:
      return "macro_tokens";
    default:
      return "quoted";
  }
}

std::string_view to_string(IncludeActivity value) {
  switch (value) {
    case IncludeActivity::active:
      return "active";
    case IncludeActivity::inactive:
      return "inactive";
    default:
      return "indeterminate";
  }
}

std::string_view to_string(IncludeResolution value) {
  switch (value) {
    case IncludeResolution::resolved_internal:
      return "resolved_internal";
    case IncludeResolution::resolved_external:
      return "resolved_external";
    case IncludeResolution::not_found:
      return "not_found";
    default:
      return "not_evaluated";
  }
}

std::string_view to_string(ConditionTermKind value) {
  switch (value) {
    case ConditionTermKind::ifdef:
      return "ifdef";
    case ConditionTermKind::ifndef:
      return "ifndef";
    case ConditionTermKind::elif:
      return "elif";
    case ConditionTermKind::elifdef:
      return "elifdef";
    case ConditionTermKind::elifndef:
      return "elifndef";
    case ConditionTermKind::else_:
      return "else";
    default:
      return "if";
  }
}

std::string_view to_string(BaseAccess value) {
  switch (value) {
    case BaseAccess::public_:
      return "public";
    case BaseAccess::protected_:
      return "protected";
    case BaseAccess::private_:
      return "private";
  }
  return "public";
}

std::string_view to_string(Confidence value) {
  switch (value) {
    case Confidence::confirmed:
      return "confirmed";
    case Confidence::possible:
      return "possible";
    case Confidence::unresolved:
      return "unresolved";
  }
  return "unknown";
}

std::string_view to_string(RelationKind value) {
  switch (value) {
    case RelationKind::contains:
      return "contains";
    case RelationKind::calls:
      return "calls";
    case RelationKind::references:
      return "references";
    case RelationKind::includes:
      return "includes";
    case RelationKind::extends:
      return "extends";
    case RelationKind::overrides:
      return "overrides";
    case RelationKind::aliases:
      return "aliases";
  }
  return "unknown";
}

std::string_view to_string(EvidenceKind value) {
  switch (value) {
    case EvidenceKind::compiler_semantic:
      return "compiler_semantic";
    case EvidenceKind::syntax:
      return "syntax";
    case EvidenceKind::build_config:
      return "build_config";
  }
  return "unknown";
}

std::string_view to_string(UnresolvedReason value) {
  switch (value) {
    case UnresolvedReason::dependent_expression:
      return "dependent_expression";
    case UnresolvedReason::ambiguous_overload:
      return "ambiguous_overload";
    case UnresolvedReason::missing_declaration:
      return "missing_declaration";
    case UnresolvedReason::indirect_callee:
      return "indirect_callee";
    case UnresolvedReason::macro_expansion:
      return "macro_expansion";
    case UnresolvedReason::analysis_failed:
      return "analysis_failed";
    case UnresolvedReason::ignored_boundary:
      return "ignored_boundary";
    case UnresolvedReason::external_boundary:
      return "external_boundary";
  }
  return "unknown";
}

std::string_view to_string(EvaluationContext value) {
  switch (value) {
    case EvaluationContext::body:
      return "body";
    case EvaluationContext::default_argument:
      return "default_argument";
    case EvaluationContext::default_member_initializer:
      return "default_member_initializer";
    case EvaluationContext::init_capture_initializer:
      return "init_capture_initializer";
    case EvaluationContext::mem_initializer:
      return "mem_initializer";
    case EvaluationContext::implicit_mem_initializer:
      return "implicit_mem_initializer";
    case EvaluationContext::capture_copy:
      return "capture_copy";
  }
  return "unknown";
}

std::string_view to_string(SubobjectRole value) {
  switch (value) {
    case SubobjectRole::none:
      return "none";
    case SubobjectRole::base:
      return "base";
    case SubobjectRole::member:
      return "member";
    case SubobjectRole::delegating:
      return "delegating";
  }
  return "unknown";
}

std::string_view to_string(LifetimeKind value) {
  switch (value) {
    case LifetimeKind::none:
      return "none";
    case LifetimeKind::automatic_object:
      return "automatic_object";
    case LifetimeKind::temporary:
      return "temporary";
    case LifetimeKind::parameter:
      return "parameter";
    case LifetimeKind::subobject:
      return "subobject";
  }
  return "unknown";
}

std::string_view to_string(LocationRole value) {
  switch (value) {
    case LocationRole::declaration:
      return "declaration";
    case LocationRole::definition:
      return "definition";
    case LocationRole::implicit_declaration:
      return "implicit_declaration";
  }
  return "unknown";
}

std::string_view to_string(CaptureKind value) {
  switch (value) {
    case CaptureKind::by_copy:
      return "by_copy";
    case CaptureKind::by_reference:
      return "by_reference";
    case CaptureKind::this_pointer:
      return "this";
    case CaptureKind::star_this:
      return "star_this";
  }
  return "unknown";
}

std::string_view to_string(DispatchKind value) {
  switch (value) {
    case DispatchKind::static_target:
      return "static_target";
    case DispatchKind::virtual_slot:
      return "virtual_slot";
    case DispatchKind::devirtualized:
      return "devirtualized";
    case DispatchKind::qualified:
      return "qualified";
  }
  return "unknown";
}

std::string_view to_string(TemplateUse value) {
  switch (value) {
    case TemplateUse::none:
      return "none";
    case TemplateUse::primary_implicit:
      return "primary_implicit";
    case TemplateUse::explicit_specialization:
      return "explicit_specialization";
    case TemplateUse::partial_implicit:
      return "partial_implicit";
    case TemplateUse::explicit_instantiation_declaration:
      return "explicit_instantiation_declaration";
    case TemplateUse::explicit_instantiation_definition:
      return "explicit_instantiation_definition";
  }
  return "unknown";
}

std::string_view to_string(SymbolPresence value) {
  switch (value) {
    case SymbolPresence::indexed:
      return "indexed";
    case SymbolPresence::external_placeholder:
      return "external_placeholder";
    case SymbolPresence::endpoint_only:
      return "endpoint_only";
    case SymbolPresence::implicit_hidden:
      return "implicit_hidden";
  }
  return "unknown";
}

StableId FactSet::add_symbol_location(const CanonicalKey& key, SymbolLocation location) {
  if (location.analysis_unit.empty()) throw std::invalid_argument("symbol location requires an analysis unit");
  if (location.role == LocationRole::implicit_declaration) {
    throw std::invalid_argument("implicit declarations are recorded with add_hidden_member");
  }
  StableId id = make_stable_id(key);
  auto [it, inserted] = symbols_.try_emplace(id, Symbol{id, key, {}, SymbolPresence::indexed});
  if (!inserted && it->second.key != key) {
    throw std::logic_error("stable id collision between different canonical keys: " + id.value);
  }
  it->second.presence = SymbolPresence::indexed;
  insert_sorted_unique(it->second.locations, std::move(location));
  return id;
}

StableId FactSet::add_external_placeholder(const CanonicalKey& key) {
  StableId id = make_stable_id(key);
  auto [it, inserted] = symbols_.try_emplace(id, Symbol{id, key, {}, SymbolPresence::external_placeholder});
  if (!inserted && it->second.key != key) {
    throw std::logic_error("stable id collision between different canonical keys: " + id.value);
  }
  return id;
}

StableId FactSet::add_hidden_member(const CanonicalKey& key, SymbolLocation location) {
  if (location.analysis_unit.empty()) throw std::invalid_argument("hidden member requires an analysis unit");
  if (location.role != LocationRole::implicit_declaration) {
    throw std::invalid_argument("hidden members carry implicit_declaration locations only");
  }
  if (location.body_hash) throw std::invalid_argument("an implicit declaration has no body to hash");
  StableId id = make_stable_id(key);
  auto [it, inserted] = symbols_.try_emplace(id, Symbol{id, key, {}, SymbolPresence::implicit_hidden});
  if (!inserted && it->second.key != key) {
    throw std::logic_error("stable id collision between different canonical keys: " + id.value);
  }
  // A real declaration from another unit outranks the hidden view; otherwise
  // the member is (again) a compiler-synthesized hidden symbol.
  if (it->second.presence != SymbolPresence::indexed) it->second.presence = SymbolPresence::implicit_hidden;
  insert_sorted_unique(it->second.locations, std::move(location));
  return id;
}

void FactSet::add_direct_include(DirectIncludeFact include) {
  if (include.analysis_unit.empty()) throw std::invalid_argument("direct include requires an analysis unit");
  insert_sorted_unique(direct_includes_, std::move(include));
}

void FactSet::add_capture(CaptureFact capture) {
  if (capture.analysis_unit.empty()) throw std::invalid_argument("capture requires an analysis unit");
  insert_sorted_unique(captures_, std::move(capture));
}

void FactSet::add_relation_evidence(const RelationKey& key, Evidence evidence) {
  if (key.confidence == Confidence::unresolved) {
    throw std::invalid_argument("unresolved sites are recorded with add_unresolved_site, not as relations");
  }
  if (evidence.analysis_unit.empty()) throw std::invalid_argument("evidence requires an analysis unit");
  auto [it, inserted] = relations_.try_emplace(key, Relation{key, {}});
  insert_sorted_unique(it->second.evidence, std::move(evidence));
}

void FactSet::add_unresolved_site(UnresolvedSite site) {
  if (site.analysis_unit.empty()) throw std::invalid_argument("unresolved site requires an analysis unit");
  insert_sorted_unique(unresolved_, std::move(site));
}

void FactSet::remove_contribution(std::string_view analysis_unit) {
  for (auto it = relations_.begin(); it != relations_.end();) {
    auto& evidence = it->second.evidence;
    std::erase_if(evidence, [&](const Evidence& e) { return e.analysis_unit == analysis_unit; });
    if (evidence.empty()) {
      it = relations_.erase(it);
    } else {
      ++it;
    }
  }

  std::erase_if(unresolved_, [&](const UnresolvedSite& s) { return s.analysis_unit == analysis_unit; });
  std::erase_if(captures_, [&](const CaptureFact& c) { return c.analysis_unit == analysis_unit; });
  std::erase_if(direct_includes_,
                [&](const DirectIncludeFact& i) { return i.analysis_unit == analysis_unit; });

  // A symbol stays alive while any surviving evidence names it: a relation
  // endpoint, the enclosing symbol of an unresolved site or the lambda of a
  // capture from another unit.
  const auto referenced_by_relation = [&](const StableId& id) {
    const bool in_relation = std::any_of(relations_.begin(), relations_.end(), [&](const auto& entry) {
      return entry.first.source == id || entry.first.target == id;
    });
    if (in_relation) return true;
    if (std::any_of(unresolved_.begin(), unresolved_.end(),
                    [&](const UnresolvedSite& site) { return site.enclosing == id; })) {
      return true;
    }
    return std::any_of(captures_.begin(), captures_.end(), [&](const CaptureFact& c) { return c.lambda == id; });
  };

  for (auto it = symbols_.begin(); it != symbols_.end();) {
    auto& symbol = it->second;
    std::erase_if(symbol.locations, [&](const SymbolLocation& l) { return l.analysis_unit == analysis_unit; });
    if (!symbol.locations.empty()) {
      // Presence follows the surviving locations: indexed while any real
      // declaration remains, implicit_hidden when only compiler-synthesized
      // anchors remain (whatever the insertion or removal order was).
      const bool real_declaration = std::any_of(symbol.locations.begin(), symbol.locations.end(), [](const SymbolLocation& l) {
        return l.role != LocationRole::implicit_declaration;
      });
      symbol.presence = real_declaration ? SymbolPresence::indexed : SymbolPresence::implicit_hidden;
      ++it;
      continue;
    }
    // No location left. Keep the symbol only while some surviving relation
    // still names it, and say so truthfully instead of pretending it is
    // indexed or dropping the other unit's evidence.
    if (!referenced_by_relation(symbol.id)) {
      it = symbols_.erase(it);
      continue;
    }
    if (symbol.presence == SymbolPresence::indexed || symbol.presence == SymbolPresence::implicit_hidden) {
      symbol.presence = SymbolPresence::endpoint_only;
    }
    ++it;
  }
}

const Symbol* FactSet::find_symbol(const StableId& id) const {
  auto it = symbols_.find(id);
  return it == symbols_.end() ? nullptr : &it->second;
}

const Relation* FactSet::find_relation(const RelationKey& key) const {
  auto it = relations_.find(key);
  return it == relations_.end() ? nullptr : &it->second;
}

Sha256Digest FactSet::fact_hash() const {
  std::string out;
  put(out, "symbols", std::to_string(symbols_.size()));
  for (const auto& [id, symbol] : symbols_) {
    put(out, "id", id.value);
    put(out, "key", serialize_canonical_key(symbol.key));
    put(out, "presence", to_string(symbol.presence));
    put(out, "locations", std::to_string(symbol.locations.size()));
    for (const auto& loc : symbol.locations) {
      put(out, "role", to_string(loc.role));
      put_location(out, loc.location);
      put(out, "body", loc.body_hash ? loc.body_hash->hex() : std::string{});
      put(out, "unit", loc.analysis_unit);
      put(out, "flags", std::string(loc.callable.explicitly_defaulted ? "D" : "-") +
                            (loc.callable.deleted_as_written ? "X" : "-") +
                            (loc.callable.implicitly_deleted ? "I" : "-") + (loc.callable.trivial ? "T" : "-"));
    }
  }
  put(out, "relations", std::to_string(relations_.size()));
  for (const auto& [key, relation] : relations_) {
    put(out, "source", key.source.value);
    put(out, "target", key.target.value);
    put(out, "kind", to_string(key.kind));
    put(out, "confidence", to_string(key.confidence));
    put(out, "evidence", std::to_string(relation.evidence.size()));
    for (const auto& e : relation.evidence) {
      put(out, "unit", e.analysis_unit);
      put(out, "ekind", to_string(e.kind));
      put_location(out, e.location);
      put(out, "provider", e.provider_key.value_or(std::string{}));
      put(out, "eval", to_string(e.evaluation));
      put(out, "dispatch", to_string(e.dispatch));
      put(out, "tuse", to_string(e.template_use));
      put(out, "iil", e.immediately_invoked_lambda ? "1" : "0");
      put(out, "targs", e.template_arguments);
      put(out, "subrole", to_string(e.subobject_role));
      put(out, "subobject", e.subobject);
      put(out, "lifetime", to_string(e.lifetime));
      put(out, "elided", e.potentially_elided ? "1" : "0");
      put(out, "base", e.base ? std::string(to_string(e.base->effective_access)) +
                                    (e.base->access_written ? ",written" : ",default") +
                                    (e.base->is_virtual ? ",virtual" : ",nonvirtual") + "," +
                                    std::to_string(e.base->lexical_ordinal)
                              : std::string{});
    }
  }
  put(out, "unresolved", std::to_string(unresolved_.size()));
  for (const auto& site : unresolved_) {
    put(out, "enclosing", site.enclosing.value);
    put(out, "expr", site.expression);
    put_location(out, site.location);
    put(out, "reason", to_string(site.reason));
    put(out, "unit", site.analysis_unit);
  }
  put(out, "captures", std::to_string(captures_.size()));
  for (const auto& c : captures_) {
    put(out, "lambda", c.lambda.value);
    put(out, "ckind", to_string(c.kind));
    put(out, "cflags", std::string(c.explicit_capture ? "E" : "-") + (c.init_capture ? "I" : "-") +
                           (c.pack_expansion ? "P" : "-"));
    put(out, "name", c.name);
    put(out, "target", c.target_descriptor);
    put_location(out, c.location);
    put(out, "unit", c.analysis_unit);
  }
  put(out, "includes", std::to_string(direct_includes_.size()));
  for (const auto& i : direct_includes_) {
    put(out, "includer", i.includer.generic);
    put(out, "dkind", to_string(i.directive_kind));
    put(out, "okind", to_string(i.operand_kind));
    put(out, "operand", i.operand_as_written);
    put(out, "expanded", i.expanded_spelling.value_or(std::string{}));
    put(out, "activity", to_string(i.activity));
    put(out, "resolution", to_string(i.resolution));
    put(out, "target", i.resolved_repo_target ? i.resolved_repo_target->generic : std::string{});
    put(out, "depkey", i.external_dependency_key);
    put_location(out, i.directive_location);
    put(out, "conditions", std::to_string(i.condition_path.size()));
    for (const auto& branch : i.condition_path) {
      put(out, "else", branch.is_else ? "1" : "0");
      put(out, "own", branch.own_condition ? std::string(to_string(branch.own_condition->kind)) + " " +
                                                 branch.own_condition->expression_as_written
                                           : std::string{});
      if (branch.own_condition) put_location(out, branch.own_condition->location);
      put(out, "preceding", std::to_string(branch.preceding_branches.size()));
      for (const auto& term : branch.preceding_branches) {
        put(out, "pkind", to_string(term.kind));
        put(out, "pexpr", term.expression_as_written);
        put_location(out, term.location);
      }
    }
    put(out, "unit", i.analysis_unit);
  }
  return sha256(out);
}

}  // namespace lcm
