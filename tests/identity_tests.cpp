#include <doctest/doctest.h>

#include <stdexcept>

#include "lcm/identity.hpp"

using lcm::CanonicalKey;
using lcm::ExternalLinkage;
using lcm::InternalLinkage;
using lcm::LocalScope;
using lcm::OwnerComponent;
using lcm::RepoRelativePath;
using lcm::SymbolKind;

namespace {

CanonicalKey free_function(std::string name, std::string signature, std::vector<OwnerComponent> owners = {}) {
  CanonicalKey key;
  key.repository_member = "repo";
  key.kind = SymbolKind::function;
  key.owner_chain = std::move(owners);
  key.canonical_name = std::move(name);
  key.normalized_signature = std::move(signature);
  return key;
}

CanonicalKey method(std::string owner, std::string name, std::string signature) {
  CanonicalKey key = free_function(std::move(name), std::move(signature), {{SymbolKind::class_, owner, ""}});
  key.kind = SymbolKind::method;
  return key;
}

}  // namespace

TEST_CASE("stable id format is cm1 plus 32 lowercase hex characters") {
  const auto id = lcm::make_stable_id(free_function("f", "()"));
  CHECK(id.value.size() == 36);
  CHECK(id.value.substr(0, 4) == "cm1:");
  CHECK(lcm::is_well_formed_stable_id(id.value));
  CHECK_FALSE(lcm::is_well_formed_stable_id("cm1:XYZ"));
  CHECK_FALSE(lcm::is_well_formed_stable_id("cm2:" + id.value.substr(4)));
  CHECK_FALSE(lcm::is_well_formed_stable_id(id.value + "0"));
}

TEST_CASE("golden id pins the serialization format (computed independently in Python)") {
  const CanonicalKey key = free_function("area", "(double,double)", {{SymbolKind::namespace_, "geo", ""}});
  CHECK(lcm::serialize_canonical_key(key) ==
        "cm1|member=4:repo;lang=3:cpp;kind=8:function;owners=1:1;okind=9:namespace;oname=3:geo;osig=0:;"
        "name=4:area;sig=15:(double,double);linkage=8:external;");
  CHECK(lcm::make_stable_id(key).value == "cm1:6a0c8ba09dac947e8c869ab49174a9ee");
}

TEST_CASE("external linkage ids ignore file, line and body: only the canonical key matters") {
  // Two observations of the same external function from different files,
  // lines and bodies are represented by the same key, hence the same id.
  const CanonicalKey declared_in_header = method("Widget", "Draw", "() const");
  const CanonicalKey defined_in_cpp = method("Widget", "Draw", "() const");
  CHECK(declared_in_header == defined_in_cpp);
  CHECK(lcm::make_stable_id(declared_in_header) == lcm::make_stable_id(defined_in_cpp));

  // The key type has no place for a location or body hash; moving the
  // definition to another .cpp or editing its body cannot change the key.
  const auto serialized = lcm::serialize_canonical_key(defined_in_cpp);
  CHECK(serialized.find("Widget.cpp") == std::string::npos);
  CHECK(serialized.find("line") == std::string::npos);
}

TEST_CASE("name, owner and signature changes create new ids") {
  const auto base = lcm::make_stable_id(method("Widget", "Draw", "(int)"));
  CHECK(lcm::make_stable_id(method("Widget", "Paint", "(int)")) != base);
  CHECK(lcm::make_stable_id(method("Button", "Draw", "(int)")) != base);
  CHECK(lcm::make_stable_id(method("Widget", "Draw", "(double)")) != base);
  CHECK(lcm::make_stable_id(method("Widget", "Draw", "(int) const")) != base);
}

