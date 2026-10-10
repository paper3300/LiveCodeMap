#include "lcm/store/contribution_codec.hpp"

#include <cstdint>
#include <cstring>
#include <deque>
#include <optional>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace lcm::store {

namespace {

constexpr std::string_view kMagic = "lcm.contribution.v1";

// Body: varints, bytes and references into a string table that precedes it.
class Writer {
 public:
  void u(std::uint64_t v) {
    while (v >= 0x80) {
      body_.push_back(static_cast<char>((v & 0x7f) | 0x80));
      v >>= 7;
    }
    body_.push_back(static_cast<char>(v));
  }
  void flag(bool v) { body_.push_back(v ? 1 : 0); }
  template <class E>
  void e(E v) {
    u(static_cast<std::uint64_t>(v));
  }
  void s(std::string_view v) {
    auto it = ids_.find(v);
    if (it == ids_.end()) {
      table_.emplace_back(v);
      it = ids_.emplace(std::string_view(table_.back()), table_.size() - 1).first;
    }
    u(it->second);
  }
  void d(const Sha256Digest& v) { s(std::string_view(reinterpret_cast<const char*>(v.bytes.data()), v.bytes.size())); }
  void os(const std::optional<std::string>& v) {
    flag(v.has_value());
    if (v) s(*v);
  }
  void od(const std::optional<Sha256Digest>& v) {
    flag(v.has_value());
    if (v) d(*v);
  }

  std::string finish() {
    Writer header;
    header.body_.append(kMagic);
    header.u(table_.size());
    for (const auto& t : table_) {
      header.u(t.size());
      header.body_.append(t);
    }
    return std::move(header.body_) + body_;
  }

 private:
  std::string body_;
  std::deque<std::string> table_;  // stable addresses for the string_view keys
  std::unordered_map<std::string_view, std::size_t> ids_;
};

class Reader {
 public:
  explicit Reader(std::string_view bytes) : bytes_(bytes) {
    if (bytes_.substr(0, kMagic.size()) != kMagic) fail();
    pos_ = kMagic.size();
    const std::uint64_t count = u();
    table_.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0; i < count; ++i) {
      const std::uint64_t size = u();
      if (size > bytes_.size() - pos_) fail();
      table_.emplace_back(bytes_.substr(pos_, static_cast<std::size_t>(size)));
      pos_ += static_cast<std::size_t>(size);
    }
  }
  std::uint64_t u() {
    std::uint64_t v = 0;
    for (int shift = 0; shift < 64; shift += 7) {
      if (pos_ >= bytes_.size()) fail();
      const auto byte = static_cast<std::uint8_t>(bytes_[pos_++]);
      v |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
      if (!(byte & 0x80)) return v;
    }
    fail();
  }
  std::uint32_t u32() {
    const std::uint64_t v = u();
    if (v > 0xffffffffu) fail();
    return static_cast<std::uint32_t>(v);
  }
  bool flag() {
    if (pos_ >= bytes_.size()) fail();
    const char c = bytes_[pos_++];
    if (c != 0 && c != 1) fail();
    return c == 1;
  }
  template <class E>
  E e() {
    const std::uint64_t v = u();
    if (v > 0xff) fail();
    return static_cast<E>(v);
  }
  std::string s() { return std::string(ref()); }
  Sha256Digest d() {
    const std::string_view v = ref();
    Sha256Digest out;
    if (v.size() != out.bytes.size()) fail();
    std::memcpy(out.bytes.data(), v.data(), v.size());
    return out;
  }
  std::optional<std::string> os() { return flag() ? std::optional<std::string>(s()) : std::nullopt; }
  std::optional<Sha256Digest> od() { return flag() ? std::optional<Sha256Digest>(d()) : std::nullopt; }
  std::size_t count() {
    const std::uint64_t v = u();
    if (v > bytes_.size()) fail();  // every element takes at least one byte
    return static_cast<std::size_t>(v);
  }
  bool done() const { return pos_ == bytes_.size(); }
  [[noreturn]] static void fail() { throw StoreError("corrupt spooled contribution"); }

