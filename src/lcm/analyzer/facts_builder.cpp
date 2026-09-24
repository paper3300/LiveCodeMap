#include "lcm/analyzer/facts_builder.hpp"

#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <clang/AST/APValue.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/PrettyPrinter.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/TemplateBase.h>
#include <clang/Analysis/CFG.h>
#include <clang/Basic/FileManager.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Lex/DependencyDirectivesScanner.h>
#include <clang/Lex/Lexer.h>
#include <clang/Lex/Preprocessor.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/ADT/StringExtras.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/VirtualFileSystem.h>
#include <llvm/Support/raw_ostream.h>

#include "lcm/hash.hpp"
#include "lcm/identity.hpp"

namespace lcm::analyzer::detail {
namespace {

constexpr const char* kExternalMember = "external";
constexpr const char* kAnonymousTypeAnchor = "anon-type";
constexpr const char* kClosureDestructorAnchor = "closure-dtor";  // existing identity, kept
constexpr const char* kClosureConstructorAnchor = "closure-ctor";
constexpr const char* kClosureAssignmentAnchor = "closure-assign";
constexpr const char* kClosureConversionAnchor = "closure-conv";
constexpr const char* kClosureMemberAnchor = "closure-member";
constexpr const char* kUnnamedConstructorName = "(ctor)";
constexpr unsigned kTypeDepthLimit = 32;     // nesting budget of the type encoder; exhaustion is a limit, never a raw fallback
constexpr unsigned kValueArrayLimit = 4096;  // structural array values beyond this are an explicit limit

// The closure's real call operator (or a specialization of a generic lambda's
// call operator template). Only this member folds into the lambda callable;
// constructors, assignment, conversions and the static invoker never do.
bool is_closure_call_operator(const clang::CXXMethodDecl* method) {
  if (!method) return false;
  const clang::CXXRecordDecl* parent = method->getParent();
  if (!parent || !parent->isLambda()) return false;
  if (const clang::CXXMethodDecl* op = parent->getLambdaCallOperator()) {
    if (op->getCanonicalDecl() == method->getCanonicalDecl()) return true;
  }
  if (const clang::FunctionTemplateDecl* tmpl = parent->getDependentLambdaCallOperator()) {
    if (const clang::FunctionTemplateDecl* primary = method->getPrimaryTemplate()) {
      return primary->getCanonicalDecl() == tmpl->getCanonicalDecl();
    }
  }
  return false;
}

// Anchor of a closure member under its lambda.
const char* closure_member_anchor(const clang::CXXMethodDecl* method) {
  if (llvm::isa<clang::CXXDestructorDecl>(method)) return kClosureDestructorAnchor;
  if (llvm::isa<clang::CXXConstructorDecl>(method)) return kClosureConstructorAnchor;
  if (llvm::isa<clang::CXXConversionDecl>(method)) return kClosureConversionAnchor;
  if (method->isCopyAssignmentOperator() || method->isMoveAssignmentOperator()) return kClosureAssignmentAnchor;
  return kClosureMemberAnchor;
}

// A record that has no name of its own: closure or anonymous struct/union
// (a typedef'd anonymous struct takes the typedef name and is not unnamed).
bool is_unnamed_record(const clang::RecordDecl* rd) {
  return rd && rd->getIdentifier() == nullptr && !rd->getTypedefNameForAnonDecl();
}

// A tag declared inside a function/lambda body or inside an unnamed record:
// its spelling is not unique across the translation unit.
bool is_local_tag(const clang::TagDecl* tag) {
  for (const clang::DeclContext* ctx = tag->getDeclContext(); ctx; ctx = ctx->getParent()) {
    if (llvm::isa<clang::FunctionDecl>(ctx)) return true;
    if (const auto* rd = llvm::dyn_cast<clang::RecordDecl>(ctx)) {
      if (is_unnamed_record(rd)) return true;
      if (const auto* cxx = llvm::dyn_cast<clang::CXXRecordDecl>(rd); cxx && cxx->isLambda()) return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Per-file information as the compiler saw it.
struct FileInfo {
  std::string abs_utf8;
  std::optional<RepoRelativePath> repo;
  bool in_root = false;
  bool is_system = false;
  llvm::StringRef buffer;
  FileContentHash hash;
  std::optional<LineIndex> lines;
};

std::string absolute_utf8_path(clang::SourceManager& sm, clang::FileID fid) {
  clang::OptionalFileEntryRef ref = sm.getFileEntryRefForID(fid);
  if (!ref) return {};
  llvm::StringRef real = ref->getFileEntry().tryGetRealPathName();
  llvm::SmallString<256> path(real.empty() ? ref->getName() : real);
  if (std::error_code ec = sm.getFileManager().getVirtualFileSystem().makeAbsolute(path)) {
    (void)ec;
  }
  llvm::sys::path::remove_dots(path, /*remove_dot_dot=*/true);
  return std::string(path.str());
}

// Orthogonal aspects of one call/reference site.
struct CallAspects {
  EvaluationContext evaluation = EvaluationContext::body;
  DispatchKind dispatch = DispatchKind::static_target;
  TemplateUse template_use = TemplateUse::none;
  bool immediately_invoked_lambda = false;
  std::string template_arguments;
  SubobjectRole subobject_role = SubobjectRole::none;
  std::string subobject;
  LifetimeKind lifetime = LifetimeKind::none;
  bool potentially_elided = false;
  std::optional<BaseEdgeDetails> base;
};

// Compiler-effective access of a direct base (`class` defaults to private,
// `struct` to public); `AS_none` never reaches a resolved base specifier.
BaseAccess base_access(clang::AccessSpecifier access) {
  switch (access) {
    case clang::AS_protected:
      return BaseAccess::protected_;
    case clang::AS_private:
      return BaseAccess::private_;
    default:
      return BaseAccess::public_;
  }
}

// The construct/call expression an initializer or return value really
// evaluates, below the compiler's wrapper nodes.
const clang::Expr* core_expression(const clang::Expr* e) {
  for (;;) {
    e = e->IgnoreImplicit();
    if (const auto* bind = llvm::dyn_cast<clang::CXXBindTemporaryExpr>(e)) {
      e = bind->getSubExpr();
      continue;
    }
    if (const auto* paren = llvm::dyn_cast<clang::ParenExpr>(e)) {
      e = paren->getSubExpr();
      continue;
    }
    return e;
  }
}

// The CXXBindTemporaryExpr of a by-value argument, if the argument is one.
const clang::CXXBindTemporaryExpr* argument_temporary(const clang::Expr* arg) {
  for (;;) {
    if (const auto* def = llvm::dyn_cast<clang::CXXDefaultArgExpr>(arg)) {
      arg = def->getExpr();
      continue;
    }
    if (const auto* paren = llvm::dyn_cast<clang::ParenExpr>(arg)) {
      arg = paren->getSubExpr();
      continue;
    }
    if (const auto* cast = llvm::dyn_cast<clang::ImplicitCastExpr>(arg)) {
      arg = cast->getSubExpr();
      continue;
    }
    if (const auto* mat = llvm::dyn_cast<clang::MaterializeTemporaryExpr>(arg)) {
      arg = mat->getSubExpr();
      continue;
    }
    if (const auto* cleanups = llvm::dyn_cast<clang::ExprWithCleanups>(arg)) {
      arg = cleanups->getSubExpr();
      continue;
    }
    return llvm::dyn_cast<clang::CXXBindTemporaryExpr>(arg);
  }
}

// Per-declaration callable state exactly as the compiler reports it.
CallableFlags flags_of(const clang::FunctionDecl* fd) {
  CallableFlags f;
  f.explicitly_defaulted = fd->isExplicitlyDefaulted();
  f.deleted_as_written = fd->isDeletedAsWritten();
  f.implicitly_deleted = fd->isDeleted() && !fd->getCanonicalDecl()->isDeletedAsWritten();
  f.trivial = fd->isTrivial();
  return f;
}

// ---------------------------------------------------------------------------
// Pre-pass: deterministic ordinals for lambdas, local classes and unnamed
// member records, keyed by the structural enclosing entity so identity never
// depends on traversal timing.
struct LocalInfo {
  const clang::Decl* enclosing = nullptr;
  unsigned ordinal = 0;
};

const clang::Decl* structural_enclosing(const clang::CXXRecordDecl* closure) {
  if (const clang::Decl* ctx_decl = closure->getLambdaContextDecl()) {
    if (llvm::isa<clang::VarDecl>(ctx_decl) || llvm::isa<clang::FieldDecl>(ctx_decl) ||
        llvm::isa<clang::EnumConstantDecl>(ctx_decl)) {
      return ctx_decl;
    }
  }
  const clang::DeclContext* ctx = closure->getDeclContext();
  while (ctx && !llvm::isa<clang::TranslationUnitDecl>(ctx)) {
    if (const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(ctx)) {
      if (method->getParent()->isLambda()) return method->getParent();
      return method;
    }
    if (const auto* fd = llvm::dyn_cast<clang::FunctionDecl>(ctx)) return fd;
    ctx = ctx->getParent();
  }
  return nullptr;
}

class ScopeIndexer : public clang::RecursiveASTVisitor<ScopeIndexer> {
 public:
  std::map<const clang::Decl*, LocalInfo> lambdas;          // closure record -> info
  std::map<const clang::Decl*, LocalInfo> local_classes;    // canonical local record -> info
  std::map<const clang::Decl*, LocalInfo> unnamed_members;  // canonical unnamed member record -> info
  std::map<const clang::Decl*, LocalInfo> local_tags;       // canonical local enum/alias -> info

  bool shouldVisitTemplateInstantiations() const { return false; }
  bool shouldVisitImplicitCode() const { return false; }

  // Same-named local enums/aliases in separate blocks of one function need a
  // structural ordinal exactly like local classes (per function, kind, name).
  bool VisitEnumDecl(clang::EnumDecl* d) {
    if (!d->isThisDeclarationADefinition()) return true;
    index_local_tag(d, "enum:" + d->getNameAsString());
    return true;
  }
  bool VisitTypedefNameDecl(clang::TypedefNameDecl* d) {
    index_local_tag(d, "alias:" + d->getNameAsString());
    return true;
  }
  void index_local_tag(const clang::NamedDecl* d, const std::string& key) {
    const clang::DeclContext* fn = d->getParentFunctionOrMethod();
    if (!fn || !llvm::isa<clang::FunctionDecl>(d->getDeclContext()->getRedeclContext())) return;
    LocalInfo info;
    info.enclosing = llvm::cast<clang::Decl>(fn);
    info.ordinal = class_counters_[std::make_pair(info.enclosing, key)]++;
    local_tags.emplace(d->getCanonicalDecl(), info);
  }

  bool VisitLambdaExpr(clang::LambdaExpr* e) {
    const clang::CXXRecordDecl* closure = e->getLambdaClass();
    LocalInfo info;
    info.enclosing = structural_enclosing(closure);
    // A closure in a namespace-scope declarator initializer has no function
    // context and (in this compiler) no lambda context declaration: the
    // declarator whose initializer is being traversed is its structural anchor.
    if (!info.enclosing && !declarators_.empty()) info.enclosing = declarators_.back();
    info.ordinal = lambda_counters_[info.enclosing]++;
    lambdas.emplace(closure, info);
    return true;
  }

  bool TraverseVarDecl(clang::VarDecl* vd) {
    const bool anchor = vd->isFileVarDecl() || vd->isStaticDataMember();
    if (anchor) declarators_.push_back(vd);
    const bool result = RecursiveASTVisitor::TraverseVarDecl(vd);
    if (anchor) declarators_.pop_back();
    return result;
  }
  bool TraverseFieldDecl(clang::FieldDecl* fd) {
    declarators_.push_back(fd);
    const bool result = RecursiveASTVisitor::TraverseFieldDecl(fd);
    declarators_.pop_back();
    return result;
  }
  bool TraverseEnumConstantDecl(clang::EnumConstantDecl* ec) {
    declarators_.push_back(ec);
    const bool result = RecursiveASTVisitor::TraverseEnumConstantDecl(ec);
    declarators_.pop_back();
    return result;
  }

  bool VisitCXXRecordDecl(clang::CXXRecordDecl* d) {
    if (d->isLambda() || !d->isThisDeclarationADefinition()) return true;
    if (const clang::FunctionDecl* fn = d->isLocalClass()) {
      LocalInfo info;
      info.enclosing = fn;
      info.ordinal = class_counters_[std::make_pair(static_cast<const clang::Decl*>(fn), d->getNameAsString())]++;
      local_classes.emplace(d->getCanonicalDecl(), info);
      return true;
    }
    if (d->getIdentifier() == nullptr && !d->getTypedefNameForAnonDecl()) {
      if (const auto* parent = llvm::dyn_cast<clang::RecordDecl>(d->getDeclContext())) {
        LocalInfo info;
        info.enclosing = parent;
        info.ordinal = class_counters_[std::make_pair(static_cast<const clang::Decl*>(parent), std::string())]++;
        unnamed_members.emplace(d->getCanonicalDecl(), info);
      }
    }
    return true;
  }

 private:
  std::map<const clang::Decl*, unsigned> lambda_counters_;
  std::map<std::pair<const clang::Decl*, std::string>, unsigned> class_counters_;
  std::vector<const clang::Decl*> declarators_;  // namespace-scope/static-member/field/enumerator initializers being traversed
};

// ---------------------------------------------------------------------------
class FactsVisitor : public clang::RecursiveASTVisitor<FactsVisitor> {
 public:
  FactsVisitor(clang::ASTContext& ctx, UnitContext& unit)
      : ctx_(ctx), sm_(ctx.getSourceManager()), unit_(unit), facts_(unit.result->facts) {
    policy_ = ctx.getPrintingPolicy();
    policy_.FullyQualifiedName = true;
    policy_.SuppressScope = false;
    policy_.SuppressTagKeyword = true;
    policy_.PrintAsCanonical = true;
    policy_.Bool = true;
  }

  bool shouldVisitTemplateInstantiations() const { return false; }
  bool shouldVisitImplicitCode() const { return false; }

  void run() {
    ScopeIndexer indexer;
    indexer.TraverseDecl(ctx_.getTranslationUnitDecl());
    lambdas_ = std::move(indexer.lambdas);
    local_classes_ = std::move(indexer.local_classes);
    unnamed_members_ = std::move(indexer.unnamed_members);
    local_tags_ = std::move(indexer.local_tags);
    for (clang::Decl* d : ctx_.getTranslationUnitDecl()->decls()) TraverseDecl(d);
    // Compiler-synthesized members that a fact referenced: processed after the
    // user code so no evaluation/default-argument context leaks into them.
    while (!pending_hidden_.empty()) {
      const clang::FunctionDecl* fd = pending_hidden_.front();
      pending_hidden_.pop_front();
      process_hidden_member(fd);
    }
    finalize_files();
    finalize_includes();
  }

  // ---- declaration traversal (custom: controls attribution and default args)
  bool TraverseDecl(clang::Decl* d) {
    if (!d || d->isImplicit()) return true;
    if (auto* fd = llvm::dyn_cast<clang::FunctionDecl>(d)) return traverse_function(fd);
    if (llvm::isa<clang::ParmVarDecl>(d)) return true;
    if (auto* vd = llvm::dyn_cast<clang::VarDecl>(d)) return traverse_variable(vd);
    if (auto* field = llvm::dyn_cast<clang::FieldDecl>(d)) return traverse_field(field);
    if (auto* ec = llvm::dyn_cast<clang::EnumConstantDecl>(d)) return traverse_enum_constant(ec);
    if (auto* rd = llvm::dyn_cast<clang::CXXRecordDecl>(d)) return traverse_record(rd);
    if (auto* ns = llvm::dyn_cast<clang::NamespaceDecl>(d)) {
      record_symbol(ns, LocationRole::declaration, std::nullopt);
      for (clang::Decl* child : ns->decls()) TraverseDecl(child);
      return true;
    }
    if (auto* ed = llvm::dyn_cast<clang::EnumDecl>(d)) {
      record_symbol(ed, ed->isThisDeclarationADefinition() ? LocationRole::definition : LocationRole::declaration,
                    ed->isThisDeclarationADefinition() ? std::optional(ed->getSourceRange()) : std::nullopt);
      for (clang::Decl* child : ed->decls()) TraverseDecl(child);
      return true;
    }
    if (auto* td = llvm::dyn_cast<clang::TypedefNameDecl>(d)) {
      record_symbol(td, LocationRole::declaration, std::nullopt);
      return true;
    }
    if (auto* ftd = llvm::dyn_cast<clang::FunctionTemplateDecl>(d)) return TraverseDecl(ftd->getTemplatedDecl());
    if (auto* ctd = llvm::dyn_cast<clang::ClassTemplateDecl>(d)) return TraverseDecl(ctd->getTemplatedDecl());
    if (llvm::isa<clang::LinkageSpecDecl>(d) || llvm::isa<clang::ExportDecl>(d)) {
      for (clang::Decl* child : llvm::cast<clang::DeclContext>(d)->decls()) TraverseDecl(child);
      return true;
    }
    limit(std::string("declaration kind not modelled in this increment: ") + d->getDeclKindName());
    return true;
  }

  // ---- statements/expressions ----------------------------------------------
  bool TraverseLambdaExpr(clang::LambdaExpr* e) {
    const clang::CXXRecordDecl* closure = e->getLambdaClass();
    // Capture initializers are evaluated by the enclosing callable: an
    // init-capture evaluates its initializer, an ordinary by-copy or *this
    // capture of class type performs a compiler-inserted copy/move.
    auto init_it = e->capture_init_begin();
    for (auto cap = e->capture_begin(); cap != e->capture_end() && init_it != e->capture_init_end(); ++cap, ++init_it) {
      if (!*init_it) continue;
      evaluation_context_.push_back(e->isInitCapture(&*cap) ? EvaluationContext::init_capture_initializer
                                                            : EvaluationContext::capture_copy);
      TraverseStmt(*init_it);
      evaluation_context_.pop_back();
    }
    const std::optional<StableId> id = lambda_id(closure);
    if (!id) return true;
    record_captures(e, *id);
    if (const auto loc = make_location(e->getSourceRange())) {
      SymbolLocation sl;
      sl.role = LocationRole::definition;
      sl.location = *loc;
      if (e->getBody()) sl.body_hash = hash_of_range(e->getBody()->getSourceRange());
      sl.analysis_unit = unit_.analysis_unit;
      if (const auto key = lambda_key(closure)) {
        facts_.add_symbol_location(*key, sl);
        if (const auto owner = current_owner()) {
          add_relation(*owner, *id, RelationKind::contains, *loc, CallAspects{});
        }
      }
    }
    owners_.push_back(*id);
    if (!e->isGenericLambda()) parameter_lifetime(e->getCallOperator(), *id);
    if (e->getBody()) {
      TraverseStmt(e->getBody());
      lifetime_facts(e->getCallOperator(), e->getBody(), *id);
    }
    owners_.pop_back();
    return true;
  }

  // Aggregate initialization: the compiler's semantic form carries the
  // member constructions (`decltype(a1) x{}` constructs the Payload member);
  // the syntactic form alone would hide them. Its children are a superset of
  // the syntactic children, so traversing it once covers both.
  bool TraverseInitListExpr(clang::InitListExpr* e) {
    clang::InitListExpr* semantic = e->isSemanticForm() ? e : e->getSemanticForm();
    return RecursiveASTVisitor::TraverseSynOrSemInitListExpr(semantic ? semantic : e, nullptr);
  }

  // The return value's construction of an NRVO candidate may be elided.
  bool VisitReturnStmt(clang::ReturnStmt* rs) {
    if (!rs->getNRVOCandidate() || !rs->getRetValue()) return true;
    if (const auto* ce = llvm::dyn_cast<clang::CXXConstructExpr>(core_expression(rs->getRetValue()))) {
      elided_constructs_.insert(ce);
    }
    return true;
  }

  bool VisitCXXDefaultArgExpr(clang::CXXDefaultArgExpr* e) {
    // Evaluated at this use site by the current caller, not by the declaring function.
    default_use_.push_back(e->getUsedLocation());
    evaluation_context_.push_back(EvaluationContext::default_argument);
    TraverseStmt(e->getExpr());
    evaluation_context_.pop_back();
    default_use_.pop_back();
    return true;
  }

  bool VisitCXXDefaultInitExpr(clang::CXXDefaultInitExpr* e) {
    default_use_.push_back(e->getUsedLocation());
    evaluation_context_.push_back(EvaluationContext::default_member_initializer);
    TraverseStmt(e->getExpr());
    evaluation_context_.pop_back();
    default_use_.pop_back();
    return true;
  }

  bool VisitCallExpr(clang::CallExpr* call) {
    if (!dedupe(call)) return true;
    const auto owner = current_owner();
    if (!owner) {
      limit("call outside any attributed callable/initializer context was not recorded");
      return true;
    }
    if (auto* callee_expr = llvm::dyn_cast<clang::DeclRefExpr>(call->getCallee()->IgnoreParenImpCasts())) {
      callee_refs_.insert(callee_expr);
    }

    CallAspects aspects = base_aspects();
    const clang::FunctionDecl* callee = nullptr;
    const clang::CXXRecordDecl* lambda_target = nullptr;

    if (auto* member_call = llvm::dyn_cast<clang::CXXMemberCallExpr>(call)) {
      callee = member_call->getMethodDecl();
      if (auto* me = llvm::dyn_cast<clang::MemberExpr>(member_call->getCallee()->IgnoreParens())) {
        auto* method = llvm::dyn_cast_or_null<clang::CXXMethodDecl>(callee);
        if (is_closure_call_operator(method)) {
          // `x.operator()(...)` on a closure: the same fold as `x(...)`. Any other
          // closure member (operator=, conversion) stays an ordinary member call
          // whose target becomes a hidden member; it never executes the body.
          fold_closure_call(method, member_call->getImplicitObjectArgument(), aspects, lambda_target);
        } else if (me->hasQualifier()) {
          // p.Plain::run(), d.Base::run(), d.Derived::run(): an explicit qualifier is recorded whether
          // or not the method is virtual; the call is statically bound to the named method.
          aspects.dispatch = DispatchKind::qualified;
        } else if (method && method->isVirtual()) {
          if (!me->performsVirtualDispatch(ctx_.getLangOpts())) {
            aspects.dispatch = DispatchKind::devirtualized;  // object expression: no dispatch
          } else if (const clang::CXXMethodDecl* devirt = method->getDevirtualizedMethod(me->getBase(), false)) {
            callee = devirt;  // final class/method: the compiler proves the target
            aspects.dispatch = DispatchKind::devirtualized;
          } else {
            aspects.dispatch = DispatchKind::virtual_slot;  // compile-time selected slot; overrides are query-time
          }
        }
      }
    } else if (auto* op_call = llvm::dyn_cast<clang::CXXOperatorCallExpr>(call)) {
      callee = op_call->getDirectCallee();
      if (const auto* method = llvm::dyn_cast_or_null<clang::CXXMethodDecl>(callee); is_closure_call_operator(method)) {
        // `x(...)` on a closure. A closure `operator=` reached through `a = b`
        // is NOT this case: it is an ordinary (hidden) member call.
        fold_closure_call(method, op_call->getNumArgs() > 0 ? op_call->getArg(0) : nullptr, aspects, lambda_target);
      }
    } else {
      callee = call->getDirectCallee();
    }

    const auto loc = evidence_location(call);
    if (!loc) return true;

    // Argument ownership is decided on the CONCRETE callee (before any template
    // folding) and on the declared parameters only: for a member operator call
    // the object expression is argument 0 and not a parameter.
    {
      unsigned offset = 0;
      if (llvm::isa<clang::CXXOperatorCallExpr>(call)) {
        if (const auto* method = llvm::dyn_cast_or_null<clang::CXXMethodDecl>(callee);
            method && method->isImplicitObjectMemberFunction()) {
          offset = 1;
        }
      }
      const unsigned count = call->getNumArgs() > offset ? call->getNumArgs() - offset : 0;
      note_callee_destroyed_arguments(*owner, callee, call->getCallee(), call->getArgs() + offset, count);
    }

    if (lambda_target) {
      if (const auto id = lambda_id(lambda_target)) add_relation(*owner, *id, RelationKind::calls, *loc, aspects);
      return true;
    }
    if (!callee) {
      record_unresolved(*owner, call, *loc);
      return true;
    }
    if (!resolve_template_target(callee, aspects)) {
      record_unresolved(*owner, call, *loc, UnresolvedReason::dependent_expression);
      return true;
    }
    if (const auto target = ensure_target(callee)) add_relation(*owner, *target, RelationKind::calls, *loc, aspects);
    return true;
  }

  // Execution of a closure's call operator is the lambda callable itself; a
  // generic lambda's specialization keeps its deduced arguments as template
  // evidence, orthogonal to everything else on the site.
  void fold_closure_call(const clang::CXXMethodDecl* method, const clang::Expr* object, CallAspects& aspects,
                         const clang::CXXRecordDecl*& lambda_target) {
    lambda_target = method->getParent();
    if (object && llvm::isa<clang::LambdaExpr>(object->IgnoreParenImpCasts())) aspects.immediately_invoked_lambda = true;
    if (method->isTemplateInstantiation()) {
      aspects.template_use = TemplateUse::primary_implicit;
      if (const clang::TemplateArgumentList* args = method->getTemplateSpecializationArgs()) {
        aspects.template_arguments = print_template_args(*args);
      }
    }
  }

  bool VisitCXXConstructExpr(clang::CXXConstructExpr* e) {
    if (!dedupe(e)) return true;
    const clang::CXXConstructorDecl* ctor = e->getConstructor();
    if (!ctor) return true;
    const auto owner = current_owner();
    if (!owner) return true;
    const auto loc = evidence_location(e);
    if (!loc) return true;
    CallAspects aspects = base_aspects();
    if (const auto sub = subobject_inits_.find(e); sub != subobject_inits_.end()) {
      aspects.subobject_role = sub->second.first;
      aspects.subobject = sub->second.second;
    }
    if (e->isElidable() || elided_constructs_.count(e)) aspects.potentially_elided = true;
    note_callee_destroyed_arguments(*owner, ctor, nullptr, e->getArgs(), e->getNumArgs());
    const clang::FunctionDecl* target_decl = ctor;
    if (!resolve_template_target(target_decl, aspects)) return true;
    if (const auto target = ensure_target(target_decl)) add_relation(*owner, *target, RelationKind::calls, *loc, aspects);
    return true;
  }

  // A by-value argument whose type the compiler destroys in the callee
  // (RecordDecl::isParamDestroyedInCallee, the target ABI decision) is never a
  // caller-owned temporary. `callee` is the concrete declaration when the
  // compiler resolved one (an instantiation folds to its pattern only for the
  // owner id, after the concrete parameter types decided ownership); when the
  // callee is unknown (function pointer, callback, pointer to member) the
  // parameter types come from the callee expression's prototype and the
  // destruction is reported as an unresolved site of the caller, never as a
  // caller-owned call. `args` holds the arguments that correspond to declared
  // parameters (no object argument). A declaration-only in-root callee still
  // owns the destruction; an external callee is a boundary.
  void note_callee_destroyed_arguments(const StableId& caller, const clang::FunctionDecl* callee,
                                       const clang::Expr* callee_expr, const clang::Expr* const* args,
                                       unsigned count) {
    const clang::FunctionProtoType* proto = nullptr;
    if (!callee) {
      proto = callee_expr ? unknown_callee_prototype(callee_expr) : nullptr;
      if (!proto) {
        limit("indirect call with no recoverable parameter prototype: argument destruction ownership not decided");
        return;
      }
    }
    const unsigned params = callee ? callee->getNumParams() : proto->getNumParams();
    for (unsigned i = 0; i < count && i < params; ++i) {
      const clang::QualType param_type = callee ? callee->getParamDecl(i)->getType() : proto->getParamType(i);
      if (param_type->isReferenceType()) continue;
      const clang::CXXRecordDecl* record = param_type->getAsCXXRecordDecl();
      if (!record || !record->isParamDestroyedInCallee()) continue;
      const clang::CXXBindTemporaryExpr* temporary = argument_temporary(args[i]);
      if (!temporary) continue;
      callee_destroyed_args_.insert(temporary);  // suppresses the ABI-agnostic caller-side CFG temporary
      const auto loc = make_location(args[i]->getSourceRange());
      if (!loc) continue;
      if (!callee) {
        UnresolvedSite site;
        site.enclosing = caller;
        site.expression = std::string(clang::Lexer::getSourceText(
            clang::CharSourceRange::getTokenRange(args[i]->getSourceRange()), sm_, ctx_.getLangOpts()));
        site.location = *loc;
        site.reason = UnresolvedReason::indirect_callee;
        site.analysis_unit = unit_.analysis_unit;
        facts_.add_unresolved_site(std::move(site));
        limit("by-value argument of an indirect call is destroyed by the unknown callee: destructor ownership unresolved");
        continue;
      }
      if (!decl_in_root(callee)) {
        limit("by-value argument destroyed inside an external callee: destructor call not recorded (external boundary)");
        continue;
      }
      const clang::CXXDestructorDecl* dtor = record->getDestructor();
      if (!dtor) continue;
      CallAspects aspects;
      aspects.lifetime = LifetimeKind::parameter;
      std::optional<StableId> owner;
      if (const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(callee); method && method->getParent()->isLambda()) {
        owner = lambda_id(method->getParent());  // operator() folds into the lambda
      } else {
        const clang::FunctionDecl* owner_decl = callee;
        if (!resolve_template_target(owner_decl, aspects)) {
          limit("callee-destroyed argument of a template instantiation without a reachable pattern: owner not recorded");
          continue;
        }
        owner = ensure_target(owner_decl);
      }
      if (owner) emit_destructor_call(*owner, dtor, *loc, aspects);
    }
  }

  // Parameter prototype of a call whose callee the compiler did not resolve:
  // a function pointer/reference (`fp(x)`), or a pointer to member function
  // applied with `.*`/`->*`, whose prototype is the pointee of the RHS
  // MemberPointerType (the LHS object never names the target; no dataflow).
  static const clang::FunctionProtoType* unknown_callee_prototype(const clang::Expr* callee_expr) {
    const clang::Expr* e = callee_expr->IgnoreParenImpCasts();
    if (const auto* binary = llvm::dyn_cast<clang::BinaryOperator>(e);
        binary && (binary->getOpcode() == clang::BO_PtrMemD || binary->getOpcode() == clang::BO_PtrMemI)) {
      if (const auto* member_pointer = binary->getRHS()->getType()->getAs<clang::MemberPointerType>()) {
        return member_pointer->getPointeeType()->getAs<clang::FunctionProtoType>();
      }
      return nullptr;
    }
    clang::QualType t = callee_expr->getType();
    if (t->isPointerType() || t->isReferenceType()) t = t->getPointeeType();
    return t->getAs<clang::FunctionProtoType>();
  }

  bool VisitDeclRefExpr(clang::DeclRefExpr* e) {
    if (callee_refs_.count(e)) return true;
    const auto* fd = llvm::dyn_cast<clang::FunctionDecl>(e->getDecl());
    if (!fd) return true;
    const auto owner = current_owner();
    if (!owner) return true;
    const auto loc = evidence_location(e);
    if (!loc) return true;
    CallAspects aspects = base_aspects();
    const clang::FunctionDecl* target_decl = fd;
    if (!resolve_template_target(target_decl, aspects)) return true;
    if (const auto target = ensure_target(target_decl)) {
      add_relation(*owner, *target, RelationKind::references, *loc, aspects);
    }
    return true;
  }

 private:
  // ---- template use ---------------------------------------------------------
  // Maps an instantiation to its pattern and fills the template-use aspect.
  // Returns false when no pattern is reachable.
  bool resolve_template_target(const clang::FunctionDecl*& callee, CallAspects& aspects) {
    if (callee->isTemplateInstantiation()) {
      if (const clang::TemplateArgumentList* args = callee->getTemplateSpecializationArgs()) {
        aspects.template_arguments = print_template_args(*args);
      } else if (const auto* spec = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(callee->getParent())) {
        aspects.template_arguments = print_template_args(spec->getTemplateArgs());
      }
      const clang::FunctionDecl* pattern = callee->getTemplateInstantiationPattern();
      if (!pattern) {
        limit("template instantiation without a reachable pattern was left unresolved");
        return false;
      }
      callee = pattern;
      aspects.template_use = TemplateUse::primary_implicit;
    } else if (callee->getTemplateSpecializationKind() == clang::TSK_ExplicitSpecialization) {
      aspects.template_use = TemplateUse::explicit_specialization;
    }
    return true;
  }

  // ---- traversal helpers ----------------------------------------------------
  bool traverse_function(clang::FunctionDecl* fd) {
    if (fd->isTemplateInstantiation()) return true;  // never a public node
    const bool definition = fd->isThisDeclarationADefinition();
    std::optional<clang::SourceRange> body_range;
    // `= default;` is a definition without a user-written body: nothing to hash.
    if (fd->doesThisDeclarationHaveABody() && fd->getBody() && !fd->isExplicitlyDefaulted()) {
      body_range = fd->getBody()->getSourceRange();
    }
    record_symbol(fd, definition ? LocationRole::definition : LocationRole::declaration, body_range, flags_of(fd));

    if (auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(fd)) {
      if (const auto self = symbol_id(fd)) {
        for (const clang::CXXMethodDecl* base : method->overridden_methods()) {
          if (const auto base_id = ensure_target(base)) {
            if (const auto loc = make_location(fd->getSourceRange())) {
              add_relation(*self, *base_id, RelationKind::overrides, *loc, CallAspects{});
            }
          }
        }
      }
    }
    // Parameter types only: default arguments are NOT calls made by the declaring function.
    for (clang::ParmVarDecl* p : fd->parameters()) {
      if (auto* tsi = p->getTypeSourceInfo()) TraverseTypeLoc(tsi->getTypeLoc());
    }
    if (fd->doesThisDeclarationHaveABody()) {
      const auto id = symbol_id(fd);
      if (!id) return true;
      owners_.push_back(*id);
      if (auto* ctor = llvm::dyn_cast<clang::CXXConstructorDecl>(fd)) traverse_initializers(ctor);
      parameter_lifetime(fd, *id);
      if (clang::Stmt* body = fd->getBody()) {
        TraverseStmt(body);
        lifetime_facts(fd, body, *id);
      }
      owners_.pop_back();
    }
    return true;
  }

  // Written and compiler-synthesized base/member/delegating initializers of a
  // constructor. The construct expression each one evaluates carries the
  // subobject identity; nested calls only carry the evaluation context.
  void traverse_initializers(clang::CXXConstructorDecl* ctor) {
    for (clang::CXXCtorInitializer* init : ctor->inits()) {
      if (!init->getInit()) continue;
      SubobjectRole role = SubobjectRole::none;
      std::string name;
      if (init->isBaseInitializer()) {
        role = SubobjectRole::base;
        name = clang::QualType(init->getBaseClass(), 0).getAsString(policy_);
      } else if (init->isAnyMemberInitializer()) {
        role = SubobjectRole::member;
        if (const clang::FieldDecl* field = init->getAnyMember()) {
          name = field->getNameAsString();
          if (name.empty()) {  // closure capture field: named by its capture
            const auto capture = closure_fields_.find(field);
            if (capture != closure_fields_.end()) name = capture->second;
          }
        }
      } else if (init->isDelegatingInitializer()) {
        role = SubobjectRole::delegating;
        name = ctor->getParent()->getNameAsString();
      }
      if (const auto* ce = llvm::dyn_cast<clang::CXXConstructExpr>(core_expression(init->getInit()))) {
        subobject_inits_.emplace(ce, std::make_pair(role, name));
      }
      evaluation_context_.push_back(init->isWritten() ? EvaluationContext::mem_initializer
                                                      : EvaluationContext::implicit_mem_initializer);
      TraverseStmt(init->getInit());
      evaluation_context_.pop_back();
    }
  }

  // A by-value parameter of a type the target ABI destroys in the callee is
  // destroyed by this function; the compiler's CFG has no element for it, so
  // the parameter declaration of the definition plus the ABI flag is the
  // evidence. Declarations without a body record nothing here.
  void parameter_lifetime(const clang::FunctionDecl* fd, const StableId& owner) {
    for (const clang::ParmVarDecl* p : fd->parameters()) {
      if (p->needsDestruction(ctx_) == clang::QualType::DK_none) continue;
      const clang::CXXRecordDecl* record = ctx_.getBaseElementType(p->getType())->getAsCXXRecordDecl();
      if (!record || !record->isParamDestroyedInCallee()) continue;
      const clang::CXXDestructorDecl* dtor = record->getDestructor();
      if (!dtor) continue;
      const auto loc = make_location(p->getSourceRange());
      if (!loc) continue;
      CallAspects aspects;
      aspects.lifetime = LifetimeKind::parameter;
      emit_destructor_call(owner, dtor, *loc, aspects);
    }
  }

  // Implicit destruction as the compiler's control-flow graph states it.
  // Elements are deduplicated by the destroyed object/subobject, never
  // counted per path; blocks, edges and order are discarded.
  void lifetime_facts(const clang::FunctionDecl* fd, clang::Stmt* body, const StableId& owner) {
    if (!fd || !body) return;
    if (fd->isDependentContext() || fd->getDescribedFunctionTemplate()) {
      limit("implicit destruction inside a dependent template pattern is not recorded");
      return;
    }
    clang::CFG::BuildOptions options;
    options.AddImplicitDtors = true;
    options.AddTemporaryDtors = true;
    options.AddInitializers = true;
    std::unique_ptr<clang::CFG> cfg = clang::CFG::buildCFG(fd, body, &ctx_, options);
    if (!cfg) {
      limit("control-flow graph unavailable for '" + fd->getQualifiedNameAsString() +
            "': implicit destruction of its objects is not recorded");
      return;
    }
    std::set<const void*> seen;
    for (const clang::CFGBlock* block : *cfg) {
      for (const clang::CFGElement& element : *block) {
        if (const auto automatic = element.getAs<clang::CFGAutomaticObjDtor>()) {
          const clang::VarDecl* vd = automatic->getVarDecl();
          if (!vd || !seen.insert(vd).second) continue;
          // The CFG models a by-value parameter's destruction on the callee side
          // regardless of ABI (and only on explicit-return paths). The ABI
          // decision is applied once, in parameter_lifetime(); never here.
          if (llvm::isa<clang::ParmVarDecl>(vd)) continue;
          CallAspects aspects;
          aspects.lifetime = LifetimeKind::automatic_object;
          aspects.potentially_elided = vd->isNRVOVariable();
          if (const auto loc = make_location(vd->getSourceRange())) {
            emit_destructor_call(owner, automatic->getDestructorDecl(ctx_), *loc, aspects);
          }
        } else if (const auto temporary = element.getAs<clang::CFGTemporaryDtor>()) {
          const clang::CXXBindTemporaryExpr* bind = temporary->getBindTemporaryExpr();
          if (!bind || !seen.insert(bind).second) continue;
          if (callee_destroyed_args_.count(bind)) continue;  // the callee owns this destruction
          CallAspects aspects;
          aspects.lifetime = LifetimeKind::temporary;
          if (const auto loc = make_location(bind->getSourceRange())) {
            emit_destructor_call(owner, temporary->getDestructorDecl(ctx_), *loc, aspects);
          }
        } else if (const auto member = element.getAs<clang::CFGMemberDtor>()) {
          const clang::FieldDecl* field = member->getFieldDecl();
          if (!field || !seen.insert(field).second) continue;
          CallAspects aspects;
          aspects.lifetime = LifetimeKind::subobject;
          aspects.subobject_role = SubobjectRole::member;
          aspects.subobject = field->getNameAsString();
          if (aspects.subobject.empty()) {
            const auto capture = closure_fields_.find(field);
            if (capture != closure_fields_.end()) aspects.subobject = capture->second;
          }
          if (const auto loc = make_location(field->getSourceRange())) {
            emit_destructor_call(owner, destructor_of(*member, field->getType()), *loc, aspects);
          }
        } else if (const auto base = element.getAs<clang::CFGBaseDtor>()) {
          const clang::CXXBaseSpecifier* spec = base->getBaseSpecifier();
          if (!spec || !seen.insert(spec).second) continue;
          CallAspects aspects;
          aspects.lifetime = LifetimeKind::subobject;
          aspects.subobject_role = SubobjectRole::base;
          aspects.subobject = spec->getType().getAsString(policy_);
          if (const auto loc = make_location(spec->getSourceRange())) {
            emit_destructor_call(owner, destructor_of(*base, spec->getType()), *loc, aspects);
          }
        } else if (element.getAs<clang::CFGDeleteDtor>()) {
          limit("destructor calls of delete-expressions are not recorded in this increment");
        }
      }
    }
  }

  // Destructor named by a subobject CFG element. Clang 22 resolves it for some
  // element kinds only (getDestructorDecl returns null for base destructors),
  // so the destroyed subobject's own class type, which the element carries,
  // supplies the compiler-declared destructor. The element itself is the
  // evidence that this subobject is destroyed here.
  const clang::CXXDestructorDecl* destructor_of(const clang::CFGImplicitDtor& element, clang::QualType type) {
    if (const clang::CXXDestructorDecl* dtor = element.getDestructorDecl(ctx_)) return dtor;
    const clang::CXXRecordDecl* record = ctx_.getBaseElementType(type)->getAsCXXRecordDecl();
    return record ? record->getDestructor() : nullptr;
  }

  void emit_destructor_call(const StableId& owner, const clang::CXXDestructorDecl* dtor, const SourceLocation& loc,
                            CallAspects aspects) {
    if (!dtor) {
      limit("destructor of a destroyed object could not be determined by the compiler; not recorded");
      return;
    }
    const clang::FunctionDecl* target_decl = dtor;
    if (!resolve_template_target(target_decl, aspects)) return;
    if (const auto target = ensure_target(target_decl)) add_relation(owner, *target, RelationKind::calls, loc, aspects);
  }

  // Capture facts of one lambda (PRD FR-CPP-004): descriptor, kind,
  // explicit/implicit, init-capture; locals are not indexed for this.
  void record_captures(const clang::LambdaExpr* e, const StableId& lambda) {
    const clang::CXXRecordDecl* closure = e->getLambdaClass();
    llvm::DenseMap<const clang::ValueDecl*, clang::FieldDecl*> fields;
    clang::FieldDecl* this_field = nullptr;
    closure->getCaptureFields(fields, this_field);
    for (const auto& [var, field] : fields) closure_fields_.emplace(field, var->getNameAsString());
    if (this_field) closure_fields_.emplace(this_field, this_field->getType()->isPointerType() ? "this" : "*this");

    for (const clang::LambdaCapture& cap : e->captures()) {
      CaptureFact fact;
      fact.lambda = lambda;
      fact.analysis_unit = unit_.analysis_unit;
      switch (cap.getCaptureKind()) {
        case clang::LCK_This:
          fact.kind = CaptureKind::this_pointer;
          break;
        case clang::LCK_StarThis:
          fact.kind = CaptureKind::star_this;
          break;
        case clang::LCK_ByCopy:
          fact.kind = CaptureKind::by_copy;
          break;
        case clang::LCK_ByRef:
          fact.kind = CaptureKind::by_reference;
          break;
        case clang::LCK_VLAType:
          limit("variable-length array type capture is not recorded");
          continue;
      }
      fact.explicit_capture = cap.isExplicit();
      fact.init_capture = e->isInitCapture(&cap);
      fact.pack_expansion = cap.isPackExpansion();
      if (cap.capturesThis()) {
        fact.name = "this";
      } else if (const clang::ValueDecl* var = cap.getCapturedVar()) {
        fact.name = var->getNameAsString();
        if (const auto decl_loc = make_location(var->getSourceRange())) {
          fact.target_descriptor = sha256(decl_loc->file.generic + ":" + std::to_string(decl_loc->span.begin_offset) +
                                          "-" + std::to_string(decl_loc->span.end_offset))
                                       .hex()
                                       .substr(0, 16);
        }
      }
      const auto loc = cap.isExplicit() ? make_location(clang::SourceRange(cap.getLocation(), cap.getLocation()))
                                        : make_location(e->getIntroducerRange());
      if (!loc) continue;
      fact.location = *loc;
      facts_.add_capture(std::move(fact));
    }
  }

  // A compiler-synthesized special member referenced by a fact: hidden
  // identity with a contribution-owned implicit_declaration location anchored
  // at the class name (no user span is manufactured), its own initializer
  // calls and, from the CFG of its synthesized body, its subobject destruction.
  void process_hidden_member(const clang::FunctionDecl* fd) {
    const auto id = symbol_id(fd);
    if (!id) return;
    const auto key = keys_.find(fd->getCanonicalDecl());
    if (key == keys_.end()) return;
    const auto loc = make_location(clang::SourceRange(fd->getLocation(), fd->getLocation()));
    if (!loc) return;
    SymbolLocation sl;
    sl.role = LocationRole::implicit_declaration;
    sl.location = *loc;
    sl.analysis_unit = unit_.analysis_unit;
    sl.callable = flags_of(fd);
    facts_.add_hidden_member(key->second, sl);
    if (const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(fd)) {
      const std::optional<StableId> owner =
          method->getParent()->isLambda() ? lambda_id(method->getParent()) : ensure_target(method->getParent());
      if (owner) add_relation(*owner, *id, RelationKind::contains, *loc, CallAspects{});
    }
    if (!fd->isUsed() && !fd->isDefined()) return;  // referenced but never odr-used: no synthesized body
    owners_.push_back(*id);
    if (auto* ctor = llvm::dyn_cast<clang::CXXConstructorDecl>(const_cast<clang::FunctionDecl*>(fd))) {
      traverse_initializers(ctor);
    }
    if (clang::Stmt* body = fd->getBody()) {
      TraverseStmt(body);
      lifetime_facts(fd, body, *id);
    } else if (llvm::isa<clang::CXXDestructorDecl>(fd)) {
      limit("used implicit destructor without a synthesized body: subobject destruction not recorded");
    }
    owners_.pop_back();
  }

  bool traverse_variable(clang::VarDecl* vd) {
    const bool is_symbol = vd->isFileVarDecl() || vd->isStaticDataMember();
    if (is_symbol) {
      record_symbol(vd, vd->isThisDeclarationADefinition() ? LocationRole::definition : LocationRole::declaration,
                    std::nullopt);
      if (vd->hasInit()) {
        const auto id = symbol_id(vd);
        if (!id) return true;
        owners_.push_back(*id);
        TraverseStmt(vd->getInit());
        owners_.pop_back();
      }
      return true;
    }
    if (vd->hasInit()) TraverseStmt(vd->getInit());  // local variable: initializer belongs to the current callable
    return true;
  }

  bool traverse_field(clang::FieldDecl* field) {
    if (field->getName().empty()) {
      limit("compiler-generated unnamed field of an anonymous union is not a symbol");
      return true;
    }
    record_symbol(field, LocationRole::declaration, std::nullopt);
    if (field->hasInClassInitializer() && field->getInClassInitializer()) {
      const auto id = symbol_id(field);
      if (!id) return true;
      owners_.push_back(*id);
      TraverseStmt(field->getInClassInitializer());
      owners_.pop_back();
    }
    return true;
  }

  bool traverse_enum_constant(clang::EnumConstantDecl* ec) {
    record_symbol(ec, LocationRole::definition, std::nullopt);
    if (ec->getInitExpr()) {
      const auto id = symbol_id(ec);
      if (!id) return true;
      owners_.push_back(*id);
      TraverseStmt(ec->getInitExpr());
      owners_.pop_back();
    }
    return true;
  }

  bool traverse_record(clang::CXXRecordDecl* rd) {
    if (rd->isLambda()) return true;  // handled at the LambdaExpr
    if (auto* spec = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(rd)) {
      if (spec->getSpecializationKind() != clang::TSK_ExplicitSpecialization &&
          !llvm::isa<clang::ClassTemplatePartialSpecializationDecl>(spec)) {
        return true;  // implicit instantiation: never a public node
      }
    }
    const bool definition = rd->isThisDeclarationADefinition();
    record_symbol(rd, definition ? LocationRole::definition : LocationRole::declaration,
                  definition ? std::optional(rd->getSourceRange()) : std::nullopt);
    if (definition) {
      if (const auto self = symbol_id(rd)) {
        std::uint32_t base_ordinal = 0;
        for (const clang::CXXBaseSpecifier& base : rd->bases()) {
          const std::uint32_t ordinal = base_ordinal++;  // zero-based lexical order, counted over every written base
          const clang::CXXRecordDecl* base_decl = base.getType()->getAsCXXRecordDecl();
          if (!base_decl) {
            limit("dependent or non-record base class was not recorded");
            continue;
          }
          const clang::CXXRecordDecl* target_decl = base_decl;
          CallAspects aspects;
          if (auto* base_spec = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(base_decl)) {
            if (base_spec->getSpecializationKind() != clang::TSK_ExplicitSpecialization) {
              // An instantiated base folds to the pattern the compiler selected;
              // the instantiation never becomes a node. Its concrete arguments
              // and the use mode stay on this contribution's evidence.
              const auto selected = base_spec->getSpecializedTemplateOrPartial();
              bool partial = false;
              if (const auto* p = selected.dyn_cast<clang::ClassTemplatePartialSpecializationDecl*>()) {
                target_decl = p;
                partial = true;
              } else if (const auto* primary = selected.dyn_cast<clang::ClassTemplateDecl*>()) {
                target_decl = primary->getTemplatedDecl();
              } else {
                limit("base class template instantiation without a reachable pattern was not recorded as extends");
                continue;
              }
              switch (base_spec->getSpecializationKind()) {
                case clang::TSK_ImplicitInstantiation:
                  aspects.template_use = partial ? TemplateUse::partial_implicit : TemplateUse::primary_implicit;
                  break;
                case clang::TSK_ExplicitInstantiationDeclaration:
                  aspects.template_use = TemplateUse::explicit_instantiation_declaration;
                  break;
                case clang::TSK_ExplicitInstantiationDefinition:
                  aspects.template_use = TemplateUse::explicit_instantiation_definition;
                  break;
                default:
                  limit("base class template use of an unclassified specialization kind was not recorded as extends");
                  continue;
              }
              aspects.template_arguments = print_template_args(base_spec->getTemplateArgs());
              if (aspects.template_arguments.empty()) {
                limit("base class template arguments could not be represented; the base was not recorded as extends");
                continue;
              }
            }
          }
          if (const auto base_id = ensure_target(target_decl)) {
            // The full written base-specifier range, so the original bytes of
            // `protected virtual B` are reproducible from the evidence span.
            if (const auto loc = make_location(base.getSourceRange())) {
              BaseEdgeDetails details;
              details.effective_access = base_access(base.getAccessSpecifier());
              details.access_written = base.getAccessSpecifierAsWritten() != clang::AS_none;
              details.is_virtual = base.isVirtual();
              details.lexical_ordinal = ordinal;
              aspects.base = details;
              add_relation(*self, *base_id, RelationKind::extends, *loc, aspects);
            }
          }
        }
      }
      for (clang::Decl* child : rd->decls()) TraverseDecl(child);
    }
    return true;
  }

  // ---- identity -------------------------------------------------------------
  struct TemplateInfo {
    TemplateRole role = TemplateRole::none;
    std::string parameters;
    std::string arguments;
  };

  // Template arguments for identity and evidence: the established printer
  // unless an argument involves a local/unnamed leaf, which is atomized.
  // An unsupported local form yields an empty string (callers treat that as
  // "arguments unavailable" and record a limit).
  std::string print_template_args(const clang::TemplateArgumentList& args) {
    std::string out = "<";
    for (unsigned i = 0; i < args.size(); ++i) {
      if (i) out += ", ";
      if (template_arg_has_local(args[i], 0)) {
        const auto atom = atom_template_arg(args[i], 0);
        if (!atom) {
          limit("template argument involving a local/unnamed leaf uses an unsupported form; identity not invented");
          return {};
        }
        out += *atom;
        continue;
      }
      std::string one;
      llvm::raw_string_ostream os(one);
      args[i].print(policy_, os, /*IncludeType=*/true);
      out += one;
    }
    return out + ">";
  }

  // Canonical template-parameter signature: parameter KINDS only, never
  // names, so renaming and redeclaration keep identity while
  // template<class T> and template<int N> differ.
  std::string print_template_params(const clang::TemplateParameterList* params) const {
    if (!params) return {};
    std::string out = "<";
    for (unsigned i = 0; i < params->size(); ++i) {
      if (i) out += ", ";
      const clang::NamedDecl* p = params->getParam(i);
      if (const auto* type_param = llvm::dyn_cast<clang::TemplateTypeParmDecl>(p)) {
        out += "typename";
        if (type_param->isParameterPack()) out += "...";
      } else if (const auto* value_param = llvm::dyn_cast<clang::NonTypeTemplateParmDecl>(p)) {
        out += value_param->getType().getCanonicalType().getAsString(policy_);
        if (value_param->isParameterPack()) out += "...";
      } else if (const auto* template_param = llvm::dyn_cast<clang::TemplateTemplateParmDecl>(p)) {
        out += "template" + print_template_params(template_param->getTemplateParameters()) + " class";
        if (template_param->isParameterPack()) out += "...";
      } else {
        out += "?";
      }
    }
    return out + ">";
  }

  TemplateInfo template_info(const clang::NamedDecl* d) {
    TemplateInfo info;
    if (const auto* fd = llvm::dyn_cast<clang::FunctionDecl>(d)) {
      if (const clang::FunctionTemplateDecl* tmpl = fd->getDescribedFunctionTemplate()) {
        info.role = TemplateRole::primary;
        info.parameters = print_template_params(tmpl->getTemplateParameters());
      } else if (fd->getTemplateSpecializationKind() == clang::TSK_ExplicitSpecialization) {
        info.role = TemplateRole::explicit_specialization;
        if (const auto* args = fd->getTemplateSpecializationArgs()) info.arguments = print_template_args(*args);
      }
    } else if (const auto* rd = llvm::dyn_cast<clang::CXXRecordDecl>(d)) {
      if (const clang::ClassTemplateDecl* tmpl = rd->getDescribedClassTemplate()) {
        info.role = TemplateRole::primary;
        info.parameters = print_template_params(tmpl->getTemplateParameters());
      } else if (const auto* partial = llvm::dyn_cast<clang::ClassTemplatePartialSpecializationDecl>(rd)) {
        info.role = TemplateRole::partial_specialization;
        info.parameters = print_template_params(partial->getTemplateParameters());
        info.arguments = print_template_args(partial->getTemplateArgs());
      } else if (const auto* spec = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(rd)) {
        info.role = TemplateRole::explicit_specialization;
        info.arguments = print_template_args(spec->getTemplateArgs());
      }
    }
    return info;
  }

  static SymbolKind kind_of(const clang::NamedDecl* d) {
    if (llvm::isa<clang::CXXConstructorDecl>(d)) return SymbolKind::constructor;
    if (llvm::isa<clang::CXXDestructorDecl>(d)) return SymbolKind::destructor;
    if (llvm::isa<clang::CXXConversionDecl>(d)) return SymbolKind::conversion_function;
    if (const auto* fd = llvm::dyn_cast<clang::FunctionDecl>(d)) {
      if (fd->isOverloadedOperator()) return SymbolKind::operator_function;
      return llvm::isa<clang::CXXMethodDecl>(fd) ? SymbolKind::method : SymbolKind::function;
    }
    if (llvm::isa<clang::NamespaceDecl>(d)) return SymbolKind::namespace_;
    if (const auto* rd = llvm::dyn_cast<clang::RecordDecl>(d)) {
      if (rd->getIdentifier() == nullptr && !rd->getTypedefNameForAnonDecl() &&
          !llvm::isa<clang::ClassTemplateSpecializationDecl>(rd)) {
        return SymbolKind::anonymous_type;
      }
      if (rd->isUnion()) return SymbolKind::union_;
      if (rd->isClass()) return SymbolKind::class_;
      return SymbolKind::struct_;
    }
    if (llvm::isa<clang::EnumDecl>(d)) return SymbolKind::enum_;
    if (llvm::isa<clang::EnumConstantDecl>(d)) return SymbolKind::enum_constant;
    if (llvm::isa<clang::FieldDecl>(d)) return SymbolKind::field;
    if (llvm::isa<clang::VarDecl>(d)) return SymbolKind::variable;
    if (llvm::isa<clang::TypedefNameDecl>(d)) return SymbolKind::type_alias;
    return SymbolKind::variable;
  }

  // Name for identity: a typedef'd anonymous struct takes the typedef name;
  // the destructor of an unnamed class (closure, anonymous type) is "~" so no
  // compiler path/line spelling enters identity.
  static std::string name_of(const clang::NamedDecl* d) {
    if (const auto* rd = llvm::dyn_cast<clang::RecordDecl>(d)) {
      if (rd->getIdentifier() == nullptr) {
        if (const clang::TypedefNameDecl* td = rd->getTypedefNameForAnonDecl()) return td->getNameAsString();
        return {};
      }
    }
    if (const auto* dtor = llvm::dyn_cast<clang::CXXDestructorDecl>(d)) {
      if (is_unnamed_record(dtor->getParent())) return "~";
    }
    if (const auto* ctor = llvm::dyn_cast<clang::CXXConstructorDecl>(d)) {
      if (is_unnamed_record(ctor->getParent())) return kUnnamedConstructorName;  // fixed token, never the compiler's spelling
    }
    return d->getNameAsString();
  }

  // Conversion functions are named after their target type; when that type
  // involves a local/unnamed leaf the name goes through the same atomization
  // as signatures so no compiler path spelling enters identity.
  std::string conversion_name(const clang::CXXConversionDecl* conv) {
    const clang::QualType target = conv->getConversionType().getCanonicalType();
    if (!contains_local_type(target)) return conv->getNameAsString();
    const std::optional<std::string> atom = atom_type(target);
    if (!atom) {
      limit("conversion function to an unsupported local type form: symbol skipped");
      return {};
    }
    return "operator " + *atom;
  }

  // ---- type spelling for identity ---------------------------------------------
  // Named, non-local types keep the established canonical printer (and thus
  // every published ID). A type that involves a local or unnamed leaf (closure,
  // anonymous record, block-scope class/enum/alias) is spelled with a tagged
  // canonical representation whose leaves are the entities' stable IDs, so no
  // path, line, body text or colliding bare name enters identity, while every
  // type constructor, qualifier and function ABI attribute stays distinct.
  // Unsupported forms fail (nullopt) and the dependent symbol is skipped with a
  // precise limit instead of an invented identity.

  bool is_local_leaf(const clang::TagDecl* tag) const {
    if (const auto* rd = llvm::dyn_cast<clang::CXXRecordDecl>(tag); rd && rd->isLambda()) return true;
    if (const auto* rd = llvm::dyn_cast<clang::RecordDecl>(tag); rd && is_unnamed_record(rd)) return true;
    return is_local_tag(tag);
  }

  // Whether a type involves a local/unnamed leaf. `unknown` (depth budget
  // exhausted) is NOT "proven non-local": such a type goes through the atom
  // encoder, which either produces a stable encoding or fails with a limit.
  enum class Locality { none, local, unknown };

  static Locality merge(Locality a, Locality b) {
    if (a == Locality::local || b == Locality::local) return Locality::local;
    if (a == Locality::unknown || b == Locality::unknown) return Locality::unknown;
    return Locality::none;
  }

  Locality locality(clang::QualType type, unsigned depth = 0) const {
    if (type.isNull()) return Locality::none;
    if (depth > kTypeDepthLimit) return Locality::unknown;
    const clang::Type* t = type.getCanonicalType().getTypePtr();
    if (const auto* tag = t->getAsTagDecl()) return tag_locality(tag, depth);
    if (const auto* p = llvm::dyn_cast<clang::PointerType>(t)) return locality(p->getPointeeType(), depth + 1);
    if (const auto* r = llvm::dyn_cast<clang::ReferenceType>(t)) return locality(r->getPointeeType(), depth + 1);
    if (const auto* m = llvm::dyn_cast<clang::MemberPointerType>(t)) {
      Locality out = locality(m->getPointeeType(), depth + 1);
      if (const clang::CXXRecordDecl* cls = m->getMostRecentCXXRecordDecl()) {
        out = merge(out, locality(ctx_.getCanonicalTagType(cls), depth + 1));  // class incl. its template arguments
      } else {
        // A dependent member-pointer class has no record decl; the stored
        // nested-name-specifier is what the printer spells (`Box<decltype(g), T>::*`).
        out = merge(out, qualifier_locality(m->getQualifier(), depth + 1));
      }
      return out;
    }
    if (const auto* a = llvm::dyn_cast<clang::ArrayType>(t)) {
      Locality out = locality(a->getElementType(), depth + 1);
      // A dependent bound is printed as its written expression, whose types are
      // printed canonically: `int (*)[sizeof(decltype(g))]` must not be non-local.
      if (const auto* dep = llvm::dyn_cast<clang::DependentSizedArrayType>(a)) {
        out = merge(out, expr_locality(dep->getSizeExpr(), depth + 1));
      } else if (const auto* vla = llvm::dyn_cast<clang::VariableArrayType>(a)) {
        out = merge(out, expr_locality(vla->getSizeExpr(), depth + 1));
      }
      return out;
    }
    if (const auto* f = llvm::dyn_cast<clang::FunctionType>(t)) {
      Locality out = locality(f->getReturnType(), depth + 1);
      if (const auto* proto = llvm::dyn_cast<clang::FunctionProtoType>(f)) {
        for (const clang::QualType p : proto->getParamTypes()) out = merge(out, locality(p, depth + 1));
        if (const clang::Expr* noexcept_expr = proto->getNoexceptExpr()) {
          out = merge(out, expr_locality(noexcept_expr, depth + 1));  // dependent specification is printed verbatim
        }
      }
      return out;
    }
    // Wrappers: classified by their contained types, never by their own shape.
    if (const auto* atomic = llvm::dyn_cast<clang::AtomicType>(t)) return locality(atomic->getValueType(), depth + 1);
    if (const auto* vec = llvm::dyn_cast<clang::VectorType>(t)) return locality(vec->getElementType(), depth + 1);
    if (const auto* cx = llvm::dyn_cast<clang::ComplexType>(t)) return locality(cx->getElementType(), depth + 1);
    if (const auto* block = llvm::dyn_cast<clang::BlockPointerType>(t)) return locality(block->getPointeeType(), depth + 1);
    if (const auto* pack = llvm::dyn_cast<clang::PackExpansionType>(t)) return locality(pack->getPattern(), depth + 1);
    if (const auto* pipe = llvm::dyn_cast<clang::PipeType>(t)) return locality(pipe->getElementType(), depth + 1);
    if (const auto* spec = llvm::dyn_cast<clang::TemplateSpecializationType>(t)) {  // dependent specialization
      Locality out = Locality::none;
      for (const clang::TemplateArgument& arg : spec->template_arguments()) out = merge(out, arg_locality(arg, depth + 1));
      return out;
    }
    if (const auto* dep_vec = llvm::dyn_cast<clang::DependentSizedExtVectorType>(t)) {
      return merge(locality(dep_vec->getElementType(), depth + 1), expr_locality(dep_vec->getSizeExpr(), depth + 1));
    }
    if (t->isBuiltinType() || llvm::isa<clang::BitIntType>(t)) return Locality::none;
    // The only proven dependent LEAF is a template parameter (`type-parameter-N-M`).
    if (llvm::isa<clang::TemplateTypeParmType>(t)) return Locality::none;
    // A dependent qualified name (`typename Box<decltype(g), T>::type`) and an
    // unresolved using type are NOT leaves: their qualifier can carry a
    // concrete local leaf, so the qualifier chain is classified.
    if (const auto* dependent = llvm::dyn_cast<clang::DependentNameType>(t)) {
      return qualifier_locality(dependent->getQualifier(), depth + 1);
    }
    if (const auto* unresolved = llvm::dyn_cast<clang::UnresolvedUsingType>(t)) {
      const clang::UnresolvedUsingTypenameDecl* decl = unresolved->getDecl();
      return decl ? qualifier_locality(decl->getQualifier(), depth + 1) : Locality::unknown;
    }
    // Every other unclassified form (dependent decltype/typeof, deduced
    // placeholders, ObjC, ...) is NOT proven non-local and goes through the
    // encoder, which fails with a limit.
    return Locality::unknown;
  }

  // Locality contributed by the enclosing declaration contexts alone. The
  // canonical printer writes the whole owner chain, so `Outer<decltype(g)>::Inner`
  // carries the closure even though `Inner` is neither local nor a specialization.
  Locality owner_locality(const clang::Decl* d, unsigned depth) const {
    if (depth > kTypeDepthLimit) return Locality::unknown;
    Locality out = Locality::none;
    for (const clang::DeclContext* ctx = d->getDeclContext(); ctx; ctx = ctx->getParent()) {
      if (const auto* spec = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(ctx)) {
        out = merge(out, spec_args_locality(spec, depth));
      }
    }
    return out;
  }

  Locality spec_args_locality(const clang::ClassTemplateSpecializationDecl* spec, unsigned depth) const {
    Locality out = Locality::none;
    for (const clang::TemplateArgument& arg : spec->getTemplateArgs().asArray()) {
      out = merge(out, arg_locality(arg, depth + 1));
    }
    return out;
  }

  // Whether the declaration itself is a template specialization whose own
  // arguments the printer does not spell (functions and variable templates).
  static bool has_own_specialization_arguments(const clang::ValueDecl* d) {
    if (const auto* fd = llvm::dyn_cast<clang::FunctionDecl>(d)) {
      return fd->getTemplateSpecializationArgs() != nullptr;
    }
    return llvm::isa<clang::VarTemplateSpecializationDecl>(d);
  }

  // Whether the referenced declaration's stable ID cannot be trusted to stand
  // for this specialization: template instantiations are never published as
  // nodes, an implicit function specialization records no arguments in its key,
  // and a member reached through a class specialization can fold onto the
  // primary's member. Explicit specializations do record their arguments, but
  // a declaration-valued specialization identity is unsupported here either
  // way, so the whole shape is refused rather than partly encoded.
  static bool declaration_identity_folds(const clang::ValueDecl* d) {
    if (has_own_specialization_arguments(d)) return true;
    if (const auto* fd = llvm::dyn_cast<clang::FunctionDecl>(d)) {
      if (fd->getTemplateSpecializationKind() != clang::TSK_Undeclared) return true;
    }
    if (const auto* vd = llvm::dyn_cast<clang::VarDecl>(d)) {
      if (vd->getTemplateSpecializationKind() != clang::TSK_Undeclared) return true;
    }
    for (const clang::DeclContext* ctx = d->getDeclContext(); ctx; ctx = ctx->getParent()) {
      if (llvm::isa<clang::ClassTemplateSpecializationDecl>(ctx)) return true;
    }
    return false;
  }

  Locality tag_locality(const clang::TagDecl* tag, unsigned depth) const {
    if (depth > kTypeDepthLimit) return Locality::unknown;
    if (is_local_leaf(tag)) return Locality::local;
    Locality out = owner_locality(tag, depth);
    if (const auto* spec = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(tag)) {
      out = merge(out, spec_args_locality(spec, depth));
    }
    return out;
  }

  // Every type mentioned by a dependent expression: the expression itself is
  // printed as written, but its types print canonically, so `sizeof(decltype(g))`
  // would spell the closure's path while `sizeof(T)` stays safe.
  class ExprTypeScanner : public clang::RecursiveASTVisitor<ExprTypeScanner> {
   public:
    ExprTypeScanner(const FactsVisitor& owner, unsigned depth) : owner_(owner), depth_(depth) {}
    bool shouldWalkTypesOfTypeLocs() const { return true; }
    bool VisitExpr(const clang::Expr* e) {
      add(e->getType());
      return true;
    }
    bool VisitTypeLoc(clang::TypeLoc loc) {
      add(loc.getType());
      return true;
    }
    Locality result() const { return out_; }

   private:
    void add(clang::QualType type) { out_ = merge(out_, owner_.locality(type, depth_)); }

    const FactsVisitor& owner_;
    unsigned depth_;
    Locality out_ = Locality::none;
  };

  Locality expr_locality(const clang::Expr* e, unsigned depth) const {
    if (!e) return Locality::unknown;
    if (depth > kTypeDepthLimit) return Locality::unknown;
    ExprTypeScanner scanner(*this, depth + 1);
    scanner.TraverseStmt(const_cast<clang::Expr*>(e));
    return scanner.result();
  }

  // Classifies a nested-name-specifier chain: a type component is classified
  // as a type (its own prefix is part of the type), a namespace component
  // continues with its prefix, the global specifier is non-local, anything
  // else is unknown.
  Locality qualifier_locality(clang::NestedNameSpecifier qualifier, unsigned depth) const {
    if (depth > kTypeDepthLimit) return Locality::unknown;
    switch (qualifier.getKind()) {
      case clang::NestedNameSpecifier::Kind::Null:
      case clang::NestedNameSpecifier::Kind::Global:
        return Locality::none;
      case clang::NestedNameSpecifier::Kind::Type:
        return locality(clang::QualType(qualifier.getAsType(), 0), depth + 1);
      case clang::NestedNameSpecifier::Kind::Namespace:
        return qualifier_locality(qualifier.getAsNamespaceAndPrefix().Prefix, depth + 1);
      default:
        return Locality::unknown;
    }
  }

  Locality arg_locality(const clang::TemplateArgument& arg, unsigned depth) const {
    if (depth > kTypeDepthLimit) return Locality::unknown;
    switch (arg.getKind()) {
      case clang::TemplateArgument::Type:
        return locality(arg.getAsType(), depth);
      case clang::TemplateArgument::Declaration: {
        Locality out = locality(arg.getParamTypeForDecl(), depth);
        // A class-type non-type argument is a TemplateParamObjectDecl holding
        // the value: always encoded canonically (the raw printer would write
        // `K2{{0, 0}}` and `K2{{}}` differently for one value).
        if (llvm::isa_and_nonnull<clang::TemplateParamObjectDecl>(arg.getAsDecl())) return Locality::local;
        const clang::ValueDecl* decl = arg.getAsDecl();
        if (!decl) return out;
        if (decl->getParentFunctionOrMethod() != nullptr) out = Locality::local;
        // The printer spells the referenced declaration with its owner chain, so
        // `&Holder<decltype(g)>::value` carries the closure into the key.
        out = merge(out, owner_locality(decl, depth));
        // The printer spells the declaration's OWN specialization arguments
        // nowhere: `&target<int>` and `&target<double>` both print `&target`.
        // Not a locality fact, but the same consequence - the established printer
        // is not injective here, so it must not be used (compare NullPtr below).
        if (has_own_specialization_arguments(decl)) out = Locality::local;
        return out;
      }
      case clang::TemplateArgument::StructuralValue:
        // Values are always encoded canonically (see atom_value), so a
        // structural argument never takes the raw printer.
        return merge(Locality::local, locality(arg.getStructuralValueType(), depth));
      case clang::TemplateArgument::NullPtr:
        // The raw printer writes every typed null as `nullptr`, collapsing
        // `Wrap<(A*)nullptr>` with `Wrap<(B*)nullptr>`: always encode the type.
        return Locality::local;
      case clang::TemplateArgument::Integral:
        // A block-local enum argument prints as a bare enumerator name; its
        // canonical type decides. Non-local integrals keep the raw spelling.
        return locality(arg.getIntegralType(), depth);
      case clang::TemplateArgument::Pack: {
        Locality out = Locality::none;
        for (const clang::TemplateArgument& p : arg.pack_elements()) out = merge(out, arg_locality(p, depth + 1));
        return out;
      }
      case clang::TemplateArgument::Expression:
        // A dependent expression argument is printed verbatim: classify the
        // types it mentions instead of assuming it is non-local.
        return expr_locality(arg.getAsExpr(), depth + 1);
      case clang::TemplateArgument::Template:
      case clang::TemplateArgument::TemplateExpansion: {
        // A template name cannot itself be block-local, but it is printed with
        // its owner chain (`Outer<decltype(g)>::Inner`).
        const clang::TemplateDecl* decl = arg.getAsTemplateOrTemplatePattern().getAsTemplateDecl();
        return decl ? owner_locality(decl, depth) : Locality::unknown;
      }
      default:
        return Locality::unknown;  // never assume an unclassified argument kind is non-local
    }
  }

  bool contains_local_type(clang::QualType type) const { return locality(type) != Locality::none; }
  bool template_arg_has_local(const clang::TemplateArgument& arg, unsigned depth) const {
    return arg_locality(arg, depth) != Locality::none;
  }

  // Stable-ID atom of a local/unnamed tag leaf.
  std::optional<std::string> leaf_atom(const clang::TagDecl* tag) {
    std::optional<StableId> id;
    const char* label = "local";
    if (const auto* rd = llvm::dyn_cast<clang::CXXRecordDecl>(tag); rd && rd->isLambda()) {
      id = lambda_id(rd);
      label = "lambda";
    } else if (const auto* unnamed = llvm::dyn_cast<clang::RecordDecl>(tag); unnamed && is_unnamed_record(unnamed)) {
      id = symbol_id(unnamed);
      label = "anon";
    } else {
      id = symbol_id(tag);
    }
    if (!id) return std::nullopt;  // recursion guard hit or unmodelled: no invented identity
    return std::string("[") + label + " " + id->value + "]";
  }

  std::optional<std::string> atom_type(clang::QualType type, unsigned depth = 0) {
    if (depth > kTypeDepthLimit || type.isNull()) return std::nullopt;
    const clang::SplitQualType split = type.getCanonicalType().split();
    std::string quals;
    if (split.Quals.hasConst()) quals += "const ";
    if (split.Quals.hasVolatile()) quals += "volatile ";
    if (split.Quals.hasRestrict()) quals += "restrict ";
    if (split.Quals.hasAddressSpace() || split.Quals.hasObjCLifetime()) return std::nullopt;
    const clang::Type* t = split.Ty;
    std::optional<std::string> inner;
    if (const auto* tag = t->getAsTagDecl()) {
      if (is_local_leaf(tag)) {
        inner = leaf_atom(tag);
      } else if (owner_locality(tag, depth) != Locality::none) {
        return std::nullopt;  // named type inside a local-carrying specialization: unsupported composite
      } else if (const auto* spec = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(tag)) {
        std::string out = spec->getSpecializedTemplate()->getQualifiedNameAsString() + "<";
        bool first = true;
        for (const clang::TemplateArgument& arg : spec->getTemplateArgs().asArray()) {
          const auto a = atom_template_arg(arg, depth + 1);
          if (!a) return std::nullopt;
          if (!first) out += ", ";
          first = false;
          out += *a;
        }
        inner = out + ">";
      } else {
        inner = clang::QualType(t, 0).getAsString(policy_);  // named, non-local: established spelling
      }
    } else if (t->isBuiltinType()) {
      inner = clang::QualType(t, 0).getAsString(policy_);
    } else if (const auto* p = llvm::dyn_cast<clang::PointerType>(t)) {
      if (const auto a = atom_type(p->getPointeeType(), depth + 1)) inner = "ptr(" + *a + ")";
    } else if (const auto* lref = llvm::dyn_cast<clang::LValueReferenceType>(t)) {
      if (const auto a = atom_type(lref->getPointeeType(), depth + 1)) inner = "ref(" + *a + ")";
    } else if (const auto* rref = llvm::dyn_cast<clang::RValueReferenceType>(t)) {
      if (const auto a = atom_type(rref->getPointeeType(), depth + 1)) inner = "rref(" + *a + ")";
    } else if (const auto* m = llvm::dyn_cast<clang::MemberPointerType>(t)) {
      const clang::CXXRecordDecl* cls = m->getMostRecentCXXRecordDecl();
      if (!cls) return std::nullopt;
      // The class goes through the same encoder: `int Box<decltype(g1)>::*` keeps
      // its specialization arguments, never a bare qualified name.
      const auto c = atom_type(ctx_.getCanonicalTagType(cls), depth + 1);
      const auto a = atom_type(m->getPointeeType(), depth + 1);
      if (c && a) inner = "memptr(" + *c + ", " + *a + ")";
    } else if (const auto* carr = llvm::dyn_cast<clang::ConstantArrayType>(t)) {
      if (const auto e = atom_type(carr->getElementType(), depth + 1)) {
        inner = "arr[" + llvm::toString(carr->getSize(), 10, false) + "](" + *e + ")";
      }
    } else if (const auto* iarr = llvm::dyn_cast<clang::IncompleteArrayType>(t)) {
      if (const auto e = atom_type(iarr->getElementType(), depth + 1)) inner = "arr[](" + *e + ")";
    } else if (const auto* proto = llvm::dyn_cast<clang::FunctionProtoType>(t)) {
      inner = atom_function(proto, depth + 1);
    } else if (llvm::isa<clang::TemplateTypeParmType>(t)) {
      inner = clang::QualType(t, 0).getAsString(policy_);  // proven leaf: `type-parameter-N-M`, path-free
    }
    // Everything else (atomic, vector, complex, block pointers, pipes, packs,
    // non-prototype functions, dependent composites, ObjC, unclassified) is an
    // unsupported form: no encoding, the symbol is omitted with a limit.
    if (!inner) return std::nullopt;
    return quals + *inner;
  }

  // Function type with its ABI: calling convention, exception specification,
  // parameters, variadic marker, return type and (for member pointers) the
  // method qualifiers. Plain, noexcept and __vectorcall overloads differ.
  std::optional<std::string> atom_function(const clang::FunctionProtoType* proto, unsigned depth) {
    const auto ret = atom_type(proto->getReturnType(), depth);
    if (!ret) return std::nullopt;
    std::string out = std::string("fn[cc=") + clang::FunctionType::getNameForCallConv(proto->getCallConv()).str();
    switch (proto->getExceptionSpecType()) {
      case clang::EST_None:
        break;
      case clang::EST_BasicNoexcept:
      case clang::EST_NoexceptTrue:
      case clang::EST_DynamicNone:
        out += ",noexcept";
        break;
      case clang::EST_NoexceptFalse:
        break;
      case clang::EST_Dynamic:
        out += ",throw(...)";  // dynamic specification: which types is not part of the type identity here
        break;
      default:
        return std::nullopt;  // dependent or unevaluated/uninstantiated specification: unsupported
    }
    out += "](" + *ret + ")(";
    for (unsigned i = 0; i < proto->getNumParams(); ++i) {
      const auto p = atom_type(proto->getParamType(i), depth);
      if (!p) return std::nullopt;
      if (i) out += ", ";
      out += *p;
    }
    if (proto->isVariadic()) out += proto->getNumParams() ? ", ..." : "...";
    out += ")";
    if (proto->getMethodQuals().hasConst()) out += " const";
    if (proto->getMethodQuals().hasVolatile()) out += " volatile";
    if (proto->getRefQualifier() == clang::RQ_LValue) out += " &";
    if (proto->getRefQualifier() == clang::RQ_RValue) out += " &&";
    return out;
  }

  // Template argument with kind, canonical type and actual value.
  std::optional<std::string> atom_template_arg(const clang::TemplateArgument& arg, unsigned depth) {
    switch (arg.getKind()) {
      case clang::TemplateArgument::Type:
        return atom_type(arg.getAsType(), depth);
      case clang::TemplateArgument::Integral: {
        const auto t = atom_type(arg.getIntegralType(), depth);
        if (!t) return std::nullopt;
        return "int(" + *t + "=" + llvm::toString(arg.getAsIntegral(), 10) + ")";
      }
      case clang::TemplateArgument::NullPtr: {
        const auto t = atom_type(arg.getNullPtrType(), depth);
        return t ? std::optional("nullptr(" + *t + ")") : std::nullopt;
      }
      case clang::TemplateArgument::Declaration: {
        const clang::ValueDecl* decl = arg.getAsDecl();
        if (!decl) return std::nullopt;
        if (const auto* object = llvm::dyn_cast<clang::TemplateParamObjectDecl>(decl)) {
          const auto t = atom_type(object->getType(), depth);
          const auto v = atom_value(object->getValue(), object->getType(), depth + 1);
          if (!t || !v) return std::nullopt;
          return "val(" + *t + "=" + *v + ")";
        }
        // `decl(T=&<id>)` is truthful only when the stable ID identifies exactly
        // this declaration. It does not when the key folds onto a pattern, and no
        // identity is invented for that case in this increment (see the limits).
        if (declaration_identity_folds(decl)) {
          limit(has_own_specialization_arguments(decl)
                    ? "declaration-valued template argument names a template specialization (`&f<T>`, `&v<T>`): a "
                      "specialization-valued declaration identity is unsupported in this increment and could fold "
                      "onto the pattern, so the symbol is skipped instead of receiving a colliding identity"
                    : "declaration-valued template argument names a member of a template specialization: that "
                      "declaration identity is unsupported in this increment and could fold onto the pattern's "
                      "member, so the symbol is skipped instead of receiving a colliding identity");
          return std::nullopt;
        }
        const auto t = atom_type(arg.getParamTypeForDecl(), depth);
        if (!t) return std::nullopt;
        const auto id = symbol_id(decl);
        if (!id) return std::nullopt;
        return "decl(" + *t + "=&" + id->value + ")";
      }
      case clang::TemplateArgument::StructuralValue: {
        const auto t = atom_type(arg.getStructuralValueType(), depth);
        const auto v = atom_value(arg.getAsStructuralValue(), arg.getStructuralValueType(), depth + 1);
        if (!t || !v) return std::nullopt;
        return "val(" + *t + "=" + *v + ")";
      }
      case clang::TemplateArgument::Pack: {
        std::string out = "pack(";
        bool first = true;
        for (const clang::TemplateArgument& p : arg.pack_elements()) {
          const auto a = atom_template_arg(p, depth + 1);
          if (!a) return std::nullopt;
          if (!first) out += ", ";
          first = false;
          out += *a;
        }
        return out + ")";
      }
      default:
        return std::nullopt;  // template, template-expansion, expression (dependent), null
    }
  }

  // Constant value of a structural non-type argument, recursively by the
  // compiler's APValue: `K{1}` and `K{2}` differ, member order is the layout.
  std::optional<std::string> atom_value(const clang::APValue& value, clang::QualType type, unsigned depth) {
    if (depth > kTypeDepthLimit) return std::nullopt;
    if (value.isInt()) return llvm::toString(value.getInt(), 10);
    if (value.isFloat()) return "f:" + llvm::toString(value.getFloat().bitcastToAPInt(), 16, false);
    if (value.isLValue()) {
      if (value.isNullPointer()) return std::string("nullptr");  // only meaningful for lvalues
      if (value.hasLValuePath() && !value.getLValuePath().empty()) return std::nullopt;
      if (!value.getLValueOffset().isZero()) return std::nullopt;
      const auto* decl = value.getLValueBase().dyn_cast<const clang::ValueDecl*>();
      if (!decl) return std::nullopt;
      const auto id = symbol_id(decl);
      return id ? std::optional("&" + id->value) : std::nullopt;
    }
    if (value.isStruct()) {
      const clang::CXXRecordDecl* rd = type->getAsCXXRecordDecl();
      if (!rd) return std::nullopt;
      std::string out = "{";
      unsigned base_index = 0;
      for (const clang::CXXBaseSpecifier& base : rd->bases()) {
        if (base_index >= value.getStructNumBases()) return std::nullopt;
        const auto b = atom_value(value.getStructBase(base_index++), base.getType(), depth + 1);
        if (!b) return std::nullopt;
        out += "base:" + *b + ";";
      }
      unsigned field_index = 0;
      for (const clang::FieldDecl* field : rd->fields()) {
        if (field_index >= value.getStructNumFields()) return std::nullopt;
        const auto f = atom_value(value.getStructField(field_index++), field->getType(), depth + 1);
        if (!f) return std::nullopt;
        out += *f + ";";
      }
      return out + "}";
    }
    if (value.isArray()) {
      // Canonical expansion: every element is written out, whether the
      // compiler stored it as an initialized prefix or as the filler, so
      // `K{{0, 0}}` and `K{}` (same value) encode identically.
      const clang::ArrayType* at = ctx_.getAsArrayType(type);
      if (!at) return std::nullopt;
      const unsigned size = value.getArraySize();
      if (size > kValueArrayLimit) return std::nullopt;
      std::string out = "arr[" + std::to_string(size) + "]{";
      for (unsigned i = 0; i < size; ++i) {
        const clang::APValue* element = nullptr;
        if (i < value.getArrayInitializedElts()) {
          element = &value.getArrayInitializedElt(i);
        } else if (value.hasArrayFiller()) {
          element = &value.getArrayFiller();
        } else {
          return std::nullopt;
        }
        const auto e = atom_value(*element, at->getElementType(), depth + 1);
        if (!e) return std::nullopt;
        out += *e + ";";
      }
      return out + "}";
    }
    if (value.isUnion()) {
      const clang::FieldDecl* field = value.getUnionField();
      if (!field) return std::optional<std::string>("union{}");
      const auto v = atom_value(value.getUnionValue(), field->getType(), depth + 1);
      return v ? std::optional("union{" + field->getNameAsString() + "=" + *v + "}") : std::nullopt;
    }
    if (value.isMemberPointer()) {
      const clang::ValueDecl* decl = value.getMemberPointerDecl();
      if (!decl) return std::optional<std::string>("memptr{null}");
      const auto id = symbol_id(decl);
      return id ? std::optional("memptr{" + id->value + "}") : std::nullopt;
    }
    return std::nullopt;  // vector, complex, fixed-point, indeterminate, address-label differences
  }

  // Parameter/argument spelling for identity: unchanged printer unless a local
  // leaf is involved. Records a precise limit and returns nullopt when the
  // local form is unsupported.
  std::optional<std::string> type_spelling(clang::QualType type) {
    if (!contains_local_type(type)) return type.getCanonicalType().getAsString(policy_);
    const auto atom = atom_type(type);
    if (!atom) limit("type uses an unsupported form for identity; identity not invented");
    return atom;
  }

  // Normalized signature. Empty when a parameter type could not be spelled
  // without a fabricated identity (the key then fails validation and the
  // symbol is skipped with the limit recorded above).
  std::string signature_of(const clang::FunctionDecl* fd) {
    std::string out = "(";
    if (const auto* proto = fd->getType()->getAs<clang::FunctionProtoType>()) {
      for (unsigned i = 0; i < proto->getNumParams(); ++i) {
        if (i) out += ", ";
        const auto spelled = type_spelling(proto->getParamType(i));
        if (!spelled) return {};
        out += *spelled;
      }
      if (proto->isVariadic()) out += proto->getNumParams() ? ", ..." : "...";
    }
    out += ")";
    if (const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(fd)) {
      if (method->isConst()) out += " const";
      if (method->isVolatile()) out += " volatile";
      switch (method->getRefQualifier()) {
        case clang::RQ_LValue:
          out += " &";
          break;
        case clang::RQ_RValue:
          out += " &&";
          break;
        default:
          break;
      }
    }
    return out;
  }

  // Owner chain from the DeclContext parents (outermost first). Returns false
  // for contexts this increment does not model.
  bool owner_chain(const clang::Decl* d, std::vector<OwnerComponent>& out) {
    std::vector<OwnerComponent> reversed;
    for (const clang::DeclContext* ctx = d->getDeclContext(); ctx && !llvm::isa<clang::TranslationUnitDecl>(ctx);
         ctx = ctx->getParent()) {
      if (llvm::isa<clang::LinkageSpecDecl>(ctx) || llvm::isa<clang::ExportDecl>(ctx)) continue;
      OwnerComponent c;
      if (const auto* ns = llvm::dyn_cast<clang::NamespaceDecl>(ctx)) {
        c.kind = SymbolKind::namespace_;
        c.name = ns->isAnonymousNamespace() ? std::string{} : ns->getNameAsString();
      } else if (const auto* rd = llvm::dyn_cast<clang::CXXRecordDecl>(ctx)) {
        if (rd->isLambda()) {
          c.kind = SymbolKind::lambda;
          c.normalized_signature = signature_of(rd->getLambdaCallOperator());
        } else {
          c.kind = kind_of(rd);  // unnamed records stay `anonymous_type` owners (members get LocalScope)
          c.name = name_of(rd);
          const TemplateInfo ti = template_info(rd);
          c.template_role = ti.role;
          c.template_parameters = ti.parameters;
          c.template_arguments = ti.arguments;
        }
      } else if (const auto* plain_record = llvm::dyn_cast<clang::RecordDecl>(ctx)) {
        c.kind = plain_record->isUnion() ? SymbolKind::union_ : SymbolKind::struct_;
        c.name = name_of(plain_record);
      } else if (const auto* ed = llvm::dyn_cast<clang::EnumDecl>(ctx)) {
        c.kind = SymbolKind::enum_;
        c.name = ed->getNameAsString();
        if (c.name.empty()) {
          limit("member of an unnamed enum was not modelled");
          return false;
        }
      } else if (const auto* fd = llvm::dyn_cast<clang::FunctionDecl>(ctx)) {
        if (const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(fd); method && method->getParent()->isLambda()) {
          continue;  // the closure record above provides the lambda component
        }
        c.kind = kind_of(fd);
        c.name = fd->getNameAsString();
        c.normalized_signature = signature_of(fd);
        const TemplateInfo ti = template_info(fd);
        c.template_role = ti.role;
        c.template_parameters = ti.parameters;
        c.template_arguments = ti.arguments;
      } else {
        limit(std::string("owner context not modelled: ") + ctx->getDeclKindName());
        return false;
      }
      reversed.push_back(std::move(c));
    }
    out.assign(reversed.rbegin(), reversed.rend());
    return true;
  }

  bool decl_in_root(const clang::Decl* d) {
    const clang::SourceLocation loc = sm_.getExpansionLoc(d->getLocation());
    if (loc.isInvalid()) return false;
    return file_info(sm_.getFileID(loc)).in_root;
  }

  // File scope for internal linkage: the repository-relative file of the
  // canonical declaration, or a stable pseudo-scope for external files.
  std::optional<RepoRelativePath> file_scope_of(const clang::Decl* d) {
    const clang::SourceLocation loc = sm_.getExpansionLoc(d->getCanonicalDecl()->getLocation());
    if (loc.isInvalid()) return std::nullopt;
    const FileInfo& fi = file_info(sm_.getFileID(loc));
    if (fi.in_root && fi.repo) return fi.repo;
    return RepoRelativePath{"external/" + sha256(fi.abs_utf8).hex().substr(0, 16), fi.abs_utf8};
  }

  std::optional<CanonicalKey> lambda_key(const clang::CXXRecordDecl* closure) {
    const auto info_it = lambdas_.find(closure);
    if (info_it == lambdas_.end() || !info_it->second.enclosing) {
      limit("lambda without a modelled enclosing entity was not recorded");
      return std::nullopt;
    }
    const auto enclosing_id = symbol_id(llvm::cast<clang::NamedDecl>(info_it->second.enclosing));
    if (!enclosing_id) return std::nullopt;
    CanonicalKey key;
    key.repository_member = decl_in_root(closure) ? unit_.request->repository_member : kExternalMember;
    key.kind = SymbolKind::lambda;
    if (!owner_chain(closure, key.owner_chain)) return std::nullopt;
    key.normalized_signature = signature_of(closure->getLambdaCallOperator());
    // The exceptional anchor applies only when the closure has NO semantic
    // owner (translation-unit scope) and is anchored to an actual declarator;
    // every lambda that is valid otherwise keeps the established anchor and ID.
    const bool declarator_anchor = llvm::isa<clang::VarDecl>(info_it->second.enclosing) ||
                                   llvm::isa<clang::FieldDecl>(info_it->second.enclosing) ||
                                   llvm::isa<clang::EnumConstantDecl>(info_it->second.enclosing);
    const std::string anchor =
        key.owner_chain.empty() && declarator_anchor ? std::string(kLambdaDeclInitAnchor) : std::string("lambda");
    key.linkage = LocalScope{*enclosing_id, anchor, info_it->second.ordinal};
    return key;
  }

  std::optional<StableId> lambda_id(const clang::CXXRecordDecl* closure) {
    const auto cached = ids_.find(closure);
    if (cached != ids_.end()) return cached->second;
    const auto key = lambda_key(closure);
    std::optional<StableId> id;
    if (key && !validate_canonical_key(*key)) id = make_stable_id(*key);
    ids_.emplace(closure, id);
    return id;
  }

  // The declarator (variable/field) whose type is this unnamed record, e.g.
  // `struct { int a; } x;` -> x. Searched in the record's own context.
  const clang::NamedDecl* declarator_for_anonymous(const clang::RecordDecl* rd) const {
    const clang::Decl* canonical = rd->getCanonicalDecl();
    for (const clang::Decl* sibling : rd->getDeclContext()->decls()) {
      const auto* declarator = llvm::dyn_cast<clang::DeclaratorDecl>(sibling);
      if (!declarator || llvm::isa<clang::FunctionDecl>(declarator)) continue;
      // The compiler's unnamed field for an anonymous union is not a declarator anchor.
      if (declarator->isImplicit() || declarator->getName().empty()) continue;
      const clang::Type* t = declarator->getType()->getBaseElementTypeUnsafe();
      if (const auto* target = llvm::dyn_cast_or_null<clang::RecordDecl>(t->getAsTagDecl())) {
        if (target->getCanonicalDecl() == canonical) return declarator;
      }
    }
    return nullptr;
  }

  std::optional<CanonicalKey> key_of(const clang::NamedDecl* d) {
    if (const auto* rd = llvm::dyn_cast<clang::CXXRecordDecl>(d); rd && rd->isLambda()) return lambda_key(rd);
    CanonicalKey key;
    key.repository_member = decl_in_root(d) ? unit_.request->repository_member : kExternalMember;
    key.kind = kind_of(d);
    if (!owner_chain(d, key.owner_chain)) return std::nullopt;
    if (const auto* conv = llvm::dyn_cast<clang::CXXConversionDecl>(d)) {
      key.canonical_name = conversion_name(conv);
      if (key.canonical_name.empty()) return std::nullopt;
    } else {
      key.canonical_name = name_of(d);
    }
    if (const auto* fd = llvm::dyn_cast<clang::FunctionDecl>(d)) key.normalized_signature = signature_of(fd);
    const TemplateInfo ti = template_info(d);
    key.template_role = ti.role;
    key.template_parameters = ti.parameters;
    key.template_arguments = ti.arguments;

    // Members of a closure are anchored to the lambda callable, at namespace
    // scope as well as in a function (getParentFunctionOrMethod() is null for
    // a translation-unit-scope closure). The owner chain already carries the
    // lambda component; the anchor names the member kind and the destructor
    // keeps its established anchor.
    if (const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(d); method && method->getParent()->isLambda()) {
      const auto lambda = lambda_id(method->getParent());
      if (!lambda) return std::nullopt;
      key.linkage = LocalScope{*lambda, closure_member_anchor(method), 0};
      return key;
    }

    // Members of an unnamed type are scoped to that type's identity (its
    // owner component has no name to distinguish two unnamed siblings).
    if (const auto* parent_record = llvm::dyn_cast<clang::RecordDecl>(d->getDeclContext());
        parent_record && kind_of(parent_record) == SymbolKind::anonymous_type && !parent_record->isLambda()) {
      const auto enclosing_id = symbol_id(parent_record);
      if (!enclosing_id) return std::nullopt;
      key.linkage = LocalScope{*enclosing_id, key.canonical_name.empty() ? std::string(kAnonymousTypeAnchor) : key.canonical_name, 0};
      return key;
    }

    const clang::DeclContext* fn_ctx = d->getParentFunctionOrMethod();
    if (fn_ctx) {
      // Block-scope entity: scoped to its enclosing symbol.
      const clang::Decl* enclosing = nullptr;
      std::string anchor = key.canonical_name.empty() ? std::string(kAnonymousTypeAnchor) : key.canonical_name;
      unsigned ordinal = 0;
      if (const auto* rd = llvm::dyn_cast<clang::CXXRecordDecl>(d)) {
        const auto it = local_classes_.find(rd->getCanonicalDecl());
        if (it == local_classes_.end()) {
          limit("local class without ordinal information was not recorded");
          return std::nullopt;
        }
        enclosing = it->second.enclosing;
        ordinal = it->second.ordinal;
      } else if (const auto* parent_record = llvm::dyn_cast<clang::CXXRecordDecl>(d->getDeclContext())) {
        enclosing = parent_record;  // member of a local class: scoped to that class
      } else if (llvm::isa<clang::EnumDecl>(d) || llvm::isa<clang::TypedefNameDecl>(d)) {
        // Same-named local enums/aliases in separate blocks of one function
        // need their structural ordinal, like local classes.
        const auto it = local_tags_.find(d->getCanonicalDecl());
        if (it == local_tags_.end()) {
          limit("local enum/alias without ordinal information was not recorded");
          return std::nullopt;
        }
        enclosing = it->second.enclosing;
        ordinal = it->second.ordinal;
      } else {
        enclosing = llvm::cast<clang::Decl>(fn_ctx);
      }
      std::optional<StableId> enclosing_id;
      if (const auto* closure = llvm::dyn_cast<clang::CXXRecordDecl>(enclosing); closure && closure->isLambda()) {
        enclosing_id = lambda_id(closure);
      } else {
        enclosing_id = symbol_id(llvm::cast<clang::NamedDecl>(enclosing));
      }
      if (!enclosing_id) return std::nullopt;
      key.linkage = LocalScope{*enclosing_id, anchor, ordinal};
      return key;
    }

    if (key.kind == SymbolKind::anonymous_type) {
      // Unnamed type outside any function (documented anchor choice, see
      // docs/analyzer.md): scoped to the declarator that introduces it
      // (`struct {..} x;` -> variable/field x); an unnamed member record
      // without a declarator (anonymous union) is scoped to its enclosing
      // record with a per-record ordinal. Nothing else is guessed.
      const auto* rd = llvm::cast<clang::RecordDecl>(d);
      if (const clang::NamedDecl* declarator = declarator_for_anonymous(rd)) {
        const auto enclosing_id = symbol_id(declarator);
        if (!enclosing_id) return std::nullopt;
        key.linkage = LocalScope{*enclosing_id, kAnonymousTypeAnchor, 0};
        return key;
      }
      const auto member = unnamed_members_.find(rd->getCanonicalDecl());
      if (member != unnamed_members_.end()) {
        const auto enclosing_id = symbol_id(llvm::cast<clang::NamedDecl>(member->second.enclosing));
        if (!enclosing_id) return std::nullopt;
        key.linkage = LocalScope{*enclosing_id, kAnonymousTypeAnchor, member->second.ordinal};
        return key;
      }
      limit("unnamed type without a declarator or enclosing record anchor was not recorded");
      return std::nullopt;
    }
    if (d->getFormalLinkage() == clang::Linkage::Internal || d->isInAnonymousNamespace()) {
      const auto scope = file_scope_of(d);
      if (!scope) return std::nullopt;
      key.linkage = InternalLinkage{*scope};
    } else {
      key.linkage = ExternalLinkage{};
    }
    return key;
  }

  std::optional<StableId> symbol_id(const clang::NamedDecl* d) {
    const clang::Decl* canonical = d->getCanonicalDecl();
    const auto cached = ids_.find(canonical);
    if (cached != ids_.end()) return cached->second;
    ids_.emplace(canonical, std::nullopt);  // guard against recursive anchors
    std::optional<StableId> id;
    if (const auto key = key_of(d)) {
      if (const auto error = validate_canonical_key(*key)) {
        limit("symbol skipped, canonical key invalid: " + *error);
      } else {
        id = make_stable_id(*key);
        keys_.emplace(canonical, *key);
      }
    }
    ids_[canonical] = id;
    return id;
  }

  // Target of a relation: an in-root symbol id, an external placeholder, or a
  // hidden compiler-synthesized member (queued for its own facts).
  std::optional<StableId> ensure_target(const clang::NamedDecl* d) {
    if (const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(d); method && method->isLambdaStaticInvoker()) {
      limit("compiler-internal closure static invoker (__invoke) is a boundary, not a node");
      return std::nullopt;
    }
    const auto id = symbol_id(d);
    if (!id) return std::nullopt;
    if (!decl_in_root(d)) {
      const auto key = keys_.find(d->getCanonicalDecl());
      if (key != keys_.end()) facts_.add_external_placeholder(key->second);
      return id;
    }
    if (const auto* fd = llvm::dyn_cast<clang::FunctionDecl>(d); fd && fd->isImplicit()) {
      if (hidden_seen_.insert(fd->getCanonicalDecl()).second) pending_hidden_.push_back(fd);
    }
    return id;
  }

  // ---- facts ----------------------------------------------------------------
  void record_symbol(const clang::NamedDecl* d, LocationRole role, std::optional<clang::SourceRange> body_range,
                     CallableFlags flags = {}) {
    if (!decl_in_root(d)) return;
    const auto id = symbol_id(d);
    if (!id) return;
    const auto loc = make_location(d->getSourceRange());
    if (!loc) return;
    SymbolLocation sl;
    sl.role = role;
    sl.location = *loc;
    if (body_range) sl.body_hash = hash_of_range(*body_range);
    sl.analysis_unit = unit_.analysis_unit;
    sl.callable = flags;
    const auto key = keys_.find(d->getCanonicalDecl());
    if (key == keys_.end()) return;
    facts_.add_symbol_location(key->second, sl);

    // contains: innermost modelled owner -> symbol
    for (const clang::DeclContext* ctx = d->getDeclContext(); ctx && !llvm::isa<clang::TranslationUnitDecl>(ctx);
         ctx = ctx->getParent()) {
      if (llvm::isa<clang::LinkageSpecDecl>(ctx) || llvm::isa<clang::ExportDecl>(ctx)) continue;
      if (const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(ctx); method && method->getParent()->isLambda()) {
        if (const auto owner = lambda_id(method->getParent())) {
          add_relation(*owner, *id, RelationKind::contains, *loc, CallAspects{});
        }
        return;
      }
      if (const auto* owner_decl = llvm::dyn_cast<clang::NamedDecl>(ctx)) {
        if (const auto owner = ensure_target(owner_decl)) {
          add_relation(*owner, *id, RelationKind::contains, *loc, CallAspects{});
        }
      }
      return;
    }
  }

  void add_relation(const StableId& source, const StableId& target, RelationKind kind, const SourceLocation& loc,
                    const CallAspects& aspects) {
    Evidence e;
    e.analysis_unit = unit_.analysis_unit;
    e.kind = EvidenceKind::compiler_semantic;
    e.location = loc;
    e.evaluation = aspects.evaluation;
    e.dispatch = aspects.dispatch;
    e.template_use = aspects.template_use;
    e.immediately_invoked_lambda = aspects.immediately_invoked_lambda;
    e.template_arguments = aspects.template_arguments;
    e.subobject_role = aspects.subobject_role;
    e.subobject = aspects.subobject;
    e.lifetime = aspects.lifetime;
    e.potentially_elided = aspects.potentially_elided;
    e.base = aspects.base;
    facts_.add_relation_evidence(RelationKey{source, target, kind, Confidence::confirmed}, std::move(e));
  }

  void record_unresolved(const StableId& owner, const clang::CallExpr* call, const SourceLocation& loc,
                         std::optional<UnresolvedReason> forced = std::nullopt) {
    UnresolvedSite site;
    site.enclosing = owner;
    site.location = loc;
    site.analysis_unit = unit_.analysis_unit;
    const clang::Expr* callee = call->getCallee()->IgnoreParens();
    site.expression = std::string(
        clang::Lexer::getSourceText(clang::CharSourceRange::getTokenRange(callee->getSourceRange()), sm_, ctx_.getLangOpts()));
    if (forced) {
      site.reason = *forced;
    } else if (llvm::isa<clang::RecoveryExpr>(callee) || callee->containsErrors()) {
      site.reason = UnresolvedReason::analysis_failed;
    } else if (call->isTypeDependent() || call->isValueDependent() || llvm::isa<clang::UnresolvedLookupExpr>(callee) ||
               llvm::isa<clang::DependentScopeDeclRefExpr>(callee) || llvm::isa<clang::CXXDependentScopeMemberExpr>(callee) ||
               llvm::isa<clang::UnresolvedMemberExpr>(callee)) {
      site.reason = UnresolvedReason::dependent_expression;
    } else {
      site.reason = UnresolvedReason::indirect_callee;
    }
    facts_.add_unresolved_site(std::move(site));
  }

  // ---- locations --------------------------------------------------------------
  FileInfo& file_info(clang::FileID fid) {
    auto it = files_.find(fid);
    if (it != files_.end()) return it->second;
    FileInfo fi;
    fi.abs_utf8 = absolute_utf8_path(sm_, fid);
    bool invalid = false;
    fi.buffer = sm_.getBufferData(fid, &invalid);
    if (invalid) fi.buffer = llvm::StringRef();
    fi.hash = hash_file_content(std::string_view(fi.buffer.data(), fi.buffer.size()));
    fi.lines.emplace(std::string_view(fi.buffer.data(), fi.buffer.size()));
    fi.is_system = clang::SrcMgr::isSystem(sm_.getFileCharacteristic(sm_.getLocForStartOfFile(fid)));
    if (!fi.abs_utf8.empty()) {
      fi.repo = make_repo_relative_on_disk(unit_.request->repository_root, path_from_utf8(fi.abs_utf8));
      fi.in_root = fi.repo.has_value();
    }
    return files_.emplace(fid, std::move(fi)).first->second;
  }

  std::optional<SourceLocation> make_location(clang::SourceRange range) {
    clang::SourceLocation begin = sm_.getExpansionLoc(range.getBegin());
    clang::SourceLocation end = sm_.getExpansionLoc(range.getEnd());
    if (begin.isInvalid()) return std::nullopt;
    if (end.isInvalid() || sm_.getFileID(end) != sm_.getFileID(begin)) end = begin;
    const clang::FileID fid = sm_.getFileID(begin);
    FileInfo& fi = file_info(fid);
    if (!fi.in_root || !fi.repo) return std::nullopt;
    const unsigned b = sm_.getFileOffset(begin);
    clang::SourceLocation end_tok = clang::Lexer::getLocForEndOfToken(end, 0, sm_, ctx_.getLangOpts());
    unsigned e = end_tok.isValid() ? sm_.getFileOffset(end_tok) : sm_.getFileOffset(end);
    if (e < b) e = b;
    const auto span = fi.lines->span(b, e);
    if (!span) return std::nullopt;
    SourceLocation loc;
    loc.file = *fi.repo;
    loc.content_hash = fi.hash;
    loc.span = *span;
    loc.span_hash = hash_span(std::string_view(fi.buffer.data(), fi.buffer.size()), *span);
    return loc;
  }

  std::optional<Sha256Digest> hash_of_range(clang::SourceRange range) {
    clang::SourceLocation begin = sm_.getExpansionLoc(range.getBegin());
    clang::SourceLocation end = sm_.getExpansionLoc(range.getEnd());
    if (begin.isInvalid() || end.isInvalid() || sm_.getFileID(begin) != sm_.getFileID(end)) return std::nullopt;
    FileInfo& fi = file_info(sm_.getFileID(begin));
    const unsigned b = sm_.getFileOffset(begin);
    clang::SourceLocation end_tok = clang::Lexer::getLocForEndOfToken(end, 0, sm_, ctx_.getLangOpts());
    const unsigned e = end_tok.isValid() ? sm_.getFileOffset(end_tok) : sm_.getFileOffset(end);
    const auto span = fi.lines->span(b, e < b ? b : e);
    if (!span) return std::nullopt;
    return hash_span(std::string_view(fi.buffer.data(), fi.buffer.size()), *span);
  }

  // Evidence location: the use site when inside a default argument/initializer.
  std::optional<SourceLocation> evidence_location(const clang::Stmt* s) {
    if (!default_use_.empty() && default_use_.back().isValid()) {
      return make_location(clang::SourceRange(default_use_.back(), default_use_.back()));
    }
    return make_location(s->getSourceRange());
  }

  CallAspects base_aspects() const {
    CallAspects a;
    if (!evaluation_context_.empty()) a.evaluation = evaluation_context_.back();
    return a;
  }

  std::optional<StableId> current_owner() const {
    return owners_.empty() ? std::nullopt : std::optional(owners_.back());
  }

  // A shared expression (default argument) is visited once per use site.
  bool dedupe(const clang::Stmt* s) {
    const unsigned use = default_use_.empty() ? 0u : default_use_.back().getRawEncoding();
    return seen_.insert(std::make_pair(s, use)).second;
  }

  void limit(std::string text) {
    auto& limits = unit_.result->limits;
    for (const auto& existing : limits) {
      if (existing == text) return;
    }
    limits.push_back(std::move(text));
  }

  // ---- direct includes (PRD FR-GPH-004) -------------------------------------
  // The pinned dependency-directive scanner supplies the original bytes of every
  // directive, including ones in skipped branches; the preprocessor callbacks
  // supply what was actually processed and resolved. Neither alone is enough:
  // the scanner does not evaluate conditions, and a missing callback is not
  // proof of inactivity. Correlation is per VISIT (FileID + `#` offset), so the
  // same header entered twice under different macro state keeps both outcomes.

  struct ConditionGroup {
    std::vector<ConditionTerm> terms;  // branches of this #if group seen so far
  };

  void finalize_includes() {
    for (clang::FileID fid : unit_.entered_files) {
      if (fid.isInvalid()) continue;
      FileInfo& fi = file_info(fid);
      // Only an in-root includer produces a public fact; an external includer's
      // directives stay out of the repository graph entirely.
      if (!fi.in_root || !fi.repo || !fi.lines || fi.buffer.empty()) continue;
      scan_file_includes(fid, fi);
    }
  }

  void scan_file_includes(clang::FileID fid, FileInfo& fi) {
    namespace ds = clang::dependency_directives_scan;
    llvm::SmallVector<ds::Token, 64> tokens;
    llvm::SmallVector<ds::Directive, 32> directives;
    const llvm::StringRef input(fi.buffer.data(), fi.buffer.size());
    if (clang::scanSourceForDependencyDirectives(input, tokens, directives)) {
      limit("preprocessor directive scan failed in " + fi.repo->generic +
            ": the direct includes of this file are incomplete, not empty");
      return;
    }
    std::set<unsigned> reported;
    for (const ds::Directive& d : directives) {
      if (!d.Tokens.empty()) reported.insert(d.Tokens.front().Offset);
    }
    // Scanner success is never proof of complete lexical coverage: a legal
    // digraph `%:include` is silently invisible to it on the pinned SDK, and it
    // also succeeds on malformed input the real front end diagnoses. A missed
    // CONDITIONAL would leave the surrounding ordinary includes looking
    // unconditional, so an incomplete file publishes no direct-include facts at
    // all; the limitation carries the truth and the private preprocessor
    // observations are untouched.
    if (!verify_directive_coverage(fi, reported)) return;

    std::vector<ConditionGroup> stack;
    for (const ds::Directive& d : directives) {
      if (d.Tokens.empty()) continue;
      switch (d.Kind) {
        case ds::pp_if:
        case ds::pp_ifdef:
        case ds::pp_ifndef:
          stack.push_back(ConditionGroup{{condition_term(fi, d)}});
          break;
        case ds::pp_elif:
        case ds::pp_elifdef:
        case ds::pp_elifndef:
        case ds::pp_else:
          if (stack.empty()) {
            limit("conditional branch without an opening directive in " + fi.repo->generic +
                  ": include conditions of this file are incomplete");
            break;
          }
          stack.back().terms.push_back(condition_term(fi, d));
          break;
        case ds::pp_endif:
          if (stack.empty()) {
            limit("unbalanced #endif in " + fi.repo->generic +
                  ": include conditions of this file are incomplete");
            break;
          }
          stack.pop_back();
          break;
        case ds::pp_include:
        case ds::pp_include_next:
        case ds::pp_import:
          emit_include(fid, fi, d, stack);
          break;
        default:
          break;
      }
    }
    if (!stack.empty()) {
      limit("unterminated conditional group in " + fi.repo->generic +
            ": include conditions of this file are incomplete");
    }
  }

  // Raw compiler lex of the same bytes: every line-leading `#` (or its `%:`
  // digraph) that introduces a directive this increment models must have been
  // reported by the scanner. Anything else becomes an explicit limitation, so a
  // scanner blind spot never reads as "this file has no includes".
  bool verify_directive_coverage(const FileInfo& fi, const std::set<unsigned>& reported) {
    const char* const begin = fi.buffer.data();
    const char* const end = begin + fi.buffer.size();
    clang::Lexer lexer(clang::SourceLocation(), ctx_.getLangOpts(), begin, begin, end);
    clang::Token tok;
    bool pending_hash = false;
    bool complete = true;
    unsigned pending_offset = 0;
    while (!lexer.LexFromRawLexer(tok)) {
      // The lexer was built with a null file location, so a token's raw
      // encoding is its byte offset into this buffer.
      const unsigned offset = tok.getLocation().getRawEncoding();
      if (pending_hash) {
        pending_hash = false;
        if (tok.is(clang::tok::raw_identifier) && reported.find(pending_offset) == reported.end()) {
          const llvm::StringRef name = tok.getRawIdentifier();
          // A keyword split by an escaped newline (`%:inc\<newline>lude`) has a
          // raw spelling that no literal comparison can match, and normalizing
          // it here would be hand-rolled lexing. It is treated as uncovered.
          if (tok.needsCleaning()) {
            complete = false;
            limit("preprocessor directive with a spliced spelling is not covered by the dependency scanner in " +
                  fi.repo->generic + " (offset " + std::to_string(pending_offset) +
                  "): no direct include of this file is published, because a missed conditional would make the "
                  "others look unconditional");
          } else if (name == "include" || name == "include_next" || name == "import" || name == "if" ||
                     name == "ifdef" || name == "ifndef" || name == "elif" || name == "elifdef" ||
                     name == "elifndef" || name == "else" || name == "endif") {
            complete = false;
            limit("preprocessor directive spelling not covered by the dependency scanner in " + fi.repo->generic +
                  " (offset " + std::to_string(pending_offset) + ", " + name.str() +
                  "): no direct include of this file is published, because a missed conditional would make the "
                  "others look unconditional");
          }
        }
      }
      if (tok.isAtStartOfLine() && tok.is(clang::tok::hash)) {
        pending_hash = true;
        pending_offset = offset;
      }
    }
    return complete;
  }

  std::string_view directive_bytes(const FileInfo& fi, unsigned begin, unsigned end) const {
    if (end > fi.buffer.size() || begin > end) return {};
    return std::string_view(fi.buffer.data() + begin, end - begin);
  }

  // The scanner ends every directive with an end-of-directive token that runs
  // past the end of the line. The logical directive excludes ONLY that final
  // line terminator and keeps every other byte: the continuation backslash and
  // its CRLF, a trailing comment and any remaining spelling.
  unsigned trim_directive_end(const FileInfo& fi, unsigned begin, unsigned end) const {
    if (end > fi.buffer.size()) end = static_cast<unsigned>(fi.buffer.size());
    if (end > begin && fi.buffer[end - 1] == '\n') --end;
    if (end > begin && fi.buffer[end - 1] == '\r') --end;
    return end;
  }

  // End of the last real token of a directive: the operand and the condition
  // expression stop there, so neither swallows the terminator or a comment the
  // end-of-directive token covers.
  static unsigned last_spelled_token_end(const clang::dependency_directives_scan::Directive& d) {
    for (auto it = d.Tokens.rbegin(); it != d.Tokens.rend(); ++it) {
      if (it->Kind != clang::tok::eod) return it->getEnd();
    }
    return d.Tokens.back().getEnd();
  }

  std::optional<SourceLocation> offsets_location(FileInfo& fi, unsigned begin, unsigned end) {
    const auto span = fi.lines->span(begin, end);
    if (!span) return std::nullopt;
    SourceLocation loc;
    loc.file = *fi.repo;
    loc.content_hash = fi.hash;
    loc.span = *span;
    loc.span_hash = hash_span(std::string_view(fi.buffer.data(), fi.buffer.size()), *span);
    return loc;
  }

  ConditionTerm condition_term(FileInfo& fi, const clang::dependency_directives_scan::Directive& d) {
    namespace ds = clang::dependency_directives_scan;
    ConditionTerm term;
    switch (d.Kind) {
      case ds::pp_ifdef:
        term.kind = ConditionTermKind::ifdef;
        break;
      case ds::pp_ifndef:
        term.kind = ConditionTermKind::ifndef;
        break;
      case ds::pp_elif:
        term.kind = ConditionTermKind::elif;
        break;
      case ds::pp_elifdef:
        term.kind = ConditionTermKind::elifdef;
        break;
      case ds::pp_elifndef:
        term.kind = ConditionTermKind::elifndef;
        break;
      case ds::pp_else:
        term.kind = ConditionTermKind::else_;
        break;
      default:
        term.kind = ConditionTermKind::if_;
        break;
    }
    const unsigned begin = d.Tokens.front().Offset;
    const unsigned end = trim_directive_end(fi, begin, d.Tokens.back().getEnd());
    // Original expression bytes exactly as written; never evaluated. The
    // directive keyword is the second token, the condition follows it.
    if (d.Tokens.size() > 2) {
      const unsigned expr_end = last_spelled_token_end(d);
      if (expr_end > d.Tokens[2].Offset) {
        term.expression_as_written = std::string(directive_bytes(fi, d.Tokens[2].Offset, expr_end));
      }
    }
    if (const auto loc = offsets_location(fi, begin, end)) term.location = *loc;
    return term;
  }

  void emit_include(clang::FileID fid, FileInfo& fi, const clang::dependency_directives_scan::Directive& d,
                    const std::vector<ConditionGroup>& stack) {
    namespace ds = clang::dependency_directives_scan;
    const unsigned begin = d.Tokens.front().Offset;
    const unsigned end = trim_directive_end(fi, begin, d.Tokens.back().getEnd());
    const auto loc = offsets_location(fi, begin, end);
    if (!loc) {
      limit("a direct include directive could not be located in the original bytes of " + fi.repo->generic);
      return;
    }

    DirectIncludeFact fact;
    fact.includer = *fi.repo;
    fact.directive_kind = d.Kind == ds::pp_include_next ? IncludeDirectiveKind::include_next
                          : d.Kind == ds::pp_import     ? IncludeDirectiveKind::import
                                                        : IncludeDirectiveKind::include;
    if (d.Tokens.size() > 2) {
      const unsigned operand_end = last_spelled_token_end(d);
      if (operand_end > d.Tokens[2].Offset) {
        fact.operand_as_written = std::string(directive_bytes(fi, d.Tokens[2].Offset, operand_end));
      }
    }
    fact.operand_kind = fact.operand_as_written.rfind('"', 0) == 0   ? IncludeOperandKind::quoted
                        : fact.operand_as_written.rfind('<', 0) == 0 ? IncludeOperandKind::angled
                                                                     : IncludeOperandKind::macro_tokens;
    fact.directive_location = *loc;
    fact.analysis_unit = unit_.analysis_unit;
    for (const ConditionGroup& group : stack) {
      if (group.terms.empty()) continue;
      ConditionalBranch branch;
      branch.preceding_branches.assign(group.terms.begin(), group.terms.end() - 1);
      const ConditionTerm& current = group.terms.back();
      branch.is_else = current.kind == ConditionTermKind::else_;
      if (!branch.is_else) branch.own_condition = current;
      fact.condition_path.push_back(std::move(branch));
    }

    const unsigned visit = fid.getHashValue();
    bool processed_any = false;
    for (const ProcessedInclusion& processed : unit_.processed_includes) {
      if (processed.includer_file_id != visit || processed.hash_offset != begin) continue;
      processed_any = true;
      DirectIncludeFact active = fact;
      active.activity = IncludeActivity::active;
      active.expanded_spelling = processed.expanded_spelling;
      if (!processed.found) {
        active.resolution = IncludeResolution::not_found;
      } else if (const auto repo = make_repo_relative_on_disk(unit_.request->repository_root,
                                                              path_from_utf8(processed.resolved_path_utf8))) {
        active.resolution = IncludeResolution::resolved_internal;
        active.resolved_repo_target = *repo;
      } else {
        // External: no public path and no ranking edge. The opaque key keeps
        // distinct files distinct and is recomputable from the unit's private
        // file observations.
        active.resolution = IncludeResolution::resolved_external;
        active.external_dependency_key = external_dependency_key(processed.resolved_path_utf8);
      }
      facts_.add_direct_include(std::move(active));
    }
    if (processed_any) return;

    if (inside_skipped_range(visit, begin)) {
      fact.activity = IncludeActivity::inactive;  // proven skipped: no filesystem guessing
      fact.resolution = IncludeResolution::not_evaluated;
    } else {
      fact.activity = IncludeActivity::indeterminate;  // unvisited or aborted: NOT proven inactive
      fact.resolution = IncludeResolution::not_evaluated;
      limit("a direct include in " + fi.repo->generic +
            " was neither processed nor proven skipped; it is recorded as indeterminate, not inactive");
    }
    facts_.add_direct_include(std::move(fact));
  }

  bool inside_skipped_range(unsigned visit, unsigned offset) const {
    for (const SkippedRange& range : unit_.skipped_ranges) {
      if (range.file_id == visit && offset >= range.begin && offset <= range.end) return true;
    }
    return false;
  }

  // Opaque: neither a path nor a content hash on its own, so two distinct files
  // with identical bytes stay distinct, and the unit's private file
  // observations can recompute it to link back to the resolved path.
  std::string external_dependency_key(const std::string& absolute_path_utf8) {
    std::string material = "lcm-extdep";
    material.push_back('\0');
    material.append(absolute_path_utf8);
    material.push_back('\0');
    const auto hash = file_hashes_.find(absolute_path_utf8);
    if (hash != file_hashes_.end()) material.append(hash->second.digest.hex());
    return sha256(material).hex();
  }

  void finalize_files() {
    unit_.entered_files.insert(sm_.getMainFileID());
    for (clang::FileID fid : unit_.entered_files) {
      if (fid.isInvalid()) continue;
      const FileInfo& fi = file_info(fid);
      if (fi.abs_utf8.empty()) continue;
      FileObservation obs;
      obs.path_utf8 = fi.abs_utf8;
      obs.repo_path = fi.repo;
      obs.in_root = fi.in_root;
      obs.is_main_file = fid == sm_.getMainFileID();
      obs.is_system = fi.is_system;
      obs.content_hash = fi.hash;
      file_hashes_[fi.abs_utf8] = fi.hash;
      unit_.result->files.push_back(std::move(obs));
    }
  }

  clang::ASTContext& ctx_;
  clang::SourceManager& sm_;
  UnitContext& unit_;
  FactSet& facts_;
  clang::PrintingPolicy policy_{clang::LangOptions()};

  std::map<clang::FileID, FileInfo> files_;
  std::map<std::string, FileContentHash> file_hashes_;  // absolute path -> bytes hash (private)
  std::map<const clang::Decl*, LocalInfo> lambdas_;
  std::map<const clang::Decl*, LocalInfo> local_classes_;
  std::map<const clang::Decl*, LocalInfo> unnamed_members_;
  std::map<const clang::Decl*, LocalInfo> local_tags_;
  std::map<const clang::Decl*, std::optional<StableId>> ids_;
  std::map<const clang::Decl*, CanonicalKey> keys_;

  std::vector<StableId> owners_;                        // attribution stack
  std::vector<EvaluationContext> evaluation_context_;   // default argument / member init / init capture
  std::vector<clang::SourceLocation> default_use_;      // use sites of default expressions
  std::set<std::pair<const clang::Stmt*, unsigned>> seen_;
  std::set<const clang::Expr*> callee_refs_;

  // Phase 1B callable/lambda state.
  std::map<const clang::CXXConstructExpr*, std::pair<SubobjectRole, std::string>> subobject_inits_;
  std::set<const clang::CXXConstructExpr*> elided_constructs_;          // return values of NRVO candidates
  std::set<const clang::CXXBindTemporaryExpr*> callee_destroyed_args_;  // by-value arguments the callee destroys
  std::map<const clang::FieldDecl*, std::string> closure_fields_;       // closure field -> capture name
  std::set<const clang::Decl*> hidden_seen_;
  std::deque<const clang::FunctionDecl*> pending_hidden_;
};

// ---------------------------------------------------------------------------
class PPObserver : public clang::PPCallbacks {
 public:
  PPObserver(UnitContext& unit, clang::SourceManager& sm) : unit_(unit), sm_(sm) {}

  void FileChanged(clang::SourceLocation loc, FileChangeReason reason, clang::SrcMgr::CharacteristicKind,
                   clang::FileID) override {
    if (reason != EnterFile) return;
    const clang::FileID fid = sm_.getFileID(sm_.getExpansionLoc(loc));
    if (fid.isValid() && sm_.getFileEntryRefForID(fid)) unit_.entered_files.insert(fid);
  }

  void InclusionDirective(clang::SourceLocation hash_loc, const clang::Token&, llvm::StringRef file_name, bool is_angled,
                          clang::CharSourceRange, clang::OptionalFileEntryRef file, llvm::StringRef, llvm::StringRef,
                          const clang::Module*, bool, clang::SrcMgr::CharacteristicKind) override {
    IncludeObservation obs;
    std::optional<std::string> obs_resolved;
    const clang::SourceLocation loc = sm_.getExpansionLoc(hash_loc);
    const clang::FileID fid = sm_.getFileID(loc);
    obs.includer_path_utf8 = absolute_utf8_path(sm_, fid);
    if (!obs.includer_path_utf8.empty()) {
      obs.includer_repo_path = make_repo_relative_on_disk(unit_.request->repository_root, path_from_utf8(obs.includer_path_utf8));
    }
    obs.spelling = std::string(file_name);
    obs.angled = is_angled;
    obs.line = sm_.getSpellingLineNumber(loc);
    if (file) {
      llvm::StringRef real = file->getFileEntry().tryGetRealPathName();
      llvm::SmallString<256> path(real.empty() ? file->getName() : real);
      (void)sm_.getFileManager().getVirtualFileSystem().makeAbsolute(path);
      llvm::sys::path::remove_dots(path, true);
      obs.resolved_path_utf8 = std::string(path.str());
      obs.resolved_repo_path = make_repo_relative_on_disk(unit_.request->repository_root, path_from_utf8(*obs.resolved_path_utf8));
      obs.resolved_in_root = obs.resolved_repo_path.has_value();
      obs_resolved = obs.resolved_path_utf8;
    }
    unit_.result->includes.push_back(std::move(obs));

    // Per-visit correlation input for the direct-include facts.
    const auto [decomposed_fid, hash_offset] = sm_.getDecomposedExpansionLoc(hash_loc);
    ProcessedInclusion processed;
    processed.includer_file_id = decomposed_fid.getHashValue();
    processed.hash_offset = hash_offset;
    processed.expanded_spelling = std::string(file_name);
    processed.angled = is_angled;
    processed.found = file.has_value();
    if (file) processed.resolved_path_utf8 = *obs_resolved;
    unit_.processed_includes.push_back(std::move(processed));
  }

  void SourceRangeSkipped(clang::SourceRange range, clang::SourceLocation) override {
    const auto [begin_fid, begin_offset] = sm_.getDecomposedExpansionLoc(range.getBegin());
    const auto [end_fid, end_offset] = sm_.getDecomposedExpansionLoc(range.getEnd());
    if (begin_fid != end_fid || begin_fid.isInvalid()) return;
    unit_.skipped_ranges.push_back(SkippedRange{begin_fid.getHashValue(), begin_offset, end_offset});
  }

 private:
  UnitContext& unit_;
  clang::SourceManager& sm_;
};

class Consumer : public clang::ASTConsumer {
 public:
  explicit Consumer(UnitContext& unit) : unit_(unit) {}
  void HandleTranslationUnit(clang::ASTContext& ctx) override {
    FactsVisitor visitor(ctx, unit_);
    visitor.run();
  }

 private:
  UnitContext& unit_;
};

}  // namespace

bool AnalyzerAction::BeginSourceFileAction(clang::CompilerInstance& ci) {
  ci.getPreprocessor().addPPCallbacks(std::make_unique<PPObserver>(unit_, ci.getSourceManager()));
  return true;
}

std::unique_ptr<clang::ASTConsumer> AnalyzerAction::CreateASTConsumer(clang::CompilerInstance&, llvm::StringRef) {
  return std::make_unique<Consumer>(unit_);
}

}  // namespace lcm::analyzer::detail
