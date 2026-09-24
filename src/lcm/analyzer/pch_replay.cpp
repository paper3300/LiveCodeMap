#include "lcm/analyzer/pch_replay_internal.hpp"

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <optional>

#include <clang/Basic/FileManager.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendActions.h>
#include <clang/Basic/TokenKinds.h>
#include <clang/Lex/MacroArgs.h>
#include <clang/Lex/MacroInfo.h>
#include <clang/Lex/PPCallbacks.h>
#include <clang/Lex/Preprocessor.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/Support/MemoryBuffer.h>

#include "lcm/analyzer/safety.hpp"
#include "lcm/hash.hpp"
#include "lcm/response_file.hpp"
#include "lcm/source.hpp"

namespace lcm::analyzer {

std::string_view to_string(PchReplayMode value) {
  switch (value) {
    case PchReplayMode::not_requested:
      return "not_requested";
    case PchReplayMode::rejected:
      return "rejected";
    case PchReplayMode::textual_snapshot:
      return "textual_snapshot";
  }
  return "unknown";
}

namespace detail {
namespace {

// --- small helpers -----------------------------------------------------------

// MSVC file/PCH options are CASE-SENSITIVE and are not interchangeable with
// their lowercase namesakes: `/Fp` names a precompiled header while `/fp:` is
// the floating-point model, and `/FI` is a forced include while `/Fi` is
// preprocessed output. Matching them case-insensitively silently conflates the
// two, so option matching below is case-sensitive; only content sniffing (the
// binary PCH magic) is not.
bool starts_with(std::string_view value, std::string_view prefix) {
  return value.substr(0, prefix.size()) == prefix;
}

bool starts_with_ci(std::string_view value, std::string_view prefix) {
  if (value.size() < prefix.size()) return false;
  for (std::size_t i = 0; i < prefix.size(); ++i) {
    const auto a = static_cast<unsigned char>(value[i]);
    const auto b = static_cast<unsigned char>(prefix[i]);
    if (std::tolower(a) != std::tolower(b)) return false;
  }
  return true;
}

// Rolling digest. Events are folded in as they occur and never stored, so
// memory is bounded however many expansions the header chain performs.
class Rolling {
 public:
  void field(std::string_view value) {
    char header[24];
    const int n = std::snprintf(header, sizeof(header), "%zu:", value.size());
    sha_.update(std::string_view(header, static_cast<std::size_t>(n)));
    sha_.update(value);
    sha_.update(std::string_view("\n", 1));
  }
  void field(std::size_t value) { field(std::to_string(value)); }
  [[nodiscard]] std::string hex() { return sha_.finish().hex(); }

 private:
  Sha256 sha_;
};

std::string spelling_of(const clang::Token& token, clang::Preprocessor& pp) {
  // An ANNOTATION token carries no spelling at all -- asking for one is
  // undefined. The parser inserts them (for `#pragma pack` and friends), so the
  // final parse sees them where a preprocess-only capture never does.
  if (token.isAnnotation()) {
    return std::string("<annotation:") + clang::tok::getTokenName(token.getKind()) + ">";
  }
  if (const clang::IdentifierInfo* ii = token.getIdentifierInfo()) return ii->getName().str();
  return pp.getSpelling(token);
}

// A macro definition rendered so that parameter NAMES and ORDER and the
// variadic flag are all part of the comparison, not just the parameter count:
// `#define F(a,b)` and `#define F(b,a)` have the same arity and different
// meaning.
void fold_macro(Rolling& digest, const clang::MacroInfo* mi, clang::Preprocessor& pp) {
  if (!mi) {
    digest.field("<undefined>");
    return;
  }
  digest.field(mi->isFunctionLike() ? "function-like" : "object-like");
  digest.field(mi->isVariadic() ? "variadic" : "fixed");
  digest.field(mi->isGNUVarargs() ? "gnu-varargs" : "c99-varargs");
  digest.field(static_cast<std::size_t>(mi->getNumParams()));
  for (const clang::IdentifierInfo* param : mi->params()) {
    digest.field(param ? param->getName() : llvm::StringRef("<null>"));
  }
  digest.field(static_cast<std::size_t>(mi->getNumTokens()));
  for (const clang::Token& token : mi->tokens()) digest.field(spelling_of(token, pp));
}

// --- the frozen filesystem ---------------------------------------------------

struct Entry {
  std::string canonical;  // the first path this file was frozen under
  std::string sha256;
  std::size_t bytes = 0;
  bool virtual_only = false;
};

}  // namespace

struct FrozenFileSystem::Impl {
  // Keys are absolute and EXACT: the front end reports a file under whichever
  // spelling resolved it, so a relative `/I` and an absolute `/FI` must map to
  // one key -- but case is preserved, because folding it would make two
  // case-distinct files in a case-sensitive directory collide. Genuine aliases
  // of one file are collapsed separately, by asking the filesystem.
  std::string key_for(const std::string& path, llvm::vfs::FileSystem& base) const {
    const std::filesystem::path spelled = path_from_utf8(path);
    if (spelled.is_absolute()) return path_to_utf8_generic(spelled.lexically_normal());
    if (auto cwd = base.getCurrentWorkingDirectory()) {
      return path_to_utf8_generic(
          (path_from_utf8(std::string_view(cwd->data(), cwd->size())) / spelled).lexically_normal());
    }
    return path_to_utf8_generic(spelled.lexically_normal());
  }

