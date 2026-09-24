#include "lcm/compile_db.hpp"

#include <fstream>
#include <iterator>
#include <map>

#include <nlohmann/json.hpp>

#include "lcm/hash.hpp"
#include "lcm/source.hpp"

namespace lcm {
namespace {

using json = nlohmann::json;

bool is_separator(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

// Windows rules as used by llvm::cl::TokenizeWindowsCommandLine: backslashes
// are literal unless followed by a quote; 2n backslashes + quote -> n
// backslashes and a quote toggle; 2n+1 backslashes + quote -> n backslashes
// and a literal quote; a doubled quote inside quotes is a literal quote.
TokenizedCommand tokenize_windows(std::string_view input) {
  TokenizedCommand result;
  std::string token;
  bool in_token = false;
  bool quoted = false;
  std::size_t i = 0;
  while (i < input.size()) {
    const char c = input[i];
    if (c == '\\') {
      std::size_t backslashes = 0;
      while (i < input.size() && input[i] == '\\') {
        ++backslashes;
        ++i;
      }
      if (i < input.size() && input[i] == '"') {
        token.append(backslashes / 2, '\\');
        if (backslashes % 2 == 1) {
          token.push_back('"');
          ++i;
        }
        // even: the quote is handled by the loop as a toggle
      } else {
        token.append(backslashes, '\\');
      }
      in_token = true;
      continue;
    }
    if (c == '"') {
      if (quoted && i + 1 < input.size() && input[i + 1] == '"') {
        token.push_back('"');
        i += 2;
        continue;
      }
      quoted = !quoted;
      in_token = true;
      ++i;
      continue;
    }
    if (!quoted && is_separator(c)) {
      if (in_token) {
        result.arguments.push_back(std::move(token));
        token.clear();
        in_token = false;
      }
      ++i;
      continue;
    }
    token.push_back(c);
    in_token = true;
    ++i;
  }
  if (in_token) result.arguments.push_back(std::move(token));
  result.unterminated_quote = quoted;
  return result;
}

// GNU rules as used by Clang's JSON database parser: backslash escapes the
// next character outside and inside double quotes; single quotes are
// literal; whitespace separates arguments.
TokenizedCommand tokenize_gnu(std::string_view input) {
  TokenizedCommand result;
  std::string token;
  bool in_token = false;
  std::size_t i = 0;
  while (i < input.size()) {
    const char c = input[i];
    if (!in_token && is_separator(c)) {
      ++i;
      continue;
    }
    if (c == '"') {
      in_token = true;
      ++i;
      bool closed = false;
      while (i < input.size()) {
        if (input[i] == '"') {
          closed = true;
          ++i;
          break;
        }
        if (input[i] == '\\' && i + 1 < input.size()) ++i;
        token.push_back(input[i]);
        ++i;
      }
      if (!closed) result.unterminated_quote = true;
      continue;
    }
    if (c == '\'') {
      in_token = true;
      ++i;
      bool closed = false;
      while (i < input.size()) {
        if (input[i] == '\'') {
          closed = true;
          ++i;
          break;
        }
        token.push_back(input[i]);
        ++i;
      }
      if (!closed) result.unterminated_quote = true;
      continue;
    }
    if (is_separator(c)) {
      result.arguments.push_back(std::move(token));
      token.clear();
      in_token = false;
      ++i;
      continue;
    }
    if (c == '\\' && i + 1 < input.size()) ++i;
    token.push_back(input[i]);
    in_token = true;
    ++i;
  }
  if (in_token) result.arguments.push_back(std::move(token));
  return result;
}

void put(std::string& out, std::string_view value) {
  out.append(std::to_string(value.size()));
  out.push_back(':');
  out.append(value);
  out.push_back(';');
}

std::filesystem::path resolve_against(const std::filesystem::path& base, const std::filesystem::path& value) {
  return (value.is_absolute() ? value : base / value).lexically_normal();
}

void diag(std::vector<CompileDbDiagnostic>& out, DiagnosticSeverity severity, std::optional<std::size_t> index,
          std::string message) {
  out.push_back(CompileDbDiagnostic{severity, index, std::move(message)});
}

std::optional<CompileCommand> parse_entry(const json& entry, std::size_t index, const CompileDbParseOptions& options,
                                          std::vector<CompileDbDiagnostic>& diagnostics) {
  if (!entry.is_object()) {
    diag(diagnostics, DiagnosticSeverity::error, index, "entry is not an object");
    return std::nullopt;
  }
  const auto string_field = [&](const char* name) -> std::optional<std::string> {
    auto it = entry.find(name);
    if (it == entry.end()) return std::nullopt;
    if (!it->is_string()) {
      diag(diagnostics, DiagnosticSeverity::error, index, std::string("field '") + name + "' is not a string");
      return std::nullopt;
    }
    return it->get<std::string>();
  };

  CompileCommand command;
  command.entry_index = index;

  const auto directory = string_field("directory");
  const auto file = string_field("file");
  if (!directory || !file) {
    diag(diagnostics, DiagnosticSeverity::error, index, "entry requires string fields 'directory' and 'file'");
    return std::nullopt;
  }
  if (directory->empty() || file->empty()) {
    diag(diagnostics, DiagnosticSeverity::error, index, "'directory' and 'file' must not be empty");
    return std::nullopt;
  }
  command.directory_as_written = *directory;
  command.file_as_written = *file;

  const auto arguments_it = entry.find("arguments");
  const auto command_it = entry.find("command");
  if (arguments_it != entry.end()) {
    if (!arguments_it->is_array()) {
      diag(diagnostics, DiagnosticSeverity::error, index, "'arguments' is not an array");
      return std::nullopt;
    }
    for (const auto& arg : *arguments_it) {
      if (!arg.is_string()) {
        diag(diagnostics, DiagnosticSeverity::error, index, "'arguments' contains a non-string element");
        return std::nullopt;
      }
      command.arguments.push_back(arg.get<std::string>());
    }
    if (command_it != entry.end()) {
      diag(diagnostics, DiagnosticSeverity::warning, index,
           "entry has both 'arguments' and 'command'; 'arguments' is used and 'command' is kept as written");
      if (command_it->is_string()) command.command_as_written = command_it->get<std::string>();
    }
  } else if (command_it != entry.end()) {
    if (!command_it->is_string()) {
      diag(diagnostics, DiagnosticSeverity::error, index, "'command' is not a string");
      return std::nullopt;
    }
    command.command_as_written = command_it->get<std::string>();
    auto tokenized = tokenize_command_line(*command.command_as_written, options.syntax);
    if (tokenized.unterminated_quote) {
      // The argv boundary is unknowable; do not let a guessed argv acquire a
      // command identity that could merge with a valid neighbour.
      diag(diagnostics, DiagnosticSeverity::error, index,
           "'command' has an unterminated quote; entry rejected because its argument list cannot be determined");
      return std::nullopt;
    }
    command.arguments = std::move(tokenized.arguments);
  } else {
    diag(diagnostics, DiagnosticSeverity::error, index, "entry has neither 'arguments' nor 'command'");
    return std::nullopt;
  }
  if (command.arguments.empty()) {
    diag(diagnostics, DiagnosticSeverity::error, index, "entry has an empty argument list");
    return std::nullopt;
  }
  if (command.arguments.front().empty()) {
    diag(diagnostics, DiagnosticSeverity::error, index, "first argument (compiler) is empty");
    return std::nullopt;
  }

  std::filesystem::path directory_path = path_from_utf8(*directory);
  if (!directory_path.is_absolute()) {
    if (options.database_directory.empty()) {
      diag(diagnostics, DiagnosticSeverity::error, index,
           "'directory' is relative and no database location is known to resolve it");
      return std::nullopt;
    }
    diag(diagnostics, DiagnosticSeverity::warning, index,
         "'directory' is relative; resolved against the database location");
    directory_path = options.database_directory / directory_path;
  }
  command.directory = directory_path.lexically_normal();
  command.file = resolve_against(command.directory, path_from_utf8(*file));

  if (const auto output_it = entry.find("output"); output_it != entry.end()) {
    const auto output = string_field("output");
    if (!output) return std::nullopt;  // present but not a string: malformed entry, diagnosed above
    if (output->empty()) {
      diag(diagnostics, DiagnosticSeverity::error, index, "'output' must not be empty when present");
      return std::nullopt;
    }
    command.output_as_written = *output;
    command.output = resolve_against(command.directory, path_from_utf8(*output));
  }

  command.command_id = compute_command_id(command);
  return command;
}

}  // namespace

namespace {

// Identity spelling of a path: for an existing path the filesystem's own
// canonical spelling (aliases of one object agree, case-distinct siblings on a
// case-sensitive volume differ); otherwise the lexical comparison key.
std::string identity_path_key(const std::filesystem::path& path) {
  std::error_code ec;
  if (!path.empty() && std::filesystem::exists(path, ec) && !ec) {
    std::error_code canon_ec;
    const std::filesystem::path canonical = std::filesystem::canonical(path, canon_ec);
    if (!canon_ec) return "fs:" + path_to_utf8_generic(canonical);
  }
  return "lex:" + path_key(path);
}

// Identity spelling of an output path: case-PRESERVING lexical normalization
// (lexically normal, generic separators, no trailing slash). Deliberately not
// identity_path_key: an artifact is created and deleted by builds, so its
// identity must not depend on whether it exists, and case aliases stay
// distinct (`out.obj` vs `Out.obj` are two identities, possibly an ambiguity,
// never a silent merge) because the volume's case rule cannot be asked for a
// path that may not exist yet.
std::string output_identity_key(const std::filesystem::path& path) {
  std::string key = path_to_utf8_generic(path.lexically_normal());
  while (key.size() > 1 && key.back() == '/') key.pop_back();
  return "out:" + key;
}

}  // namespace

std::string compute_command_id(const CompileCommand& command) {
  std::string out;
  put(out, identity_path_key(command.directory));
  put(out, identity_path_key(command.file));
  put(out, std::to_string(command.arguments.size()));
  for (const auto& arg : command.arguments) put(out, arg);
  put(out, command.output ? output_identity_key(*command.output) : std::string{});
  return sha256(out).hex().substr(0, 32);
}

TokenizedCommand tokenize_command_line(std::string_view command, CommandSyntax syntax) {
  if (syntax == CommandSyntax::auto_detect) {
#ifdef _WIN32
    syntax = CommandSyntax::windows;
#else
    syntax = CommandSyntax::gnu;
#endif
  }
  return syntax == CommandSyntax::windows ? tokenize_windows(command) : tokenize_gnu(command);
}

std::vector<const CompileCommand*> CompilationDatabase::commands_for_file(const std::filesystem::path& file,
                                                                           const std::filesystem::path& base) const {
  const std::filesystem::path resolved = (file.is_absolute() || base.empty()) ? file : base / file;
  const std::string key = path_key(resolved);
  // When both the queried file and a command's file exist, the filesystem is
  // authoritative (aliases of one file match; distinct case-sensitive files
  // never merge). The lexical key is only the fallback when an existing-file
  // comparison is not possible.
  std::error_code ec;
  const bool query_exists = std::filesystem::exists(resolved, ec) && !ec;
  std::vector<const CompileCommand*> out;
  for (const auto& command : commands) {
    std::error_code ec2;
    const bool command_exists = query_exists && std::filesystem::exists(command.file, ec2) && !ec2;
    const bool match = command_exists ? same_existing_file(command.file, resolved) : path_key(command.file) == key;
    if (match) out.push_back(&command);
  }
  return out;
}

CommandSelection CompilationDatabase::select_unique(const std::filesystem::path& file,
                                                    const std::filesystem::path& base) const {
  CommandSelection selection;
  selection.candidates = commands_for_file(file, base);
  if (selection.candidates.empty()) {
    selection.kind = CommandSelection::Kind::none;
    return selection;
  }
  const std::string& first_id = selection.candidates.front()->command_id;
  for (const auto* candidate : selection.candidates) {
    if (candidate->command_id != first_id) {
      selection.kind = CommandSelection::Kind::ambiguous;
      return selection;
    }
  }
  selection.kind = CommandSelection::Kind::unique;
  selection.command = selection.candidates.front();
  return selection;
}

CompileDbParseResult parse_compilation_database(std::string_view json_utf8, const CompileDbParseOptions& options) {
  CompileDbParseResult result;
  if (!options.database_directory.empty() && !options.database_directory.is_absolute()) {
    diag(result.diagnostics, DiagnosticSeverity::error, std::nullopt,
         "database_directory must be absolute (got '" + path_to_utf8_generic(options.database_directory) +
             "'); resolved command paths would otherwise depend on the process working directory");
    return result;
  }
  if (json_utf8.size() >= 3 && json_utf8.substr(0, 3) == "\xEF\xBB\xBF") json_utf8.remove_prefix(3);

  const json document = json::parse(json_utf8, nullptr, /*allow_exceptions=*/false);
  if (document.is_discarded()) {
    diag(result.diagnostics, DiagnosticSeverity::error, std::nullopt, "compilation database is not valid JSON");
    return result;
  }
  if (!document.is_array()) {
    diag(result.diagnostics, DiagnosticSeverity::error, std::nullopt, "compilation database must be a JSON array");
    return result;
  }

  CompilationDatabase database;
  for (std::size_t i = 0; i < document.size(); ++i) {
    if (auto command = parse_entry(document[i], i, options, result.diagnostics)) {
      database.commands.push_back(std::move(*command));
    }
  }
  result.database = std::move(database);
  return result;
}

CompileDbParseResult load_compilation_database(const std::filesystem::path& path, CommandSyntax syntax) {
  CompileDbParseResult result;
  std::error_code ec;
  std::filesystem::path absolute_path = std::filesystem::absolute(path, ec);
  if (ec) {
    diag(result.diagnostics, DiagnosticSeverity::error, std::nullopt,
         "cannot resolve compilation database path: " + path_to_utf8_generic(path));
    return result;
  }
  absolute_path = absolute_path.lexically_normal();
  std::ifstream in(absolute_path, std::ios::binary);
  if (!in) {
    diag(result.diagnostics, DiagnosticSeverity::error, std::nullopt,
         "cannot read compilation database: " + path_to_utf8_generic(absolute_path));
    return result;
  }
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  CompileDbParseOptions options;
  options.syntax = syntax;
  options.database_directory = absolute_path.parent_path();
  return parse_compilation_database(text, options);
}

}  // namespace lcm
