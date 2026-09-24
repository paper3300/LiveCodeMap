#include <doctest/doctest.h>

#include <algorithm>
#include <map>
#include <stdexcept>

#include "lcm/facts.hpp"
#include "lcm/hash.hpp"

using namespace lcm;

namespace {

CanonicalKey function_key(std::string name, std::string signature = "()") {
  CanonicalKey key;
  key.repository_member = "repo";
  key.kind = SymbolKind::function;
  key.canonical_name = std::move(name);
  key.normalized_signature = std::move(signature);
  return key;
}

SourceLocation location(std::string file, std::uint32_t begin, std::uint32_t end, std::string content = "") {
  SourceLocation loc;
  loc.file = RepoRelativePath{std::move(file)};
  loc.content_hash = hash_file_content(content.empty() ? loc.file.generic : content);
  loc.span.begin_offset = begin;
  loc.span.end_offset = end;
  loc.span.begin_line = 1;
  loc.span.begin_column = begin + 1;
  loc.span.end_line = 1;
  loc.span.end_column = end + 1;
  return loc;
}

Evidence evidence(std::string unit, SourceLocation loc) {
  Evidence e;
  e.analysis_unit = std::move(unit);
  e.kind = EvidenceKind::compiler_semantic;
  e.location = std::move(loc);
  return e;
}

SymbolLocation symbol_location(LocationRole role, std::string unit, SourceLocation loc) {
  SymbolLocation s;
  s.role = role;
  s.location = std::move(loc);
  s.analysis_unit = std::move(unit);
  return s;
}

}  // namespace

TEST_CASE("declaration and definition merge into one logical symbol with two locations") {
  FactSet facts;
  const auto key = function_key("area", "(int,int)");
  const auto id_from_header =
      facts.add_symbol_location(key, symbol_location(LocationRole::declaration, "tu:a.cpp", location("inc/geo.h", 10, 30)));
  const auto id_from_cpp =
      facts.add_symbol_location(key, symbol_location(LocationRole::definition, "tu:geo.cpp", location("src/geo.cpp", 40, 90)));
  CHECK(id_from_header == id_from_cpp);
  REQUIRE(facts.symbols().size() == 1);
  const Symbol* symbol = facts.find_symbol(id_from_header);
  REQUIRE(symbol);
  CHECK(symbol->locations.size() == 2);
  CHECK(symbol->presence == SymbolPresence::indexed);

  SUBCASE("the same declaration seen from a second TU is not duplicated") {
    facts.add_symbol_location(key, symbol_location(LocationRole::declaration, "tu:a.cpp", location("inc/geo.h", 10, 30)));
    CHECK(facts.find_symbol(id_from_header)->locations.size() == 2);
  }

  SUBCASE("a second TU seeing the same header adds a distinct contribution") {
    facts.add_symbol_location(key, symbol_location(LocationRole::declaration, "tu:b.cpp", location("inc/geo.h", 10, 30)));
    CHECK(facts.find_symbol(id_from_header)->locations.size() == 3);
    facts.remove_contribution("tu:a.cpp");
    CHECK(facts.find_symbol(id_from_header)->locations.size() == 2);
  }
}

TEST_CASE("a relation survives while any evidence remains and disappears with the last one") {
  FactSet facts;
  const auto caller = facts.add_symbol_location(
      function_key("main"), symbol_location(LocationRole::definition, "tu:main.cpp", location("src/main.cpp", 0, 50)));
  const auto callee = facts.add_symbol_location(
      function_key("area", "(int,int)"),
      symbol_location(LocationRole::definition, "tu:geo.cpp", location("src/geo.cpp", 0, 50)));
  const RelationKey key{caller, callee, RelationKind::calls, Confidence::confirmed};

  facts.add_relation_evidence(key, evidence("tu:main.cpp", location("src/main.cpp", 20, 30)));
  facts.add_relation_evidence(key, evidence("tu:main_alt.cpp", location("src/main.cpp", 20, 30)));
  facts.add_relation_evidence(key, evidence("tu:main.cpp", location("src/main.cpp", 20, 30)));  // duplicate
  REQUIRE(facts.find_relation(key));
  CHECK(facts.find_relation(key)->evidence.size() == 2);

  facts.remove_contribution("tu:main.cpp");
  REQUIRE(facts.find_relation(key));
  CHECK(facts.find_relation(key)->evidence.size() == 1);
  CHECK(facts.find_relation(key)->evidence.front().analysis_unit == "tu:main_alt.cpp");

  facts.remove_contribution("tu:main_alt.cpp");
  CHECK(facts.find_relation(key) == nullptr);
  CHECK(facts.relations().empty());
}