  llvm::IntrusiveRefCntPtr<llvm::vfs::InMemoryFileSystem> memory{new llvm::vfs::InMemoryFileSystem(true)};
  std::map<std::string, std::string> path_to_canonical;  // path_key -> canonical path
  std::map<std::string, Entry> entries;                  // canonical path -> entry
  std::vector<Frozen> order;
  bool sealed = false;
  // Every path whose existence was ASKED ABOUT before sealing, and the answer.
  // Immutable buffers freeze what was read; they do not freeze what was merely
  // queried, so a file appearing between validation and the final parse would
  // otherwise flip `__has_include` or an include search without any new file
  // being entered.
  std::map<std::string, bool> query_outcome;
};

FrozenFileSystem::FrozenFileSystem(llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> physical)
    : ProxyFileSystem(std::move(physical)), impl_(std::make_unique<Impl>()) {}

FrozenFileSystem::~FrozenFileSystem() = default;

const std::vector<FrozenFileSystem::Frozen>& FrozenFileSystem::frozen() const { return impl_->order; }

void FrozenFileSystem::seal() { impl_->sealed = true; }

std::string FrozenFileSystem::content_sha256(const std::string& path) const {
  const auto it = impl_->path_to_canonical.find(impl_->key_for(path, const_cast<FrozenFileSystem&>(*this)));
  if (it == impl_->path_to_canonical.end()) return {};
  const auto entry = impl_->entries.find(it->second);
  return entry == impl_->entries.end() ? std::string() : entry->second.sha256;
}

bool FrozenFileSystem::add_virtual_file(const std::string& path, std::string contents) {
  const std::string key = impl_->key_for(path, *this);
  if (impl_->path_to_canonical.contains(key)) return false;
  const std::string digest = sha256(contents).hex();
  const std::size_t bytes = contents.size();
  if (!impl_->memory->addFile(path, 0, llvm::MemoryBuffer::getMemBufferCopy(contents, path))) return false;
  impl_->path_to_canonical[key] = path;
  impl_->entries[path] = Entry{path, digest, bytes, true};
  impl_->order.push_back(Frozen{path, digest, bytes, true});
  return true;
}

llvm::ErrorOr<llvm::vfs::Status> FrozenFileSystem::status(const llvm::Twine& path) {
  llvm::SmallString<256> buffer;
  const llvm::StringRef spelled = path.toStringRef(buffer);
  const std::string key = impl_->key_for(std::string(spelled.data(), spelled.size()), *this);
  if (const auto it = impl_->path_to_canonical.find(key); it != impl_->path_to_canonical.end()) {
    return impl_->memory->status(it->second);
  }
  if (impl_->sealed) {
    // Replay the answer validation got, so a path that was absent then is still
    // absent now however the real filesystem has changed.
    if (const auto known = impl_->query_outcome.find(key); known != impl_->query_outcome.end()) {
      if (!known->second) return std::make_error_code(std::errc::no_such_file_or_directory);
    }
  }
  // Also answer for paths the in-memory layer knows but the canonical map does
  // not -- in particular the PARENT DIRECTORIES of a virtual file. Clang's
  // FileManager resolves a file's directory before the file itself, so without
  // this the order guard is reported as not found even though its bytes are here.
  if (auto in_memory = impl_->memory->status(path)) return in_memory;
  auto physical = ProxyFileSystem::status(path);
  if (!impl_->sealed) impl_->query_outcome[key] = static_cast<bool>(physical);
  return physical;
}

llvm::ErrorOr<std::unique_ptr<llvm::vfs::File>> FrozenFileSystem::openFileForRead(const llvm::Twine& path) {
  llvm::SmallString<256> buffer;
  const llvm::StringRef spelled = path.toStringRef(buffer);
  const std::string spelled_str(spelled.data(), spelled.size());
  const std::string key = impl_->key_for(spelled_str, *this);
  if (const auto it = impl_->path_to_canonical.find(key); it != impl_->path_to_canonical.end()) {
    return impl_->memory->openFileForRead(it->second);
  }
  if (impl_->sealed) {
    // A path recorded as ABSENT during validation stays absent, so an open
    // cannot bypass the frozen query outcome that `status` replays.
    if (const auto known = impl_->query_outcome.find(key);
        known != impl_->query_outcome.end() && !known->second) {
      return std::make_error_code(std::errc::no_such_file_or_directory);
    }
  }

  // Not frozen yet: read it once from the physical filesystem and keep it.
  auto file = ProxyFileSystem::openFileForRead(path);
  if (!file) return file;
  const auto st = (*file)->status();
  if (!st) return llvm::errorToErrorCode(llvm::createStringError(st.getError(), "status failed"));
  auto contents = (*file)->getBuffer(spelled, /*FileSize=*/-1, /*RequiresNullTerminator=*/true);
  if (!contents) return llvm::errorToErrorCode(llvm::createStringError(contents.getError(), "read failed"));
  const llvm::StringRef bytes = (*contents)->getBuffer();

  // Alias handling: two spellings of one physical file share a UniqueID, so the
  // second spelling is registered as a hard link to the first rather than as a
  // second in-memory file. Without this one header reached through two include
  // prefixes would get two identities and `#pragma once` would stop working.
  std::string canonical;
  for (const auto& [existing_key, existing_path] : impl_->path_to_canonical) {
    (void)existing_key;
    const auto& entry = impl_->entries[existing_path];
    if (entry.virtual_only) continue;
    std::error_code ec;
    if (same_existing_file(path_from_utf8(existing_path), path_from_utf8(spelled_str))) {
      canonical = existing_path;
      break;
    }
    (void)ec;
  }
  if (!canonical.empty()) {
    impl_->memory->addHardLink(spelled_str, canonical);
    impl_->path_to_canonical[key] = canonical;
    return impl_->memory->openFileForRead(spelled_str);
  }

  const std::string digest = sha256(std::string_view(bytes.data(), bytes.size())).hex();
  impl_->memory->addFile(spelled_str, 0, llvm::MemoryBuffer::getMemBufferCopy(bytes, spelled));
  impl_->path_to_canonical[key] = spelled_str;
  impl_->entries[spelled_str] = Entry{spelled_str, digest, bytes.size(), false};
  impl_->order.push_back(Frozen{spelled_str, digest, bytes.size(), false});
  return impl_->memory->openFileForRead(spelled_str);
}

namespace {

// --- the capture run ---------------------------------------------------------

struct Capture {
  PchPrefixCapture out;
  Rolling events;
  Rolling tokens;
  Rolling macros;
  Rolling files;
  std::string wrapper_key;          // folded key, used only as a cheap prefilter
  std::filesystem::path wrapper_path;  // the real file; identity is decided against this
  // The compiler reports a file under the spelling that RESOLVED it, which is
  // relative whenever the include directory was relative. Names are made
  // absolute against this directory before they are compared or hashed.
  std::filesystem::path directory;
  const FrozenFileSystem* frozen = nullptr;
  int window_depth = -1;
  bool in_window = false;
  // Files entered directly by the main source file. For the producer capture
  // this is how the wrapper-only boundary is VERIFIED rather than assumed: the
  // producer source must pull in the wrapper and nothing else.
  std::vector<std::string> top_level_entries;
  // Every path whose contents were entered inside the window, in order.
  std::vector<std::string> window_files;
  bool boundary_captured = false;
};

// Folds the macro table and the counter as they stand AT THE PREFIX BOUNDARY.
// Taking them at end of translation would absorb everything the unit defines
// after the PCH -- its second forced include and its own headers -- and call
// that the prefix state.
void capture_boundary_state(Capture& capture, clang::Preprocessor& pp) {
  if (capture.boundary_captured) return;
  capture.boundary_captured = true;
  capture.out.boundary_reached = true;
  std::map<std::string, const clang::MacroInfo*> table;
  for (const auto& entry : pp.macros()) {
    auto* ii = const_cast<clang::IdentifierInfo*>(entry.first);
    const clang::MacroDefinition def = pp.getMacroDefinition(ii);
    if (!def) continue;
    table[ii->getName().str()] = def.getMacroInfo();
  }
  for (const auto& [name, info] : table) {
    capture.macros.field(name);
    fold_macro(capture.macros, info, pp);
  }
  capture.out.counter = pp.getCounterValue();
}

class Recorder : public clang::PPCallbacks {
 public:
  Recorder(clang::Preprocessor& pp, Capture& capture) : pp_(pp), capture_(capture) {}

  // Absolute, normalized form of a name the front end reported.
  std::string absolute_of(llvm::StringRef name) const {
    const std::filesystem::path spelled = path_from_utf8(std::string_view(name.data(), name.size()));
    return path_to_utf8_generic(
        (spelled.is_absolute() ? spelled : capture_.directory / spelled).lexically_normal());
  }