TEST_CASE("overloads are distinguished by normalized signature") {
  const auto by_int = lcm::make_stable_id(free_function("print", "(int)"));
  const auto by_double = lcm::make_stable_id(free_function("print", "(double)"));
  const auto by_ref = lcm::make_stable_id(free_function("print", "(const std::string&)"));
  CHECK(by_int != by_double);
  CHECK(by_int != by_ref);
  CHECK(by_double != by_ref);
  CHECK(lcm::make_stable_id(free_function("print", "(int)")) == by_int);
}

TEST_CASE("owner kind matters: a::f in namespace a differs from A::f in class A with the same spelling") {
  const auto in_namespace = lcm::make_stable_id(free_function("f", "()", {{SymbolKind::namespace_, "x", ""}}));
  auto class_key = free_function("f", "()", {{SymbolKind::class_, "x", ""}});
  class_key.kind = SymbolKind::method;
  const auto in_class = lcm::make_stable_id(class_key);
  CHECK(in_namespace != in_class);

  // Same name, different symbol kind at the same scope.
  auto struct_key = free_function("f", "");
  struct_key.kind = SymbolKind::struct_;
  CHECK(lcm::make_stable_id(struct_key) != lcm::make_stable_id(free_function("f", "()")));
}

TEST_CASE("internal linkage is scoped to the repository-relative file") {
  auto helper_a = free_function("helper", "()");
  helper_a.linkage = InternalLinkage{RepoRelativePath{"src/a.cpp"}};
  auto helper_b = free_function("helper", "()");
  helper_b.linkage = InternalLinkage{RepoRelativePath{"src/b.cpp"}};
  auto helper_a_again = helper_a;

  CHECK(lcm::make_stable_id(helper_a) != lcm::make_stable_id(helper_b));
  CHECK(lcm::make_stable_id(helper_a) == lcm::make_stable_id(helper_a_again));

  // Same spelling with external linkage is yet another symbol.
  CHECK(lcm::make_stable_id(helper_a) != lcm::make_stable_id(free_function("helper", "()")));
}

TEST_CASE("internal symbol ids survive repository relocation because the scope is repository-relative") {
  const auto old_root = lcm::path_from_utf8("C:/work/old-checkout");
  const auto new_root = lcm::path_from_utf8("D:/Git/moved 위치/checkout");
  const auto before = lcm::make_repo_relative(old_root, lcm::path_from_utf8("C:/work/old-checkout/src/a.cpp"));
  const auto after = lcm::make_repo_relative(new_root, lcm::path_from_utf8("D:/Git/moved 위치/checkout/src/a.cpp"));
  REQUIRE(before);
  REQUIRE(after);
  CHECK(*before == *after);

  auto helper_before = free_function("helper", "()");
  helper_before.linkage = InternalLinkage{*before};
  auto helper_after = free_function("helper", "()");
  helper_after.linkage = InternalLinkage{*after};
  CHECK(lcm::make_stable_id(helper_before) == lcm::make_stable_id(helper_after));
}

TEST_CASE("anonymous namespace members carry the file scope through the linkage discriminator") {
  auto in_a = free_function("detail_fn", "()", {{SymbolKind::namespace_, "", ""}});
  in_a.linkage = InternalLinkage{RepoRelativePath{"src/a.cpp"}};
  auto in_b = in_a;
  in_b.linkage = InternalLinkage{RepoRelativePath{"src/b.cpp"}};
  CHECK(lcm::make_stable_id(in_a) != lcm::make_stable_id(in_b));
}

