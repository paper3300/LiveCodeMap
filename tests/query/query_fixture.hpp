// Fixture store for the query tests: three units over a small workspace whose
// files exist on disk, covering every public-projection rule and the R7
// dispatch cases.
//
//   include/shapes.h   geo::Shape (class), Shape::area (decl), Shape::draw
//                      (virtual), ~Shape (implicit), geo::Circle, Circle::draw,
//                      geo::Square, Square::draw, geo::twice, fields f0..f11;
//                      includes detail.inl
//   include/hidden.h   hidden_fn (excluded by some tests)
//   include/gone.h     gone_fn (decl only) and shared_fn (decl; def in b.cpp)
//   include/detail.inl detail_fn
//   src/a.cpp (unit A) run_A, Shape::area (def), counter (internal),
//                      scoped_helper (internal to hidden.h), a lambda local to
//                      hidden_fn, one::dup
//   src/b.cpp (unit B) run_B, shared_fn (def), two::dup
//   src/c.cpp (unit C) c_only (retired by some tests: endpoint-only)
//   src/한글.cpp (A)   조회_함수 (non-ASCII path and identifier)
//
// Calls: run_A -> area (static), Shape::draw (virtual slot), ext::api
// (placeholder), ~Shape (implicit), hidden_fn, c_only, run_B; run_B -> Circle::draw
// (qualified), run_A, area (stored possible, virtual slot); twice -> area (both
// units). Overrides: Circle::draw -> Shape::draw; Square::draw -> Circle::draw
// and Shape::draw (two override paths).
#pragma once

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "store_fixture.hpp"