  // The prefix window is exactly the wrapper header: everything it causes to be
  // read, and nothing before or after it. The consumer legitimately opens more
  // files once the wrapper exits -- its second forced include, its own headers
  // -- and those are not part of what the precompiled header contributed.
  void FileChanged(clang::SourceLocation loc, FileChangeReason reason, clang::SrcMgr::CharacteristicKind,
                   clang::FileID prev_fid) override {
    clang::SourceManager& sm = pp_.getSourceManager();
    if (reason == ExitFile) {
      // PrevFID is documented as the file that was exited, so the window closes
      // on the wrapper's own exit. Inferring it from the location returned to
      // would depend on that file having an entry, which a forced include's
      // predefines parent does not.
      if (capture_.in_window && prev_fid == wrapper_fid_) {
        capture_.in_window = false;
        capture_boundary_state(capture_, pp_);
      }
      return;
    }
    if (reason != EnterFile) return;
    const clang::FileID fid = sm.getFileID(loc);
    const clang::OptionalFileEntryRef entry = sm.getFileEntryRefForID(fid);
    if (!entry) return;
    const std::string absolute = absolute_of(entry->getName());
    // The window is ONE-SHOT. Repeated forced includes are observable, so an
    // unguarded wrapper can legitimately be entered again later; that later
    // entry is ordinary post-prefix work, not a second prefix. Reopening here
    // would keep rolling events and files after the boundary and report valid
    // processing as drift.
    // Folded comparison is a prefilter only; the filesystem confirms identity,
    // so a case-distinct sibling of the wrapper never opens the window.
    if (!capture_.in_window && !capture_.boundary_captured &&
        path_key(path_from_utf8(absolute)) == capture_.wrapper_key &&
        same_existing_file(path_from_utf8(absolute), capture_.wrapper_path)) {
      capture_.in_window = true;
      capture_.out.window_seen = true;
      wrapper_fid_ = fid;
    }
    if (capture_.in_window) {
      ++capture_.out.files;
      capture_.window_files.push_back(absolute);
      // Identity by CONTENT, not by the spelling that reached the file: the
      // producer reaches the wrapper through an #include in its own source and
      // the consumer through a forced include, so the resolved names
      // legitimately differ for one and the same file.
      const std::string digest = capture_.frozen ? capture_.frozen->content_sha256(absolute) : std::string();
      capture_.files.field(digest.empty() ? std::string("<unfrozen>") : digest);
      capture_.files.field(static_cast<std::size_t>(entry->getSize()));
    }
  }

  void PragmaDirective(clang::SourceLocation loc, clang::PragmaIntroducerKind introducer) override {
    if (!capture_.in_window) return;
    event("pragma");
    capture_.events.field(static_cast<std::size_t>(introducer));
    // A raw re-lex of the reported location is NOT used: at a macro expansion
    // the spelling there is the macro's unexpanded parameter, not the pragma's
    // effective argument. Position and origin are recorded; the arguments come
    // from the expansion events below, which carry the real tokens.
    const clang::SourceManager& sm = pp_.getSourceManager();
    capture_.events.field(loc.isMacroID() ? "macro-origin" : "file-origin");
    if (loc.isMacroID()) {
      capture_.events.field(clang::Lexer::getImmediateMacroName(loc, sm, pp_.getLangOpts()));
    }
  }
  void PragmaWarningPush(clang::SourceLocation, int level) override {
    if (!capture_.in_window) return;
    event("warning-push");
    capture_.events.field(static_cast<std::size_t>(level < 0 ? 0 : level));
  }
  void PragmaWarningPop(clang::SourceLocation) override {
    if (capture_.in_window) event("warning-pop");
  }
  void PragmaWarning(clang::SourceLocation, PragmaWarningSpecifier spec, llvm::ArrayRef<int> ids) override {
    if (!capture_.in_window) return;
    event("warning");
    capture_.events.field(static_cast<std::size_t>(spec));
    for (int id : ids) capture_.events.field(static_cast<std::size_t>(id));
  }
  void PragmaDetectMismatch(clang::SourceLocation, llvm::StringRef name, llvm::StringRef value) override {
    if (!capture_.in_window) return;
    event("detect-mismatch");
    capture_.events.field(name);
    capture_.events.field(value);
  }

  void MacroDefined(const clang::Token& name, const clang::MacroDirective* md) override {
    if (!capture_.in_window) return;
    event("define");
    capture_.events.field(spelling_of(name, pp_));
    fold_macro(capture_.events, md ? md->getMacroInfo() : nullptr, pp_);
  }
  void MacroUndefined(const clang::Token& name, const clang::MacroDefinition& md,
                      const clang::MacroDirective*) override {
    if (!capture_.in_window) return;
    event("undef");
    capture_.events.field(spelling_of(name, pp_));
    fold_macro(capture_.events, md.getMacroInfo(), pp_);
  }
  // The term that closes the transient-argument hole: the macro's definition IN
  // FORCE at this expansion plus the ACTUAL argument tokens. Comparing names and
  // final definitions alone accepts two prefixes that push different identifiers
  // onto the unreadable push-macro stack.
  void MacroExpands(const clang::Token& name, const clang::MacroDefinition& md, clang::SourceRange,
                    const clang::MacroArgs* args) override {
    if (!capture_.in_window) return;
    event("expand");
    capture_.events.field(spelling_of(name, pp_));
    fold_macro(capture_.events, md.getMacroInfo(), pp_);
    if (!args) {
      capture_.events.field("no-args");
      return;
    }
    capture_.events.field(static_cast<std::size_t>(args->getNumMacroArguments()));
    for (unsigned i = 0; i < args->getNumMacroArguments(); ++i) {
      capture_.events.field("arg");
      for (const clang::Token* t = args->getUnexpArgument(i); t && t->isNot(clang::tok::eof); ++t) {
        capture_.events.field(spelling_of(*t, pp_));
      }
    }
  }
  void Defined(const clang::Token& name, const clang::MacroDefinition& md, clang::SourceRange) override {
    if (!capture_.in_window) return;
    event("defined-query");
    capture_.events.field(spelling_of(name, pp_));
    capture_.events.field(md ? "yes" : "no");
  }
  void If(clang::SourceLocation, clang::SourceRange, ConditionValueKind value) override {
    if (!capture_.in_window) return;
    event("if");
    capture_.events.field(static_cast<std::size_t>(value));
  }
  void Elif(clang::SourceLocation, clang::SourceRange, ConditionValueKind value, clang::SourceLocation) override {
    if (!capture_.in_window) return;
    event("elif");
    capture_.events.field(static_cast<std::size_t>(value));
  }
  void Ifdef(clang::SourceLocation, const clang::Token& name, const clang::MacroDefinition& md) override {
    if (!capture_.in_window) return;
    event("ifdef");
    capture_.events.field(spelling_of(name, pp_));
    capture_.events.field(md ? "yes" : "no");
  }
  void Ifndef(clang::SourceLocation, const clang::Token& name, const clang::MacroDefinition& md) override {
    if (!capture_.in_window) return;
    event("ifndef");
    capture_.events.field(spelling_of(name, pp_));
    capture_.events.field(md ? "yes" : "no");
  }
  void InclusionDirective(clang::SourceLocation hash_loc, const clang::Token&, llvm::StringRef file, bool angled,
                          clang::CharSourceRange, clang::OptionalFileEntryRef found, llvm::StringRef,
                          llvm::StringRef, const clang::Module*, bool,
                          clang::SrcMgr::CharacteristicKind) override {
    // Recorded regardless of the window: this is how the producer's
    // wrapper-only boundary is verified, by observing what its own source
    // actually includes.
    clang::SourceManager& sm = pp_.getSourceManager();
    if (sm.getFileID(hash_loc) == sm.getMainFileID()) {
      capture_.top_level_entries.push_back(found ? absolute_of(found->getName()) : std::string(file));
    }
    if (!capture_.in_window) return;
    event("include");
    capture_.events.field(file);
    capture_.events.field(angled ? "angled" : "quoted");
    // Recorded by RESOLVED file, deliberately not by which search path found it:
    // preprocessor behaviour depends on the file that was opened. The producer
    // and consumer legitimately differ in their include lists, and this is the
    // axis where that difference is allowed to be invisible -- but only when it
    // resolves to the same file.
    capture_.events.field(found ? absolute_of(found->getName()) : std::string("<not found>"));
  }
  void HasInclude(clang::SourceLocation, llvm::StringRef file, bool angled, clang::OptionalFileEntryRef found,
                  clang::SrcMgr::CharacteristicKind) override {
    if (!capture_.in_window) return;
    event("has-include");
    capture_.events.field(file);
    capture_.events.field(angled ? "angled" : "quoted");
    capture_.events.field(found ? absolute_of(found->getName()) : std::string("<not found>"));
  }
  void FileSkipped(const clang::FileEntryRef& skipped, const clang::Token&,
                   clang::SrcMgr::CharacteristicKind) override {
    if (!capture_.in_window) return;
    event("skipped");
    capture_.events.field(absolute_of(skipped.getName()));
  }