TEST_CASE("lambdas are scoped to the enclosing symbol id, anchor and ordinal") {
  const CanonicalKey enclosing = free_function("run", "()");
  const auto enclosing_id = lcm::make_stable_id(enclosing);

  CanonicalKey lambda0;
  lambda0.repository_member = "repo";
  lambda0.kind = SymbolKind::lambda;
  lambda0.normalized_signature = "(int) const";
  lambda0.owner_chain = {{SymbolKind::function, "run", "()"}};
  lambda0.linkage = LocalScope{enclosing_id, "lambda", 0};
  CanonicalKey lambda1 = lambda0;
  lambda1.linkage = LocalScope{enclosing_id, "lambda", 1};

  CHECK(lcm::make_stable_id(lambda0) != lcm::make_stable_id(lambda1));
  CHECK(lcm::make_stable_id(lambda0) == lcm::make_stable_id(CanonicalKey{lambda0}));

  SUBCASE("identical lambdas under same-named static helpers in different files do not collide") {
    auto helper_a = free_function("helper", "()");
    helper_a.linkage = InternalLinkage{RepoRelativePath{"src/a.cpp"}};
    auto helper_b = free_function("helper", "()");
    helper_b.linkage = InternalLinkage{RepoRelativePath{"src/b.cpp"}};

    CanonicalKey lambda_in_a = lambda0;
    lambda_in_a.owner_chain = {{SymbolKind::function, "helper", "()"}};
    lambda_in_a.linkage = LocalScope{lcm::make_stable_id(helper_a), "lambda", 0};
    CanonicalKey lambda_in_b = lambda_in_a;
    lambda_in_b.linkage = LocalScope{lcm::make_stable_id(helper_b), "lambda", 0};

    CHECK(lcm::make_stable_id(lambda_in_a) != lcm::make_stable_id(lambda_in_b));
  }
}

TEST_CASE("repository member participates in identity") {
  auto in_main = free_function("f", "()");
  auto in_plugin = in_main;
  in_plugin.repository_member = "plugins/audio";
  CHECK(lcm::make_stable_id(in_main) != lcm::make_stable_id(in_plugin));
}

TEST_CASE("keys that would leak absolute paths or lack required parts are rejected") {
  auto abs_scope = free_function("helper", "()");
  abs_scope.linkage = InternalLinkage{RepoRelativePath{"D:/Git/LiveCodeMap/src/a.cpp"}};
  CHECK(lcm::validate_canonical_key(abs_scope).has_value());
  CHECK_THROWS_AS((void)lcm::make_stable_id(abs_scope), std::invalid_argument);

  auto abs_member = free_function("f", "()");
  abs_member.repository_member = "C:/repo";
  CHECK_THROWS_AS((void)lcm::make_stable_id(abs_member), std::invalid_argument);

  auto unnamed = free_function("", "()");
  CHECK_THROWS_AS((void)lcm::make_stable_id(unnamed), std::invalid_argument);

  auto empty_scope = free_function("helper", "()");
  empty_scope.linkage = InternalLinkage{RepoRelativePath{""}};
  CHECK_THROWS_AS((void)lcm::make_stable_id(empty_scope), std::invalid_argument);

  CanonicalKey orphan_lambda;
  orphan_lambda.repository_member = "repo";
  orphan_lambda.kind = SymbolKind::lambda;
  orphan_lambda.normalized_signature = "()";
  orphan_lambda.linkage = LocalScope{lcm::StableId{"cm1:0123456789abcdef0123456789abcdef"}, "lambda", 0};
  CHECK_THROWS_AS((void)lcm::make_stable_id(orphan_lambda), std::invalid_argument);  // no enclosing owner

  CanonicalKey bad_enclosing;
  bad_enclosing.repository_member = "repo";
  bad_enclosing.kind = SymbolKind::lambda;
  bad_enclosing.normalized_signature = "()";
  bad_enclosing.owner_chain = {{SymbolKind::function, "run", "()"}};
  bad_enclosing.linkage = LocalScope{lcm::StableId{"not-an-id"}, "lambda", 0};
  CHECK_THROWS_AS((void)lcm::make_stable_id(bad_enclosing), std::invalid_argument);
}