TEST_CASE("removing the unit that owned a symbol's last location keeps surviving relation endpoints truthful") {
  FactSet facts;
  // Unit A owns the only location of `helper`; unit B contributes a call to it.
  const auto helper = facts.add_symbol_location(
      function_key("helper"), symbol_location(LocationRole::definition, "tu:A", location("src/a.cpp", 0, 20)));
  const auto caller = facts.add_symbol_location(
      function_key("caller"), symbol_location(LocationRole::definition, "tu:B", location("src/b.cpp", 0, 20)));
  const RelationKey call{caller, helper, RelationKind::calls, Confidence::confirmed};
  facts.add_relation_evidence(call, evidence("tu:B", location("src/b.cpp", 5, 12)));

  facts.remove_contribution("tu:A");

  // B's evidence is untouched and both endpoints still exist.
  REQUIRE(facts.find_relation(call));
  CHECK(facts.find_relation(call)->evidence.size() == 1);
  const Symbol* helper_symbol = facts.find_symbol(helper);
  REQUIRE(helper_symbol);
  CHECK(helper_symbol->locations.empty());
  CHECK(helper_symbol->presence == SymbolPresence::endpoint_only);
  CHECK(facts.find_symbol(caller)->presence == SymbolPresence::indexed);

  SUBCASE("re-analysing A restores the indexed location") {
    facts.add_symbol_location(function_key("helper"),
                              symbol_location(LocationRole::definition, "tu:A", location("src/a.cpp", 0, 20)));
    CHECK(facts.find_symbol(helper)->presence == SymbolPresence::indexed);
    CHECK(facts.find_symbol(helper)->locations.size() == 1);
  }

  SUBCASE("once B is removed too, the unlocated endpoint is dropped") {
    facts.remove_contribution("tu:B");
    CHECK(facts.find_relation(call) == nullptr);
    CHECK(facts.find_symbol(helper) == nullptr);
    CHECK(facts.find_symbol(caller) == nullptr);
    CHECK(facts.symbols().empty());
  }
}

TEST_CASE("an unresolved site from another unit keeps its enclosing symbol alive") {
  FactSet facts;
  const auto caller = facts.add_symbol_location(
      function_key("caller"), symbol_location(LocationRole::definition, "tu:A", location("src/a.cpp", 0, 40)));
  UnresolvedSite site;
  site.enclosing = caller;
  site.expression = "fp()";
  site.location = location("src/b.cpp", 10, 14);
  site.reason = UnresolvedReason::indirect_callee;
  site.analysis_unit = "tu:B";
  facts.add_unresolved_site(site);

  facts.remove_contribution("tu:A");
  REQUIRE(facts.unresolved_sites().size() == 1);
  const Symbol* enclosing = facts.find_symbol(caller);
  REQUIRE(enclosing);  // no dangling enclosing id
  CHECK(enclosing->presence == SymbolPresence::endpoint_only);
  CHECK(enclosing->locations.empty());

  facts.remove_contribution("tu:B");
  CHECK(facts.unresolved_sites().empty());
  CHECK(facts.find_symbol(caller) == nullptr);
  CHECK(facts.symbols().empty());
}

TEST_CASE("possible and confirmed relations between the same endpoints are distinct facts") {
  FactSet facts;
  const auto caller = facts.add_symbol_location(
      function_key("caller"), symbol_location(LocationRole::definition, "tu:x", location("src/x.cpp", 0, 20)));
  const auto target = facts.add_symbol_location(
      function_key("Derived::Draw"), symbol_location(LocationRole::definition, "tu:y", location("src/y.cpp", 0, 20)));

  const RelationKey possible{caller, target, RelationKind::calls, Confidence::possible};
  const RelationKey confirmed{caller, target, RelationKind::calls, Confidence::confirmed};
  facts.add_relation_evidence(possible, evidence("tu:x", location("src/x.cpp", 5, 10)));
  facts.add_relation_evidence(possible, evidence("tu:x2", location("src/x.cpp", 5, 10)));

  // More possible evidence never turns into a confirmed relation.
  CHECK(facts.find_relation(possible) != nullptr);
  CHECK(facts.find_relation(confirmed) == nullptr);
  CHECK(facts.relations().size() == 1);
}