 private:
  void event(std::string_view kind) {
    ++capture_.out.events;
    capture_.events.field(kind);
  }
  clang::Preprocessor& pp_;
  Capture& capture_;
  clang::FileID wrapper_fid_;
};

// Installs the recorder and the canonical token watcher.
void attach_prefix_recorder(clang::Preprocessor& pp, Capture& capture) {
  pp.addPPCallbacks(std::make_unique<Recorder>(pp, capture));
  Capture* target = &capture;
  clang::Preprocessor* preprocessor = &pp;
  pp.setTokenWatcher([target, preprocessor](const clang::Token& token) {
    // ANNOTATION tokens are the parser's re-encoding of a pragma and have no
    // spelling at all. They are excluded so the token axis means the same thing
    // in every run; the pragma itself is still compared, as an event.
    if (!target->in_window || token.isAnnotation()) return;
    target->tokens.field(spelling_of(token, *preprocessor));
  });
}

void finish_capture(Capture& capture) {
  capture.out.event_digest = capture.events.hex();
  capture.out.token_digest = capture.tokens.hex();
  capture.out.macro_digest = capture.macros.hex();
  capture.out.file_digest = capture.files.hex();
}

// PARSER-AWARE capture. The final analysis parses, and the Parser registers
// pragma handlers the preprocessor does not have on its own (`intrinsic`,
// `optimize`, `vtordisp`, `float_control`, `clang loop`, `comment`). Those
// handlers lex their own arguments THROUGH the preprocessor, so a macro used as
// a pragma argument expands -- and fires MacroExpands -- only when a parser is
// present. Measured in build/implementer-replay-20260923/stage2-probe: that is
// the entire event difference between the two modes.
//
// So the capture parses too. Aligning the measurement is the fix; excluding the
// axis would have been a weaker contract dressed up as a technical limitation.
class CaptureAction : public clang::SyntaxOnlyAction {
 public:
  explicit CaptureAction(Capture& capture) : capture_(capture) {}

 protected:
  bool BeginSourceFileAction(clang::CompilerInstance& ci) override {
    if (!clang::SyntaxOnlyAction::BeginSourceFileAction(ci)) return false;
    attach_prefix_recorder(ci.getPreprocessor(), capture_);
    return true;
  }
  void EndSourceFileAction() override {
    finish_capture(capture_);
    clang::SyntaxOnlyAction::EndSourceFileAction();
  }

 private:
  Capture& capture_;
};

class CaptureDiagnostics : public clang::DiagnosticConsumer {
 public:
  CaptureDiagnostics(std::vector<std::string>& sink, const Capture& capture) : sink_(sink), capture_(capture) {}
  void HandleDiagnostic(clang::DiagnosticsEngine::Level level, const clang::Diagnostic& info) override {
    clang::DiagnosticConsumer::HandleDiagnostic(level, info);
    if (level < clang::DiagnosticsEngine::Error) return;
    // Only errors raised while the PREFIX is being processed are prefix errors.
    // The capture main is the command's real source and the capture supplies
    // only the guard and the wrapper, so the unit body legitimately misses its
    // other forced includes; complaints from there say nothing about the prefix.
    if (capture_.boundary_captured) return;
    llvm::SmallString<256> text;
    info.FormatDiagnostic(text);
    sink_.push_back(std::string(text));
  }

