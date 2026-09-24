#include "lcm/source.hpp"

#include <algorithm>
#include <cctype>

namespace lcm {

bool looks_absolute(std::string_view path) {
  if (path.empty()) return false;
  if (path[0] == '/' || path[0] == '\\') return true;
  if (path.size() >= 2 && path[1] == ':' && std::isalpha(static_cast<unsigned char>(path[0]))) return true;
  return false;
}

std::filesystem::path path_from_utf8(std::string_view utf8) {
  return std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string path_to_utf8_generic(const std::filesystem::path& path) {
  const std::u8string generic = path.generic_u8string();
  return std::string(reinterpret_cast<const char*>(generic.data()), generic.size());
}

namespace {

std::string strip_trailing_slashes(std::string s) {
  while (s.size() > 1 && s.back() == '/') s.pop_back();
  return s;
}

}  // namespace

bool is_canonical_repo_relative(std::string_view generic) {
  if (generic.empty() || looks_absolute(generic)) return false;
  if (generic.find('\\') != std::string_view::npos) return false;
  if (generic.back() == '/') return false;
  std::size_t start = 0;
  while (start <= generic.size()) {
    const std::size_t end = generic.find('/', start);
    const std::string_view segment = generic.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
    if (segment.empty() || segment == "." || segment == "..") return false;
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return true;
}

std::optional<RepoRelativePath> make_repo_relative(const std::filesystem::path& root,
                                                   const std::filesystem::path& absolute) {
  const std::string root_key = path_key(root);
  const std::string abs_key = path_key(absolute);
  if (abs_key.size() < root_key.size() || abs_key.compare(0, root_key.size(), root_key) != 0) {
    return std::nullopt;
  }
  if (abs_key.size() == root_key.size()) return RepoRelativePath{std::string{}, std::string{}};
  if (abs_key[root_key.size()] != '/') return std::nullopt;

  // The prefix matched under the comparison key. path_key only changes ASCII
  // case, so byte offsets are identical in the original spelling; take the
  // tail from there to preserve case.
  const std::string abs_generic = strip_trailing_slashes(path_to_utf8_generic(absolute.lexically_normal()));
  std::string tail = abs_generic.substr(root_key.size() + 1);
  if (!is_canonical_repo_relative(tail)) return std::nullopt;

  // Raw (un-normalised) tail for display when the raw spelling starts with
  // the same root; otherwise the normalised spelling is all we have.
  std::string as_written = tail;
  const std::string raw_generic = path_to_utf8_generic(absolute);
  if (raw_generic.size() > root_key.size() + 1 && raw_generic[root_key.size()] == '/' &&
      path_key(path_from_utf8(raw_generic.substr(0, root_key.size()))) == root_key) {
    as_written = raw_generic.substr(root_key.size() + 1);
  }
  return RepoRelativePath{std::move(tail), std::move(as_written)};
}

std::optional<RepoRelativePath> make_repo_relative_on_disk(const std::filesystem::path& root,
                                                           const std::filesystem::path& absolute) {
  auto lexical = make_repo_relative(root, absolute);
  std::error_code ec;
  if (!std::filesystem::exists(absolute, ec) || ec) return lexical;

  std::error_code root_ec;
  const std::filesystem::path canonical_root = std::filesystem::canonical(root, root_ec);
  std::error_code abs_ec;
  const std::filesystem::path canonical_abs = std::filesystem::canonical(absolute, abs_ec);
  if (root_ec || abs_ec) return lexical;

  // Containment by filesystem ancestry: walk up from the file until an
  // ancestor is the same directory object as the root. Spelling is never
  // compared, so a case-distinct sibling of the root (CaseRoot / caseroot on
  // a case-sensitive volume) is outside, while aliases of the root or of
  // path components resolve to the same objects and are inside. Symlinks are
  // followed by canonical(): a link inside the root pointing outside is
  // outside.
  if (same_existing_file(canonical_abs, canonical_root)) return RepoRelativePath{std::string{}, std::string{}};
  std::vector<std::string> tail;
  std::filesystem::path cursor = canonical_abs;
  while (true) {
    const std::filesystem::path parent = cursor.parent_path();
    if (parent.empty() || parent == cursor) return std::nullopt;  // reached the volume root
    tail.push_back(path_to_utf8_generic(cursor.filename()));
    if (same_existing_file(parent, canonical_root)) break;
    cursor = parent;
  }
  std::string generic;
  for (auto it = tail.rbegin(); it != tail.rend(); ++it) {
    if (!generic.empty()) generic += '/';
    generic += *it;
  }
  if (!is_canonical_repo_relative(generic)) return std::nullopt;
  RepoRelativePath on_disk{std::move(generic), lexical ? lexical->as_written : std::string{}};
  if (on_disk.as_written.empty()) on_disk.as_written = on_disk.generic;
  return on_disk;
}

std::string path_key(const std::filesystem::path& path) {
  std::string key = strip_trailing_slashes(path_to_utf8_generic(path.lexically_normal()));
#ifdef _WIN32
  for (char& c : key) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
#endif
  return key;
}

bool same_existing_file(const std::filesystem::path& a, const std::filesystem::path& b) {
  std::error_code ec;
  const bool same = std::filesystem::equivalent(a, b, ec);
  return !ec && same;
}

FileContentHash hash_file_content(std::string_view bytes) { return FileContentHash{sha256(bytes), bytes.size()}; }

LineIndex::LineIndex(std::string_view content) : size_(static_cast<std::uint32_t>(content.size())) {
  line_starts_.push_back(0);
  for (std::uint32_t i = 0; i < size_; ++i) {
    if (content[i] == '\n') line_starts_.push_back(i + 1);
  }
}

std::uint32_t LineIndex::line_count() const { return static_cast<std::uint32_t>(line_starts_.size()); }

std::pair<std::uint32_t, std::uint32_t> LineIndex::line_column(std::uint32_t offset) const {
  auto it = std::upper_bound(line_starts_.begin(), line_starts_.end(), offset);
  const auto line_idx = static_cast<std::uint32_t>(std::distance(line_starts_.begin(), it) - 1);
  return {line_idx + 1, offset - line_starts_[line_idx] + 1};
}

std::optional<SourceSpan> LineIndex::span(std::uint32_t begin_offset, std::uint32_t end_offset) const {
  if (begin_offset > end_offset || end_offset > size_) return std::nullopt;
  SourceSpan s;
  s.begin_offset = begin_offset;
  s.end_offset = end_offset;
  std::tie(s.begin_line, s.begin_column) = line_column(begin_offset);
  std::tie(s.end_line, s.end_column) = line_column(end_offset);
  return s;
}

std::optional<std::string_view> slice(std::string_view content, const SourceSpan& span) {
  if (span.begin_offset > span.end_offset || span.end_offset > content.size()) return std::nullopt;
  return content.substr(span.begin_offset, span.end_offset - span.begin_offset);
}

std::optional<Sha256Digest> hash_span(std::string_view content, const SourceSpan& span) {
  const auto bytes = slice(content, span);
  if (!bytes) return std::nullopt;
  return sha256(*bytes);
}

SnippetResult read_snippet(std::string_view current_bytes, const FileContentHash& recorded,
                           const SourceSpan& span) {
  if (hash_file_content(current_bytes) != recorded) return SnippetFailure::source_changed;
  const auto bytes = slice(current_bytes, span);
  if (!bytes) return SnippetFailure::span_out_of_range;
  return Snippet{std::string(*bytes), span, recorded};
}

}  // namespace lcm
