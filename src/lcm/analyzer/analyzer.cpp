#include "lcm/analyzer/analyzer.hpp"

#include <memory>
#include <set>

#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/FileManager.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendAction.h>
#include <clang/Frontend/FrontendActions.h>
#include <clang/Lex/PPCallbacks.h>
#include <clang/Lex/Preprocessor.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/ADT/IntrusiveRefCntPtr.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/Support/VirtualFileSystem.h>

#include "lcm/analyzer/facts_builder.hpp"
#include "lcm/hash.hpp"
#include "lcm/analyzer/pch_replay_internal.hpp"
#include "lcm/analyzer/safety.hpp"

namespace lcm::analyzer {
namespace {

class CollectingDiagnostics : public clang::DiagnosticConsumer {
 public:
  explicit CollectingDiagnostics(std::vector<AnalysisDiagnostic>& out) : out_(out) {}

  void HandleDiagnostic(clang::DiagnosticsEngine::Level level, const clang::Diagnostic& info) override {
    clang::DiagnosticConsumer::HandleDiagnostic(level, info);
    AnalysisDiagnostic d;
    switch (level) {
      case clang::DiagnosticsEngine::Note:
        d.level = AnalysisDiagnostic::Level::note;
        break;
      case clang::DiagnosticsEngine::Remark:
        d.level = AnalysisDiagnostic::Level::remark;
        break;
      case clang::DiagnosticsEngine::Warning:
        d.level = AnalysisDiagnostic::Level::warning;
        break;
      case clang::DiagnosticsEngine::Error:
        d.level = AnalysisDiagnostic::Level::error;
        break;
      case clang::DiagnosticsEngine::Fatal:
        d.level = AnalysisDiagnostic::Level::fatal;
        break;
      default:
        d.level = AnalysisDiagnostic::Level::note;
        break;
    }
    llvm::SmallString<256> message;
    info.FormatDiagnostic(message);
    d.message = std::string(message.str());
    if (info.hasSourceManager() && info.getLocation().isValid()) {
      const clang::PresumedLoc ploc = info.getSourceManager().getPresumedLoc(info.getLocation());
      if (ploc.isValid()) {
        d.file = std::string(ploc.getFilename());
        d.line = ploc.getLine();
        d.column = ploc.getColumn();
      }
    }
    out_.push_back(std::move(d));
  }

 private:
  std::vector<AnalysisDiagnostic>& out_;
};

// True when PCH emulation is the ONLY reason the context is degraded: no
// unsupported option, no extra input, no unexpanded response file. Anything
// else must keep rejecting regardless of the PCH proof.
bool degraded_only_by_pch(const NormalizedCompileContext& context) {
  if (!context.unexpanded_response_files.empty()) return false;
  if (!context.source_identified) return false;
  for (const auto& d : context.dispositions) {
    if (d.action == OptionAction::dropped_unsupported) return false;
    if (d.action == OptionAction::transformed && d.category != OptionCategory::precompiled_header) return false;
  }
  return true;
}

// Attaches the SAME prefix measurement the validation captures use to the final
// parse, so the analysed prefix is compared to the proven prefix on every axis
// it was proven on. Checking only which files were entered is strictly weaker:
// a file appearing between validation and the final parse can flip a
// `__has_include` or a conditional without any new file being entered.
class PrefixCheckedAction : public clang::WrapperFrontendAction {
 public:
  PrefixCheckedAction(std::unique_ptr<clang::FrontendAction> wrapped, detail::PrefixObserver& observer)
      : WrapperFrontendAction(std::move(wrapped)), observer_(observer) {}

 protected:
  bool BeginSourceFileAction(clang::CompilerInstance& ci) override {
    if (!WrapperFrontendAction::BeginSourceFileAction(ci)) return false;
    observer_.attach(ci.getPreprocessor());
    return true;
  }

