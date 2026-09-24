#include "lcm/response_file.hpp"

#include <algorithm>
#include <fstream>
#include <system_error>

#include "lcm/compile_context.hpp"
#include "lcm/hash.hpp"
#include "lcm/source.hpp"

namespace lcm {
namespace {

// --- byte-level decoding -----------------------------------------------------
// Only the three encodings the native cl.exe oracle validated are accepted. A
// BOM-less non-ASCII file is refused instead of being guessed at as UTF-8 or as
// the active code page: that guess is exactly what misdecodes on this compiler.

struct Decoded {
  ResponseEncoding encoding = ResponseEncoding::ascii;
  std::string text;   // UTF-8
  std::string error;  // non-empty: nothing was decoded
};

constexpr std::string_view kUtf8Bom = "\xEF\xBB\xBF";
constexpr std::string_view kUtf16LeBom = "\xFF\xFE";
constexpr std::string_view kUtf16BeBom = "\xFE\xFF";
constexpr std::string_view kUtf32LeBom{"\xFF\xFE\x00\x00", 4};
constexpr std::string_view kUtf32BeBom{"\x00\x00\xFE\xFF", 4};

bool starts_with(std::string_view s, std::string_view prefix) { return s.substr(0, prefix.size()) == prefix; }

void append_utf8(std::string& out, std::uint32_t code_point) {
  if (code_point < 0x80) {
    out.push_back(static_cast<char>(code_point));
  } else if (code_point < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
    out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  } else if (code_point < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
    out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
    out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  }
}

// Strict UTF-8: rejects overlong forms, surrogate code points, values above
// U+10FFFF, truncated sequences and embedded NUL.
std::string validate_utf8(std::string_view bytes) {
  std::size_t i = 0;
  while (i < bytes.size()) {
    const auto b0 = static_cast<unsigned char>(bytes[i]);
    if (b0 == 0) return "embedded NUL byte";
    std::size_t length = 0;
    std::uint32_t code_point = 0;
    if (b0 < 0x80) {
      ++i;
      continue;
    }
    if ((b0 & 0xE0) == 0xC0) {
      length = 2;
      code_point = b0 & 0x1Fu;
    } else if ((b0 & 0xF0) == 0xE0) {
      length = 3;
      code_point = b0 & 0x0Fu;
    } else if ((b0 & 0xF8) == 0xF0) {
      length = 4;
      code_point = b0 & 0x07u;
    } else {
      return "invalid UTF-8 lead byte";
    }
    if (i + length > bytes.size()) return "truncated UTF-8 sequence";
    for (std::size_t k = 1; k < length; ++k) {
      const auto bk = static_cast<unsigned char>(bytes[i + k]);
      if ((bk & 0xC0) != 0x80) return "invalid UTF-8 continuation byte";
      code_point = (code_point << 6) | (bk & 0x3Fu);
    }
    if (length == 2 && code_point < 0x80) return "overlong UTF-8 sequence";
    if (length == 3 && code_point < 0x800) return "overlong UTF-8 sequence";
    if (length == 4 && code_point < 0x10000) return "overlong UTF-8 sequence";
    if (code_point >= 0xD800 && code_point <= 0xDFFF) return "UTF-8 encoded surrogate code point";
    if (code_point > 0x10FFFF) return "UTF-8 code point above U+10FFFF";
    i += length;
  }
  return {};
}

Decoded decode_utf16le(std::string_view bytes) {
  Decoded out;
  out.encoding = ResponseEncoding::utf16le_bom;
  if (bytes.size() % 2 != 0) {
    out.error = "UTF-16LE content has an odd number of bytes";
    return out;
  }
  for (std::size_t i = 0; i < bytes.size(); i += 2) {
    const auto lo = static_cast<unsigned char>(bytes[i]);
    const auto hi = static_cast<unsigned char>(bytes[i + 1]);
    std::uint32_t unit = static_cast<std::uint32_t>(lo) | (static_cast<std::uint32_t>(hi) << 8);
    if (unit == 0) {
      out.error = "embedded NUL code unit";
      return out;
    }
    if (unit >= 0xDC00 && unit <= 0xDFFF) {
      out.error = "unpaired UTF-16 low surrogate";
      return out;
    }
    if (unit >= 0xD800 && unit <= 0xDBFF) {
      if (i + 4 > bytes.size()) {
        out.error = "truncated UTF-16 surrogate pair";
        return out;
      }
      const auto lo2 = static_cast<unsigned char>(bytes[i + 2]);
      const auto hi2 = static_cast<unsigned char>(bytes[i + 3]);
      const std::uint32_t trail = static_cast<std::uint32_t>(lo2) | (static_cast<std::uint32_t>(hi2) << 8);
      if (trail < 0xDC00 || trail > 0xDFFF) {
        out.error = "unpaired UTF-16 high surrogate";
        return out;
      }
      unit = 0x10000 + ((unit - 0xD800) << 10) + (trail - 0xDC00);
      i += 2;
    }
    append_utf8(out.text, unit);
  }
  return out;
}

Decoded decode(std::string_view bytes) {
  Decoded out;
  // UTF-32LE shares its first two bytes with UTF-16LE, so it is ruled out first.
  if (starts_with(bytes, kUtf32LeBom)) {
    out.error = "UTF-32LE byte order mark is not a supported response-file encoding";
    return out;
  }
  if (starts_with(bytes, kUtf32BeBom)) {
    out.error = "UTF-32BE byte order mark is not a supported response-file encoding";
    return out;
  }
  if (starts_with(bytes, kUtf16BeBom)) {
    out.error = "UTF-16BE byte order mark is not a supported response-file encoding";
    return out;
  }
  if (starts_with(bytes, kUtf8Bom)) {
    const std::string_view body = bytes.substr(kUtf8Bom.size());
    if (std::string problem = validate_utf8(body); !problem.empty()) {
      out.error = "UTF-8 with byte order mark is malformed: " + problem;
      return out;
    }
    out.encoding = ResponseEncoding::utf8_bom;
    out.text = std::string(body);
    return out;
  }
  if (starts_with(bytes, kUtf16LeBom)) return decode_utf16le(bytes.substr(kUtf16LeBom.size()));

  for (std::size_t i = 0; i < bytes.size(); ++i) {
    const auto b = static_cast<unsigned char>(bytes[i]);
    if (b == 0) {
      out.error = "embedded NUL byte";
      return out;
    }
    if (b >= 0x80) {
      out.error =
          "non-ASCII byte without a byte order mark; the verified compiler misdecodes such content, so the "
          "encoding is not guessed (supported: ASCII without a BOM, UTF-8 with a BOM, UTF-16LE with a BOM)";
      return out;
    }
  }
  out.encoding = ResponseEncoding::ascii;
  out.text = std::string(bytes);
  return out;
}

// --- expansion ---------------------------------------------------------------

struct Expander {
  const CompileCommand& command;
  const ResponseFileLimits& limits;
  ResponseExpansion out;
  std::size_t total_bytes = 0;
  std::size_t visited = 0;  // cumulative references and examined tokens, cache hits included
  // Tokens of each snapshot, parsed once from the bytes that were hashed.
  std::vector<std::vector<std::string>> snapshot_tokens;
  // Every file on the current resolution path: a repeat here is recursion, a
  // repeat outside it is legal reuse. Compared with same_response_file(), so a
  // case or hardlink alias of a file already on the path is still recursion.
  std::vector<std::filesystem::path> active;