 private:
  std::string_view ref() {
    const std::uint64_t id = u();
    if (id >= table_.size()) fail();
    return table_[static_cast<std::size_t>(id)];
  }

  std::string_view bytes_;
  std::size_t pos_ = 0;
  std::vector<std::string_view> table_;
};

// ---- encoders --------------------------------------------------------------

void put(Writer& w, const RepoRelativePath& p) {
  w.s(p.generic);
  w.s(p.as_written);
}
void put(Writer& w, const FileContentHash& h) {
  w.d(h.digest);
  w.u(h.size);
}
void put(Writer& w, const SourceLocation& l) {
  put(w, l.file);
  put(w, l.content_hash);
  w.u(l.span.begin_offset);
  w.u(l.span.end_offset);
  w.u(l.span.begin_line);
  w.u(l.span.begin_column);
  w.u(l.span.end_line);
  w.u(l.span.end_column);
  w.od(l.span_hash);
}
void put(Writer& w, const OwnerComponent& o) {
  w.e(o.kind);
  w.s(o.name);
  w.s(o.normalized_signature);
  w.e(o.template_role);
  w.s(o.template_parameters);
  w.s(o.template_arguments);
}
void put(Writer& w, const CanonicalKey& k) {
  w.s(k.repository_member);
  w.e(k.language);
  w.e(k.kind);
  w.u(k.owner_chain.size());
  for (const auto& o : k.owner_chain) put(w, o);
  w.s(k.canonical_name);
  w.s(k.normalized_signature);
  w.u(k.linkage.index());
  if (const auto* internal = std::get_if<InternalLinkage>(&k.linkage)) put(w, internal->file_scope);
  if (const auto* local = std::get_if<LocalScope>(&k.linkage)) {
    w.s(local->enclosing.value);
    w.s(local->anchor);
    w.u(local->ordinal);
  }
  w.e(k.template_role);
  w.s(k.template_parameters);
  w.s(k.template_arguments);
}
void put(Writer& w, const SymbolLocation& l) {
  w.e(l.role);
  put(w, l.location);
  w.od(l.body_hash);
  w.s(l.analysis_unit);
  w.flag(l.callable.explicitly_defaulted);
  w.flag(l.callable.deleted_as_written);
  w.flag(l.callable.implicitly_deleted);
  w.flag(l.callable.trivial);
}
void put(Writer& w, const Evidence& e) {
  w.s(e.analysis_unit);
  w.e(e.kind);
  put(w, e.location);
  w.os(e.provider_key);
  w.e(e.evaluation);
  w.e(e.dispatch);
  w.e(e.template_use);
  w.flag(e.immediately_invoked_lambda);
  w.s(e.template_arguments);
  w.e(e.subobject_role);
  w.s(e.subobject);
  w.e(e.lifetime);
  w.flag(e.potentially_elided);
  w.flag(e.base.has_value());
  if (e.base) {
    w.e(e.base->effective_access);
    w.flag(e.base->access_written);
    w.flag(e.base->is_virtual);
    w.u(e.base->lexical_ordinal);
  }
}
void put(Writer& w, const ConditionTerm& t) {
  w.e(t.kind);
  w.s(t.expression_as_written);
  put(w, t.location);
}
void put(Writer& w, const DirectIncludeFact& inc) {
  put(w, inc.includer);
  w.e(inc.directive_kind);
  w.e(inc.operand_kind);
  w.s(inc.operand_as_written);
  w.os(inc.expanded_spelling);
  w.e(inc.activity);
  w.e(inc.resolution);
  w.flag(inc.resolved_repo_target.has_value());
  if (inc.resolved_repo_target) put(w, *inc.resolved_repo_target);
  w.s(inc.external_dependency_key);
  put(w, inc.directive_location);
  w.u(inc.condition_path.size());
  for (const auto& b : inc.condition_path) {
    w.u(b.preceding_branches.size());
    for (const auto& t : b.preceding_branches) put(w, t);
    w.flag(b.own_condition.has_value());
    if (b.own_condition) put(w, *b.own_condition);
    w.flag(b.is_else);
  }
  w.s(inc.analysis_unit);
}
void put(Writer& w, const FactSet& f) {
  w.u(f.symbols().size());
  for (const auto& [id, symbol] : f.symbols()) {
    put(w, symbol.key);
    w.e(symbol.presence);
    w.u(symbol.locations.size());
    for (const auto& l : symbol.locations) put(w, l);
  }
  w.u(f.relations().size());
  for (const auto& [key, relation] : f.relations()) {
    w.s(key.source.value);
    w.s(key.target.value);
    w.e(key.kind);
    w.e(key.confidence);
    w.u(relation.evidence.size());
    for (const auto& e : relation.evidence) put(w, e);
  }
  w.u(f.unresolved_sites().size());
  for (const auto& site : f.unresolved_sites()) {
    w.s(site.enclosing.value);
    w.s(site.expression);
    put(w, site.location);
    w.e(site.reason);
    w.s(site.analysis_unit);
  }
  w.u(f.captures().size());
  for (const auto& c : f.captures()) {
    w.s(c.lambda.value);
    w.e(c.kind);
    w.flag(c.explicit_capture);
    w.flag(c.init_capture);
    w.flag(c.pack_expansion);
    w.s(c.name);
    w.s(c.target_descriptor);
    put(w, c.location);
    w.s(c.analysis_unit);
  }
  w.u(f.direct_includes().size());
  for (const auto& inc : f.direct_includes()) put(w, inc);
}

