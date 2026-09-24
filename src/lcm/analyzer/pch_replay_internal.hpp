// Internal surface of the stage 2 PCH replay. Depends on LLVM types, so it is
// included only by the analyzer implementation and its own translation unit.
#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <clang/Lex/Preprocessor.h>
#include <llvm/ADT/IntrusiveRefCntPtr.h>
#include <llvm/Support/VirtualFileSystem.h>

#include "lcm/analyzer/pch_replay.hpp"
#include "lcm/compile_context.hpp"
#include "lcm/compile_db.hpp"

namespace lcm::analyzer::detail {

// A read-only filesystem that serves every file it has already served from one
// frozen buffer. The first open of a path reads the physical file and keeps the
// bytes; every later open, in ANY run sharing this object, returns those same
// bytes. Aliases of one physical file (case spellings, hardlinks) are registered
// against a single entry so `#pragma once` and include guards keep deduplicating.
//
// This is what makes "the bytes that were validated are the bytes that were
// analysed" a fact rather than an inference: validation and the final parse read
// the same buffers, so no stat/hash bookend and no TOCTOU claim is involved.
// Directory listing and working-directory handling stay with the physical
// filesystem, so include search behaves exactly as it does without the cache.
class FrozenFileSystem : public llvm::vfs::ProxyFileSystem {
 public:
  explicit FrozenFileSystem(llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> physical);
  ~FrozenFileSystem() override;

  llvm::ErrorOr<llvm::vfs::Status> status(const llvm::Twine& path) override;
  llvm::ErrorOr<std::unique_ptr<llvm::vfs::File>> openFileForRead(const llvm::Twine& path) override;

  // Adds a file that exists only in memory (the order guard). Fails if the path
  // is already known.
  bool add_virtual_file(const std::string& path, std::string contents);

  // Paths frozen so far, in first-open order, with the sha256 of their bytes.
  struct Frozen {
    std::string path;
    std::string sha256;
    std::size_t bytes = 0;
    bool virtual_only = false;
  };
  [[nodiscard]] const std::vector<Frozen>& frozen() const;

  // Closes the snapshot. Afterwards every file already read keeps serving its
  // frozen bytes AND every existence answer already given is replayed, so a path
  // that was absent during validation stays absent however the real filesystem
  // changes. Immutable buffers alone would not do this: a file appearing later
  // flips `__has_include` or an include search without any new file being read.
  void seal();
  // sha256 of the frozen bytes of `path`, or empty when it was never frozen.
  [[nodiscard]] std::string content_sha256(const std::string& path) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Measures a PCH prefix window during ANY run, using exactly the same recorder
// the validation captures use. The final parse is compared with this, so the
// prefix that was analysed is checked against the prefix that was proven on the
// same axes -- not by the weaker proxy of which files were entered.
class PrefixObserver {
 public:
  PrefixObserver(const FrozenFileSystem& frozen, std::filesystem::path wrapper,
                 std::filesystem::path directory);
  ~PrefixObserver();
  PrefixObserver(const PrefixObserver&) = delete;
  PrefixObserver& operator=(const PrefixObserver&) = delete;

  // Installs the preprocessor callbacks and the token watcher. The token axis
  // needs the watcher because the final parse's action lexes internally and no
  // callback exposes tokens.
  void attach(clang::Preprocessor& pp);
  [[nodiscard]] PchPrefixCapture finish();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

struct PchReplayRequest {
  const CompileCommand* consumer = nullptr;
  const NormalizedCompileContext* consumer_context = nullptr;
  const CompileCommand* producer = nullptr;
  std::filesystem::path resource_dir;
};

struct PchReplayOutcome {
  PchReplay replay;
  // Valid only when replay.mode == textual_snapshot: the analyzer arguments to
  // use instead of the normalized ones (order guard first, the `/Yu`-emulated
  // forced include de-duplicated, the rest of the `/FI` order preserved), and
  // the frozen filesystem the final parse must read through.
  std::vector<std::string> analyzer_arguments;
  llvm::IntrusiveRefCntPtr<FrozenFileSystem> filesystem;
};

// Runs the ladder and the two capture runs. Never writes to disk. `consumer`
// must already be normalized into `consumer_context`.
[[nodiscard]] PchReplayOutcome verify_pch_replay(const PchReplayRequest& request);

}  // namespace lcm::analyzer::detail
