// Source content identity: repository-relative paths, byte spans, content
// hashes and hash-verified snippet extraction (PRD FR-RET-004, FR-IDX-004).
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "lcm/hash.hpp"

namespace lcm {

// Path relative to the repository/workspace member root.
//
// `generic` is the canonical identity spelling: forward slashes, lexically
// normal (no "." or ".." segments, no empty segments, no trailing slash) and,
// when produced by make_repo_relative_on_disk() for an existing file, the
// filesystem's own spelling of each component. Identity, ordering and hashing
// use `generic` only.
//
// `as_written` keeps the spelling that was observed (case, dot segments) for
// display; it never participates in comparison or identity.
struct RepoRelativePath {
  std::string generic;
  std::string as_written;

  [[nodiscard]] bool empty() const { return generic.empty(); }
  friend bool operator==(const RepoRelativePath& a, const RepoRelativePath& b) { return a.generic == b.generic; }
  friend auto operator<=>(const RepoRelativePath& a, const RepoRelativePath& b) { return a.generic <=> b.generic; }
};

// True when the string denotes an absolute or rooted path (drive letter,
// UNC, or leading slash). Such values must never appear in stable IDs.
[[nodiscard]] bool looks_absolute(std::string_view path);

// True when `generic` is an acceptable canonical repository-relative spelling:
// non-empty, relative, forward slashes only, no "." / ".." / empty segments,
// no trailing slash. Stable IDs only accept file scopes that pass this check.
[[nodiscard]] bool is_canonical_repo_relative(std::string_view generic);

// All std::string path spellings in LiveCodeMap are UTF-8. On Windows the
// native narrow conversion of std::filesystem::path uses the ANSI code page
// and is lossy, so conversions go through these two helpers only.
[[nodiscard]] std::filesystem::path path_from_utf8(std::string_view utf8);
[[nodiscard]] std::string path_to_utf8_generic(const std::filesystem::path& path);  // generic (forward slash) form

// Builds a repository-relative path for `absolute` under `root` using lexical
// normalisation only (no filesystem access). Returns nullopt when `absolute`
// is not inside `root`. Comparison of the root prefix follows path_key();
// `generic` keeps the case of `absolute`, `as_written` keeps its raw tail.
[[nodiscard]] std::optional<RepoRelativePath> make_repo_relative(const std::filesystem::path& root,
                                                                 const std::filesystem::path& absolute);

// Like make_repo_relative, but when `absolute` exists the filesystem is
// authoritative: containment is decided by walking the file's ancestors until
// one is the same directory object as the root (never by spelling), and the
// filesystem's own spelling (std::filesystem::canonical) is used for
// `generic`. Case aliases of one existing file therefore map to one identity,
// a case-distinct sibling of the root on a case-sensitive volume stays
// outside, and a symlink inside the root that resolves outside is outside.
// Non-existing paths fall back to the lexical rule. `as_written` records the
// observed spelling.
[[nodiscard]] std::optional<RepoRelativePath> make_repo_relative_on_disk(const std::filesystem::path& root,
                                                                         const std::filesystem::path& absolute);

// Comparison key for paths that may not exist on disk: lexically normal
// generic UTF-8 form without trailing slash. On Windows ASCII letters are
// folded to lower case (drive letters, typical build-tool casing). Non-ASCII
// characters are kept verbatim; full NTFS case folding is not emulated. For
// files that exist, use same_existing_file() which asks the filesystem.
[[nodiscard]] std::string path_key(const std::filesystem::path& path);

// True when both paths resolve to the same existing file or directory
// according to the filesystem (case/normalisation rules of the volume apply).
// False when either does not exist.
[[nodiscard]] bool same_existing_file(const std::filesystem::path& a, const std::filesystem::path& b);

// Half-open byte range [begin_offset, end_offset) plus 1-based line/column
// (column counted in bytes) of both ends.
struct SourceSpan {
  std::uint32_t begin_offset = 0;
  std::uint32_t end_offset = 0;
  std::uint32_t begin_line = 0;
  std::uint32_t begin_column = 0;
  std::uint32_t end_line = 0;
  std::uint32_t end_column = 0;

  friend bool operator==(const SourceSpan&, const SourceSpan&) = default;
  friend auto operator<=>(const SourceSpan&, const SourceSpan&) = default;
};

struct FileContentHash {
  Sha256Digest digest;
  std::uint64_t size = 0;

  friend bool operator==(const FileContentHash&, const FileContentHash&) = default;
  friend auto operator<=>(const FileContentHash&, const FileContentHash&) = default;
};

[[nodiscard]] FileContentHash hash_file_content(std::string_view bytes);

// Line-start table over one file's bytes for offset <-> line/column mapping.
// Lines are split on '\n'; a '\r' before it stays part of the line.
class LineIndex {
 public:
  explicit LineIndex(std::string_view content);

  [[nodiscard]] std::uint32_t line_count() const;
  // 1-based line and byte column for a byte offset in [0, size].
  [[nodiscard]] std::pair<std::uint32_t, std::uint32_t> line_column(std::uint32_t offset) const;
  // Span for [begin_offset, end_offset); nullopt when out of range or inverted.
  [[nodiscard]] std::optional<SourceSpan> span(std::uint32_t begin_offset, std::uint32_t end_offset) const;

 private:
  std::vector<std::uint32_t> line_starts_;
  std::uint32_t size_ = 0;
};

// Bytes of `content` covered by `span`; nullopt if the span is out of range.
[[nodiscard]] std::optional<std::string_view> slice(std::string_view content, const SourceSpan& span);

// Hash of the bytes a span covers, computed from the same content that
// produced the span. Stored next to evidence so span-level integrity can be
// checked independently of the whole file.
[[nodiscard]] std::optional<Sha256Digest> hash_span(std::string_view content, const SourceSpan& span);

struct Snippet {
  std::string text;
  SourceSpan span;
  FileContentHash source_hash;
};

enum class SnippetFailure {
  source_changed,     // current bytes differ from the hash that produced the span
  span_out_of_range,  // span does not fit in the recorded content
};

using SnippetResult = std::variant<Snippet, SnippetFailure>;

// Returns the snippet only when `current_bytes` hash to `recorded`, i.e. the
// bytes actually read are the bytes the span was computed from. Never quotes
// a stale span against changed content.
[[nodiscard]] SnippetResult read_snippet(std::string_view current_bytes, const FileContentHash& recorded,
                                         const SourceSpan& span);

}  // namespace lcm