// ---- decoders --------------------------------------------------------------

RepoRelativePath get_path(Reader& r) {
  RepoRelativePath p;
  p.generic = r.s();
  p.as_written = r.s();
  return p;
}
FileContentHash get_hash(Reader& r) {
  FileContentHash h;
  h.digest = r.d();
  h.size = r.u();
  return h;
}
SourceLocation get_location(Reader& r) {
  SourceLocation l;
  l.file = get_path(r);
  l.content_hash = get_hash(r);
  l.span.begin_offset = r.u32();
  l.span.end_offset = r.u32();
  l.span.begin_line = r.u32();
  l.span.begin_column = r.u32();
  l.span.end_line = r.u32();
  l.span.end_column = r.u32();
  l.span_hash = r.od();
  return l;
}
CanonicalKey get_key(Reader& r) {
  CanonicalKey k;
  k.repository_member = r.s();
  k.language = r.e<Language>();
  k.kind = r.e<SymbolKind>();
  const std::size_t owners = r.count();
  for (std::size_t i = 0; i < owners; ++i) {
    OwnerComponent o;
    o.kind = r.e<SymbolKind>();
    o.name = r.s();
    o.normalized_signature = r.s();
    o.template_role = r.e<TemplateRole>();
    o.template_parameters = r.s();
    o.template_arguments = r.s();
    k.owner_chain.push_back(std::move(o));
  }
  k.canonical_name = r.s();
  k.normalized_signature = r.s();
  switch (r.u()) {
    case 0: k.linkage = ExternalLinkage{}; break;
    case 1: k.linkage = InternalLinkage{get_path(r)}; break;
    case 2: {
      LocalScope local;
      local.enclosing = StableId{r.s()};
      local.anchor = r.s();
      local.ordinal = r.u32();
      k.linkage = std::move(local);
      break;
    }
    default: Reader::fail();
  }
  k.template_role = r.e<TemplateRole>();
  k.template_parameters = r.s();
  k.template_arguments = r.s();
  return k;
}
SymbolLocation get_symbol_location(Reader& r) {
  SymbolLocation l;
  l.role = r.e<LocationRole>();
  l.location = get_location(r);
  l.body_hash = r.od();
  l.analysis_unit = r.s();
  l.callable.explicitly_defaulted = r.flag();
  l.callable.deleted_as_written = r.flag();
  l.callable.implicitly_deleted = r.flag();
  l.callable.trivial = r.flag();
  return l;
}
Evidence get_evidence(Reader& r) {
  Evidence e;
  e.analysis_unit = r.s();
  e.kind = r.e<EvidenceKind>();
  e.location = get_location(r);
  e.provider_key = r.os();
  e.evaluation = r.e<EvaluationContext>();
  e.dispatch = r.e<DispatchKind>();
  e.template_use = r.e<TemplateUse>();
  e.immediately_invoked_lambda = r.flag();
  e.template_arguments = r.s();
  e.subobject_role = r.e<SubobjectRole>();
  e.subobject = r.s();
  e.lifetime = r.e<LifetimeKind>();
  e.potentially_elided = r.flag();
  if (r.flag()) {
    BaseEdgeDetails b;
    b.effective_access = r.e<BaseAccess>();
    b.access_written = r.flag();
    b.is_virtual = r.flag();
    b.lexical_ordinal = r.u32();
    e.base = b;
  }
  return e;
}
ConditionTerm get_term(Reader& r) {
  ConditionTerm t;
  t.kind = r.e<ConditionTermKind>();
  t.expression_as_written = r.s();
  t.location = get_location(r);
  return t;
}
DirectIncludeFact get_include(Reader& r) {
  DirectIncludeFact inc;
  inc.includer = get_path(r);
  inc.directive_kind = r.e<IncludeDirectiveKind>();
  inc.operand_kind = r.e<IncludeOperandKind>();
  inc.operand_as_written = r.s();
  inc.expanded_spelling = r.os();
  inc.activity = r.e<IncludeActivity>();
  inc.resolution = r.e<IncludeResolution>();
  if (r.flag()) inc.resolved_repo_target = get_path(r);
  inc.external_dependency_key = r.s();
  inc.directive_location = get_location(r);
  const std::size_t branches = r.count();
  for (std::size_t i = 0; i < branches; ++i) {
    ConditionalBranch b;
    const std::size_t preceding = r.count();
    for (std::size_t j = 0; j < preceding; ++j) b.preceding_branches.push_back(get_term(r));
    if (r.flag()) b.own_condition = get_term(r);
    b.is_else = r.flag();
    inc.condition_path.push_back(std::move(b));
  }
  inc.analysis_unit = r.s();
  return inc;
}
// Through the public API, exactly as the store rebuilds committed facts.
FactSet get_facts(Reader& r) {
  FactSet f;
  const std::size_t symbols = r.count();
  for (std::size_t i = 0; i < symbols; ++i) {
    const CanonicalKey key = get_key(r);
    const auto presence = r.e<SymbolPresence>();
    if (presence == SymbolPresence::external_placeholder) f.add_external_placeholder(key);
    if (presence == SymbolPresence::endpoint_only) f.add_endpoint_only(key);
    const std::size_t locations = r.count();
    for (std::size_t j = 0; j < locations; ++j) {
      SymbolLocation l = get_symbol_location(r);
      if (l.role == LocationRole::implicit_declaration) {
        f.add_hidden_member(key, std::move(l));
      } else {
        f.add_symbol_location(key, std::move(l));
      }
    }
  }
  const std::size_t relations = r.count();
  for (std::size_t i = 0; i < relations; ++i) {
    RelationKey key;
    key.source = StableId{r.s()};
    key.target = StableId{r.s()};
    key.kind = r.e<RelationKind>();
    key.confidence = r.e<Confidence>();
    const std::size_t evidence = r.count();
    for (std::size_t j = 0; j < evidence; ++j) f.add_relation_evidence(key, get_evidence(r));
  }
  const std::size_t sites = r.count();
  for (std::size_t i = 0; i < sites; ++i) {
    UnresolvedSite site;
    site.enclosing = StableId{r.s()};
    site.expression = r.s();
    site.location = get_location(r);
    site.reason = r.e<UnresolvedReason>();
    site.analysis_unit = r.s();
    f.add_unresolved_site(std::move(site));
  }
  const std::size_t captures = r.count();
  for (std::size_t i = 0; i < captures; ++i) {
    CaptureFact c;
    c.lambda = StableId{r.s()};
    c.kind = r.e<CaptureKind>();
    c.explicit_capture = r.flag();
    c.init_capture = r.flag();
    c.pack_expansion = r.flag();
    c.name = r.s();
    c.target_descriptor = r.s();
    c.location = get_location(r);
    c.analysis_unit = r.s();
    f.add_capture(std::move(c));
  }
  const std::size_t includes = r.count();
  for (std::size_t i = 0; i < includes; ++i) f.add_direct_include(get_include(r));
  return f;
}

}  // namespace