TEST_CASE("internal file scopes must be canonical: dotted or aliased spellings are rejected, not hashed") {
  auto canonical = free_function("helper", "()");
  canonical.linkage = InternalLinkage{RepoRelativePath{"src/a.cpp"}};
  CHECK_FALSE(lcm::validate_canonical_key(canonical));

  for (const char* bad : {"src/x/../a.cpp", "./src/a.cpp", "src//a.cpp", "src\\a.cpp", "src/a.cpp/", "/src/a.cpp",
                          "C:/repo/src/a.cpp", "src/./a.cpp", ".."}) {
    auto key = canonical;
    key.linkage = InternalLinkage{RepoRelativePath{bad}};
    CAPTURE(bad);
    CHECK(lcm::validate_canonical_key(key).has_value());
    CHECK_THROWS_AS((void)lcm::make_stable_id(key), std::invalid_argument);
  }

  // The canonical producer normalises the dotted spelling to the same identity
  // and keeps the observed spelling for display only.
  const auto root = lcm::path_from_utf8("C:/repo");
  const auto plain = lcm::make_repo_relative(root, lcm::path_from_utf8("C:/repo/src/a.cpp"));
  const auto dotted = lcm::make_repo_relative(root, lcm::path_from_utf8("C:/repo/src/x/../a.cpp"));
  REQUIRE(plain);
  REQUIRE(dotted);
  CHECK(plain->generic == "src/a.cpp");
  CHECK(dotted->generic == "src/a.cpp");
  CHECK(dotted->as_written == "src/x/../a.cpp");
  CHECK(*plain == *dotted);
  auto via_plain = canonical;
  via_plain.linkage = InternalLinkage{*plain};
  auto via_dotted = canonical;
  via_dotted.linkage = InternalLinkage{*dotted};
  CHECK(lcm::make_stable_id(via_plain) == lcm::make_stable_id(via_dotted));
}

TEST_CASE("kind, signature and linkage combinations are validated") {
  // Callables need a signature; non-callables must not carry one.
  CHECK(lcm::validate_canonical_key(free_function("overloaded", "")).has_value());
  for (SymbolKind kind : {SymbolKind::method, SymbolKind::constructor, SymbolKind::destructor,
                          SymbolKind::conversion_function, SymbolKind::operator_function}) {
    auto key = free_function("Widget", "", {{SymbolKind::class_, "Widget", ""}});
    key.kind = kind;
    CAPTURE(lcm::to_string(kind));
    CHECK(lcm::validate_canonical_key(key).has_value());
    key.normalized_signature = "()";
    CHECK_FALSE(lcm::validate_canonical_key(key));
  }
  auto variable = free_function("counter", "(int)");
  variable.kind = SymbolKind::variable;
  CHECK(lcm::validate_canonical_key(variable).has_value());
  variable.normalized_signature.clear();
  CHECK_FALSE(lcm::validate_canonical_key(variable));

  // Callable owners need their signature; non-callable owners must not have one.
  CHECK(lcm::validate_canonical_key(free_function("f", "()", {{SymbolKind::function, "run", ""}})).has_value());
  CHECK(lcm::validate_canonical_key(free_function("f", "()", {{SymbolKind::class_, "C", "()"}})).has_value());
  CHECK(lcm::validate_canonical_key(free_function("f", "()", {{SymbolKind::variable, "v", ""}})).has_value());
  CHECK(lcm::validate_canonical_key(free_function("f", "()", {{SymbolKind::class_, "", ""}})).has_value());

  // Local-only kinds require LocalScope; other kinds may not use it.
  CanonicalKey external_lambda;
  external_lambda.repository_member = "repo";
  external_lambda.kind = SymbolKind::lambda;
  external_lambda.normalized_signature = "()";
  external_lambda.owner_chain = {{SymbolKind::function, "run", "()"}};
  external_lambda.linkage = ExternalLinkage{};
  CHECK(lcm::validate_canonical_key(external_lambda).has_value());
  external_lambda.linkage = InternalLinkage{RepoRelativePath{"src/a.cpp"}};
  CHECK(lcm::validate_canonical_key(external_lambda).has_value());
  external_lambda.linkage = LocalScope{lcm::make_stable_id(free_function("run", "()")), "lambda", 0};
  CHECK_FALSE(lcm::validate_canonical_key(external_lambda));

  CanonicalKey local_namespace;
  local_namespace.repository_member = "repo";
  local_namespace.kind = SymbolKind::namespace_;
  local_namespace.canonical_name = "inner";
  local_namespace.owner_chain = {{SymbolKind::function, "run", "()"}};
  local_namespace.linkage = LocalScope{lcm::make_stable_id(free_function("run", "()")), "inner", 0};
  CHECK(lcm::validate_canonical_key(local_namespace).has_value());

  CanonicalKey local_variable;
  local_variable.repository_member = "repo";
  local_variable.kind = SymbolKind::local_variable;
  local_variable.canonical_name = "count";
  local_variable.owner_chain = {{SymbolKind::function, "run", "()"}};
  local_variable.linkage = LocalScope{lcm::make_stable_id(free_function("run", "()")), "count", 0};
  CHECK_FALSE(lcm::validate_canonical_key(local_variable));
}

