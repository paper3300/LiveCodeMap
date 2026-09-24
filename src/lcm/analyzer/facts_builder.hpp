// Internal: front-end action producing the staged facts of one unit.
#pragma once

#include <memory>
#include <set>
#include <string>
#include <vector>

#include <clang/AST/ASTConsumer.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendAction.h>
#include <clang/Lex/PPCallbacks.h>

#include "lcm/analyzer/analyzer.hpp"

namespace lcm::analyzer::detail {

// One #include the preprocessor actually processed. Correlated by the
// includer's per-visit FileID and the byte offset of its `#`, never by physical
// path alone, so one inactive visit of a file cannot contaminate a later active
// visit of the same file under different macro state.
struct ProcessedInclusion {
  unsigned includer_file_id = 0;  // clang::FileID hash value of this visit
  unsigned hash_offset = 0;       // offset of `#` in the includer's buffer
  std::string expanded_spelling;  // header name after macro expansion
  bool angled = false;
  bool found = false;
  std::string resolved_path_utf8;  // absolute; stays private
};

// A range the preprocessor proved skipped, in one visit of one file.
struct SkippedRange {
  unsigned file_id = 0;
  unsigned begin = 0;
  unsigned end = 0;
};

// Shared per-unit state between the preprocessor observer and the AST consumer.
struct UnitContext {
  const AnalysisRequest* request = nullptr;
  AnalysisResult* result = nullptr;
  std::string analysis_unit;
  std::set<clang::FileID> entered_files;
  std::vector<ProcessedInclusion> processed_includes;
  std::vector<SkippedRange> skipped_ranges;
};

class AnalyzerAction : public clang::ASTFrontendAction {
 public:
  explicit AnalyzerAction(UnitContext& unit) : unit_(unit) {}
  bool BeginSourceFileAction(clang::CompilerInstance& ci) override;
  std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(clang::CompilerInstance& ci, llvm::StringRef file) override;

 private:
  UnitContext& unit_;
};

}  // namespace lcm::analyzer::detail
