// Clang JSON compilation database ingestion (PRD FR-BLD-004).
//
// `arguments` and `command` are compiler option data. They are never passed
// to a shell. Relative `file`/`output` values resolve against the entry's
// `directory`. Every entry keeps its original spelling plus a command
// identity so that several commands for one file are distinguished instead
// of silently merged or picked.
#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lcm {

// How a `command` string is split into argv. `auto_detect` follows Clang:
// Windows rules on Windows hosts, GNU shell rules elsewhere.
enum class CommandSyntax : std::uint8_t {
  auto_detect,
  windows,
  gnu,
};

struct TokenizedCommand {
  std::vector<std::string> arguments;
  bool unterminated_quote = false;
};

[[nodiscard]] TokenizedCommand tokenize_command_line(std::string_view command, CommandSyntax syntax);

struct CompileCommand {
  std::size_t entry_index = 0;  // position in the database array

  std::filesystem::path directory;  // absolute, lexically normal
  std::filesystem::path file;       // absolute, lexically normal
  std::optional<std::filesystem::path> output;

  std::string directory_as_written;  // UTF-8, verbatim from the database
  std::string file_as_written;
  std::optional<std::string> output_as_written;

  std::vector<std::string> arguments;         // argv including the compiler; data only
  std::optional<std::string> command_as_written;  // present when the entry used `command`

  // Identity of the compile context: hash of the normalized directory and
  // file keys, argv and output. Two entries with the same id describe the
  // same compilation.
  std::string command_id;
};

enum class DiagnosticSeverity : std::uint8_t {
  warning,
  error,
};

struct CompileDbDiagnostic {
  DiagnosticSeverity severity = DiagnosticSeverity::error;
  std::optional<std::size_t> entry_index;  // nullopt for database-level problems
  std::string message;
};

struct CommandSelection {
  enum class Kind : std::uint8_t {
    none,       // no command for the file
    unique,     // exactly one distinct command identity
    ambiguous,  // several distinct command identities; caller must choose explicitly
  };
  Kind kind = Kind::none;
  const CompileCommand* command = nullptr;         // set for `unique`
  std::vector<const CompileCommand*> candidates;   // all commands for the file (every kind)
};

// Identity of a compile command's content: hash of the directory and file
// identity spellings, argv and output. For the directory and file, existing
// paths use the filesystem's canonical spelling (aliases of one file agree;
// distinct case-sensitive siblings differ) and non-existing paths fall back to
// the lexical key. The output uses a case-preserving lexical spelling that is
// independent of whether the artifact exists: dot/separator variants of one
// output agree, case variants (`out.obj`/`Out.obj`) are conservatively kept
// distinct. This is what `CompileCommand::command_id` holds for parsed entries;
// consumers that build commands by hand must use it so provenance cannot be
// forged by a hand-written label.
[[nodiscard]] std::string compute_command_id(const CompileCommand& command);

class CompilationDatabase {
 public:
  std::vector<CompileCommand> commands;  // database order

  // Every command whose resolved file matches `file` (compared via path_key).
  // A relative `file` is resolved against `base` when provided.
  [[nodiscard]] std::vector<const CompileCommand*> commands_for_file(
      const std::filesystem::path& file, const std::filesystem::path& base = {}) const;

  // Unique when all commands for the file share one command identity.
  [[nodiscard]] CommandSelection select_unique(const std::filesystem::path& file,
                                               const std::filesystem::path& base = {}) const;
};

struct CompileDbParseOptions {
  CommandSyntax syntax = CommandSyntax::auto_detect;
  // Absolute base for a relative `directory` value (the specification
  // requires an absolute directory; relative ones are accepted with a warning
  // when a base is known and rejected otherwise). Usually the database's
  // folder. A relative base is rejected with a database-level error.
  std::filesystem::path database_directory;
};

struct CompileDbParseResult {
  std::optional<CompilationDatabase> database;  // nullopt when the document itself is unusable
  std::vector<CompileDbDiagnostic> diagnostics;
};

[[nodiscard]] CompileDbParseResult parse_compilation_database(std::string_view json_utf8,
                                                              const CompileDbParseOptions& options);

// Reads the file and parses it with `database_directory` set to its parent.
[[nodiscard]] CompileDbParseResult load_compilation_database(const std::filesystem::path& path,
                                                             CommandSyntax syntax = CommandSyntax::auto_detect);

}  // namespace lcm