TEST_CASE("anonymous namespaces are representable only as file-scoped nodes") {
  CanonicalKey anonymous_namespace;
  anonymous_namespace.repository_member = "repo";
  anonymous_namespace.kind = SymbolKind::namespace_;
  anonymous_namespace.linkage = InternalLinkage{RepoRelativePath{"src/a.cpp"}};
  CHECK_FALSE(lcm::validate_canonical_key(anonymous_namespace));
  const auto in_a = lcm::make_stable_id(anonymous_namespace);
  anonymous_namespace.linkage = InternalLinkage{RepoRelativePath{"src/b.cpp"}};
  CHECK(lcm::make_stable_id(anonymous_namespace) != in_a);

  // Unnamed namespace without a file scope, or a named namespace with one, is rejected.
  anonymous_namespace.linkage = ExternalLinkage{};
  CHECK(lcm::validate_canonical_key(anonymous_namespace).has_value());
  CanonicalKey named_namespace = anonymous_namespace;
  named_namespace.canonical_name = "geo";
  CHECK_FALSE(lcm::validate_canonical_key(named_namespace));
  named_namespace.linkage = InternalLinkage{RepoRelativePath{"src/a.cpp"}};
  CHECK(lcm::validate_canonical_key(named_namespace).has_value());

  // A member of an anonymous namespace must be file-scoped; with external linkage it is rejected.
  auto member = free_function("detail_fn", "()", {{SymbolKind::namespace_, "", ""}});
  CHECK(lcm::validate_canonical_key(member).has_value());
  member.linkage = InternalLinkage{RepoRelativePath{"src/a.cpp"}};
  CHECK_FALSE(lcm::validate_canonical_key(member));

  SUBCASE("namespace { namespace named { void f(); } } is representable as file-scoped nodes") {
    CanonicalKey nested = named_namespace;  // kind namespace_, name "geo"
    nested.canonical_name = "named";
    nested.owner_chain = {{SymbolKind::namespace_, "", ""}};
    nested.linkage = InternalLinkage{RepoRelativePath{"src/a.cpp"}};
    CHECK_FALSE(lcm::validate_canonical_key(nested));
    nested.linkage = ExternalLinkage{};
    CHECK(lcm::validate_canonical_key(nested).has_value());  // an anonymous ancestor forbids external linkage

    auto f = free_function("f", "()", {{SymbolKind::namespace_, "", ""}, {SymbolKind::namespace_, "named", ""}});
    f.linkage = InternalLinkage{RepoRelativePath{"src/a.cpp"}};
    CHECK_FALSE(lcm::validate_canonical_key(f));
    auto f_in_b = f;
    f_in_b.linkage = InternalLinkage{RepoRelativePath{"src/b.cpp"}};
    CHECK(lcm::make_stable_id(f) != lcm::make_stable_id(f_in_b));
  }
}