 private:
  std::vector<std::string>& sink_;
  const Capture& capture_;
};

struct CaptureResult {
  PchPrefixCapture out;
  std::vector<std::string> top_level_entries;
  std::vector<std::string> window_files;
};


}  // namespace

struct PrefixObserver::Impl {
  Capture capture;
};

PrefixObserver::PrefixObserver(const FrozenFileSystem& frozen, std::filesystem::path wrapper,
                               std::filesystem::path directory)
    : impl_(std::make_unique<Impl>()) {
  impl_->capture.wrapper_key = path_key(wrapper);
  impl_->capture.wrapper_path = std::move(wrapper);
  impl_->capture.directory = std::move(directory);
  impl_->capture.frozen = &frozen;
}

PrefixObserver::~PrefixObserver() = default;

void PrefixObserver::attach(clang::Preprocessor& pp) { attach_prefix_recorder(pp, impl_->capture); }

PchPrefixCapture PrefixObserver::finish() {
  finish_capture(impl_->capture);
  return std::move(impl_->capture.out);
}

namespace {

// Runs one capture. When `forced_prefix` is true the main file is a synthetic
// empty source and the prefix arrives through the order guard plus the wrapper
// as forced includes (the consumer shape). Otherwise `main_source` is the
// producer's own translation unit and the wrapper must arrive through it.
CaptureResult capture_prefix(FrozenFileSystem& fs, const std::filesystem::path& directory,
                             const std::filesystem::path& resource_dir, const std::string& guard,
                             const std::string& wrapper, const std::vector<std::string>& options,
                             const std::string& main_source, bool forced_prefix) {
  Capture capture;
  capture.wrapper_key = path_key(path_from_utf8(wrapper));
  capture.wrapper_path = path_from_utf8(wrapper);
  capture.directory = directory;
  capture.frozen = &fs;

  llvm::IntrusiveRefCntPtr<FrozenFileSystem> vfs(&fs);
  vfs->setCurrentWorkingDirectory(path_to_utf8_generic(directory));
  auto files = llvm::makeIntrusiveRefCnt<clang::FileManager>(clang::FileSystemOptions(), vfs);

  std::vector<std::string> argv{"lcm-pch-capture", "--driver-mode=cl", "/Zs"};
  argv.push_back("-resource-dir=" + path_to_utf8_generic(resource_dir));
  if (forced_prefix) {
    argv.push_back("/FI" + guard);
    argv.push_back("/FI" + wrapper);
  }
  argv.insert(argv.end(), options.begin(), options.end());
  argv.push_back("--");
  argv.push_back(main_source);

  CaptureDiagnostics diagnostics(capture.out.diagnostics, capture);
  clang::tooling::ToolInvocation invocation(argv, std::make_unique<CaptureAction>(capture), files.get());
  invocation.setDiagnosticConsumer(&diagnostics);
  invocation.run();
  CaptureResult result;
  result.out = std::move(capture.out);
  result.top_level_entries = std::move(capture.top_level_entries);
  result.window_files = std::move(capture.window_files);
  return result;
}

// --- ladder helpers ----------------------------------------------------------

std::optional<std::string> attached_value(const std::vector<std::string>& argv, std::string_view option) {
  for (const std::string& argument : argv) {
    if (argument.size() <= option.size() + 1) continue;
    if ((argument[0] != '/' && argument[0] != '-')) continue;
    if (starts_with(std::string_view(argument).substr(1), option)) {
      return argument.substr(1 + option.size());
    }
  }
  return std::nullopt;
}

std::size_t count_option(const std::vector<std::string>& argv, std::string_view option) {
  std::size_t n = 0;
  for (const std::string& argument : argv) {
    if (argument.size() <= option.size()) continue;
    if ((argument[0] != '/' && argument[0] != '-')) continue;
    if (starts_with(std::string_view(argument).substr(1), option)) ++n;
  }
  return n;
}

// Identity of two path spellings. When both exist the FILESYSTEM decides, so a
// case alias or a hardlink of one file matches while two case-distinct files in
// a case-sensitive directory stay apart. Unconditional case folding is not
// identity; it is only the fallback for paths that do not exist and therefore
// cannot be compared any other way.
bool same_path(std::string_view a, std::string_view b) {
  const std::filesystem::path pa = path_from_utf8(a);
  const std::filesystem::path pb = path_from_utf8(b);
  std::error_code ea;
  std::error_code eb;
  const bool a_exists = std::filesystem::exists(pa, ea) && !ea;
  const bool b_exists = std::filesystem::exists(pb, eb) && !eb;
  if (a_exists && b_exists) return same_existing_file(pa, pb);
  return path_key(pa) == path_key(pb);
}

// Options that must agree between producer and consumer for the prefix to mean
// the same thing. Include directories are deliberately NOT here: they are
// allowed to differ, and the capture comparison decides whether the difference
// mattered.
std::string semantic_signature(const NormalizedCompileContext& context) {
  Rolling digest;
  digest.field(context.language_standard.value_or("<none>"));
  digest.field(context.language.value_or("<none>"));
  digest.field(context.runtime_library.value_or("<none>"));
  // ORDER IS SIGNIFICANT and must not be normalised away: `/DX=1 /DX=2` and
  // `/DX=2 /DX=1` leave different macros in force, and later semantic switches
  // override earlier ones. Sorting these would hide exactly the disagreements
  // this signature exists to catch.
  for (const std::string& d : context.defines) digest.field("D" + d);
  for (const std::string& u : context.undefines) digest.field("U" + u);
  for (const auto& disposition : context.dispositions) {
    const bool semantic = disposition.category == OptionCategory::semantics ||
                          disposition.category == OptionCategory::language ||
                          disposition.category == OptionCategory::language_standard ||
                          disposition.category == OptionCategory::runtime_library;
    if (!semantic || disposition.action != OptionAction::kept) continue;
    for (const std::string& token : disposition.replacement) digest.field(token);
  }
  return digest.hex();
}

// Analyzer options minus everything PCH-related and minus forced includes: the
// capture supplies its own `/FI` pair so both runs see exactly the same prefix.
std::vector<std::string> capture_options(const NormalizedCompileContext& context) {
  std::vector<std::string> out;
  for (const std::string& argument : context.analyzer_arguments) {
    // Only the PCH options and forced includes are withheld, because the
    // capture supplies its own prefix. EVERY other option is kept, including
    // one-letter ones such as native-cl `/J`: dropping a short option would
    // make the captured prefix a different prefix from the one the final parse
    // uses, and equal-but-wrong captures prove nothing.
    if (argument.size() >= 3) {
      const std::string_view body = std::string_view(argument).substr(1);
      if (starts_with(body, "FI") || starts_with(body, "Yu") || starts_with(body, "Fp")) continue;
    }
    out.push_back(argument);
  }
  return out;
}

std::string first_forced_include(const NormalizedCompileContext& context) {
  return context.forced_includes.empty() ? std::string() : context.forced_includes.front();
}

bool looks_like_binary_pch(const std::filesystem::path& header) {
  std::ifstream input(header, std::ios::binary);
  if (!input) return false;
  char magic[8] = {};
  input.read(magic, sizeof(magic));
  const std::string_view head(magic, static_cast<std::size_t>(input.gcount()));
  return starts_with_ci(head, "VCPCH") || starts_with_ci(head, "CPCH");
}

// The producer's source must be a wrapper translation unit: nothing but one
// include of the /Yc header. Checked against the file's actual CONTENT, not
// against a file-name resemblance, and confirmed again during the capture by
// observing what the source actually pulled in.
// The producer's source must be a wrapper translation unit and nothing else:
// comments, whitespace and EXACTLY ONE include directive. There is no pragma
// exemption. A `#pragma push_macro` before the wrapper mutates preprocessor
// state that the capture window -- which opens only when the wrapper is entered
// -- never sees, so the boundary could be accepted with unequal hidden state.
// Anything that is not a comment, whitespace or that single include fails
// closed. Bytes come from the frozen snapshot, so what is validated here is what
// the capture reads.
std::string wrapper_only_source_problem(std::string_view text, std::string& include_spelling,
                                        bool& include_is_angled) {
  std::size_t i = 0;
  int includes = 0;
  while (i < text.size()) {
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
    if (i >= text.size()) break;
    if (text.compare(i, 2, "//") == 0) {
      const std::size_t nl = text.find('\n', i);
      if (nl == std::string_view::npos) break;
      i = nl;
      continue;
    }
    if (text.compare(i, 2, "/*") == 0) {
      const std::size_t end = text.find("*/", i + 2);
      if (end == std::string_view::npos) return "the producer source has an unterminated comment";
      i = end + 2;
      continue;
    }
    if (text[i] != '#') return "the producer source contains code outside its single wrapper include";
    const std::size_t line_end = std::min(text.find('\n', i), text.size());
    const std::string line(text.substr(i, line_end - i));
    i = line_end;
    const std::size_t name = line.find_first_not_of(" \t", 1);
    if (name == std::string::npos) return "the producer source contains a bare '#' directive";
    const std::size_t name_end = line.find_first_of(" \t\"<", name);
    const std::string directive = line.substr(name, name_end == std::string::npos ? std::string::npos : name_end - name);
    if (directive != "include") {
      return "the producer source uses the directive '#" + directive +
             "'; a wrapper-only boundary allows comments, whitespace and exactly one #include";
    }
    const std::size_t open = line.find_first_of("\"<", name);
    if (open == std::string::npos) return "the producer include is malformed";
    include_is_angled = line[open] == '<';
    const char close = include_is_angled ? '>' : '"';
    const std::size_t close_at = line.find(close, open + 1);
    if (close_at == std::string::npos) return "the producer include is malformed";
    include_spelling = line.substr(open + 1, close_at - open - 1);
    ++includes;
  }
  if (includes != 1) {
    return "the producer source does not consist of exactly one wrapper include (found " +
           std::to_string(includes) + ")";
  }
  return {};
}

// Resolves a header spelling the way the command's own include search does.
// An absolute spelling is taken as written; otherwise the quote directory (for
// a quoted spelling) is tried first, then the command's include directories in
// the order the normalizer recorded them. Resolving `/Yu` or `/Yc` only against
// the working directory rejects the ordinary UE and MSVC shape, where the
// header is named plainly and found through `/I`.
std::optional<std::filesystem::path> resolve_header(std::string_view spelling,
                                                    const NormalizedCompileContext& context,
                                                    const std::filesystem::path& directory,
                                                    const std::filesystem::path& quote_dir, bool angled) {
  const std::filesystem::path spelled = path_from_utf8(spelling);
  std::error_code ec;
  const auto usable = [&ec](const std::filesystem::path& candidate) {
    return std::filesystem::is_regular_file(candidate, ec) && !ec;
  };
  if (spelled.is_absolute()) {
    return usable(spelled) ? std::optional<std::filesystem::path>(spelled.lexically_normal()) : std::nullopt;
  }
  if (!angled && !quote_dir.empty()) {
    const std::filesystem::path candidate = (quote_dir / spelled).lexically_normal();
    if (usable(candidate)) return candidate;
  }
  const std::filesystem::path relative_to_cwd = (directory / spelled).lexically_normal();
  if (usable(relative_to_cwd)) return relative_to_cwd;
  for (const IncludeDirectory& dir : context.includes) {
    const std::filesystem::path candidate = (dir.resolved / spelled).lexically_normal();
    if (usable(candidate)) return candidate;
  }
  return std::nullopt;
}


PchReplayOutcome reject(PchReplay replay, std::string reason) {
  replay.mode = PchReplayMode::rejected;
  replay.reject_reason = std::move(reason);
  PchReplayOutcome outcome;
  outcome.replay = std::move(replay);
  return outcome;
}

}  // namespace

PchReplayOutcome verify_pch_replay(const PchReplayRequest& request) {
  PchReplay replay;
  if (!request.consumer || !request.consumer_context) return reject(std::move(replay), "no consumer command");
  const CompileCommand& consumer = *request.consumer;
  const NormalizedCompileContext& cc = *request.consumer_context;
  replay.consumer_command_id = compute_command_id(consumer);

  if (!request.producer) {
    return reject(std::move(replay),
                  "the unit emulates a precompiled header, but no producer compile command was supplied; textual "
                  "PCH replay is never attempted without the command that built the PCH");
  }
  const CompileCommand& raw_producer = *request.producer;
  replay.producer_command_id = compute_command_id(raw_producer);
  // The producer is held to the same provenance rule as the consumer: a
  // supplied identity that disagrees with the command's content is refused
  // rather than trusted.
  if (!raw_producer.command_id.empty() && raw_producer.command_id != replay.producer_command_id) {
    return reject(std::move(replay), "the producer command_id does not match its content identity");
  }
  // ...and to the same response-file contract, so its real options are seen.
  const ResponseExpansion producer_expansion = expand_response_files(raw_producer);
  CompileCommand expanded_producer = raw_producer;
  if (producer_expansion.attempted) {
    if (!producer_expansion.ok) {
      std::string why = "the producer response file could not be expanded";
      if (!producer_expansion.errors.empty()) why += ": " + producer_expansion.errors.front();
      return reject(std::move(replay), why);
    }
    expanded_producer.arguments = producer_expansion.arguments;
    replay.producer_replay_id = producer_expansion.replay_id;
  } else {
    replay.producer_replay_id = replay.producer_command_id;
  }
  replay.producer_expansion = producer_expansion;
  const CompileCommand& producer = expanded_producer;

  // --- rung 1: same compiler, same working directory -------------------------
  const NormalizedCompileContext pc = normalize_compile_context(producer);
  if (cc.compiler_kind != CompilerKind::native_cl || pc.compiler_kind != CompilerKind::native_cl) {
    return reject(std::move(replay), "textual PCH replay is verified for native cl.exe only");
  }
  if (consumer.arguments.empty() || producer.arguments.empty() ||
      !same_path(consumer.arguments.front(), producer.arguments.front())) {
    return reject(std::move(replay), "producer and consumer were built by different compiler executables");
  }
  if (!same_path(path_to_utf8_generic(consumer.directory), path_to_utf8_generic(producer.directory))) {
    return reject(std::move(replay), "producer and consumer have different working directories");
  }
  if (!pc.source_identified) {
    return reject(std::move(replay), "the producer command does not identify its own source file");
  }
  if (pc.status == NormalizationStatus::unusable) {
    return reject(std::move(replay), "the producer compile context is unusable");
  }
  // The producer may be degraded only by creating a PCH; anything else it could
  // not honour (extra inputs, unsupported options, an unexpanded response file)
  // disqualifies it as a source of truth.
  for (const auto& d : pc.dispositions) {
    if (d.action == OptionAction::dropped_unsupported) {
      return reject(std::move(replay), "the producer context is degraded: " + d.raw + ": " + d.reason);
    }
  }
  if (!pc.unexpanded_response_files.empty()) {
    return reject(std::move(replay), "the producer context still contains an unexpanded response file");
  }

  // --- the safety gate runs on BOTH contexts BEFORE anything is executed -----
  // No capture may run on arguments the analyzer would refuse to forward: an
  // unknown or dangerous option must reject the unit, not reach a front end
  // through the validation path.
  for (const auto& [label, context] :
       {std::pair<const char*, const NormalizedCompileContext*>{"consumer", &cc},
        std::pair<const char*, const NormalizedCompileContext*>{"producer", &pc}}) {
    const SafetyVerdict verdict =
        check_analyzer_arguments(context->analyzer_arguments, context->driver, context->compiler_kind);
    if (!verdict.safe) {
      return reject(std::move(replay), std::string("the ") + label + " command has " +
                                           std::to_string(verdict.rejected.size()) +
                                           " argument(s) outside the analyzer allowlist: " + verdict.rejected.front());
    }
  }

  // --- rung 2: unique, conflict-free PCH options ----------------------------
  if (count_option(consumer.arguments, "Y-") > 0 || count_option(producer.arguments, "Y-") > 0) {
    return reject(std::move(replay), "a /Y- disables precompiled headers; the /Yu context is contradictory");
  }
  if (count_option(consumer.arguments, "Yu") != 1) {
    return reject(std::move(replay), "the consumer must name exactly one /Yu header");
  }
  if (count_option(consumer.arguments, "Yc") != 0) {
    return reject(std::move(replay), "the consumer both uses and creates a precompiled header");
  }
  if (count_option(producer.arguments, "Yc") != 1) {
    return reject(std::move(replay), "the producer must name exactly one /Yc header");
  }
  if (count_option(producer.arguments, "Yu") != 0) {
    return reject(std::move(replay), "the producer both creates and uses a precompiled header");
  }
  if (count_option(consumer.arguments, "Fp") != 1 || count_option(producer.arguments, "Fp") != 1) {
    return reject(std::move(replay),
                  "each command must name exactly one /Fp precompiled header (consumer has " +
                      std::to_string(count_option(consumer.arguments, "Fp")) + ", producer has " +
                      std::to_string(count_option(producer.arguments, "Fp")) + ")");
  }
  // Producer forced-include policy: the producer's prefix must come from its own
  // source, so a forced include would silently add state the consumer never
  // replays.
  if (!pc.forced_includes.empty()) {
    return reject(std::move(replay), "the producer uses forced includes; only a wrapper-only boundary is supported");
  }

  const std::optional<std::string> yc = attached_value(producer.arguments, "Yc");
  const std::optional<std::string> yu = attached_value(consumer.arguments, "Yu");
  if (!yu || yu->empty()) return reject(std::move(replay), "the consumer does not name a /Yu header");
  if (!yc || yc->empty()) return reject(std::move(replay), "the producer does not name a /Yc header");
  replay.wrapper_header = *yu;

  // --- rung 3: the same PCH artifact ----------------------------------------
  const std::optional<std::string> consumer_fp = attached_value(consumer.arguments, "Fp");
  const std::optional<std::string> producer_fp = attached_value(producer.arguments, "Fp");
  if (!consumer_fp || !producer_fp) return reject(std::move(replay), "a /Fp path is missing");
  if (!same_path(*consumer_fp, *producer_fp)) {
    return reject(std::move(replay), "producer and consumer name different /Fp precompiled headers");
  }
  replay.pch_binary = *consumer_fp;

  // --- rung 4: the supported /Yu layout, resolved through include search ----
  // `/Yu`, `/Yc` and `/FI` are ordinarily spelled plainly and found through the
  // command's own `/I` list (the shape UE and MSVC use), so comparing spellings
  // or resolving only against the working directory rejects valid input. Every
  // one of them is resolved the way the compiler would, then compared by file.
  const std::filesystem::path consumer_quote_dir = consumer.file.parent_path();
  const std::optional<std::filesystem::path> wrapper_resolved =
      resolve_header(*yu, cc, consumer.directory, consumer_quote_dir, /*angled=*/false);
  if (!wrapper_resolved) {
    return reject(std::move(replay),
                  "the /Yu header '" + *yu + "' does not resolve to a readable file through the consumer's "
                  "include search");
  }
  const std::filesystem::path wrapper_path = *wrapper_resolved;
  replay.wrapper_header = path_to_utf8_generic(wrapper_path);

  const std::string first_fi = first_forced_include(cc);
  if (first_fi.empty()) {
    return reject(std::move(replay), "the consumer has no forced include; this /Yu layout is not supported");
  }
  const std::optional<std::filesystem::path> first_fi_resolved =
      resolve_header(first_fi, cc, consumer.directory, consumer_quote_dir, /*angled=*/false);
  if (!first_fi_resolved || !same_existing_file(*first_fi_resolved, wrapper_path)) {
    return reject(std::move(replay),
                  "the consumer's first forced include ('" + first_fi + "') is not its /Yu header ('" + *yu +
                      "'); this /Yu layout is not supported");
  }
  std::error_code ec;
  if (looks_like_binary_pch(wrapper_path)) {
    return reject(std::move(replay), "the /Yu header is a binary precompiled header, not a text header");
  }

  // --- rung 5: the producer really built THIS wrapper's PCH -----------------
  // Presence of a `/Yc` proves nothing. It must resolve, through the producer's
  // own include search, to the same file as the consumer's `/Yu`; otherwise a
  // command that precompiled a different header would be accepted as this
  // unit's producer.
  const std::filesystem::path producer_quote_dir = producer.file.parent_path();
  const std::optional<std::filesystem::path> yc_resolved =
      resolve_header(*yc, pc, producer.directory, producer_quote_dir, /*angled=*/false);
  if (!yc_resolved) {
    return reject(std::move(replay),
                  "the producer's /Yc header '" + *yc + "' does not resolve through its own include search");
  }
  if (!same_existing_file(*yc_resolved, wrapper_path)) {
    return reject(std::move(replay),
                  "the producer's /Yc header ('" + *yc + "' -> " + path_to_utf8_generic(*yc_resolved) +
                      ") is not the consumer's /Yu header (" + path_to_utf8_generic(wrapper_path) +
                      "); this producer did not build this precompiled header");
  }

  // --- rung 6: no conflicting semantic options ------------------------------
  if (semantic_signature(cc) != semantic_signature(pc)) {
    return reject(std::move(replay),
                  "producer and consumer disagree on language, runtime, macro or semantic options (order included)");
  }

  // --- the order guard, in memory only --------------------------------------
  Rolling guard_digest;
  guard_digest.field("lcm.pch-order-guard.v1");
  guard_digest.field(replay.consumer_command_id);
  guard_digest.field(replay.producer_command_id);
  guard_digest.field(path_to_utf8_generic(wrapper_path));
  // Rooted on the same volume as the command directory: a driveless absolute
  // path would be re-rooted against the working directory by the driver, and the
  // in-memory entry would then never be found.
  const std::string virtual_root = path_to_utf8_generic(consumer.directory.root_path()) + "lcm-virtual/";
  const std::string guard_path = virtual_root + "pch-order-guard-" + guard_digest.hex() + ".h";
  replay.guard_path = guard_path;
  // The guard exists so the real wrapper is never the FIRST forced include: the
  // driver rewrites only the first `/FI` into `-include-pch` when a sidecar
  // exists beside it. Both the guard path and its own would-be sidecar must be
  // absent from the real filesystem, or the guard could shadow a real input or
  // itself be substituted.
  if (std::filesystem::exists(path_from_utf8(guard_path), ec) ||
      std::filesystem::exists(path_from_utf8(guard_path + ".pch"), ec)) {
    return reject(std::move(replay), "the generated order-guard path collides with a real file on disk");
  }

  llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> physical(llvm::vfs::createPhysicalFileSystem().release());
  auto frozen = llvm::makeIntrusiveRefCnt<FrozenFileSystem>(physical);
  if (!frozen->add_virtual_file(guard_path, "")) {
    return reject(std::move(replay), "the order guard could not be created in memory");
  }
  // The capture main files are the REAL sources of each command. A synthetic
  // in-memory main was tried first and does not work: the driver existence-checks
  // its inputs outside the filesystem it was handed, the same bypass that makes
  // the implicit sidecar lookup unavoidable. Using the real sources is also more
  // faithful, and costs nothing extra because the comparison window closes when
  // the wrapper header exits, long before the unit body matters.

  // --- the producer boundary, validated from the FROZEN bytes ---------------
  // Reading the source directly here would validate bytes that the capture need
  // not then read: the snapshot is what the front end sees, so the wrapper-only
  // check is made against the same immutable buffer.
  const std::string producer_source_path = path_to_utf8_generic(producer.file);
  std::string producer_source_text;
  {
    auto opened = frozen->openFileForRead(producer_source_path);
    if (!opened) {
      return reject(std::move(replay), "the producer source could not be read: " + producer_source_path);
    }
    auto buffer = (*opened)->getBuffer(producer_source_path);
    if (!buffer) {
      return reject(std::move(replay), "the producer source could not be buffered: " + producer_source_path);
    }
    producer_source_text.assign((*buffer)->getBuffer().data(), (*buffer)->getBuffer().size());
  }
  std::string producer_include;
  bool producer_include_angled = false;
  if (const std::string problem =
          wrapper_only_source_problem(producer_source_text, producer_include, producer_include_angled);
      !problem.empty()) {
    return reject(std::move(replay), problem);
  }
  replay.producer_wrapper_include = producer_include;
  // ...and that single include must itself be the wrapper, resolved the way the
  // producer would resolve it. `/Yc` naming the wrapper is not enough if the
  // source actually includes something else.
  const std::optional<std::filesystem::path> producer_include_resolved =
      resolve_header(producer_include, pc, producer.directory, producer_quote_dir, producer_include_angled);
  if (!producer_include_resolved || !same_existing_file(*producer_include_resolved, wrapper_path)) {
    return reject(std::move(replay),
                  "the producer source includes '" + producer_include +
                      "', which is not the /Yc wrapper header; only a wrapper-only boundary is supported");
  }

  // --- the argument list the replay will use, built BEFORE the captures -----
  // The consumer capture then runs on exactly these arguments, so what is proven
  // is literally what is replayed rather than an approximation of it.
  //
  // The `/Yu` emulation appended ONE synthetic forced include. It is removed BY
  // ORIGIN: the normalizer appends each disposition's replacement tokens to
  // `analyzer_arguments` in order, so walking the dispositions gives the exact
  // index the `/Yu` transformation produced. Scanning for "the last matching
  // /FI" would delete a genuine later forced include the user wrote and silently
  // reorder the observable prefix.
  std::vector<std::size_t> synthetic_indices;
  {
    std::size_t emitted = 0;
    for (const auto& d : cc.dispositions) {
      const bool contributes = d.action == OptionAction::kept || d.action == OptionAction::kept_unknown ||
                               d.action == OptionAction::transformed;
      if (contributes && d.action == OptionAction::transformed &&
          d.category == OptionCategory::precompiled_header) {
        for (std::size_t k = 0; k < d.replacement.size(); ++k) synthetic_indices.push_back(emitted + k);
      }
      if (contributes) emitted += d.replacement.size();
    }
  }
  std::vector<std::string> replay_arguments;
  replay_arguments.push_back("/FI" + guard_path);
  for (std::size_t i = 0; i < cc.analyzer_arguments.size(); ++i) {
    if (std::find(synthetic_indices.begin(), synthetic_indices.end(), i) != synthetic_indices.end()) continue;
    replay_arguments.push_back(cc.analyzer_arguments[i]);
  }

  // --- the two capture runs, sharing one frozen snapshot --------------------
  const std::string wrapper = path_to_utf8_generic(wrapper_path);
  CaptureResult producer_capture =
      capture_prefix(*frozen, producer.directory, request.resource_dir, guard_path, wrapper, capture_options(pc),
                     path_to_utf8_generic(producer.file), /*forced_prefix=*/false);
  // The consumer capture IS the companion for the final parse: same frozen
  // filesystem, same working directory, and the exact replay argument list.
  CaptureResult consumer_capture =
      capture_prefix(*frozen, consumer.directory, request.resource_dir, guard_path, wrapper, replay_arguments,
                     path_to_utf8_generic(consumer.file), /*forced_prefix=*/false);
  replay.producer = producer_capture.out;
  replay.consumer = consumer_capture.out;

  if (!replay.producer.diagnostics.empty() || !replay.consumer.diagnostics.empty()) {
    const std::string& first = replay.producer.diagnostics.empty() ? replay.consumer.diagnostics.front()
                                                                   : replay.producer.diagnostics.front();
    return reject(std::move(replay), "a prefix capture reported front-end errors: " + first);
  }
  if (!replay.producer.window_seen || !replay.consumer.window_seen) {
    std::string seen;
    const auto& entries = replay.producer.window_seen ? consumer_capture.top_level_entries
                                                      : producer_capture.top_level_entries;
    for (const std::string& e : entries) {
      if (!seen.empty()) seen += ", ";
      seen += e;
    }
    return reject(std::move(replay),
                  std::string("the /Yu header was not entered during the ") +
                      (replay.producer.window_seen ? "consumer" : "producer") + " prefix capture (expected " +
                      wrapper + ", top-level entries were [" + seen + "])");
  }
  if (!replay.producer.boundary_reached || !replay.consumer.boundary_reached) {
    return reject(std::move(replay), "a prefix capture never reached the /Yu header's closing boundary");
  }
  // The wrapper-only boundary, confirmed by observation: the producer source
  // pulled in the wrapper and nothing else.
  if (producer_capture.top_level_entries.size() != 1 ||
      path_key(path_from_utf8(producer_capture.top_level_entries.front())) != path_key(wrapper_path)) {
    std::string observed;
    for (const std::string& entry : producer_capture.top_level_entries) {
      if (!observed.empty()) observed += ", ";
      observed += entry;
    }
    return reject(std::move(replay),
                  "the producer source did not resolve to exactly the /Yc wrapper header (expected " +
                      path_to_utf8_generic(wrapper_path) + ", observed [" + observed +
                      "]); only a wrapper-only boundary is supported");
  }

  if (replay.producer.event_digest != replay.consumer.event_digest) replay.mismatches.push_back("preprocessor events");
  if (replay.producer.token_digest != replay.consumer.token_digest) replay.mismatches.push_back("token stream");
  if (replay.producer.macro_digest != replay.consumer.macro_digest) replay.mismatches.push_back("macro table");
  if (replay.producer.file_digest != replay.consumer.file_digest) replay.mismatches.push_back("entered files");
  if (replay.producer.counter != replay.consumer.counter) replay.mismatches.push_back("__COUNTER__");
  if (!replay.mismatches.empty()) {
    std::string why = "the producer and consumer prefixes are not equivalent (";
    for (std::size_t i = 0; i < replay.mismatches.size(); ++i) {
      if (i) why += ", ";
      why += replay.mismatches[i];
    }
    return reject(std::move(replay), why + " differ)");
  }

  // --- accepted: freeze the verified prefix and build the replay arguments ---
  Rolling prefix_digest;
  prefix_digest.field("lcm.pch-prefix.v1");
  for (const auto& file : frozen->frozen()) {
    prefix_digest.field(file.path);
    prefix_digest.field(file.sha256);
    prefix_digest.field(file.bytes);
  }
  replay.prefix_identity = prefix_digest.hex();
  replay.prefix_files = consumer_capture.window_files;
  frozen->seal();

  PchReplayOutcome outcome;
  outcome.filesystem = frozen;
  outcome.analyzer_arguments = replay_arguments;
  replay.mode = PchReplayMode::textual_snapshot;
  replay.replay_arguments = outcome.analyzer_arguments;
  outcome.replay = std::move(replay);
  return outcome;
}

}  // namespace detail
}  // namespace lcm::analyzer