 private:
  detail::PrefixObserver& observer_;
};

void add_note(AnalysisResult& result, std::string message) {
  AnalysisDiagnostic d;
  d.level = AnalysisDiagnostic::Level::note;
  d.message = std::move(message);
  result.diagnostics.push_back(std::move(d));
}

}  // namespace

std::string AnalysisDiagnostic::text() const {
  std::string out;
  if (file) {
    out += *file;
    out += ":" + std::to_string(line) + ":" + std::to_string(column) + ": ";
  }
  switch (level) {
    case Level::note:
      out += "note: ";
      break;
    case Level::remark:
      out += "remark: ";
      break;
    case Level::warning:
      out += "warning: ";
      break;
    case Level::error:
      out += "error: ";
      break;
    case Level::fatal:
      out += "fatal error: ";
      break;
  }
  out += message;
  return out;
}

bool AnalysisResult::has_errors() const {
  for (const auto& d : diagnostics) {
    if (d.level == AnalysisDiagnostic::Level::error || d.level == AnalysisDiagnostic::Level::fatal) return true;
  }
  return false;
}

AnalysisResult analyze_translation_unit(const AnalysisRequest& request) {
  AnalysisResult result;
  if (!request.command) {
    add_note(result, "rejected: request lacks a compile command");
    return result;
  }
  const CompileCommand& command = *request.command;
  if (!command.directory.is_absolute() || !command.file.is_absolute()) {
    add_note(result, "rejected: compile command directory and file must be absolute");
    return result;
  }
  // Provenance binding: the identity is recomputed from the command's own
  // content; a hand-written or stale id is refused rather than trusted.
  result.command_id = compute_command_id(command);
  if (!command.command_id.empty() && command.command_id != result.command_id) {
    add_note(result, "rejected: command_id '" + command.command_id +
                         "' does not match the command's content identity '" + result.command_id + "'");
    return result;
  }
  result.analysis_unit = request.analysis_unit.empty() ? result.command_id : request.analysis_unit;

  // Response-file expansion runs before normalization so that the existing
  // source, safety and duplicate-input gates see every option the original
  // driver would have seen. It is all-or-nothing: on any failure the raw
  // command is normalized instead, and because it still carries its `@` token
  // the context stays degraded and the unit rejects. Nothing partial is used.
  result.expansion = expand_response_files(command);
  result.replay_id = result.command_id;
  CompileCommand expanded_command;
  const CompileCommand* effective = &command;
  if (result.expansion.attempted) {
    if (result.expansion.ok) {
      expanded_command = command;  // directory, file and raw identity are unchanged
      expanded_command.arguments = result.expansion.arguments;
      effective = &expanded_command;
      result.replay_id = result.expansion.replay_id;
      add_note(result, "expanded " + std::to_string(result.expansion.files.size()) +
                           " response file(s) into " + std::to_string(result.expansion.arguments.size()) +
                           " argument(s); replay context " + result.replay_id);
    } else {
      for (const std::string& error : result.expansion.errors) {
        add_note(result, "response-file expansion refused: " + error);
      }
    }
  }
  // The context is always derived from the actual command.
  result.context = normalize_compile_context(*effective);
  const NormalizedCompileContext& context = result.context;
  result.context_status = context.status;
  result.context_complete = context.status == NormalizationStatus::ok;

  if (context.status == NormalizationStatus::unusable) {
    add_note(result, "rejected: compile context is unusable (driver not recognised); nothing was analysed");
    return result;
  }
  if (!context.source_identified) {
    add_note(result, "rejected: the command does not identify its source file; the analyzer never appends one");
    return result;
  }
  // Stage 2: a context degraded ONLY by PCH emulation may still be analysed, but
  // only after the supplied producer command proves that the `/Yu` header
  // produces the same preprocessor state under both contexts. Every other
  // degrading cause still rejects, and so does a failed or unattempted proof.
  llvm::IntrusiveRefCntPtr<detail::FrozenFileSystem> frozen_filesystem;
  std::vector<std::string> analyzer_arguments = context.analyzer_arguments;
  if (context.status == NormalizationStatus::degraded && context.pch_emulated && degraded_only_by_pch(context)) {
    detail::PchReplayRequest pch_request;
    pch_request.consumer = effective;
    pch_request.consumer_context = &result.context;
    pch_request.producer = request.pch_producer;
    pch_request.resource_dir = request.resource_dir;
    detail::PchReplayOutcome outcome = detail::verify_pch_replay(pch_request);
    result.pch = std::move(outcome.replay);
    if (result.pch.mode == PchReplayMode::textual_snapshot) {
      analyzer_arguments = std::move(outcome.analyzer_arguments);
      frozen_filesystem = outcome.filesystem;
      result.context.status = NormalizationStatus::ok;
      result.context_status = NormalizationStatus::ok;
      result.context_complete = true;
      // The replayed context is no longer described by the consumer's response
      // bytes alone: it also depends on the producer command, the producer's own
      // response bytes and the exact prefix inputs that were frozen.
      {
        Sha256 hash;
        const auto field = [&hash](std::string_view value) {
          hash.update(std::to_string(value.size()));
          hash.update(":");
          hash.update(value);
          hash.update("\n");
        };
        field("lcm.replay-context.pch.v1");
        field(result.replay_id);
        field(result.pch.producer_command_id);
        field(result.pch.producer_replay_id);
        field(result.pch.wrapper_header);
        field(result.pch.guard_path);
        field(result.pch.prefix_identity);
        result.replay_id = hash.finish().hex();
      }
      add_note(result, "precompiled header replayed textually against the supplied producer command; this is an "
                       "existing-input textual snapshot and makes no claim about the historical binary PCH or about "
                       "input currency (prefix identity " + result.pch.prefix_identity + ")");
    } else {
      add_note(result, "rejected: textual PCH replay was not established: " + result.pch.reject_reason);
      return result;
    }
  }

  if (context.status == NormalizationStatus::degraded) {
    // Bounded policy: degraded contexts (a response file that was not expanded,
    // PCH emulation without a verified producer, unsupported or malformed
    // options) reject before the front end. The dispositions say why.
    std::string why;
    for (const auto& d : context.dispositions) {
      if (d.action == OptionAction::dropped_unsupported || d.action == OptionAction::transformed) {
        if (!why.empty()) why += "; ";
        why += d.raw + ": " + d.reason;
      }
    }
    for (const auto& n : context.notes) {
      if (!why.empty()) why += "; ";
      why += n;
    }
    add_note(result, "rejected: compile context is degraded and would not be a verified context (" + why + ")");
    return result;
  }
  if (request.repository_root.empty() || !request.repository_root.is_absolute()) {
    add_note(result, "rejected: repository root must be an absolute path");
    return result;
  }
  if (request.resource_dir.empty() || !request.resource_dir.is_absolute()) {
    add_note(result, "rejected: a pinned absolute Clang resource directory is required");
    return result;
  }
  const SafetyVerdict verdict = check_analyzer_arguments(analyzer_arguments, context.driver, context.compiler_kind);
  if (!verdict.safe) {
    result.rejected_arguments = verdict.rejected;
    add_note(result, "rejected: " + std::to_string(verdict.rejected.size()) +
                         " argument(s) outside the analyzer allowlist; raw command preserved, nothing executed");
    return result;
  }
  // Per-unit filesystem view: an INDEPENDENT physical filesystem object whose
  // working directory is this command's directory. (The process-linked
  // getRealFileSystem() singleton would change the host process CWD.)
  llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> physical(llvm::vfs::createPhysicalFileSystem().release());
  // When a PCH prefix was validated, the final parse reads through the SAME
  // frozen buffers the validation read, so the bytes analysed are the bytes
  // proven equivalent rather than a re-read that might differ.
  llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> base =
      frozen_filesystem ? llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem>(frozen_filesystem) : physical;
  auto overlay = llvm::makeIntrusiveRefCnt<llvm::vfs::OverlayFileSystem>(base);
  if (std::error_code ec = overlay->setCurrentWorkingDirectory(path_to_utf8_generic(command.directory))) {
    add_note(result, "rejected: cannot use compile directory as working directory: " +
                         path_to_utf8_generic(command.directory) + " (" + ec.message() + ")");
    return result;
  }
  auto files = llvm::makeIntrusiveRefCnt<clang::FileManager>(clang::FileSystemOptions(), overlay);

  std::vector<std::string> argv;
  argv.push_back("lcm-analyzer");
  if (context.driver == DriverFlavor::msvc) {
    argv.push_back("--driver-mode=cl");
    argv.push_back("/Zs");
  } else {
    argv.push_back("--driver-mode=g++");
    argv.push_back("-fsyntax-only");
  }
  argv.push_back("-resource-dir=" + path_to_utf8_generic(request.resource_dir));
  argv.insert(argv.end(), analyzer_arguments.begin(), analyzer_arguments.end());
  // Our own invocation is always Clang (both driver modes honour `--`): the
  // entry path follows the delimiter so an option-shaped file name such as
  // `-dash.cc` is an input, never an option. This says nothing about how the
  // original compiler parsed its raw command; that was decided above.
  argv.push_back("--");
  argv.push_back(path_to_utf8_generic(command.file));

  detail::UnitContext unit;
  unit.request = &request;
  unit.result = &result;
  unit.analysis_unit = result.analysis_unit;

  CollectingDiagnostics diagnostics(result.diagnostics);
  std::unique_ptr<detail::PrefixObserver> prefix_observer;
  std::unique_ptr<clang::FrontendAction> action = std::make_unique<detail::AnalyzerAction>(unit);
  if (frozen_filesystem) {
    prefix_observer = std::make_unique<detail::PrefixObserver>(
        *frozen_filesystem, path_from_utf8(result.pch.wrapper_header), command.directory);
    action = std::make_unique<PrefixCheckedAction>(std::move(action), *prefix_observer);
  }
  clang::tooling::ToolInvocation invocation(argv, std::move(action), files.get());
  invocation.setDiagnosticConsumer(&diagnostics);
  const bool ran = invocation.run();

  if (prefix_observer) {
    result.pch.final_parse = prefix_observer->finish();
    const PchPrefixCapture& proven = result.pch.consumer;
    const PchPrefixCapture& actual = result.pch.final_parse;
    // The final parse is held to EVERY axis the prefix was proven on. The
    // captures parse too, so the measurement means the same thing in all three
    // runs: parser-registered pragma handlers are present everywhere, and the
    // token digest counts only ordinary tokens, never the parser's annotation
    // re-encoding of a pragma. Aligning the measurement is what makes this
    // comparison possible; nothing is excluded to make a unit pass.
    result.pch.final_axes_compared = {"preprocessor events", "token stream", "macro table", "entered files",
                                      "__COUNTER__", "prefix boundary"};
    if (!actual.boundary_reached) result.pch.final_drift.push_back("prefix boundary not reached");
    if (actual.event_digest != proven.event_digest) result.pch.final_drift.push_back("preprocessor events");
    if (actual.token_digest != proven.token_digest) result.pch.final_drift.push_back("token stream");
    if (actual.macro_digest != proven.macro_digest) result.pch.final_drift.push_back("macro table");
    if (actual.file_digest != proven.file_digest) result.pch.final_drift.push_back("entered files");
    if (actual.counter != proven.counter) result.pch.final_drift.push_back("__COUNTER__");
    if (!result.pch.final_drift.empty()) {
      // Fail closed: the analysed prefix is not the prefix that was proven.
      std::string why = "the PCH prefix analysed differs from the prefix that was validated (";
      for (std::size_t i = 0; i < result.pch.final_drift.size(); ++i) {
        if (i) why += ", ";
        why += result.pch.final_drift[i];
      }
      why += " differ)";
      result.status = AnalysisStatus::rejected;
      result.context_complete = false;
      result.pch.mode = PchReplayMode::rejected;
      result.pch.reject_reason = why;
      result.facts = FactSet{};
      result.files.clear();
      result.includes.clear();
      add_note(result, "rejected: " + why);
      return result;
    }
  }
  const bool errors = !ran || result.has_errors();
  result.status = errors ? AnalysisStatus::completed_with_errors : AnalysisStatus::ok;
  if (!ran && !result.has_errors()) add_note(result, "front end did not complete; see diagnostics");
  return result;
}

PublishOutcome publish_contribution(FactSet& target, const AnalysisResult& result, PublishPolicy policy) {
  PublishOutcome outcome;
  if (result.status == AnalysisStatus::rejected) {
    outcome.reason = "unit was rejected; nothing to publish";
    return outcome;
  }
  if (policy == PublishPolicy::accepted_only) {
    if (result.status != AnalysisStatus::ok) {
      outcome.reason = "unit completed with errors; provisional facts are not published under accepted_only";
      return outcome;
    }
    if (!result.context_complete) {
      outcome.reason = "compile context was degraded; facts are not published under accepted_only";
      return outcome;
    }
  }
  for (const auto& [id, symbol] : result.facts.symbols()) {
    if (symbol.presence == SymbolPresence::external_placeholder) {
      target.add_external_placeholder(symbol.key);
      ++outcome.placeholders;
      continue;
    }
    for (const auto& location : symbol.locations) {
      if (location.role == LocationRole::implicit_declaration) {
        target.add_hidden_member(symbol.key, location);
      } else {
        target.add_symbol_location(symbol.key, location);
      }
      ++outcome.symbol_locations;
    }
  }
  for (const auto& capture : result.facts.captures()) {
    target.add_capture(capture);
    ++outcome.captures;
  }
  for (const auto& include : result.facts.direct_includes()) {
    target.add_direct_include(include);
    ++outcome.direct_includes;
  }
  for (const auto& [key, relation] : result.facts.relations()) {
    for (const auto& evidence : relation.evidence) {
      target.add_relation_evidence(key, evidence);
      ++outcome.relation_evidence;
    }
  }
  for (const auto& site : result.facts.unresolved_sites()) {
    target.add_unresolved_site(site);
    ++outcome.unresolved_sites;
  }
  outcome.published = true;
  return outcome;
}

UnitSelection select_by_command_ids(const CompilationDatabase& database, const std::vector<std::string>& command_ids) {
  UnitSelection selection;
  std::set<std::string> wanted(command_ids.begin(), command_ids.end());
  std::set<std::string> found;
  for (const CompileCommand& command : database.commands) {
    if (wanted.count(command.command_id) && !found.count(command.command_id)) {
      selection.selected.push_back(&command);
      found.insert(command.command_id);
    }
  }
  for (const auto& id : command_ids) {
    if (!found.count(id)) selection.unknown_command_ids.push_back(id);
  }
  return selection;
}

UnitSelection select_by_files(const CompilationDatabase& database, const std::vector<std::filesystem::path>& files) {
  UnitSelection selection;
  for (const auto& file : files) {
    const CommandSelection choice = database.select_unique(file);
    switch (choice.kind) {
      case CommandSelection::Kind::unique:
        selection.selected.push_back(choice.command);
        break;
      case CommandSelection::Kind::ambiguous:
        selection.ambiguous.push_back(UnitSelection::Ambiguity{file, choice.candidates});
        break;
      case CommandSelection::Kind::none:
        selection.missing_files.push_back(file);
        break;
    }
  }
  return selection;
}

std::vector<AnalysisResult> analyze_selected(const UnitSelection& selection, const DatabaseAnalysisOptions& options) {
  std::vector<AnalysisResult> results;
  results.reserve(selection.selected.size());
  for (const CompileCommand* command : selection.selected) {
    AnalysisRequest request;
    request.command = command;
    request.repository_root = options.repository_root;
    request.repository_member = options.repository_member;
    request.resource_dir = options.resource_dir;
    results.push_back(analyze_translation_unit(request));
  }
  return results;
}

}  // namespace lcm::analyzer