TEST_CASE("unnamed types at namespace scope are anchored to their declarator; their members to the type") {
  const auto anchor = lcm::make_stable_id([] {
    auto v = free_function("anonymous_one", "");
    v.kind = SymbolKind::variable;
    return v;
  }());
  CanonicalKey anon;
  anon.repository_member = "repo";
  anon.kind = SymbolKind::anonymous_type;
  anon.linkage = LocalScope{anchor, "anon-type", 0};  // no semantic owner: global namespace
  CHECK_FALSE(lcm::validate_canonical_key(anon));
  CanonicalKey other = anon;
  other.linkage = LocalScope{lcm::make_stable_id([] {
                               auto v = free_function("anonymous_two", "");
                               v.kind = SymbolKind::variable;
                               return v;
                             }()),
                             "anon-type", 0};
  CHECK(lcm::make_stable_id(anon) != lcm::make_stable_id(other));

  // Members of two unnamed types with the same field spelling stay distinct.
  CanonicalKey field_a;
  field_a.repository_member = "repo";
  field_a.kind = SymbolKind::field;
  field_a.canonical_name = "value";
  field_a.owner_chain = {{SymbolKind::anonymous_type, "", ""}};
  field_a.linkage = LocalScope{lcm::make_stable_id(anon), "value", 0};
  CanonicalKey field_b = field_a;
  field_b.linkage = LocalScope{lcm::make_stable_id(other), "value", 0};
  CHECK_FALSE(lcm::validate_canonical_key(field_a));
  CHECK(lcm::make_stable_id(field_a) != lcm::make_stable_id(field_b));

  // Other kinds still need an owner under LocalScope.
  CanonicalKey stray;
  stray.repository_member = "repo";
  stray.kind = SymbolKind::local_variable;
  stray.canonical_name = "x";
  stray.linkage = LocalScope{anchor, "x", 0};
  CHECK(lcm::validate_canonical_key(stray).has_value());
}

TEST_CASE("named local classes and their members use LocalScope so same-named locals in different blocks differ") {
  const auto run_id = lcm::make_stable_id(free_function("run", "()"));

  CanonicalKey local_struct;
  local_struct.repository_member = "repo";
  local_struct.kind = SymbolKind::struct_;
  local_struct.canonical_name = "Local";
  local_struct.owner_chain = {{SymbolKind::function, "run", "()"}};
  local_struct.linkage = LocalScope{run_id, "Local", 0};
  CHECK_FALSE(lcm::validate_canonical_key(local_struct));
  const auto first_block = lcm::make_stable_id(local_struct);

  CanonicalKey second = local_struct;
  second.linkage = LocalScope{run_id, "Local", 1};
  CHECK(lcm::make_stable_id(second) != first_block);

  // A method of the local struct is scoped to the local struct's own id.
  CanonicalKey method;
  method.repository_member = "repo";
  method.kind = SymbolKind::method;
  method.canonical_name = "value";
  method.normalized_signature = "() const";
  method.owner_chain = {{SymbolKind::function, "run", "()"}, {SymbolKind::struct_, "Local", ""}};
  method.linkage = LocalScope{first_block, "value", 0};
  CHECK_FALSE(lcm::validate_canonical_key(method));
  CanonicalKey method_in_second = method;
  method_in_second.linkage = LocalScope{lcm::make_stable_id(second), "value", 0};
  CHECK(lcm::make_stable_id(method) != lcm::make_stable_id(method_in_second));

  // Local enum and alias are representable the same way.
  CanonicalKey local_enum = local_struct;
  local_enum.kind = SymbolKind::enum_;
  local_enum.canonical_name = "Mode";
  local_enum.linkage = LocalScope{run_id, "Mode", 0};
  CHECK_FALSE(lcm::validate_canonical_key(local_enum));
  CanonicalKey local_alias = local_struct;
  local_alias.kind = SymbolKind::type_alias;
  local_alias.canonical_name = "T";
  local_alias.linkage = LocalScope{run_id, "T", 0};
  CHECK_FALSE(lcm::validate_canonical_key(local_alias));
}