namespace lcm::query::test {

using namespace lcm::store;
using namespace lcm::store::test;
namespace fs = std::filesystem;

inline OwnerComponent fn_owner(std::string name, std::string signature = "()") {
  OwnerComponent o;
  o.kind = SymbolKind::function;
  o.name = std::move(name);
  o.normalized_signature = std::move(signature);
  return o;
}

struct Keys {
  CanonicalKey shape = key("Shape", SymbolKind::class_, "", {ns("geo")});
  CanonicalKey area = key("area", SymbolKind::method, "() const", {ns("geo"), cls("Shape")});
  CanonicalKey draw = key("draw", SymbolKind::method, "()", {ns("geo"), cls("Shape")});
  CanonicalKey dtor = key("~Shape", SymbolKind::destructor, "()", {ns("geo"), cls("Shape")});
  CanonicalKey circle = key("Circle", SymbolKind::class_, "", {ns("geo")});
  CanonicalKey circle_draw = key("draw", SymbolKind::method, "()", {ns("geo"), cls("Circle")});
  CanonicalKey square = key("Square", SymbolKind::class_, "", {ns("geo")});
  CanonicalKey square_draw = key("draw", SymbolKind::method, "()", {ns("geo"), cls("Square")});
  CanonicalKey twice = key("twice", SymbolKind::function, "(int)", {ns("geo")});
  CanonicalKey run_a = key("run_A");
  CanonicalKey run_b = key("run_B");
  CanonicalKey c_only = key("c_only");
  CanonicalKey ext = key("api", SymbolKind::function, "()", {ns("ext")});
  CanonicalKey hidden = key("hidden_fn");
  CanonicalKey gone = key("gone_fn");
  CanonicalKey shared = key("shared_fn");
  CanonicalKey detail = key("detail_fn");
  CanonicalKey dup1 = key("dup", SymbolKind::function, "()", {ns("one")});
  CanonicalKey dup2 = key("dup", SymbolKind::function, "()", {ns("two")});
  CanonicalKey korean = key("조회_함수");
  CanonicalKey counter = [] {
    CanonicalKey k = key("counter", SymbolKind::variable, "");
    k.linkage = InternalLinkage{repo("src/a.cpp")};
    return k;
  }();
  CanonicalKey scoped = [] {
    CanonicalKey k = key("scoped_helper");
    k.linkage = InternalLinkage{repo("include/hidden.h")};
    return k;
  }();
  CanonicalKey lambda = [] {
    CanonicalKey k = key("", SymbolKind::lambda, "() const", {fn_owner("hidden_fn")});
    k.linkage = LocalScope{make_stable_id(key("hidden_fn")), "lambda", 0};
    return k;
  }();
  std::vector<CanonicalKey> fields = [] {
    std::vector<CanonicalKey> out;
    for (int i = 0; i < 12; ++i) out.push_back(key("f" + std::to_string(i), SymbolKind::field, "", {ns("geo"), cls("Shape")}));
    return out;
  }();
};

inline StableId id(const CanonicalKey& k) { return make_stable_id(k); }

inline void write_text(const fs::path& p, const std::string& text) {
  fs::create_directories(p.parent_path());
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out << text;
}

// Common header facts every unit including shapes.h contributes.
inline void add_header(FactSet& f, const std::string& unit, const Keys& k) {
  const auto h = repo("include/shapes.h");
  const auto def = [&](const CanonicalKey& key, std::uint32_t off) {
    f.add_symbol_location(key, SymbolLocation{LocationRole::definition, loc(h, off), sha256(key.canonical_name), unit, {}});
  };
  const auto decl = [&](const CanonicalKey& key, std::uint32_t off) {
    f.add_symbol_location(key, SymbolLocation{LocationRole::declaration, loc(h, off), std::nullopt, unit, {}});
  };
  def(k.shape, 10);
  decl(k.area, 20);
  decl(k.draw, 30);
  def(k.circle, 50);
  decl(k.circle_draw, 60);
  def(k.square, 70);
  def(k.square_draw, 80);
  def(k.twice, 90);
  for (std::size_t i = 0; i < k.fields.size(); ++i) decl(k.fields[i], 200 + static_cast<std::uint32_t>(i) * 10);
  SymbolLocation implicit{LocationRole::implicit_declaration, loc(h, 10), std::nullopt, unit, {}};
  f.add_hidden_member(k.dtor, implicit);
  Evidence e;
  e.analysis_unit = unit;
  e.location = loc(h, 92);
  f.add_relation_evidence(RelationKey{id(k.twice), id(k.area), RelationKind::calls, Confidence::confirmed}, e);
  Evidence o = e;
  o.location = loc(h, 61);
  f.add_relation_evidence(RelationKey{id(k.circle_draw), id(k.draw), RelationKind::overrides, Confidence::confirmed}, o);
  o.location = loc(h, 81);
  f.add_relation_evidence(RelationKey{id(k.square_draw), id(k.circle_draw), RelationKind::overrides, Confidence::confirmed}, o);
  f.add_relation_evidence(RelationKey{id(k.square_draw), id(k.draw), RelationKind::overrides, Confidence::confirmed}, o);
  DirectIncludeFact inl;
  inl.includer = h;
  inl.operand_as_written = "\"detail.inl\"";
  inl.expanded_spelling = "detail.inl";
  inl.activity = IncludeActivity::active;
  inl.resolution = IncludeResolution::resolved_internal;
  inl.resolved_repo_target = repo("include/detail.inl");
  inl.directive_location = loc(h, 1);
  inl.analysis_unit = unit;
  f.add_direct_include(inl);
  f.add_symbol_location(k.detail, SymbolLocation{LocationRole::definition, loc("include/detail.inl", 5), sha256("d"), unit, {}});
}

inline DirectIncludeFact include_of(const std::string& unit, const std::string& includer, std::uint32_t off,
                                    const std::string& operand, std::optional<std::string> target) {
  DirectIncludeFact inc;
  inc.includer = repo(includer);
  inc.operand_as_written = operand;
  inc.activity = IncludeActivity::active;
  inc.resolution = target ? IncludeResolution::resolved_internal : IncludeResolution::resolved_external;
  if (target) inc.resolved_repo_target = repo(*target);
  if (!target) inc.external_dependency_key = "dep:" + sha256(operand).hex();
  inc.directive_location = loc(includer, off);
  inc.analysis_unit = unit;
  return inc;
}

inline Evidence call_at(const std::string& unit, const std::string& file, std::uint32_t off,
                        DispatchKind dispatch = DispatchKind::static_target) {
  Evidence e;
  e.analysis_unit = unit;
  e.location = loc(file, off);
  e.dispatch = dispatch;
  return e;
}

inline Contribution unit_a(const Keys& k) {
  FactSet f;
  add_header(f, "A", k);
  const std::string a = "src/a.cpp";
  const auto here = [&](const CanonicalKey& key, std::uint32_t off, LocationRole role = LocationRole::definition) {
    f.add_symbol_location(key, SymbolLocation{role, loc(a, off), role == LocationRole::definition
                                                                      ? std::optional<Sha256Digest>(sha256(key.canonical_name))
                                                                      : std::nullopt,
                                              "A", {}});
  };
  here(k.run_a, 10);
  here(k.area, 20);
  here(k.counter, 30);
  here(k.scoped, 40);
  here(k.lambda, 50);
  here(k.dup1, 60);
  f.add_symbol_location(k.hidden, SymbolLocation{LocationRole::declaration, loc("include/hidden.h", 3), std::nullopt, "A", {}});
  f.add_symbol_location(k.korean, SymbolLocation{LocationRole::definition, loc("src/한글.cpp", 5), sha256("k"), "A", {}});
  f.add_external_placeholder(k.ext);
  f.add_endpoint_only(k.c_only);
  f.add_symbol_location(k.run_b, SymbolLocation{LocationRole::declaration, loc(a, 5), std::nullopt, "A", {}});
  const auto calls = [&](const CanonicalKey& target, std::uint32_t off, DispatchKind d = DispatchKind::static_target) {
    f.add_relation_evidence(RelationKey{id(k.run_a), id(target), RelationKind::calls, Confidence::confirmed}, call_at("A", a, off, d));
  };
  calls(k.area, 100);
  calls(k.draw, 110, DispatchKind::virtual_slot);
  calls(k.ext, 120);
  calls(k.dtor, 130);
  calls(k.hidden, 140);
  calls(k.c_only, 150);
  calls(k.run_b, 160);
  f.add_relation_evidence(RelationKey{id(k.korean), id(k.run_a), RelationKind::calls, Confidence::confirmed},
                          call_at("A", "src/한글.cpp", 7));
  UnresolvedSite site;
  site.enclosing = id(k.run_a);
  site.expression = "callback()";
  site.location = loc(a, 170);
  site.reason = UnresolvedReason::indirect_callee;
  site.analysis_unit = "A";
  f.add_unresolved_site(site);
  site.enclosing = id(k.hidden);
  site.expression = "hidden_cb()";
  site.location = loc(a, 180);
  f.add_unresolved_site(site);
  CaptureFact cap;
  cap.lambda = id(k.lambda);
  cap.name = "x";
  cap.location = loc(a, 51);
  cap.analysis_unit = "A";
  f.add_capture(cap);
  f.add_direct_include(include_of("A", a, 1, "\"shapes.h\"", "include/shapes.h"));
  f.add_direct_include(include_of("A", a, 3, "\"hidden.h\"", "include/hidden.h"));
  f.add_direct_include(include_of("A", a, 6, "<vector>", std::nullopt));
  return contribution("A", "A", "cmd-a", std::move(f));
}

inline Contribution unit_b(const Keys& k) {
  FactSet f;
  add_header(f, "B", k);
  const std::string b = "src/b.cpp";
  f.add_symbol_location(k.run_b, SymbolLocation{LocationRole::definition, loc(b, 10), sha256("b"), "B", {}});
  f.add_symbol_location(k.run_a, SymbolLocation{LocationRole::declaration, loc(b, 5), std::nullopt, "B", {}});
  f.add_symbol_location(k.shared, SymbolLocation{LocationRole::definition, loc(b, 30), sha256("s"), "B", {}});
  f.add_symbol_location(k.dup2, SymbolLocation{LocationRole::definition, loc(b, 40), sha256("dup2"), "B", {}});
  f.add_symbol_location(k.gone, SymbolLocation{LocationRole::declaration, loc("include/gone.h", 3), std::nullopt, "B", {}});
  f.add_symbol_location(k.shared, SymbolLocation{LocationRole::declaration, loc("include/gone.h", 9), std::nullopt, "B", {}});
  f.add_relation_evidence(RelationKey{id(k.run_b), id(k.circle_draw), RelationKind::calls, Confidence::confirmed},
                          call_at("B", b, 100, DispatchKind::qualified));
  f.add_relation_evidence(RelationKey{id(k.run_b), id(k.run_a), RelationKind::calls, Confidence::confirmed}, call_at("B", b, 110));
  f.add_relation_evidence(RelationKey{id(k.run_b), id(k.area), RelationKind::calls, Confidence::possible},
                          call_at("B", b, 120, DispatchKind::virtual_slot));
  f.add_relation_evidence(RelationKey{id(k.run_b), id(k.gone), RelationKind::calls, Confidence::confirmed}, call_at("B", b, 130));
  f.add_relation_evidence(RelationKey{id(k.run_b), id(k.shared), RelationKind::calls, Confidence::confirmed}, call_at("B", b, 140));
  f.add_direct_include(include_of("B", b, 1, "\"shapes.h\"", "include/shapes.h"));
  f.add_direct_include(include_of("B", b, 3, "\"gone.h\"", "include/gone.h"));
  return contribution("B", "B", "cmd-b", std::move(f));
}

inline Contribution unit_c(const Keys& k) {
  FactSet f;
  f.add_symbol_location(k.c_only, SymbolLocation{LocationRole::definition, loc("src/c.cpp", 10), sha256("c"), "C", {}});
  return contribution("C", "C", "cmd-c", std::move(f));
}

struct QueryFx {
  fs::path dir;
  fs::path workspace;
  fs::path db;
  Keys k;

