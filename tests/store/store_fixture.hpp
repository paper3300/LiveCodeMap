// Synthetic contributions shared by the store tests and the child writer
// process. Two TUs share `include/shared.h`; every supported fact kind is
// represented.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "lcm/facts.hpp"
#include "lcm/hash.hpp"
#include "lcm/identity.hpp"
#include "lcm/source.hpp"
#include "lcm/store/store.hpp"

namespace lcm::store::test {

inline RepoRelativePath repo(const std::string& generic, std::string as_written = {}) {
  return RepoRelativePath{generic, as_written.empty() ? generic : std::move(as_written)};
}

inline CanonicalKey key(std::string name, SymbolKind kind = SymbolKind::function, std::string signature = "()",
                        std::vector<OwnerComponent> owners = {}) {
  CanonicalKey k;
  k.repository_member = "repo";
  k.kind = kind;
  k.canonical_name = std::move(name);
  k.normalized_signature = std::move(signature);
  k.owner_chain = std::move(owners);
  return k;
}

inline OwnerComponent ns(std::string name) {
  OwnerComponent o;
  o.kind = SymbolKind::namespace_;
  o.name = std::move(name);
  return o;
}

inline OwnerComponent cls(std::string name) {
  OwnerComponent o;
  o.kind = SymbolKind::class_;
  o.name = std::move(name);
  return o;
}

// A location whose content hash is derived from `file` plus `seed`.
inline SourceLocation loc(const RepoRelativePath& file, std::uint32_t offset, const std::string& seed = "v1") {
  SourceLocation l;
  l.file = file;
  l.content_hash = hash_file_content(file.generic + "#" + seed);
  l.span = SourceSpan{offset, offset + 7, offset / 10 + 1, offset % 10 + 1, offset / 10 + 1, offset % 10 + 8};
  l.span_hash = sha256(file.generic + "@" + std::to_string(offset) + seed);
  return l;
}

inline SourceLocation loc(const std::string& file, std::uint32_t offset, const std::string& seed = "v1") {
  return loc(repo(file), offset, seed);
}

struct TuOptions {
  std::string body_seed = "body-1";   // changes the definition body hash of run_<slot>
  std::string signature = "()";       // changes the stable identity of run_<slot>
  bool with_conditional_include = false;
  bool with_capture = false;
};

// One ordinary TU `source` including the shared header.
inline FactSet shared_tu_facts(const std::string& label, const std::string& source, const std::string& run_name,
                               const TuOptions& options = {}) {
  FactSet f;
  const auto header = repo("include/shared.h", "include/Shared.h");
  const auto shape = key("Shape", SymbolKind::class_, "", {ns("geo")});
  const auto area = key("area", SymbolKind::method, "() const", {ns("geo"), cls("Shape")});
  const auto twice = key("twice", SymbolKind::function, "(int)", {ns("geo")});
  const auto run = key(run_name, SymbolKind::function, options.signature);
  CanonicalKey ext = key("api", SymbolKind::function, "()", {ns("ext")});

  SymbolLocation decl{LocationRole::declaration, loc(header, 10), std::nullopt, label, {}};
  f.add_symbol_location(shape, SymbolLocation{LocationRole::definition, loc(header, 5), sha256("shape-body"), label, {}});
  f.add_symbol_location(area, decl);
  SymbolLocation twice_def{LocationRole::definition, loc(header, 30), sha256("twice-body"), label, {}};
  f.add_symbol_location(twice, twice_def);
  const StableId run_id =
      f.add_symbol_location(run, SymbolLocation{LocationRole::definition, loc(source, 40), sha256(options.body_seed), label, {}});
  f.add_external_placeholder(ext);

  CanonicalKey dtor = key("~Shape", SymbolKind::destructor, "()", {ns("geo"), cls("Shape")});
  SymbolLocation hidden{LocationRole::implicit_declaration, loc(header, 5), std::nullopt, label, {}};
  hidden.callable.trivial = true;
  f.add_hidden_member(dtor, hidden);

  Evidence call;
  call.analysis_unit = label;
  call.location = loc(source, 44);
  call.provider_key = "c:@N@geo@S@Shape@F@area#1";
  f.add_relation_evidence(RelationKey{run_id, make_stable_id(area), RelationKind::calls, Confidence::confirmed}, call);

  Evidence virtual_call = call;
  virtual_call.location = loc(source, 48);
  virtual_call.dispatch = DispatchKind::virtual_slot;
  f.add_relation_evidence(RelationKey{run_id, make_stable_id(area), RelationKind::calls, Confidence::possible},
                          virtual_call);

  Evidence header_call;
  header_call.analysis_unit = label;
  header_call.location = loc(header, 33);
  f.add_relation_evidence(RelationKey{make_stable_id(twice), make_stable_id(area), RelationKind::calls,
                                      Confidence::confirmed},
                          header_call);

  Evidence ext_call = call;
  ext_call.location = loc(source, 52);
  ext_call.provider_key.reset();
  f.add_relation_evidence(RelationKey{run_id, make_stable_id(ext), RelationKind::calls, Confidence::confirmed}, ext_call);

  UnresolvedSite site;
  site.enclosing = make_stable_id(twice);
  site.expression = "callback(value)";
  site.location = loc(header, 35);
  site.reason = UnresolvedReason::indirect_callee;
  site.analysis_unit = label;
  f.add_unresolved_site(site);

  DirectIncludeFact inc;
  inc.includer = repo(source);
  inc.operand_as_written = "\"shared.h\"";
  inc.expanded_spelling = "shared.h";
  inc.activity = IncludeActivity::active;
  inc.resolution = IncludeResolution::resolved_internal;
  inc.resolved_repo_target = header;
  inc.directive_location = loc(source, 1);
  inc.analysis_unit = label;
  f.add_direct_include(inc);

  if (options.with_conditional_include) {
    DirectIncludeFact cond;
    cond.includer = repo(source);
    cond.operand_kind = IncludeOperandKind::angled;
    cond.operand_as_written = "<fallback.h>";
    cond.activity = IncludeActivity::inactive;
    cond.resolution = IncludeResolution::not_evaluated;
    cond.directive_location = loc(source, 20);
    cond.analysis_unit = label;
    ConditionalBranch outer;
    outer.own_condition = ConditionTerm{ConditionTermKind::ifdef, "USE_FALLBACK", loc(source, 18)};
    ConditionalBranch inner;
    inner.is_else = true;
    inner.preceding_branches.push_back(ConditionTerm{ConditionTermKind::if_, "HAS_A", loc(source, 19)});
    inner.preceding_branches.push_back(ConditionTerm{ConditionTermKind::elif, "HAS_B && !X", loc(source, 21)});
    cond.condition_path = {outer, inner};
    f.add_direct_include(cond);

    DirectIncludeFact forced;  // command-forced include: no includer file
    forced.operand_as_written = "\"pch.h\"";
    forced.activity = IncludeActivity::active;
    forced.resolution = IncludeResolution::resolved_external;
    forced.external_dependency_key = "dep:" + sha256("pch").hex();
    forced.directive_location.content_hash = hash_file_content("");
    forced.analysis_unit = label;
    f.add_direct_include(forced);
  }

  if (options.with_capture) {
    OwnerComponent enclosing;
    enclosing.kind = SymbolKind::function;
    enclosing.name = run_name;
    enclosing.normalized_signature = options.signature;
    CanonicalKey lambda = key("", SymbolKind::lambda, "(int) const", {enclosing});
    lambda.linkage = LocalScope{run_id, "lambda", 0};
    const StableId lambda_id =
        f.add_symbol_location(lambda, SymbolLocation{LocationRole::definition, loc(source, 60), sha256("lambda"), label, {}});
    CaptureFact cap;
    cap.lambda = lambda_id;
    cap.kind = CaptureKind::by_reference;
    cap.explicit_capture = true;
    cap.name = "total";
    cap.target_descriptor = sha256("total-decl").hex();
    cap.location = loc(source, 61);
    cap.analysis_unit = label;
    f.add_capture(cap);

    Evidence base;
    base.analysis_unit = label;
    base.kind = EvidenceKind::syntax;
    base.location = loc(source, 70);
    base.evaluation = EvaluationContext::default_argument;
    base.template_use = TemplateUse::partial_implicit;
    base.template_arguments = "<int *>";
    base.subobject_role = SubobjectRole::base;
    base.subobject = "geo::Shape";
    base.lifetime = LifetimeKind::temporary;
    base.potentially_elided = true;
    base.immediately_invoked_lambda = true;
    base.base = BaseEdgeDetails{BaseAccess::protected_, true, true, 2};
    f.add_relation_evidence(RelationKey{lambda_id, make_stable_id(shape), RelationKind::extends, Confidence::confirmed},
                            base);

    CanonicalKey local_static = key("counter", SymbolKind::variable, "", {ns("detail")});
    local_static.linkage = InternalLinkage{repo(source, source + "_AS_WRITTEN")};
    local_static.owner_chain.front().template_role = TemplateRole::primary;
    local_static.owner_chain.front().kind = SymbolKind::class_;
    local_static.owner_chain.front().template_parameters = "<typename>";
    SymbolLocation deleted{LocationRole::declaration, loc(source, 80), std::nullopt, label, {}};
    deleted.callable.explicitly_defaulted = true;
    deleted.callable.implicitly_deleted = true;
    f.add_symbol_location(local_static, deleted);
  }
  return f;
}

inline Contribution contribution(const std::string& slot, const std::string& label, const std::string& command,
                                 FactSet facts) {
  Contribution c;
  c.slot = slot;
  c.fact_label = label;
  c.provenance.command_id = command;
  c.provenance.replay_id = "replay:" + command;
  c.analysis_status = "ok";
  c.facts = std::move(facts);
  return c;
}

inline Contribution shared_tu(const std::string& slot, const std::string& source, const std::string& command,
                              const TuOptions& options = {}) {
  return contribution(slot, slot, command, shared_tu_facts(slot, source, "run_" + slot, options));
}

// Copies every fact of `from` into `into` through the public FactSet API.
inline void merge_into(FactSet& into, const FactSet& from) {
  for (const auto& [id, symbol] : from.symbols()) {
    if (symbol.presence == SymbolPresence::external_placeholder) into.add_external_placeholder(symbol.key);
    if (symbol.presence == SymbolPresence::endpoint_only) into.add_endpoint_only(symbol.key);
    for (const auto& l : symbol.locations) {
      if (l.role == LocationRole::implicit_declaration) {
        into.add_hidden_member(symbol.key, l);
      } else {
        into.add_symbol_location(symbol.key, l);
      }
    }
  }
  for (const auto& [k, r] : from.relations()) {
    for (const auto& e : r.evidence) into.add_relation_evidence(k, e);
  }
  for (const auto& s : from.unresolved_sites()) into.add_unresolved_site(s);
  for (const auto& c : from.captures()) into.add_capture(c);
  for (const auto& i : from.direct_includes()) into.add_direct_include(i);
}

// A fresh in-memory build of exactly these contributions: the oracle for
// what the store must contain after any sequence of replacements.
inline FactSet oracle(const std::vector<const Contribution*>& contributions) {
  FactSet out;
  for (const auto* c : contributions) merge_into(out, c->facts);
  return out;
}

}  // namespace lcm::store::test