TEST_CASE("call aspects are orthogonal, part of evidence identity and of the fact hash") {
  FactSet facts;
  const auto caller = facts.add_symbol_location(
      function_key("use"), symbol_location(LocationRole::definition, "tu:u", location("src/u.cpp", 0, 60)));
  auto primary_key = function_key("token", "()");
  primary_key.template_role = TemplateRole::primary;
  primary_key.template_parameters = "<typename>";
  const auto primary = facts.add_symbol_location(
      primary_key, symbol_location(LocationRole::definition, "tu:u", location("src/u.cpp", 0, 20)));
  const RelationKey call{caller, primary, RelationKind::calls, Confidence::confirmed};

  // One site can carry an evaluation context, a dispatch kind and a template use at once.
  Evidence combined = evidence("tu:u", location("src/u.cpp", 30, 44));
  combined.evaluation = EvaluationContext::default_argument;
  combined.dispatch = DispatchKind::virtual_slot;
  combined.template_use = TemplateUse::primary_implicit;
  combined.template_arguments = "<double>";
  Evidence other_args = combined;
  other_args.template_arguments = "<char>";
  Evidence immediate = combined;
  immediate.immediately_invoked_lambda = true;
  facts.add_relation_evidence(call, combined);
  facts.add_relation_evidence(call, other_args);   // differs only in type arguments
  facts.add_relation_evidence(call, immediate);    // differs only in the immediate-lambda flag
  facts.add_relation_evidence(call, combined);     // exact duplicate
  REQUIRE(facts.find_relation(call));
  CHECK(facts.find_relation(call)->evidence.size() == 3);
  const auto& stored = facts.find_relation(call)->evidence;
  CHECK(std::all_of(stored.begin(), stored.end(), [](const Evidence& e) {
    return e.evaluation == EvaluationContext::default_argument && e.dispatch == DispatchKind::virtual_slot &&
           e.template_use == TemplateUse::primary_implicit;
  }));
  const auto with_three = facts.fact_hash();

  // Changing any single aspect changes the fact hash.
  for (int variant = 0; variant < 4; ++variant) {
    FactSet other;
    other.add_symbol_location(function_key("use"),
                              symbol_location(LocationRole::definition, "tu:u", location("src/u.cpp", 0, 60)));
    other.add_symbol_location(primary_key, symbol_location(LocationRole::definition, "tu:u", location("src/u.cpp", 0, 20)));
    Evidence changed = combined;
    if (variant == 0) changed.evaluation = EvaluationContext::body;
    if (variant == 1) changed.dispatch = DispatchKind::static_target;
    if (variant == 2) changed.template_use = TemplateUse::none;
    if (variant == 3) changed.immediately_invoked_lambda = true;
    other.add_relation_evidence(call, changed);
    other.add_relation_evidence(call, other_args);
    other.add_relation_evidence(call, immediate);
    CAPTURE(variant);
    CHECK(other.fact_hash() != with_three);
  }
  CHECK(to_string(EvaluationContext::default_argument) == "default_argument");
  CHECK(to_string(DispatchKind::qualified) == "qualified");
  CHECK(to_string(TemplateUse::primary_implicit) == "primary_implicit");
}

TEST_CASE("lifetime, subobject and elision aspects and callable flags are part of identity and hash") {
  FactSet facts;
  const auto owner = facts.add_symbol_location(
      function_key("~Box"), symbol_location(LocationRole::definition, "tu:u", location("src/u.cpp", 0, 60)));
  const auto dtor = facts.add_symbol_location(
      function_key("~Part"), symbol_location(LocationRole::declaration, "tu:u", location("src/u.cpp", 0, 20)));
  const RelationKey call{owner, dtor, RelationKind::calls, Confidence::confirmed};
  Evidence sub = evidence("tu:u", location("src/u.cpp", 30, 44));
  sub.lifetime = LifetimeKind::subobject;
  sub.subobject_role = SubobjectRole::member;
  sub.subobject = "part";
  Evidence other_member = sub;
  other_member.subobject = "second";
  Evidence elided = sub;
  elided.potentially_elided = true;
  Evidence written = sub;
  written.lifetime = LifetimeKind::none;
  facts.add_relation_evidence(call, sub);
  facts.add_relation_evidence(call, other_member);
  facts.add_relation_evidence(call, elided);
  facts.add_relation_evidence(call, written);
  facts.add_relation_evidence(call, sub);  // duplicate
  REQUIRE(facts.find_relation(call));
  CHECK(facts.find_relation(call)->evidence.size() == 4);
  const auto baseline = facts.fact_hash();
  for (int variant = 0; variant < 4; ++variant) {
    FactSet other;
    other.add_symbol_location(function_key("~Box"),
                              symbol_location(LocationRole::definition, "tu:u", location("src/u.cpp", 0, 60)));
    other.add_symbol_location(function_key("~Part"),
                              symbol_location(LocationRole::declaration, "tu:u", location("src/u.cpp", 0, 20)));
    Evidence changed = sub;
    if (variant == 0) changed.lifetime = LifetimeKind::automatic_object;
    if (variant == 1) changed.subobject_role = SubobjectRole::base;
    if (variant == 2) changed.subobject = "x";
    if (variant == 3) changed.evaluation = EvaluationContext::implicit_mem_initializer;
    other.add_relation_evidence(call, changed);
    other.add_relation_evidence(call, other_member);
    other.add_relation_evidence(call, elided);
    other.add_relation_evidence(call, written);
    CAPTURE(variant);
    CHECK(other.fact_hash() != baseline);
  }
  // Callable flags: orthogonal, per location, in equality and in the hash.
  SymbolLocation defaulted = symbol_location(LocationRole::definition, "tu:u", location("src/u.cpp", 70, 90));
  defaulted.callable.explicitly_defaulted = true;
  SymbolLocation both = defaulted;
  both.callable.implicitly_deleted = true;
  CHECK(defaulted != both);
  FactSet flags_a;
  flags_a.add_symbol_location(function_key("Wrap", "(const Wrap &)"), defaulted);
  FactSet flags_b;
  flags_b.add_symbol_location(function_key("Wrap", "(const Wrap &)"), both);
  CHECK(flags_a.fact_hash() != flags_b.fact_hash());
  CHECK(flags_b.symbols().begin()->second.locations.front().callable.explicitly_defaulted);
  CHECK(flags_b.symbols().begin()->second.locations.front().callable.implicitly_deleted);
  CHECK(to_string(LifetimeKind::parameter) == "parameter");
  CHECK(to_string(SubobjectRole::delegating) == "delegating");
  CHECK(to_string(EvaluationContext::capture_copy) == "capture_copy");
  CHECK(to_string(LocationRole::implicit_declaration) == "implicit_declaration");
}

