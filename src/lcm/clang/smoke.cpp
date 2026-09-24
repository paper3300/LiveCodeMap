#include "lcm/clang/smoke.hpp"

#include <memory>
#include <set>

#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Basic/Version.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendAction.h>
#include <clang/Frontend/TextDiagnosticBuffer.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/ADT/IntrusiveRefCntPtr.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/VirtualFileSystem.h>

namespace lcm::clang_smoke {
namespace {

std::string owner_of(const clang::FunctionDecl* fd) {
  const clang::DeclContext* ctx = fd->getDeclContext()->getRedeclContext();
  if (const auto* named = llvm::dyn_cast<clang::NamedDecl>(ctx)) {
    if (llvm::isa<clang::TranslationUnitDecl>(ctx)) return {};
    return named->getQualifiedNameAsString();
  }
  return {};
}

std::string signature_of(const clang::FunctionDecl* fd, const clang::PrintingPolicy& policy) {
  std::string out = "(";
  const auto* proto = fd->getType()->getAs<clang::FunctionProtoType>();
  if (proto) {
    for (unsigned i = 0; i < proto->getNumParams(); ++i) {
      if (i > 0) out += ", ";
      out += proto->getParamType(i).getAsString(policy);
    }
  }
  out += ")";
  if (const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(fd)) {
    if (method->isConst()) out += " const";
  }
  return out;
}

class Visitor : public clang::RecursiveASTVisitor<Visitor> {
 public:
  Visitor(clang::ASTContext& context, SmokeFacts& facts) : context_(context), facts_(facts) {}

  bool TraverseDecl(clang::Decl* decl) {
    if (auto* fd = llvm::dyn_cast_or_null<clang::FunctionDecl>(decl)) {
      enclosing_.push_back(fd);
      const bool result = RecursiveASTVisitor::TraverseDecl(decl);
      enclosing_.pop_back();
      return result;
    }
    return RecursiveASTVisitor::TraverseDecl(decl);
  }

  bool VisitFunctionDecl(clang::FunctionDecl* fd) {
    const auto& sm = context_.getSourceManager();
    if (!sm.isInMainFile(fd->getLocation())) return true;
    FunctionFact fact;
    fact.qualified_name = fd->getQualifiedNameAsString();
    fact.owner = owner_of(fd);
    fact.signature = signature_of(fd, context_.getPrintingPolicy());
    fact.is_definition = fd->isThisDeclarationADefinition();
    fact.line = sm.getSpellingLineNumber(fd->getLocation());
    facts_.functions.push_back(std::move(fact));
    return true;
  }

  bool VisitCallExpr(clang::CallExpr* call) {
    const auto& sm = context_.getSourceManager();
    if (!sm.isInMainFile(call->getBeginLoc())) return true;
    // A default-argument expression is reachable from every redeclaration that
    // inherits it; count each CallExpr once.
    if (!seen_calls_.insert(call).second) return true;
    const clang::FunctionDecl* callee = call->getDirectCallee();
    if (!callee) return true;  // indirect call: no invented target

    // Caller contract: only calls lexically inside the enclosing function's
    // body are calls made by that function.
    const clang::FunctionDecl* enclosing = enclosing_.empty() ? nullptr : enclosing_.back();
    const clang::Stmt* body = enclosing ? enclosing->getBody() : nullptr;
    const bool in_body = body && sm.isPointWithin(call->getBeginLoc(), body->getBeginLoc(), body->getEndLoc());
    if (!in_body) {
      OutsideBodyCall outside;
      outside.declaring_function = enclosing ? enclosing->getQualifiedNameAsString() : std::string{};
      outside.callee_qualified = callee->getQualifiedNameAsString();
      outside.callee_signature = signature_of(callee, context_.getPrintingPolicy());
      outside.line = sm.getSpellingLineNumber(call->getBeginLoc());
      facts_.calls_outside_bodies.push_back(std::move(outside));
      return true;
    }
    CallFact fact;
    fact.caller_qualified = enclosing->getQualifiedNameAsString();
    fact.callee_qualified = callee->getQualifiedNameAsString();
    fact.callee_signature = signature_of(callee, context_.getPrintingPolicy());
    fact.member_call = llvm::isa<clang::CXXMemberCallExpr>(call);
    fact.line = sm.getSpellingLineNumber(call->getBeginLoc());
    facts_.calls.push_back(std::move(fact));
    return true;
  }

 private:
  clang::ASTContext& context_;
  SmokeFacts& facts_;
  std::vector<const clang::FunctionDecl*> enclosing_;
  std::set<const clang::CallExpr*> seen_calls_;
};

class Consumer : public clang::ASTConsumer {
 public:
  explicit Consumer(SmokeFacts& facts) : facts_(facts) {}
  void HandleTranslationUnit(clang::ASTContext& context) override {
    Visitor visitor(context, facts_);
    visitor.TraverseDecl(context.getTranslationUnitDecl());
  }

 private:
  SmokeFacts& facts_;
};

class Action : public clang::ASTFrontendAction {
 public:
  explicit Action(SmokeFacts& facts) : facts_(facts) {}
  std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(clang::CompilerInstance&, llvm::StringRef) override {
    return std::make_unique<Consumer>(facts_);
  }

 private:
  SmokeFacts& facts_;
};

}  // namespace

SmokeFacts collect_facts(const std::string& code, const std::string& file_name,
                         const std::vector<std::string>& compiler_args) {
  SmokeFacts facts;

  auto overlay = llvm::makeIntrusiveRefCnt<llvm::vfs::OverlayFileSystem>(llvm::vfs::getRealFileSystem());
  auto in_memory = llvm::makeIntrusiveRefCnt<llvm::vfs::InMemoryFileSystem>();
  overlay->pushOverlay(in_memory);
  in_memory->addFile(file_name, 0, llvm::MemoryBuffer::getMemBufferCopy(code));
  auto files = llvm::makeIntrusiveRefCnt<clang::FileManager>(clang::FileSystemOptions(), overlay);

  std::vector<std::string> argv;
  argv.push_back("clang-tool");
  argv.push_back("-fsyntax-only");
  argv.insert(argv.end(), compiler_args.begin(), compiler_args.end());
  argv.push_back(file_name);

  clang::TextDiagnosticBuffer buffer;
  clang::tooling::ToolInvocation invocation(argv, std::make_unique<Action>(facts), files.get());
  invocation.setDiagnosticConsumer(&buffer);
  facts.parsed_ok = invocation.run() && buffer.getNumErrors() == 0;

  for (auto it = buffer.err_begin(); it != buffer.err_end(); ++it) facts.diagnostics.push_back("error: " + it->second);
  for (auto it = buffer.warn_begin(); it != buffer.warn_end(); ++it) {
    facts.diagnostics.push_back("warning: " + it->second);
  }
  return facts;
}

std::string clang_version() { return clang::getClangFullVersion(); }

}  // namespace lcm::clang_smoke