TEST_CASE("namespace-scope declarator lambdas: the lambda-decl-init anchor is the only empty-owner lambda form") {
  CanonicalKey g1;
  g1.repository_member = "repo";
  g1.kind = SymbolKind::variable;
  g1.canonical_name = "g1";
  const auto g1_id = lcm::make_stable_id(g1);

  CanonicalKey closure;
  closure.repository_member = "repo";
  closure.kind = SymbolKind::lambda;
  closure.normalized_signature = "() const";
  closure.linkage = LocalScope{g1_id, std::string(lcm::kLambdaDeclInitAnchor), 0};
  CHECK_FALSE(lcm::validate_canonical_key(closure));  // empty owner chain accepted under this anchor only
  const auto closure_id = lcm::make_stable_id(closure);
  CanonicalKey second = closure;
  second.linkage = LocalScope{g1_id, std::string(lcm::kLambdaDeclInitAnchor), 1};  // two closures in one initializer
  CHECK(lcm::make_stable_id(second) != closure_id);

  CanonicalKey plain_anchor = closure;
  plain_anchor.linkage = LocalScope{g1_id, "lambda", 0};
  CHECK(lcm::validate_canonical_key(plain_anchor).has_value());  // orphan negative preserved
  CanonicalKey bad_id = closure;
  bad_id.linkage = LocalScope{lcm::StableId{"not-an-id"}, std::string(lcm::kLambdaDeclInitAnchor), 0};
  CHECK(lcm::validate_canonical_key(bad_id).has_value());
  CanonicalKey not_a_lambda = closure;
  not_a_lambda.kind = SymbolKind::struct_;
  not_a_lambda.canonical_name = "Local";
  not_a_lambda.normalized_signature.clear();
  CHECK(lcm::validate_canonical_key(not_a_lambda).has_value());  // the exception is for lambdas only

  // Closure members always carry the lambda owner component; the anchor names the member kind and the
  // destructor keeps its established anchor. An empty owner chain is rejected for them.
  CanonicalKey dtor;
  dtor.repository_member = "repo";
  dtor.kind = SymbolKind::destructor;
  dtor.canonical_name = "~";
  dtor.normalized_signature = "()";
  dtor.owner_chain = {{SymbolKind::lambda, "", "() const"}};
  dtor.linkage = LocalScope{closure_id, "closure-dtor", 0};
  CHECK_FALSE(lcm::validate_canonical_key(dtor));
  CanonicalKey ctor = dtor;
  ctor.kind = SymbolKind::constructor;
  ctor.canonical_name = "(ctor)";
  ctor.normalized_signature = "(const [lambda " + closure_id.value + "] &)";
  ctor.linkage = LocalScope{closure_id, "closure-ctor", 0};
  CHECK_FALSE(lcm::validate_canonical_key(ctor));
  CanonicalKey move_ctor = ctor;
  move_ctor.normalized_signature = "(rref([lambda " + closure_id.value + "]))";
  CHECK(lcm::make_stable_id(ctor) != lcm::make_stable_id(move_ctor));
  CHECK(lcm::make_stable_id(ctor) != lcm::make_stable_id(dtor));
  CanonicalKey orphan_member = dtor;
  orphan_member.owner_chain.clear();
  CHECK(lcm::validate_canonical_key(orphan_member).has_value());
}