TEST_CASE("hidden implicit members are contribution-owned and never indexed") {
  FactSet facts;
  const auto user = facts.add_symbol_location(
      function_key("copy_plain"), symbol_location(LocationRole::definition, "tu:A", location("src/a.cpp", 0, 40)));
  CanonicalKey hidden_key = function_key("Plain", "(const Plain &)");
  hidden_key.kind = SymbolKind::constructor;
  OwnerComponent plain;
  plain.kind = SymbolKind::struct_;
  plain.name = "Plain";
  hidden_key.owner_chain.push_back(plain);
  SymbolLocation anchor = symbol_location(LocationRole::implicit_declaration, "tu:A", location("src/a.cpp", 50, 55));
  anchor.callable.trivial = true;

  CHECK_THROWS_AS(facts.add_symbol_location(hidden_key, anchor), std::invalid_argument);
  SymbolLocation with_body = anchor;
  with_body.body_hash = sha256("x");
  CHECK_THROWS_AS(facts.add_hidden_member(hidden_key, with_body), std::invalid_argument);
  SymbolLocation wrong_role = anchor;
  wrong_role.role = LocationRole::declaration;
  CHECK_THROWS_AS(facts.add_hidden_member(hidden_key, wrong_role), std::invalid_argument);

  const auto hidden = facts.add_hidden_member(hidden_key, anchor);
  CHECK(facts.find_symbol(hidden)->presence == SymbolPresence::implicit_hidden);
  CHECK(facts.find_symbol(hidden)->locations.size() == 1);
  facts.add_relation_evidence({user, hidden, RelationKind::calls, Confidence::confirmed},
                              evidence("tu:A", location("src/a.cpp", 10, 20)));
  // A second unit sees the same synthesized member: a second owned location, no overwrite.
  SymbolLocation anchor_b = anchor;
  anchor_b.analysis_unit = "tu:B";
  facts.add_hidden_member(hidden_key, anchor_b);
  facts.add_relation_evidence({user, hidden, RelationKind::calls, Confidence::confirmed},
                              evidence("tu:B", location("src/a.cpp", 10, 20)));
  CHECK(facts.find_symbol(hidden)->locations.size() == 2);
  CHECK(facts.find_symbol(hidden)->presence == SymbolPresence::implicit_hidden);

  facts.remove_contribution("tu:A");
  REQUIRE(facts.find_symbol(hidden));
  CHECK(facts.find_symbol(hidden)->presence == SymbolPresence::implicit_hidden);
  CHECK(facts.find_symbol(hidden)->locations.size() == 1);
  CHECK(facts.find_symbol(hidden)->locations.front().analysis_unit == "tu:B");
  CHECK(facts.find_symbol(user)->presence == SymbolPresence::endpoint_only);  // B's call still names it

  SUBCASE("a hidden member whose units are gone but is still referenced becomes endpoint_only") {
    facts.add_relation_evidence({user, hidden, RelationKind::calls, Confidence::confirmed},
                                evidence("tu:C", location("src/c.cpp", 10, 20)));
    facts.remove_contribution("tu:B");
    REQUIRE(facts.find_symbol(hidden));
    CHECK(facts.find_symbol(hidden)->presence == SymbolPresence::endpoint_only);
    CHECK(facts.find_symbol(hidden)->locations.empty());
    facts.remove_contribution("tu:C");
    CHECK(facts.symbols().empty());
  }
  SUBCASE("removing the last unit drops the hidden member and the unlocated caller") {
    facts.remove_contribution("tu:B");
    CHECK(facts.symbols().empty());
    CHECK(facts.relations().empty());
  }
}