  [[nodiscard]] fs::path at(const std::string& rel) const { return workspace / path_from_utf8(rel); }

  // Creates the workspace files and publishes A, B and C into profile
  // "default" (UHT not applicable). The writer store is closed afterwards.
  static QueryFx create(const fs::path& dir, bool uht_applicable = false) {
    QueryFx fx;
    fx.dir = dir;
    fx.workspace = dir / "repo";
    fx.db = dir / "index.sqlite";
    for (const char* f : {"include/shapes.h", "include/hidden.h", "include/gone.h", "include/detail.inl", "src/a.cpp",
                          "src/b.cpp", "src/c.cpp", "src/한글.cpp"}) {
      fx.write(f, std::string("// ") + f + "\n");
    }
    Store store = Store::open(fx.db, fx.workspace);
    auto lock = store.acquire_writer(std::chrono::seconds(5));
    store.ensure_profile(*lock, "default", uht_applicable);
    Batch b;
    b.profile = "default";
    b.install = {unit_a(fx.k), unit_b(fx.k), unit_c(fx.k)};
    if (store.publish(*lock, b).outcome != PublishOutcome::committed) throw std::runtime_error("fixture publish failed");
    return fx;
  }

  void write(const std::string& rel, const std::string& text) const { write_text(at(rel), text); }

  Store writer() const { return Store::open(db, workspace); }

  void publish(std::vector<Contribution> install, std::vector<std::string> retire = {}) const {
    Store store = writer();
    auto lock = store.acquire_writer(std::chrono::seconds(5));
    Batch b;
    b.profile = "default";
    b.install = std::move(install);
    b.retire = std::move(retire);
    if (store.publish(*lock, b).outcome != PublishOutcome::committed) throw std::runtime_error("publish failed");
  }

  void exclude(const std::vector<std::string>& files) const {
    Store store = writer();
    auto lock = store.acquire_writer(std::chrono::seconds(5));
    std::vector<RepoRelativePath> paths;
    for (const auto& f : files) paths.push_back(repo(f));
    store.set_excluded_files(*lock, "default", paths);
  }
};

}  // namespace lcm::query::test