  Expander(const CompileCommand& c, const ResponseFileLimits& l) : command(c), limits(l) {}

  void fail(std::string message) { out.errors.push_back(std::move(message)); }

  // Charges one unit of expansion work. Every `@` reference and every token
  // examined is charged, even when the file's snapshot is already cached, so
  // that repeated and diamond-shaped expansions terminate.
  bool spend() {
    if (visited >= limits.max_visited_tokens) {
      fail("response-file expansion examined more than " + std::to_string(limits.max_visited_tokens) +
           " references and tokens; the input expands to more work than the limit allows");
      return false;
    }
    ++visited;
    return true;
  }

  // True when two paths denote the same response file. When both exist the
  // FILESYSTEM is authoritative, so hardlinks and case aliases of one file
  // collapse while genuinely distinct siblings on a case-sensitive directory
  // stay apart; path_key() is only the fallback for paths that do not exist.
  // Deciding this lexically would treat `CASE.rsp` as a cache hit for a
  // different existing `case.rsp` and never read or hash its bytes.
  static bool same_response_file(const std::filesystem::path& a, const std::filesystem::path& b) {
    std::error_code ea;
    std::error_code eb;
    const bool a_exists = std::filesystem::exists(a, ea) && !ea;
    const bool b_exists = std::filesystem::exists(b, eb) && !eb;
    if (a_exists && b_exists) return same_existing_file(a, b);
    return path_key(a) == path_key(b);
  }