TEST_CASE("mixed hidden and real declarations: presence follows the surviving locations in every order") {
  CanonicalKey key = function_key("Plain", "(const Plain &)");
  key.kind = SymbolKind::constructor;
  OwnerComponent plain;
  plain.kind = SymbolKind::struct_;
  plain.name = "Plain";
  key.owner_chain.push_back(plain);
  const SymbolLocation hidden_h = symbol_location(LocationRole::implicit_declaration, "tu:H", location("src/p.h", 8, 13));
  const SymbolLocation real_d = symbol_location(LocationRole::declaration, "tu:D", location("src/p.h", 40, 70));
  const auto user_key = function_key("copy_plain");
  const SymbolLocation user_loc = symbol_location(LocationRole::definition, "tu:E", location("src/e.cpp", 0, 30));

  // Freshly built survivors, for hash equivalence after removals.
  const auto only_hidden = [&] {
    FactSet f;
    f.add_hidden_member(key, hidden_h);
    return f.fact_hash();
  }();
  const auto only_real = [&] {
    FactSet f;
    f.add_symbol_location(key, real_d);
    return f.fact_hash();
  }();

  for (int order = 0; order < 2; ++order) {
    CAPTURE(order);
    FactSet facts;
    StableId id;
    if (order == 0) {
      id = facts.add_hidden_member(key, hidden_h);
      facts.add_symbol_location(key, real_d);
    } else {
      id = facts.add_symbol_location(key, real_d);
      facts.add_hidden_member(key, hidden_h);
    }
    // A real declaration present: indexed, both locations kept.
    CHECK(facts.find_symbol(id)->presence == SymbolPresence::indexed);
    CHECK(facts.find_symbol(id)->locations.size() == 2);

    SUBCASE("removing the real declaration returns the member to implicit_hidden") {
      facts.remove_contribution("tu:D");
      REQUIRE(facts.find_symbol(id));
      CHECK(facts.find_symbol(id)->presence == SymbolPresence::implicit_hidden);
      CHECK(facts.find_symbol(id)->locations.size() == 1);
      CHECK(facts.fact_hash() == only_hidden);
      facts.remove_contribution("tu:H");
      CHECK(facts.symbols().empty());
    }
    SUBCASE("removing the hidden view keeps the member indexed") {
      facts.remove_contribution("tu:H");
      REQUIRE(facts.find_symbol(id));
      CHECK(facts.find_symbol(id)->presence == SymbolPresence::indexed);
      CHECK(facts.fact_hash() == only_real);
      facts.remove_contribution("tu:D");
      CHECK(facts.symbols().empty());
    }
    SUBCASE("no location left but surviving evidence: endpoint_only, then gone") {
      const auto user = facts.add_symbol_location(user_key, user_loc);
      facts.add_relation_evidence({user, id, RelationKind::calls, Confidence::confirmed},
                                  evidence("tu:E", location("src/e.cpp", 5, 15)));
      facts.remove_contribution("tu:H");
      facts.remove_contribution("tu:D");
      REQUIRE(facts.find_symbol(id));
      CHECK(facts.find_symbol(id)->presence == SymbolPresence::endpoint_only);
      CHECK(facts.find_symbol(id)->locations.empty());
      // Re-adding either kind of location restores the matching presence.
      facts.add_hidden_member(key, hidden_h);
      CHECK(facts.find_symbol(id)->presence == SymbolPresence::implicit_hidden);
      facts.add_symbol_location(key, real_d);
      CHECK(facts.find_symbol(id)->presence == SymbolPresence::indexed);
      facts.remove_contribution("tu:D");
      facts.remove_contribution("tu:H");
      facts.remove_contribution("tu:E");
      CHECK(facts.symbols().empty());
      CHECK(facts.relations().empty());
    }
  }
}

TEST_CASE("captures are facts: owned by their unit, hashed, and they keep their lambda alive") {
  FactSet facts;
  CanonicalKey lambda_key = function_key("", "() const");
  lambda_key.kind = SymbolKind::lambda;
  OwnerComponent run_owner;
  run_owner.kind = SymbolKind::function;
  run_owner.name = "run";
  run_owner.normalized_signature = "()";
  lambda_key.owner_chain.push_back(run_owner);
  const auto run = facts.add_symbol_location(
      function_key("run"), symbol_location(LocationRole::definition, "tu:A", location("src/w.cpp", 0, 100)));
  lambda_key.linkage = LocalScope{run, "lambda", 0};
  const auto lambda = facts.add_symbol_location(
      lambda_key, symbol_location(LocationRole::definition, "tu:A", location("src/w.cpp", 20, 60)));
  CaptureFact x;
  x.lambda = lambda;
  x.kind = CaptureKind::by_copy;
  x.explicit_capture = true;
  x.name = "x";
  x.target_descriptor = "abcd";
  x.location = location("src/w.cpp", 21, 22);
  x.analysis_unit = "tu:A";
  CaptureFact y = x;
  y.kind = CaptureKind::by_reference;
  y.name = "y";
  CaptureFact x_again = x;
  facts.add_capture(x);
  facts.add_capture(y);
  facts.add_capture(x_again);  // duplicate
  CHECK(facts.captures().size() == 2);
  const auto with_two = facts.fact_hash();
  CaptureFact init = x;
  init.init_capture = true;
  facts.add_capture(init);  // differs only in the init-capture flag
  CHECK(facts.captures().size() == 3);
  CHECK(facts.fact_hash() != with_two);
  CaptureFact unowned = x;
  unowned.analysis_unit.clear();
  CHECK_THROWS_AS(facts.add_capture(unowned), std::invalid_argument);

  // A capture from another unit keeps the lambda alive after its own unit is removed.
  CaptureFact foreign = x;
  foreign.analysis_unit = "tu:B";
  facts.add_capture(foreign);
  facts.remove_contribution("tu:A");
  REQUIRE(facts.find_symbol(lambda));
  CHECK(facts.find_symbol(lambda)->presence == SymbolPresence::endpoint_only);
  CHECK(facts.captures().size() == 1);
  CHECK(facts.captures().front().analysis_unit == "tu:B");
  facts.remove_contribution("tu:B");
  CHECK(facts.captures().empty());
  CHECK(facts.symbols().empty());
  CHECK(to_string(CaptureKind::star_this) == "star_this");
}