std::string encode_contribution(const Contribution& c) {
  Writer w;
  w.s(c.slot);
  w.s(c.fact_label);
  w.s(c.provenance.command_id);
  w.os(c.provenance.context_id);
  w.s(c.provenance.replay_id);
  w.os(c.provenance.pch_producer_command_id);
  w.os(c.provenance.pch_producer_replay_id);
  w.s(c.settings.analyzer);
  w.s(c.settings.resource_dir);
  w.s(c.settings.repository_member);
  w.s(c.settings.publish_policy);
  w.s(c.analysis_status);
  w.e(c.build_currency);
  put(w, c.facts);
  w.u(c.inputs.size());
  for (const auto& in : c.inputs) {
    w.s(in.path_utf8);
    w.flag(in.repo_path.has_value());
    if (in.repo_path) put(w, *in.repo_path);
    w.e(in.role);
    put(w, in.content_hash);
    w.flag(in.is_system);
  }
  w.u(c.includes.size());
  for (const auto& e : c.includes) {
    w.os(e.includer_path_utf8);
    w.s(e.spelling);
    w.flag(e.angled);
    w.os(e.resolved_path_utf8);
    w.u(e.line);
  }
  w.u(c.diagnostics.size());
  for (const auto& d : c.diagnostics) {
    w.s(d.level);
    w.s(d.message);
    w.os(d.file);
    w.u(d.line);
    w.u(d.column);
  }
  w.u(c.limits.size());
  for (const auto& l : c.limits) w.s(l);
  return w.finish();
}