  // Index of the snapshot for `resolved`, reading and hashing it on first use.
  // Returns SIZE_MAX after recording an error.
  std::size_t snapshot_for(const std::string& as_written, const std::filesystem::path& resolved, unsigned depth) {
    for (std::size_t i = 0; i < out.files.size(); ++i) {
      if (!same_response_file(out.files[i].resolved, resolved)) continue;
      // Legal repeated reference. The spelling that reached it is evidence in
      // its own right: an alias resolves to the same bytes but is not the same
      // token, so every distinct spelling is recorded and hashed.
      ResponseFileSnapshot& hit = out.files[i];
      const std::string alias = path_to_utf8_generic(resolved);
      if (alias != hit.resolved_utf8 &&
          std::find(hit.alias_spellings.begin(), hit.alias_spellings.end(), alias) == hit.alias_spellings.end()) {
        hit.alias_spellings.push_back(alias);
      }
      return i;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(resolved, ec) || ec) {
      fail("response file '" + as_written + "' does not resolve to a readable file (" +
           path_to_utf8_generic(resolved) + ")");
      return static_cast<std::size_t>(-1);
    }
    const auto size = std::filesystem::file_size(resolved, ec);
    if (ec) {
      fail("cannot determine the size of response file '" + path_to_utf8_generic(resolved) + "'");
      return static_cast<std::size_t>(-1);
    }
    if (size > limits.max_file_bytes) {
      fail("response file '" + path_to_utf8_generic(resolved) + "' is " + std::to_string(size) +
           " bytes, over the per-file limit of " + std::to_string(limits.max_file_bytes));
      return static_cast<std::size_t>(-1);
    }
    if (total_bytes + size > limits.max_total_bytes) {
      fail("response files total more than the aggregate limit of " + std::to_string(limits.max_total_bytes) +
           " bytes at '" + path_to_utf8_generic(resolved) + "'");
      return static_cast<std::size_t>(-1);
    }
    std::ifstream input(resolved, std::ios::binary);
    if (!input) {
      fail("cannot open response file '" + path_to_utf8_generic(resolved) + "'");
      return static_cast<std::size_t>(-1);
    }
    std::string bytes(static_cast<std::size_t>(size), '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(size));
    if (static_cast<std::size_t>(input.gcount()) != bytes.size()) {
      fail("response file '" + path_to_utf8_generic(resolved) + "' changed size while it was being read");
      return static_cast<std::size_t>(-1);
    }
    // Hash and parse the same bytes: nothing re-reads the file afterwards.
    const Decoded decoded = decode(bytes);
    if (!decoded.error.empty()) {
      fail("response file '" + path_to_utf8_generic(resolved) + "': " + decoded.error);
      return static_cast<std::size_t>(-1);
    }
    const TokenizedCommand tokenized = tokenize_command_line(decoded.text, CommandSyntax::windows);
    if (tokenized.unterminated_quote) {
      fail("response file '" + path_to_utf8_generic(resolved) + "' ends inside a quoted argument");
      return static_cast<std::size_t>(-1);
    }
    total_bytes += static_cast<std::size_t>(size);
    ResponseFileSnapshot snapshot;
    snapshot.as_written = as_written;
    snapshot.resolved = resolved;
    snapshot.resolved_utf8 = path_to_utf8_generic(resolved);
    snapshot.encoding = decoded.encoding;
    snapshot.bytes = static_cast<std::size_t>(size);
    snapshot.content_sha256 = sha256(bytes).hex();
    snapshot.first_depth = depth;
    out.files.push_back(std::move(snapshot));
    snapshot_tokens.push_back(tokenized.arguments);
    return out.files.size() - 1;
  }

  // Appends the expansion of one `@` token. `raw_index` is the command-line
  // argument the whole subtree descends from.
  void expand(const std::string& as_written, std::size_t raw_index, unsigned depth) {
    if (!out.errors.empty()) return;
    if (depth > limits.max_depth) {
      fail("response-file nesting deeper than " + std::to_string(limits.max_depth) + " levels at '" + as_written + "'");
      return;
    }
    if (!spend()) return;  // the reference itself is work, even when the file is already snapshotted
    const std::filesystem::path spelled = path_from_utf8(as_written);
    // Measured native cl.exe rule: a relative reference resolves against the
    // process working directory, not against the naming file's directory.
    const std::filesystem::path resolved =
        (spelled.is_absolute() ? spelled : command.directory / spelled).lexically_normal();
    for (const std::filesystem::path& on_path : active) {
      if (same_response_file(on_path, resolved)) {
        fail("response file '" + path_to_utf8_generic(resolved) + "' includes itself; cyclic response files");
        return;
      }
    }
    const std::size_t index = snapshot_for(as_written, resolved, depth);
    if (index == static_cast<std::size_t>(-1)) return;

    active.push_back(resolved);
    // Copy: `snapshot_tokens` can reallocate while nested files are read.
    const std::vector<std::string> tokens = snapshot_tokens[index];
    for (std::size_t i = 0; i < tokens.size(); ++i) {
      if (!out.errors.empty()) break;
      if (!spend()) break;
      const std::string& token = tokens[i];
      if (!token.empty() && token.front() == '@') {
        expand(token.substr(1), raw_index, depth + 1);
        continue;
      }
      if (out.arguments.size() >= limits.max_tokens) {
        fail("expanded command line exceeds the limit of " + std::to_string(limits.max_tokens) + " tokens");
        break;
      }
      ExpandedTokenOrigin origin;
      origin.raw_index = raw_index;
      origin.from_response_file = true;
      origin.snapshot = index;
      origin.index_in_file = i;
      out.arguments.push_back(token);
      out.origins.push_back(origin);
    }
    active.pop_back();
  }

  // Identity of the replayed context: the raw command identity, every snapshot
  // that was read (path, encoding, size, content hash), and every expanded
  // token with the origin it came from. A changed nested response file, a
  // reordered expansion or a different origin all produce a different id.
  std::string compute_replay_id() const {
    Sha256 hash;
    const auto field = [&hash](std::string_view value) {
      hash.update(std::to_string(value.size()));
      hash.update(":");
      hash.update(value);
      hash.update("\n");
    };
    field("lcm.replay-context.v1");
    // Never the caller-populated cache field: `command_id` is optional in the
    // public request contract, so trusting it would give one command two
    // identities and, when empty, would leave `directory` and `file` out of the
    // hash entirely -- two commands resolving the same absolute response file
    // against different working directories would then collide.
    field(compute_command_id(command));
    field(std::to_string(out.files.size()));
    for (const ResponseFileSnapshot& file : out.files) {
      field(file.resolved_utf8);
      field(to_string(file.encoding));
      field(std::to_string(file.bytes));
      field(file.content_sha256);
      field(std::to_string(file.alias_spellings.size()));
      for (const std::string& alias : file.alias_spellings) field(alias);
    }
    field(std::to_string(out.arguments.size()));
    for (std::size_t i = 0; i < out.arguments.size(); ++i) {
      const ExpandedTokenOrigin& origin = out.origins[i];
      field(std::to_string(origin.raw_index));
      field(origin.from_response_file ? out.files[origin.snapshot].resolved_utf8 : "<argv>");
      field(std::to_string(origin.index_in_file));
      field(out.arguments[i]);
    }
    return hash.finish().hex();
  }
};

}  // namespace

std::string_view to_string(ResponseEncoding value) {
  switch (value) {
    case ResponseEncoding::ascii:
      return "ascii";
    case ResponseEncoding::utf8_bom:
      return "utf8_bom";
    case ResponseEncoding::utf16le_bom:
      return "utf16le_bom";
  }
  return "unknown";
}

ResponseExpansion expand_response_files(const CompileCommand& command, const ResponseFileLimits& limits) {
  ResponseExpansion result;
  if (command.arguments.empty()) return result;
  bool has_response_token = false;
  for (std::size_t i = 1; i < command.arguments.size(); ++i) {
    const std::string& argument = command.arguments[i];
    if (!argument.empty() && argument.front() == '@') {
      has_response_token = true;
      break;
    }
  }
  if (!has_response_token) return result;
  // One verified driver only. For every other compiler the caller keeps the raw
  // command, whose `@` token still degrades the context and rejects the unit.
  if (detect_compiler(command.arguments.front()) != CompilerKind::native_cl) return result;
  if (command.directory.empty() || !command.directory.is_absolute()) {
    result.attempted = true;
    result.errors.push_back(
        "response-file expansion needs an absolute command directory to resolve relative references against");
    return result;
  }

  Expander expander(command, limits);
  expander.out.attempted = true;
  // argv[0] is never a response file and never expands, but it IS a token of
  // the expanded argv, so it is subject to the output-token bound like any
  // other. Inserting it before the check would let `max_tokens == 0` produce a
  // one-token result, contradicting the documented limit.
  if (limits.max_tokens == 0) {
    expander.fail("expanded command line exceeds the limit of 0 tokens");
    expander.out.ok = false;
    return std::move(expander.out);
  }
  expander.out.arguments.push_back(command.arguments.front());
  expander.out.origins.push_back(ExpandedTokenOrigin{0, false, 0, 0});
  for (std::size_t i = 1; i < command.arguments.size(); ++i) {
    if (!expander.out.errors.empty()) break;
    const std::string& argument = command.arguments[i];
    if (!argument.empty() && argument.front() == '@') {
      expander.expand(argument.substr(1), i, 1);
      continue;
    }
    if (expander.out.arguments.size() >= limits.max_tokens) {
      expander.fail("expanded command line exceeds the limit of " + std::to_string(limits.max_tokens) + " tokens");
      break;
    }
    expander.out.arguments.push_back(argument);
    expander.out.origins.push_back(ExpandedTokenOrigin{i, false, 0, 0});
  }

  if (!expander.out.errors.empty()) {
    // Atomic: no partially expanded argv escapes, and no replay identity is
    // minted for a context that was never fully read.
    expander.out.ok = false;
    expander.out.arguments.clear();
    expander.out.origins.clear();
    return std::move(expander.out);
  }
  expander.out.ok = true;
  expander.out.replay_id = expander.compute_replay_id();
  return std::move(expander.out);
}

}  // namespace lcm