TEST_CASE("unresolved sites are recorded as evidence with a reason, never as relations") {
  FactSet facts;
  const auto caller = facts.add_symbol_location(
      function_key("caller"), symbol_location(LocationRole::definition, "tu:x", location("src/x.cpp", 0, 60)));

  UnresolvedSite site;
  site.enclosing = caller;
  site.expression = "callback(value)";
  site.location = location("src/x.cpp", 30, 45);
  site.reason = UnresolvedReason::indirect_callee;
  site.analysis_unit = "tu:x";
  facts.add_unresolved_site(site);
  facts.add_unresolved_site(site);  // duplicate observation

  REQUIRE(facts.unresolved_sites().size() == 1);
  CHECK(facts.unresolved_sites().front().expression == "callback(value)");
  CHECK(facts.unresolved_sites().front().reason == UnresolvedReason::indirect_callee);
  CHECK(facts.relations().empty());

  const RelationKey bogus{caller, caller, RelationKind::calls, Confidence::unresolved};
  CHECK_THROWS_AS(facts.add_relation_evidence(bogus, evidence("tu:x", location("src/x.cpp", 30, 45))),
                  std::invalid_argument);

  facts.remove_contribution("tu:x");
  CHECK(facts.unresolved_sites().empty());
}

TEST_CASE("external placeholders live only as long as something references them") {
  FactSet facts;
  const auto user = facts.add_symbol_location(
      function_key("user"), symbol_location(LocationRole::definition, "tu:u", location("src/u.cpp", 0, 20)));
  auto external_key = function_key("fmt::format", "(...)");
  external_key.repository_member = "external/fmt";
  const auto external = facts.add_external_placeholder(external_key);
  CHECK(facts.find_symbol(external)->presence == SymbolPresence::external_placeholder);
  CHECK(facts.find_symbol(external)->locations.empty());

  facts.add_relation_evidence({user, external, RelationKind::calls, Confidence::confirmed},
                              evidence("tu:u", location("src/u.cpp", 5, 15)));
  facts.remove_contribution("tu:other");
  CHECK(facts.find_symbol(external) != nullptr);

  facts.remove_contribution("tu:u");
  CHECK(facts.find_symbol(external) == nullptr);
  CHECK(facts.symbols().empty());
}

TEST_CASE("fact hash is independent of insertion order and sensitive to content") {
  const auto build = [](bool reversed) {
    FactSet facts;
    std::vector<std::pair<std::string, SymbolLocation>> symbols = {
        {"alpha", symbol_location(LocationRole::definition, "tu:a", location("src/a.cpp", 0, 10))},
        {"beta", symbol_location(LocationRole::definition, "tu:b", location("src/b.cpp", 0, 10))},
        {"gamma", symbol_location(LocationRole::declaration, "tu:a", location("inc/g.h", 0, 10))},
    };
    if (reversed) std::reverse(symbols.begin(), symbols.end());
    std::map<std::string, StableId> ids;
    for (auto& [name, loc] : symbols) ids[name] = facts.add_symbol_location(function_key(name), loc);
    std::vector<std::pair<RelationKey, Evidence>> relations = {
        {{ids["alpha"], ids["beta"], RelationKind::calls, Confidence::confirmed},
         evidence("tu:a", location("src/a.cpp", 2, 5))},
        {{ids["alpha"], ids["beta"], RelationKind::calls, Confidence::confirmed},
         evidence("tu:c", location("src/a.cpp", 2, 5))},
        {{ids["beta"], ids["gamma"], RelationKind::references, Confidence::possible},
         evidence("tu:b", location("src/b.cpp", 2, 5))},
    };
    if (reversed) std::reverse(relations.begin(), relations.end());
    for (auto& [key, e] : relations) facts.add_relation_evidence(key, e);
    return facts;
  };

  const auto forward = build(false).fact_hash();
  const auto backward = build(true).fact_hash();
  CHECK(forward == backward);

  FactSet changed = build(false);
  changed.add_unresolved_site(UnresolvedSite{changed.symbols().begin()->first, "x()", location("src/a.cpp", 7, 9),
                                             UnresolvedReason::dependent_expression, "tu:a"});
  CHECK(changed.fact_hash() != forward);
}