TEST_CASE("template primary, explicit specialization and non-template with equal parameter lists are distinct") {
  const CanonicalKey plain = free_function("token", "()");
  CanonicalKey primary = plain;
  primary.template_role = lcm::TemplateRole::primary;
  primary.template_parameters = "<typename>";
  CanonicalKey spec_int = plain;
  spec_int.template_role = lcm::TemplateRole::explicit_specialization;
  spec_int.template_arguments = "<int>";
  CanonicalKey spec_double = spec_int;
  spec_double.template_arguments = "<double>";

  CHECK_FALSE(lcm::validate_canonical_key(primary));
  CHECK_FALSE(lcm::validate_canonical_key(spec_int));
  const auto ids = std::vector<lcm::StableId>{lcm::make_stable_id(plain), lcm::make_stable_id(primary),
                                              lcm::make_stable_id(spec_int), lcm::make_stable_id(spec_double)};
  for (std::size_t i = 0; i < ids.size(); ++i) {
    for (std::size_t j = i + 1; j < ids.size(); ++j) CHECK(ids[i] != ids[j]);
  }
  // Non-template serialization is unchanged by the extension (published IDs stay stable).
  CHECK(lcm::serialize_canonical_key(plain).find("trole") == std::string::npos);

  // Arguments and role must agree.
  CanonicalKey args_without_role = plain;
  args_without_role.template_arguments = "<int>";
  CHECK(lcm::validate_canonical_key(args_without_role).has_value());
  CanonicalKey spec_without_args = plain;
  spec_without_args.template_role = lcm::TemplateRole::explicit_specialization;
  CHECK(lcm::validate_canonical_key(spec_without_args).has_value());
  CanonicalKey primary_with_args = primary;
  primary_with_args.template_arguments = "<T>";
  CHECK(lcm::validate_canonical_key(primary_with_args).has_value());
  CanonicalKey primary_without_params = primary;
  primary_without_params.template_parameters.clear();
  CHECK(lcm::validate_canonical_key(primary_without_params).has_value());
  CanonicalKey params_without_role = plain;
  params_without_role.template_parameters = "<typename>";
  CHECK(lcm::validate_canonical_key(params_without_role).has_value());

  // template<class T> f() and template<int N> f() are different primaries; renaming T is not.
  CanonicalKey value_primary = primary;
  value_primary.template_parameters = "<int>";
  CHECK(lcm::make_stable_id(primary) != lcm::make_stable_id(value_primary));
  CanonicalKey renamed = primary;  // parameter names never enter the key, only kinds
  CHECK(lcm::make_stable_id(renamed) == lcm::make_stable_id(primary));

  // A member of a class template primary differs from a member of a same-named class.
  auto in_class = free_function("get", "() const", {{SymbolKind::struct_, "Box", ""}});
  in_class.kind = SymbolKind::method;
  auto in_template = in_class;
  in_template.owner_chain.front().template_role = lcm::TemplateRole::primary;
  in_template.owner_chain.front().template_parameters = "<typename>";
  auto in_value_template = in_template;
  in_value_template.owner_chain.front().template_parameters = "<int>";
  CHECK(lcm::make_stable_id(in_template) != lcm::make_stable_id(in_value_template));
  auto in_specialization = in_class;
  in_specialization.owner_chain.front().template_role = lcm::TemplateRole::explicit_specialization;
  in_specialization.owner_chain.front().template_arguments = "<int>";
  CHECK(lcm::make_stable_id(in_class) != lcm::make_stable_id(in_template));
  CHECK(lcm::make_stable_id(in_template) != lcm::make_stable_id(in_specialization));
  auto bad_owner = in_class;
  bad_owner.owner_chain.front().template_arguments = "<int>";  // arguments without a specialization role
  CHECK(lcm::validate_canonical_key(bad_owner).has_value());
}

TEST_CASE("serialization is unambiguous for values containing separators") {
  // Names containing the separator characters must not collide with a
  // differently-structured key.
  auto a = free_function("f;sig=1:x", "()");
  auto b = free_function("f", ";sig=1:x()");
  CHECK(lcm::make_stable_id(a) != lcm::make_stable_id(b));
}