Contribution decode_contribution(std::string_view bytes) {
  Reader r(bytes);
  Contribution c;
  c.slot = r.s();
  c.fact_label = r.s();
  c.provenance.command_id = r.s();
  c.provenance.context_id = r.os();
  c.provenance.replay_id = r.s();
  c.provenance.pch_producer_command_id = r.os();
  c.provenance.pch_producer_replay_id = r.os();
  c.settings.analyzer = r.s();
  c.settings.resource_dir = r.s();
  c.settings.repository_member = r.s();
  c.settings.publish_policy = r.s();
  c.analysis_status = r.s();
  c.build_currency = r.e<BuildCurrency>();
  c.facts = get_facts(r);
  const std::size_t inputs = r.count();
  for (std::size_t i = 0; i < inputs; ++i) {
    InputObservation in;
    in.path_utf8 = r.s();
    if (r.flag()) in.repo_path = get_path(r);
    in.role = r.e<InputRole>();
    in.content_hash = get_hash(r);
    in.is_system = r.flag();
    c.inputs.push_back(std::move(in));
  }
  const std::size_t includes = r.count();
  for (std::size_t i = 0; i < includes; ++i) {
    IncludeEdge e;
    e.includer_path_utf8 = r.os();
    e.spelling = r.s();
    e.angled = r.flag();
    e.resolved_path_utf8 = r.os();
    e.line = r.u32();
    c.includes.push_back(std::move(e));
  }
  const std::size_t diagnostics = r.count();
  for (std::size_t i = 0; i < diagnostics; ++i) {
    UnitDiagnostic d;
    d.level = r.s();
    d.message = r.s();
    d.file = r.os();
    d.line = r.u32();
    d.column = r.u32();
    c.diagnostics.push_back(std::move(d));
  }
  const std::size_t limits = r.count();
  for (std::size_t i = 0; i < limits; ++i) c.limits.push_back(r.s());
  if (!r.done()) Reader::fail();
  return c;
}

}  // namespace lcm::store