TEST_CASE("direct-include facts live independently of symbols and follow contribution lifetime") {
  const auto make = [](const char* unit, const char* operand, IncludeActivity activity,
                       const char* target) {
    DirectIncludeFact f;
    f.includer = RepoRelativePath{"src/main.cpp", "src/main.cpp"};
    f.operand_as_written = operand;
    f.operand_kind = IncludeOperandKind::quoted;
    f.activity = activity;
    f.resolution = activity == IncludeActivity::active ? IncludeResolution::resolved_internal
                                                       : IncludeResolution::not_evaluated;
    if (activity == IncludeActivity::active) f.resolved_repo_target = RepoRelativePath{target, target};
    f.directive_location.file = f.includer;
    f.directive_location.span = SourceSpan{10, 30, 2, 1, 2, 21};
    f.analysis_unit = unit;
    return f;
  };

  FactSet facts;
  facts.add_direct_include(make("unit-a", "\"one.hpp\"", IncludeActivity::active, "one.hpp"));
  // The identical observation deduplicates.
  facts.add_direct_include(make("unit-a", "\"one.hpp\"", IncludeActivity::active, "one.hpp"));
  CHECK(facts.direct_includes().size() == 1);
  // A different outcome for the same directive is a distinct fact: repeated
  // visits under different macro states must not overwrite each other.
  facts.add_direct_include(make("unit-a", "\"one.hpp\"", IncludeActivity::inactive, ""));
  CHECK(facts.direct_includes().size() == 2);
  // No symbol, relation or placeholder is needed to hold them.
  CHECK(facts.symbols().empty());
  CHECK(facts.relations().empty());

  const Sha256Digest with_two = facts.fact_hash();
  facts.add_direct_include(make("unit-b", "\"one.hpp\"", IncludeActivity::active, "one.hpp"));
  CHECK(facts.direct_includes().size() == 3);
  CHECK(facts.fact_hash() != with_two);

  // Only the named contribution disappears.
  facts.remove_contribution("unit-b");
  CHECK(facts.direct_includes().size() == 2);
  CHECK(facts.fact_hash() == with_two);
  facts.remove_contribution("unit-a");
  CHECK(facts.direct_includes().empty());
  CHECK(facts.fact_hash() == FactSet{}.fact_hash());

  // Every distinguishing field takes part in the hash.
  const auto hash_of = [&](const DirectIncludeFact& f) {
    FactSet one;
    one.add_direct_include(f);
    return one.fact_hash();
  };
  const DirectIncludeFact base_fact = make("unit-a", "\"one.hpp\"", IncludeActivity::active, "one.hpp");
  CHECK(hash_of(base_fact) != FactSet{}.fact_hash());
  {
    DirectIncludeFact other = base_fact;
    other.directive_kind = IncludeDirectiveKind::include_next;
    CHECK(hash_of(other) != hash_of(base_fact));
  }
  {
    DirectIncludeFact other = base_fact;
    other.expanded_spelling = "one.hpp";
    CHECK(hash_of(other) != hash_of(base_fact));
  }
  {
    DirectIncludeFact other = base_fact;
    other.external_dependency_key = "abc";
    CHECK(hash_of(other) != hash_of(base_fact));
  }
  {
    DirectIncludeFact other = base_fact;
    ConditionalBranch branch;
    ConditionTerm term;
    term.kind = ConditionTermKind::if_;
    term.expression_as_written = "FLAG";
    branch.own_condition = term;
    other.condition_path.push_back(branch);
    CHECK(hash_of(other) != hash_of(base_fact));
    // A preceding sibling is part of the identity of the branch path too.
    DirectIncludeFact with_sibling = other;
    with_sibling.condition_path[0].preceding_branches.push_back(term);
    CHECK(hash_of(with_sibling) != hash_of(other));
  }
}

TEST_CASE("a direct include without an analysis unit is refused before anything is stored") {
  DirectIncludeFact owned;
  owned.includer = RepoRelativePath{"src/main.cpp", "src/main.cpp"};
  owned.operand_as_written = "\"one.hpp\"";
  owned.operand_kind = IncludeOperandKind::quoted;
  owned.activity = IncludeActivity::active;
  owned.resolution = IncludeResolution::resolved_internal;
  owned.resolved_repo_target = RepoRelativePath{"one.hpp", "one.hpp"};
  owned.directive_location.file = owned.includer;
  owned.directive_location.span = SourceSpan{10, 30, 2, 1, 2, 21};
  owned.analysis_unit = "unit-a";

  FactSet facts;
  facts.add_direct_include(owned);
  const Sha256Digest before = facts.fact_hash();

  // No contribution owns it, so it could never be removed again: refused like
  // every other insertion API, and nothing is mutated.
  DirectIncludeFact unowned = owned;
  unowned.analysis_unit.clear();
  CHECK_THROWS_AS(facts.add_direct_include(unowned), std::invalid_argument);
  CHECK(facts.direct_includes().size() == 1);
  CHECK(facts.fact_hash() == before);

  // A rejection on an empty set leaves it empty, not merely unchanged in size.
  FactSet fresh;
  CHECK_THROWS_AS(fresh.add_direct_include(unowned), std::invalid_argument);
  CHECK(fresh.direct_includes().empty());
  CHECK(fresh.fact_hash() == FactSet{}.fact_hash());

  // A fact that differs only by its missing unit is refused, so the rejected
  // one never becomes an unremovable sibling of the owned one.
  facts.remove_contribution("unit-a");
  CHECK(facts.direct_includes().empty());
  CHECK(facts.fact_hash() == FactSet{}.fact_hash());
}

TEST_CASE("appended template-use values are distinct and leave the existing ones untouched") {
  CHECK(to_string(TemplateUse::none) == "none");
  CHECK(to_string(TemplateUse::primary_implicit) == "primary_implicit");
  CHECK(to_string(TemplateUse::explicit_specialization) == "explicit_specialization");
  CHECK(to_string(TemplateUse::partial_implicit) == "partial_implicit");
  CHECK(to_string(TemplateUse::explicit_instantiation_declaration) == "explicit_instantiation_declaration");
  CHECK(to_string(TemplateUse::explicit_instantiation_definition) == "explicit_instantiation_definition");

  // Appended, so the established values keep their order: existing evidence
  // never reorders because a new use was added.
  CHECK(TemplateUse::none < TemplateUse::primary_implicit);
  CHECK(TemplateUse::primary_implicit < TemplateUse::explicit_specialization);
  CHECK(TemplateUse::explicit_specialization < TemplateUse::partial_implicit);
  CHECK(TemplateUse::partial_implicit < TemplateUse::explicit_instantiation_declaration);
  CHECK(TemplateUse::explicit_instantiation_declaration < TemplateUse::explicit_instantiation_definition);

  CanonicalKey derived_key;
  derived_key.repository_member = "repo";
  derived_key.kind = SymbolKind::struct_;
  derived_key.canonical_name = "Derived";
  CanonicalKey base_key = derived_key;
  base_key.canonical_name = "Pattern";

  SymbolLocation loc;
  loc.role = LocationRole::definition;
  loc.location.file = RepoRelativePath{"a.h", "a.h"};
  loc.analysis_unit = "unit-a";

  const auto hash_for = [&](TemplateUse use, const std::string& arguments) {
    FactSet facts;
    const StableId derived = facts.add_symbol_location(derived_key, loc);
    const StableId base = facts.add_symbol_location(base_key, loc);
    Evidence evidence;
    evidence.analysis_unit = "unit-a";
    evidence.location.file = RepoRelativePath{"a.h", "a.h"};
    evidence.location.span = SourceSpan{5, 11, 1, 6, 1, 12};
    evidence.template_use = use;
    evidence.template_arguments = arguments;
    facts.add_relation_evidence(RelationKey{derived, base, RelationKind::extends, Confidence::confirmed}, evidence);
    return facts.fact_hash();
  };

  // Every use value, and the arguments beside it, take part in the hash.
  const std::vector<TemplateUse> uses = {
      TemplateUse::none,           TemplateUse::primary_implicit,
      TemplateUse::explicit_specialization, TemplateUse::partial_implicit,
      TemplateUse::explicit_instantiation_declaration, TemplateUse::explicit_instantiation_definition};
  std::vector<std::string> hashes;
  for (TemplateUse use : uses) hashes.push_back(hash_for(use, "<int>").hex());
  std::sort(hashes.begin(), hashes.end());
  CHECK(std::unique(hashes.begin(), hashes.end()) == hashes.end());
  CHECK(hash_for(TemplateUse::partial_implicit, "<int>") != hash_for(TemplateUse::partial_implicit, "<long>"));
}

TEST_CASE("base-edge details take part in evidence equality, order and the fact hash") {
  CanonicalKey derived_key;
  derived_key.repository_member = "repo";
  derived_key.kind = SymbolKind::struct_;
  derived_key.canonical_name = "Derived";
  CanonicalKey base_key = derived_key;
  base_key.canonical_name = "Base";

  FactSet facts;
  SymbolLocation loc;
  loc.role = LocationRole::definition;
  loc.location.file = RepoRelativePath{"a.h", "a.h"};
  loc.analysis_unit = "unit-a";
  const StableId derived = facts.add_symbol_location(derived_key, loc);
  const StableId base = facts.add_symbol_location(base_key, loc);

  const RelationKey key{derived, base, RelationKind::extends, Confidence::confirmed};
  Evidence evidence;
  evidence.analysis_unit = "unit-a";
  evidence.location.file = RepoRelativePath{"a.h", "a.h"};
  evidence.location.span = SourceSpan{5, 6, 1, 6, 1, 7};
  BaseEdgeDetails details;
  details.effective_access = BaseAccess::private_;
  details.access_written = false;
  details.is_virtual = false;
  details.lexical_ordinal = 0;
  evidence.base = details;
  facts.add_relation_evidence(key, evidence);
  const Sha256Digest baseline = facts.fact_hash();

  // Same span and unit, one differing detail: a distinct piece of evidence.
  const auto with = [&](auto mutate) {
    FactSet copy;
    copy.add_symbol_location(derived_key, loc);
    copy.add_symbol_location(base_key, loc);
    Evidence changed = evidence;
    mutate(*changed.base);
    copy.add_relation_evidence(key, changed);
    return copy.fact_hash();
  };
  CHECK(with([](BaseEdgeDetails& d) { d.effective_access = BaseAccess::protected_; }) != baseline);
  CHECK(with([](BaseEdgeDetails& d) { d.access_written = true; }) != baseline);
  CHECK(with([](BaseEdgeDetails& d) { d.is_virtual = true; }) != baseline);
  CHECK(with([](BaseEdgeDetails& d) { d.lexical_ordinal = 1; }) != baseline);

  // Evidence differing only in base details is kept, not merged away.
  Evidence other = evidence;
  other.base->lexical_ordinal = 1;
  facts.add_relation_evidence(key, other);
  const Relation* rel = facts.find_relation(key);
  REQUIRE(rel);
  CHECK(rel->evidence.size() == 2);
  // Absent details differ from any present details.
  Evidence none = evidence;
  none.base.reset();
  facts.add_relation_evidence(key, none);
  CHECK(facts.find_relation(key)->evidence.size() == 3);
}
