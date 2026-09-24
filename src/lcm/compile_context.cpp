#include "lcm/compile_context.hpp"

#include <algorithm>

#include "lcm/source.hpp"

namespace lcm {
namespace {

std::string ascii_lower(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return s;
}

bool starts_with(std::string_view s, std::string_view prefix) { return s.substr(0, prefix.size()) == prefix; }

// Rule for a recognised option: exact spelling or prefix, how a value is
// attached, and what to do with it.
enum class ValueMode : std::uint8_t {
  none,               // flag only
  attached,           // value glued to the option (/Fo<file>)
  attached_or_next,   // value glued or in the next argument (/D X, -I dir)
  next,               // value always in the next argument (-x lang, -include file)
};

struct Rule {
  std::string_view spelling;
  bool prefix;  // true: any argument starting with spelling matches
  ValueMode value;
  OptionAction action;
  OptionCategory category;
  std::string_view reason;
};

// Order matters: first match wins, so longer/more specific spellings come first.
constexpr Rule kMsvcRules[] = {
    // Values that shape the preprocessor / front end: kept.
    {"D", true, ValueMode::attached_or_next, OptionAction::kept, OptionCategory::define, "macro definition"},
    {"U", true, ValueMode::attached_or_next, OptionAction::kept, OptionCategory::undefine, "macro undefinition"},
    {"I", true, ValueMode::attached_or_next, OptionAction::kept, OptionCategory::include, "include directory"},
    {"external:I", true, ValueMode::attached_or_next, OptionAction::kept, OptionCategory::include, "external include directory"},
    {"external:", true, ValueMode::none, OptionAction::dropped, OptionCategory::diagnostics, "external header warning control"},
    // clang-cl only (native cl.exe has no such option; run_msvc guards on the compiler kind).
    {"imsvc", true, ValueMode::attached_or_next, OptionAction::kept, OptionCategory::include, "system include directory (clang-cl: as if part of %INCLUDE%)"},
    {"FI", true, ValueMode::attached_or_next, OptionAction::kept, OptionCategory::forced_include, "forced include"},
    {"std:", true, ValueMode::attached, OptionAction::kept, OptionCategory::language_standard, "language standard"},
    {"TP", false, ValueMode::none, OptionAction::kept, OptionCategory::language, "treat all sources as C++"},
    {"TC", false, ValueMode::none, OptionAction::kept, OptionCategory::language, "treat all sources as C"},
    {"Tp", true, ValueMode::attached_or_next, OptionAction::source_input, OptionCategory::source, "C++ source file"},
    {"Tc", true, ValueMode::attached_or_next, OptionAction::source_input, OptionCategory::source, "C source file"},
    {"EH", true, ValueMode::none, OptionAction::kept, OptionCategory::semantics, "exception handling model"},
    {"GR", true, ValueMode::none, OptionAction::kept, OptionCategory::semantics, "RTTI"},
    {"Zc:", true, ValueMode::none, OptionAction::kept, OptionCategory::semantics, "conformance switch"},
    {"permissive", true, ValueMode::none, OptionAction::kept, OptionCategory::semantics, "conformance mode"},
    {"utf-8", false, ValueMode::none, OptionAction::kept, OptionCategory::semantics, "source/execution charset"},
    {"source-charset:", true, ValueMode::none, OptionAction::kept, OptionCategory::semantics, "source charset"},
    {"execution-charset:", true, ValueMode::none, OptionAction::kept, OptionCategory::semantics, "execution charset"},
    {"validate-charset", true, ValueMode::none, OptionAction::dropped, OptionCategory::diagnostics, "charset validation diagnostics"},
    {"MDd", false, ValueMode::none, OptionAction::kept, OptionCategory::runtime_library, "runtime library (defines _MT/_DLL/_DEBUG)"},
    {"MTd", false, ValueMode::none, OptionAction::kept, OptionCategory::runtime_library, "runtime library (defines _MT/_DEBUG)"},
    {"MD", false, ValueMode::none, OptionAction::kept, OptionCategory::runtime_library, "runtime library (defines _MT/_DLL)"},
    {"MT", false, ValueMode::none, OptionAction::kept, OptionCategory::runtime_library, "runtime library (defines _MT)"},
    {"LDd", false, ValueMode::none, OptionAction::kept, OptionCategory::runtime_library, "DLL build, debug runtime"},
    {"LD", false, ValueMode::none, OptionAction::kept, OptionCategory::runtime_library, "DLL build"},
    {"arch:", true, ValueMode::none, OptionAction::kept, OptionCategory::semantics, "target architecture macros"},
    {"Zp", true, ValueMode::none, OptionAction::kept, OptionCategory::semantics, "struct packing"},
    {"vm", true, ValueMode::none, OptionAction::kept, OptionCategory::semantics, "pointer-to-member representation"},
    {"Za", false, ValueMode::none, OptionAction::kept, OptionCategory::semantics, "disable language extensions"},
    {"Ze", false, ValueMode::none, OptionAction::kept, OptionCategory::semantics, "enable language extensions"},
    {"J", false, ValueMode::none, OptionAction::kept, OptionCategory::semantics, "unsigned char default"},
    {"openmp", true, ValueMode::none, OptionAction::kept, OptionCategory::semantics, "OpenMP (_OPENMP)"},
    {"fp:", true, ValueMode::none, OptionAction::kept, OptionCategory::semantics, "floating-point model macros"},
    {"await", true, ValueMode::none, OptionAction::kept, OptionCategory::semantics, "coroutine support"},
    // Optimization: exact spellings only, so /OUT:... or other /O-prefixed
    // linker/unknown switches are not mistaken for optimization levels.
    {"O1", false, ValueMode::none, OptionAction::kept, OptionCategory::optimization, "optimization level"},
    {"O2", false, ValueMode::none, OptionAction::kept, OptionCategory::optimization, "optimization level"},
    {"Ox", false, ValueMode::none, OptionAction::kept, OptionCategory::optimization, "optimization level"},
    {"Od", false, ValueMode::none, OptionAction::kept, OptionCategory::optimization, "optimization disabled"},
    {"Og", false, ValueMode::none, OptionAction::kept, OptionCategory::optimization, "global optimization"},
    {"Oi", false, ValueMode::none, OptionAction::kept, OptionCategory::optimization, "intrinsics"},
    {"Oi-", false, ValueMode::none, OptionAction::kept, OptionCategory::optimization, "intrinsics off"},
    {"Os", false, ValueMode::none, OptionAction::kept, OptionCategory::optimization, "favor size"},
    {"Ot", false, ValueMode::none, OptionAction::kept, OptionCategory::optimization, "favor speed"},
    {"Ob0", false, ValueMode::none, OptionAction::kept, OptionCategory::optimization, "inline expansion"},
    {"Ob1", false, ValueMode::none, OptionAction::kept, OptionCategory::optimization, "inline expansion"},
    {"Ob2", false, ValueMode::none, OptionAction::kept, OptionCategory::optimization, "inline expansion"},
    {"Ob3", false, ValueMode::none, OptionAction::kept, OptionCategory::optimization, "inline expansion"},
    {"Oy", false, ValueMode::none, OptionAction::kept, OptionCategory::optimization, "frame pointer omission"},
    {"Oy-", false, ValueMode::none, OptionAction::kept, OptionCategory::optimization, "frame pointer omission off"},
    // Precompiled headers: the binary cannot be fed to another front end.
    {"Yu", true, ValueMode::attached, OptionAction::transformed, OptionCategory::precompiled_header, "PCH use emulated by forced include of the named header"},
    {"Yc", true, ValueMode::attached, OptionAction::dropped, OptionCategory::precompiled_header, "PCH creation is a build artifact; the source includes the header itself"},
    {"Y-", false, ValueMode::none, OptionAction::dropped, OptionCategory::precompiled_header, "PCH disabled"},
    {"Fp", true, ValueMode::attached, OptionAction::dropped, OptionCategory::precompiled_header, "PCH output file"},
    // Outputs.
    {"Fo", true, ValueMode::attached, OptionAction::dropped, OptionCategory::output, "object file output"},
    {"Fd", true, ValueMode::attached, OptionAction::dropped, OptionCategory::output, "PDB output"},
    {"Fe", true, ValueMode::attached, OptionAction::dropped, OptionCategory::output, "executable output"},
    {"Fa", true, ValueMode::attached, OptionAction::dropped, OptionCategory::output, "assembly listing output"},
    {"Fm", true, ValueMode::attached, OptionAction::dropped, OptionCategory::output, "map file output"},
    {"Fi", true, ValueMode::attached, OptionAction::dropped, OptionCategory::output, "preprocessed output"},
    {"FR", true, ValueMode::attached, OptionAction::dropped, OptionCategory::output, "browse information output"},
    {"Fr", true, ValueMode::attached, OptionAction::dropped, OptionCategory::output, "browse information output"},
    {"FS", false, ValueMode::none, OptionAction::dropped, OptionCategory::output, "serialized PDB writes"},
    // Dependency generation.
    {"showIncludes", true, ValueMode::none, OptionAction::dropped, OptionCategory::dependency_generation, "include listing"},
    {"sourceDependencies", true, ValueMode::attached_or_next, OptionAction::dropped, OptionCategory::dependency_generation, "dependency JSON output"},
    {"scanDependencies", true, ValueMode::attached_or_next, OptionAction::dropped, OptionCategory::dependency_generation, "module dependency scan output"},
    // Diagnostics.
    // Native cl.exe only, exact spelling, value always in the next argument:
    // UnrealBuildTool emits `/experimental:log <file>.sarif`. run_msvc() guards
    // the compiler kind, so the same spelling under another driver stays
    // unknown. The rest of the `/experimental:` family is NOT covered.
    {"experimental:log", false, ValueMode::next, OptionAction::dropped, OptionCategory::diagnostics, "SARIF diagnostics log output"},
    {"Wall", false, ValueMode::none, OptionAction::dropped, OptionCategory::diagnostics, "warning level"},
    {"WX", true, ValueMode::none, OptionAction::dropped, OptionCategory::diagnostics, "warnings as errors"},
    {"WL", false, ValueMode::none, OptionAction::dropped, OptionCategory::diagnostics, "one-line diagnostics"},
    {"WV:", true, ValueMode::none, OptionAction::dropped, OptionCategory::diagnostics, "warning version"},
    {"W0", false, ValueMode::none, OptionAction::dropped, OptionCategory::diagnostics, "warning level"},
    {"W1", false, ValueMode::none, OptionAction::dropped, OptionCategory::diagnostics, "warning level"},
    {"W2", false, ValueMode::none, OptionAction::dropped, OptionCategory::diagnostics, "warning level"},
    {"W3", false, ValueMode::none, OptionAction::dropped, OptionCategory::diagnostics, "warning level"},
    {"W4", false, ValueMode::none, OptionAction::dropped, OptionCategory::diagnostics, "warning level"},
    {"wd", true, ValueMode::none, OptionAction::dropped, OptionCategory::diagnostics, "warning disable"},
    {"we", true, ValueMode::none, OptionAction::dropped, OptionCategory::diagnostics, "warning as error"},
    {"wo", true, ValueMode::none, OptionAction::dropped, OptionCategory::diagnostics, "warning once"},
    {"w1", true, ValueMode::none, OptionAction::dropped, OptionCategory::diagnostics, "warning level override"},
    {"w2", true, ValueMode::none, OptionAction::dropped, OptionCategory::diagnostics, "warning level override"},
    {"w3", true, ValueMode::none, OptionAction::dropped, OptionCategory::diagnostics, "warning level override"},
    {"w4", true, ValueMode::none, OptionAction::dropped, OptionCategory::diagnostics, "warning level override"},
    {"w", false, ValueMode::none, OptionAction::dropped, OptionCategory::diagnostics, "warnings off"},
    {"errorReport", true, ValueMode::none, OptionAction::dropped, OptionCategory::diagnostics, "error reporting"},
    {"diagnostics:", true, ValueMode::none, OptionAction::dropped, OptionCategory::diagnostics, "diagnostics format"},
    {"analyze", true, ValueMode::none, OptionAction::dropped, OptionCategory::diagnostics, "static analysis"},
    {"sdl", true, ValueMode::none, OptionAction::dropped, OptionCategory::diagnostics, "SDL checks"},
    // Debug info.
    {"Zi", false, ValueMode::none, OptionAction::dropped, OptionCategory::debug_info, "debug information"},
    {"ZI", false, ValueMode::none, OptionAction::dropped, OptionCategory::debug_info, "debug information (edit and continue)"},
    {"Z7", false, ValueMode::none, OptionAction::dropped, OptionCategory::debug_info, "debug information"},
    {"Zo", true, ValueMode::none, OptionAction::dropped, OptionCategory::debug_info, "enhanced debug info"},
    {"Zf", false, ValueMode::none, OptionAction::dropped, OptionCategory::debug_info, "faster PDB generation"},
    // Build process.
    {"nologo", false, ValueMode::none, OptionAction::dropped, OptionCategory::build_process, "banner suppression"},
    {"c", false, ValueMode::none, OptionAction::dropped, OptionCategory::build_process, "compile only"},
    {"MP", true, ValueMode::none, OptionAction::dropped, OptionCategory::build_process, "parallel build"},
    {"bigobj", false, ValueMode::none, OptionAction::dropped, OptionCategory::build_process, "object section limit"},
    {"cgthreads", true, ValueMode::none, OptionAction::dropped, OptionCategory::build_process, "codegen threads"},
    {"Brepro", false, ValueMode::none, OptionAction::dropped, OptionCategory::build_process, "reproducible output"},
    {"Bt", true, ValueMode::none, OptionAction::dropped, OptionCategory::build_process, "timing output"},
    {"FC", false, ValueMode::none, OptionAction::dropped, OptionCategory::build_process, "full paths in diagnostics"},
    {"Zl", false, ValueMode::none, OptionAction::dropped, OptionCategory::build_process, "omit default library name"},
    // Code generation without preprocessor impact.
    {"Gm", true, ValueMode::none, OptionAction::dropped, OptionCategory::codegen, "minimal rebuild"},
    {"GL", true, ValueMode::none, OptionAction::dropped, OptionCategory::codegen, "whole program optimization"},
    {"Gy", true, ValueMode::none, OptionAction::dropped, OptionCategory::codegen, "function-level linking"},
    {"GF", true, ValueMode::none, OptionAction::dropped, OptionCategory::codegen, "string pooling"},
    {"GS", true, ValueMode::none, OptionAction::dropped, OptionCategory::codegen, "buffer security check"},
    {"Gw", true, ValueMode::none, OptionAction::dropped, OptionCategory::codegen, "global data sections"},
    {"guard:", true, ValueMode::none, OptionAction::dropped, OptionCategory::codegen, "control flow guard"},
    {"Qspectre", true, ValueMode::none, OptionAction::dropped, OptionCategory::codegen, "Spectre mitigation"},
    {"QIntel-jcc-erratum", false, ValueMode::none, OptionAction::dropped, OptionCategory::codegen, "JCC erratum mitigation"},
    {"Qpar", true, ValueMode::none, OptionAction::dropped, OptionCategory::codegen, "auto-parallelizer"},
    {"Qvec", true, ValueMode::none, OptionAction::dropped, OptionCategory::codegen, "auto-vectorizer"},
    {"favor:", true, ValueMode::none, OptionAction::dropped, OptionCategory::codegen, "optimization target"},
    {"hotpatch", false, ValueMode::none, OptionAction::dropped, OptionCategory::codegen, "hotpatchable image"},
    // Native cl.exe only, exact spelling, no value: a back-end switch that adds
    // detail to warning output and changes nothing the front end parses. The
    // `/d2` family as a whole stays unknown.
    {"d2ExtendedWarningInfo", false, ValueMode::none, OptionAction::dropped, OptionCategory::codegen, "extended warning detail from the back end"},
    {"homeparams", false, ValueMode::none, OptionAction::dropped, OptionCategory::codegen, "register parameter homing"},
    // Unsupported semantics.
    {"clr", true, ValueMode::none, OptionAction::dropped_unsupported, OptionCategory::semantics, "managed C++ (C++/CLI) is not analyzable"},
    {"ZW", false, ValueMode::none, OptionAction::dropped_unsupported, OptionCategory::semantics, "C++/CX is not analyzable"},
    {"kernel", false, ValueMode::none, OptionAction::dropped_unsupported, OptionCategory::semantics, "kernel mode compilation is not modelled"},
};

constexpr Rule kGnuRules[] = {
    {"-D", true, ValueMode::attached_or_next, OptionAction::kept, OptionCategory::define, "macro definition"},
    {"-U", true, ValueMode::attached_or_next, OptionAction::kept, OptionCategory::undefine, "macro undefinition"},
    {"-isystem", true, ValueMode::attached_or_next, OptionAction::kept, OptionCategory::include, "system include directory"},
    {"-iquote", true, ValueMode::attached_or_next, OptionAction::kept, OptionCategory::include, "quote include directory"},
    {"-idirafter", true, ValueMode::attached_or_next, OptionAction::kept, OptionCategory::include, "after-system include directory"},
    {"-include-pch", false, ValueMode::next, OptionAction::dropped_unsupported, OptionCategory::precompiled_header, "precompiled header binary from another front end cannot be used"},
    {"-include", false, ValueMode::next, OptionAction::kept, OptionCategory::forced_include, "forced include"},
    {"-imacros", false, ValueMode::next, OptionAction::kept, OptionCategory::forced_include, "forced macro include"},
    {"-isysroot", false, ValueMode::next, OptionAction::kept, OptionCategory::semantics, "system root"},
    {"-I", true, ValueMode::attached_or_next, OptionAction::kept, OptionCategory::include, "include directory"},
    {"-std=", true, ValueMode::attached, OptionAction::kept, OptionCategory::language_standard, "language standard"},
    {"-x", true, ValueMode::attached_or_next, OptionAction::kept, OptionCategory::language, "source language"},
    {"-ansi", false, ValueMode::none, OptionAction::kept, OptionCategory::semantics, "ANSI mode"},
    {"--target=", true, ValueMode::attached, OptionAction::kept, OptionCategory::semantics, "target triple"},
    {"-target", false, ValueMode::next, OptionAction::kept, OptionCategory::semantics, "target triple"},
    {"--sysroot=", true, ValueMode::attached, OptionAction::kept, OptionCategory::semantics, "system root"},
    {"--sysroot", false, ValueMode::next, OptionAction::kept, OptionCategory::semantics, "system root"},
    {"-nostdinc", true, ValueMode::none, OptionAction::kept, OptionCategory::semantics, "no standard include directories"},
    {"-nostdlib", true, ValueMode::none, OptionAction::kept, OptionCategory::semantics, "no standard library"},
    {"-stdlib=", true, ValueMode::attached, OptionAction::kept, OptionCategory::semantics, "standard library selection"},
    {"-pthread", false, ValueMode::none, OptionAction::kept, OptionCategory::semantics, "threads (_REENTRANT)"},
    {"-Xclang", false, ValueMode::next, OptionAction::kept, OptionCategory::semantics, "front-end passthrough"},
    {"-Xpreprocessor", false, ValueMode::next, OptionAction::kept, OptionCategory::semantics, "preprocessor passthrough"},
    // Diagnostics-only -f flags before the generic -f rule.
    {"-fdiagnostics-", true, ValueMode::none, OptionAction::dropped, OptionCategory::diagnostics, "diagnostics presentation"},
    {"-fno-diagnostics-", true, ValueMode::none, OptionAction::dropped, OptionCategory::diagnostics, "diagnostics presentation"},
    {"-fcolor-diagnostics", false, ValueMode::none, OptionAction::dropped, OptionCategory::diagnostics, "diagnostics colour"},
    {"-fno-color-diagnostics", false, ValueMode::none, OptionAction::dropped, OptionCategory::diagnostics, "diagnostics colour"},
    {"-fansi-escape-codes", false, ValueMode::none, OptionAction::dropped, OptionCategory::diagnostics, "diagnostics colour"},
    {"-ferror-limit=", true, ValueMode::none, OptionAction::dropped, OptionCategory::diagnostics, "error limit"},
    {"-fmessage-length=", true, ValueMode::none, OptionAction::dropped, OptionCategory::diagnostics, "message wrapping"},
    {"-fshow-", true, ValueMode::none, OptionAction::dropped, OptionCategory::diagnostics, "diagnostics presentation"},
    {"-fsyntax-only", false, ValueMode::none, OptionAction::dropped, OptionCategory::build_process, "the analyzer adds its own front-end action"},
    {"-ftime-report", true, ValueMode::none, OptionAction::dropped, OptionCategory::build_process, "timing report"},
    {"-f", true, ValueMode::none, OptionAction::kept, OptionCategory::semantics, "front-end feature flag (may define macros)"},
    {"-m", true, ValueMode::none, OptionAction::kept, OptionCategory::semantics, "target feature flag (defines macros)"},
    {"-O", true, ValueMode::none, OptionAction::kept, OptionCategory::optimization, "optimization level (defines __OPTIMIZE__)"},
    // Dropped: outputs, dependency files, diagnostics, debug info, build process, linker.
    // Joined-or-separate value options (gcc/clang accept both -ofoo and -o foo).
    {"-o", true, ValueMode::attached_or_next, OptionAction::dropped, OptionCategory::output, "output file"},
    {"-MF", true, ValueMode::attached_or_next, OptionAction::dropped, OptionCategory::dependency_generation, "dependency output file"},
    {"-MT", true, ValueMode::attached_or_next, OptionAction::dropped, OptionCategory::dependency_generation, "dependency target"},
    {"-MQ", true, ValueMode::attached_or_next, OptionAction::dropped, OptionCategory::dependency_generation, "dependency target"},
    {"-MJ", true, ValueMode::attached_or_next, OptionAction::dropped, OptionCategory::dependency_generation, "compilation database fragment"},
    {"-MMD", false, ValueMode::none, OptionAction::dropped, OptionCategory::dependency_generation, "dependency generation"},
    {"-MD", false, ValueMode::none, OptionAction::dropped, OptionCategory::dependency_generation, "dependency generation"},
    {"-MM", false, ValueMode::none, OptionAction::dropped, OptionCategory::dependency_generation, "dependency generation"},
    {"-MG", false, ValueMode::none, OptionAction::dropped, OptionCategory::dependency_generation, "dependency generation"},
    {"-MP", false, ValueMode::none, OptionAction::dropped, OptionCategory::dependency_generation, "dependency phony targets"},
    {"-M", false, ValueMode::none, OptionAction::dropped, OptionCategory::dependency_generation, "dependency generation"},
    {"-Wl,", true, ValueMode::none, OptionAction::dropped, OptionCategory::linker, "linker passthrough"},
    {"-Wa,", true, ValueMode::none, OptionAction::dropped, OptionCategory::codegen, "assembler passthrough"},
    {"-W", true, ValueMode::none, OptionAction::dropped, OptionCategory::diagnostics, "warning control"},
    {"-w", false, ValueMode::none, OptionAction::dropped, OptionCategory::diagnostics, "warnings off"},
    {"-pedantic", true, ValueMode::none, OptionAction::dropped, OptionCategory::diagnostics, "pedantic warnings"},
    {"-g", true, ValueMode::none, OptionAction::dropped, OptionCategory::debug_info, "debug information"},
    {"-c", false, ValueMode::none, OptionAction::dropped, OptionCategory::build_process, "compile only"},
    {"-S", false, ValueMode::none, OptionAction::dropped, OptionCategory::build_process, "assemble only"},
    {"-E", false, ValueMode::none, OptionAction::dropped, OptionCategory::build_process, "preprocess only"},
    {"-pipe", false, ValueMode::none, OptionAction::dropped, OptionCategory::build_process, "pipes between stages"},
    {"-save-temps", true, ValueMode::none, OptionAction::dropped, OptionCategory::build_process, "keep intermediates"},
    {"-v", false, ValueMode::none, OptionAction::dropped, OptionCategory::build_process, "verbose driver"},
    {"-###", false, ValueMode::none, OptionAction::dropped, OptionCategory::build_process, "dry run"},
    {"-l", true, ValueMode::attached_or_next, OptionAction::dropped, OptionCategory::linker, "library"},
    {"-L", true, ValueMode::attached_or_next, OptionAction::dropped, OptionCategory::linker, "library directory"},
    {"-shared", false, ValueMode::none, OptionAction::dropped, OptionCategory::linker, "shared library link"},
    {"-static", true, ValueMode::none, OptionAction::dropped, OptionCategory::linker, "static link"},
    {"-rdynamic", false, ValueMode::none, OptionAction::dropped, OptionCategory::linker, "dynamic symbol export"},
};

const Rule* match_rule(std::string_view option, const Rule* begin, const Rule* end) {
  for (const Rule* rule = begin; rule != end; ++rule) {
    if (rule->prefix ? starts_with(option, rule->spelling) : option == rule->spelling) return rule;
  }
  return nullptr;
}

bool is_msvc_option(std::string_view arg) { return arg.size() > 1 && (arg[0] == '/' || arg[0] == '-'); }
bool is_gnu_option(std::string_view arg) { return arg.size() > 1 && arg[0] == '-'; }

// Search group a directory option places its path in.
IncludeKind include_kind_of(std::string_view rule_spelling) {
  if (rule_spelling == "external:I" || rule_spelling == "-isystem") return IncludeKind::system;
  if (rule_spelling == "imsvc") return IncludeKind::internal_system;
  if (rule_spelling == "-iquote") return IncludeKind::quote;
  if (rule_spelling == "-idirafter") return IncludeKind::after;
  return IncludeKind::user;
}

constexpr std::string_view kEndOfOptions = "--";
constexpr std::string_view kAuditedClangClWarning = "-Wno-deprecated-declarations";

struct Normalizer {
  const CompileCommand& command;
  NormalizedCompileContext out;
  std::string entry_file_key;
  bool end_of_options = false;  // a verified Clang-family `--` was consumed: everything after is an input

  explicit Normalizer(const CompileCommand& c) : command(c), entry_file_key(path_key(c.file)) {}

  // `--` is honoured only for compiler executables whose driver is verified to
  // treat it as the end of options (clang-cl, clang/clang++). Native cl.exe
  // ignores it (D9002) and the GNU aliases are not verified here, so for them
  // the token stays an unknown option and the gate rejects the unit.
  bool supports_end_of_options() const {
    return out.compiler_kind == CompilerKind::clang_cl || out.compiler_kind == CompilerKind::clang;
  }

  // Returns true when the argument was handled as the structural delimiter or
  // as an input after it.
  bool structural(std::size_t index) {
    const std::string& arg = command.arguments[index];
    if (end_of_options) {
      positional(index);
      return true;
    }
    if (arg == kEndOfOptions && supports_end_of_options()) {
      end_of_options = true;
      add_disposition(index, OptionAction::dropped, OptionCategory::structural,
                      "end of options (verified Clang driver): every following argument is an input");
      return true;
    }
    return false;
  }

  void add_disposition(std::size_t index, OptionAction action, OptionCategory category, std::string reason,
                       std::vector<std::string> replacement = {}) {
    OptionDisposition d;
    d.index = index;
    d.raw = command.arguments[index];
    d.action = action;
    d.category = category;
    d.reason = std::move(reason);
    d.replacement = std::move(replacement);
    if (action == OptionAction::kept || action == OptionAction::kept_unknown || action == OptionAction::transformed) {
      out.analyzer_arguments.insert(out.analyzer_arguments.end(), d.replacement.begin(), d.replacement.end());
    }
    out.dispositions.push_back(std::move(d));
  }

  // When both the spelled path and the entry file exist, the filesystem is
  // decisive (NTFS folds case by default, a case-sensitive directory does
  // not), so an alias of the entry file matches while a genuinely distinct
  // file never does, even when the lexical keys are equal. The lexical key is
  // only a fallback when an existing-file comparison is unavailable.
  bool is_entry_source(std::string_view arg) const {
    const std::filesystem::path p = path_from_utf8(arg);
    const std::filesystem::path resolved = (p.is_absolute() ? p : command.directory / p).lexically_normal();
    std::error_code ec1;
    std::error_code ec2;
    const bool spelled_exists = std::filesystem::exists(resolved, ec1) && !ec1;
    const bool entry_exists = std::filesystem::exists(command.file, ec2) && !ec2;
    if (spelled_exists && entry_exists) return same_existing_file(resolved, command.file);
    return path_key(resolved) == entry_file_key;
  }


  void record_value(OptionCategory category, const std::string& value, IncludeKind include_kind) {
    switch (category) {
      case OptionCategory::define:
        out.defines.push_back(value);
        break;
      case OptionCategory::undefine:
        out.undefines.push_back(value);
        break;
      case OptionCategory::include: {
        const std::filesystem::path p = path_from_utf8(value);
        out.includes.push_back(
            IncludeDirectory{value, (p.is_absolute() ? p : command.directory / p).lexically_normal(), include_kind});
        break;
      }
      case OptionCategory::forced_include:
        out.forced_includes.push_back(value);
        break;
      default:
        break;
    }
  }

  void positional(std::size_t index) {
    const std::string& arg = command.arguments[index];
    if (is_entry_source(arg)) {
      if (out.source_identified) {
        duplicate_source(index);
        return;
      }
      out.source_identified = true;
      add_disposition(index, OptionAction::source_input, OptionCategory::source, "the entry's source file");
    } else if (end_of_options) {
      // After the delimiter every token is an INPUT to the original driver,
      // whatever it looks like (`-DSTEALTH=1`, `/Iextra`, a second `--`). It
      // is never forwarded (a string allowlist could mistake it for an
      // option) and the context is degraded: the command compiles an
      // additional input this analysis does not cover.
      out.status = NormalizationStatus::degraded;
      add_disposition(index, OptionAction::dropped_unsupported, OptionCategory::source,
                      "additional input after the end of options; the original driver compiles it as a separate "
                      "input, so it is not forwarded and the context is degraded");
    } else {
      add_disposition(index, OptionAction::kept_unknown, OptionCategory::unknown,
                      "positional argument that is not the entry's source file; forwarded unchanged", {arg});
    }
  }

  // A second ACTUAL occurrence of the entry source (same spelling, a lexical
  // or filesystem alias such as `./main.cc`, before or after `--`, through
  // `/Tp`/`/Tc` or as a positional): the driver schedules a second compilation
  // job for it, which this analysis does not cover. The token is not forwarded
  // and the context is degraded so the analyzer rejects before the front end.
  // Include/define operands that merely equal the file name never get here:
  // they are consumed as option values first.
  void duplicate_source(std::size_t index) {
    out.status = NormalizationStatus::degraded;
    add_disposition(index, OptionAction::dropped_unsupported, OptionCategory::source,
                    "the entry's source file appears a second time: the driver would run a second compilation job; "
                    "not forwarded and the context is degraded");
  }

  void response_file(std::size_t index) {
    const std::string& arg = command.arguments[index];
    out.unexpanded_response_files.push_back(arg.substr(1));
    out.status = NormalizationStatus::degraded;
    add_disposition(index, OptionAction::dropped_unsupported, OptionCategory::response_file,
                    "response file not expanded; options inside it are missing from the analyzer context");
  }

  // Returns the value for a rule and how many extra arguments were consumed.
  // Operand consumption mirrors the driver exactly: a separated value is the
  // next argument whatever it looks like (`-I -DNAME` is an include directory
  // literally named -DNAME; `-D src/a.cpp` is a macro definition). No intent
  // is guessed from lookahead. Whether the command still names its source is
  // validated once, after parsing (source_identified).
  struct Value {
    std::string text;
    bool from_next = false;
    bool missing = false;
    std::string missing_reason;
  };
  Value take_value(const Rule& rule, std::string_view option_body, std::size_t index) const {
    Value v;
    if (rule.value == ValueMode::none) return v;
    std::string attached(option_body.substr(rule.spelling.size()));
    if (rule.value == ValueMode::attached) {
      v.text = attached;
      return v;
    }
    if (rule.value == ValueMode::attached_or_next && !attached.empty()) {
      v.text = attached;
      return v;
    }
    if (index + 1 >= command.arguments.size()) {
      v.missing = true;
      v.missing_reason = "value missing at end of command line";
      return v;
    }
    if (rule.value == ValueMode::next && command.arguments[index + 1].empty()) {
      // An option that always takes a separate value cannot be honoured with an
      // empty one; forwarding it would hand the front end a nameless operand.
      v.missing = true;
      v.missing_reason = "separated value is empty";
      return v;
    }
    v.text = command.arguments[index + 1];
    v.from_next = true;
    return v;
  }

  void run_msvc() {
    const auto& args = command.arguments;
    for (std::size_t i = 1; i < args.size(); ++i) {
      const std::string& arg = args[i];
      if (!arg.empty() && arg[0] == '@') {
        response_file(i);  // expansion precedes option parsing, before and after `--`
        continue;
      }
      if (structural(i)) continue;
      if (!is_msvc_option(arg)) {
        positional(i);
        continue;
      }
      const std::string_view body = std::string_view(arg).substr(1);
      if (out.compiler_kind == CompilerKind::clang_cl && arg == kAuditedClangClWarning) {
        // Exact audited token only: clang-cl forwards it to the front end as a
        // diagnostic control with no semantic effect. Native cl.exe would
        // ignore it with D9002, so it stays unknown there; `/W...` spellings and
        // every other warning token are not covered.
        add_disposition(i, OptionAction::dropped, OptionCategory::diagnostics,
                        "audited clang-cl warning control (-Wno-deprecated-declarations); diagnostic-only");
        continue;
      }
      if (starts_with(body, "imsvc") && out.compiler_kind != CompilerKind::clang_cl) {
        add_disposition(i, OptionAction::kept_unknown, OptionCategory::unknown,
                        "clang-cl-only option; this compiler does not recognise it; forwarded unchanged", {arg});
        continue;
      }
      if (body == "experimental:log" || body == "d2ExtendedWarningInfo") {
        // Audited for native cl.exe and for the SLASH spelling only. The MSVC
        // syntax accepts a leading '-' for both, and `run_msvc` strips either
        // prefix before matching, so without this check the unaudited
        // `-experimental:log` / `-d2ExtendedWarningInfo` forms would receive the
        // same silent drop. The exception is deliberately narrower than the
        // driver: anything outside the audited spelling stays unknown and the
        // safety gate rejects the unit.
        if (out.compiler_kind != CompilerKind::native_cl || arg[0] != '/') {
          add_disposition(i, OptionAction::kept_unknown, OptionCategory::unknown,
                          "audited for the native cl.exe '/' spelling only; not recognised here; forwarded unchanged",
                          {arg});
          continue;
        }
      }
      if (body == "link") {
        add_disposition(i, OptionAction::dropped, OptionCategory::linker, "linker options follow");
        for (std::size_t j = i + 1; j < args.size(); ++j) {
          add_disposition(j, OptionAction::dropped, OptionCategory::linker, "linker option after /link");
        }
        return;
      }
      const Rule* rule = match_rule(body, std::begin(kMsvcRules), std::end(kMsvcRules));
      if (!rule) {
        add_disposition(i, OptionAction::kept_unknown, OptionCategory::unknown,
                        "option not recognised by the normalizer; forwarded unchanged", {arg});
        continue;
      }
      apply_rule(*rule, body, i, '/');
    }
  }

  void run_gnu() {
    const auto& args = command.arguments;
    for (std::size_t i = 1; i < args.size(); ++i) {
      const std::string& arg = args[i];
      if (!arg.empty() && arg[0] == '@') {
        response_file(i);
        continue;
      }
      if (structural(i)) continue;
      if (!is_gnu_option(arg)) {
        positional(i);
        continue;
      }
      if (starts_with(arg, "-Wp,")) {
        preprocessor_passthrough(i);
        continue;
      }
      const Rule* rule = match_rule(arg, std::begin(kGnuRules), std::end(kGnuRules));
      if (!rule) {
        add_disposition(i, OptionAction::kept_unknown, OptionCategory::unknown,
                        "option not recognised by the normalizer; forwarded unchanged", {arg});
        continue;
      }
      apply_rule(*rule, arg, i, '-');
    }
  }

  // -Wp,<opt>[,<opt>...] hands options straight to the preprocessor. Only the
  // forms whose meaning is certain are unpacked (-D, -U, -I); anything else
  // would change active semantics invisibly, so it is reported as unsupported.
  void preprocessor_passthrough(std::size_t index) {
    const std::string& arg = command.arguments[index];
    std::vector<std::string> parts;
    std::size_t start = 4;  // after "-Wp,"
    while (start <= arg.size()) {
      const std::size_t comma = arg.find(',', start);
      parts.push_back(arg.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
      if (comma == std::string::npos) break;
      start = comma + 1;
    }
    std::vector<std::string> replacement;
    std::vector<std::pair<OptionCategory, std::string>> values;
    bool understood = !parts.empty();
    for (std::size_t p = 0; p < parts.size() && understood; ++p) {
      const std::string& part = parts[p];
      OptionCategory category = OptionCategory::unknown;
      if (starts_with(part, "-D")) category = OptionCategory::define;
      else if (starts_with(part, "-U")) category = OptionCategory::undefine;
      else if (starts_with(part, "-I")) category = OptionCategory::include;
      else {
        understood = false;
        break;
      }
      std::string value = part.substr(2);
      if (value.empty() && p + 1 < parts.size()) value = parts[++p];  // "-Wp,-D,NAME" form
      if (value.empty() || value[0] == '-') {
        // "-Wp,-D" or "-Wp,-D," would forward a bare value-taking flag that
        // could consume whatever the analyzer appends next.
        understood = false;
        break;
      }
      replacement.push_back(part.substr(0, 2) + value);
      values.emplace_back(category, value);
    }
    if (!understood) {
      out.status = NormalizationStatus::degraded;
      add_disposition(index, OptionAction::dropped_unsupported, OptionCategory::semantics,
                      "preprocessor passthrough with options the normalizer does not understand; "
                      "dropping it may change active semantics, so the context is degraded");
      return;
    }
    for (const auto& [category, value] : values) record_value(category, value, IncludeKind::user);
    add_disposition(index, OptionAction::transformed, OptionCategory::semantics,
                    "preprocessor passthrough unpacked into direct preprocessor options", replacement);
  }

  void apply_rule(const Rule& rule, std::string_view body, std::size_t& index, char option_char) {
    const Value value = take_value(rule, body, index);
    if (rule.value == ValueMode::attached && value.text.empty()) {
      // cl.exe's behaviour for a separated value ("/Fo out.obj") is not
      // modelled; instead of guessing, report the option and, when the next
      // argument looks like its value, that argument too.
      out.status = NormalizationStatus::degraded;
      add_disposition(index, OptionAction::dropped_unsupported, rule.category,
                      std::string(rule.reason) + ": attached value missing; separated-value form is not modelled");
      if (index + 1 < command.arguments.size() && !is_msvc_option(command.arguments[index + 1]) &&
          !is_entry_source(command.arguments[index + 1])) {
        ++index;
        add_disposition(index, OptionAction::dropped_unsupported, rule.category,
                        "probably the separated value of the preceding option; not forwarded and not treated as input");
      }
      return;
    }
    if (value.missing) {
      add_disposition(index, OptionAction::dropped_unsupported, rule.category,
                      std::string(rule.reason) + ": " + value.missing_reason);
      out.status = NormalizationStatus::degraded;
      return;
    }
    if (value.from_next && !value.text.empty() && value.text[0] == '@') {
      // The driver expands a standalone `@file` token BEFORE operand
      // consumption (`/I @x.rsp` makes the file's contents the include
      // directory), so the option's real value is unknown here. Neither token
      // is forwarded; the response file is recorded unexpanded and the context
      // is degraded. A joined `/I@literal` is one token and stays a literal path.
      out.status = NormalizationStatus::degraded;
      add_disposition(index, OptionAction::dropped_unsupported, rule.category,
                      std::string(rule.reason) + ": separated value is a standalone response-file token that the "
                                                 "driver expands before operand consumption; value unknown");
      ++index;
      response_file(index);
      return;
    }
    // Canonical spelling for the analyzer: option char + spelling, value attached
    // for single-token forms, separate for options that require a separate value.
    const std::string spelling = std::string(rule.spelling);
    const std::string head = (option_char == '/' ? "/" + spelling : spelling);
    std::vector<std::string> replacement;
    if (rule.value == ValueMode::none) {
      replacement = {option_char == '/' ? "/" + std::string(body) : std::string(body)};
    } else if (rule.value == ValueMode::next) {
      replacement = {head, value.text};
    } else {
      replacement = {head + value.text};
    }

    switch (rule.action) {
      case OptionAction::kept: {
        record_value(rule.category, value.text, include_kind_of(rule.spelling));
        if (rule.category == OptionCategory::language_standard) {
          out.language_standard = value.text.empty() ? std::string(body.substr(rule.spelling.size())) : value.text;
        }
        if (rule.category == OptionCategory::language) {
          out.language = rule.value == ValueMode::none ? (rule.spelling == "TC" ? "c" : "c++") : value.text;
          if (rule.spelling == "-x") replacement = {"-x", value.text};  // canonical separate form
        }
        if (rule.category == OptionCategory::runtime_library) out.runtime_library = std::string(rule.spelling);
        add_disposition(index, OptionAction::kept, rule.category, std::string(rule.reason), replacement);
        break;
      }
      case OptionAction::source_input: {
        if (is_entry_source(value.text) && out.source_identified) {
          duplicate_source(index);  // the source-option path counts like a positional occurrence
        } else if (is_entry_source(value.text)) {
          // /Tp and /Tc override the language for this file; the analyzer
          // invocation must carry that, not just the metadata.
          const std::string language_flag = rule.spelling == "Tp" ? "/TP" : "/TC";
          out.language = rule.spelling == "Tp" ? "c++" : "c";
          out.source_identified = true;
          add_disposition(index, OptionAction::source_input, OptionCategory::source,
                          std::string(rule.reason) + "; language override forwarded as " + language_flag);
          out.analyzer_arguments.push_back(language_flag);
          out.dispositions.back().replacement = {language_flag};
        } else {
          add_disposition(index, OptionAction::kept_unknown, OptionCategory::unknown,
                          "names a source file that is not the entry's file; forwarded unchanged", replacement);
        }
        break;
      }
      case OptionAction::transformed: {
        // Only /Yu reaches here: emulate PCH use with a forced include.
        if (value.text.empty()) {
          add_disposition(index, OptionAction::dropped_unsupported, rule.category,
                          "PCH use without a header name cannot be emulated");
          out.status = NormalizationStatus::degraded;
        } else {
          out.pch_emulated = true;
          out.forced_includes.push_back(value.text);
          if (out.status == NormalizationStatus::ok) out.status = NormalizationStatus::degraded;
          out.notes.push_back("pch_emulated: /Yu" + value.text + " replaced by /FI" + value.text);
          add_disposition(index, OptionAction::transformed, rule.category, std::string(rule.reason),
                          {"/FI" + value.text});
        }
        break;
      }
      case OptionAction::dropped:
        if (rule.category == OptionCategory::precompiled_header && rule.spelling == "Yc") out.pch_emulated = true;
        add_disposition(index, OptionAction::dropped, rule.category, std::string(rule.reason));
        break;
      case OptionAction::dropped_unsupported:
        out.status = NormalizationStatus::degraded;
        add_disposition(index, OptionAction::dropped_unsupported, rule.category, std::string(rule.reason));
        break;
      default:
        add_disposition(index, OptionAction::kept_unknown, OptionCategory::unknown, "unexpected rule action", replacement);
        break;
    }

    if (value.from_next) {
      ++index;
      add_disposition(index, out.dispositions.back().action == OptionAction::kept ? OptionAction::kept
                                                                                   : out.dispositions.back().action,
                      rule.category, "value of the preceding option");
    }
  }
};

}  // namespace

std::string_view to_string(DriverFlavor value) {
  switch (value) {
    case DriverFlavor::unknown:
      return "unknown";
    case DriverFlavor::msvc:
      return "msvc";
    case DriverFlavor::gnu:
      return "gnu";
  }
  return "unknown";
}

std::string_view to_string(CompilerKind value) {
  switch (value) {
    case CompilerKind::unknown:
      return "unknown";
    case CompilerKind::native_cl:
      return "native_cl";
    case CompilerKind::clang_cl:
      return "clang_cl";
    case CompilerKind::clang:
      return "clang";
    case CompilerKind::gnu_alias:
      return "gnu_alias";
  }
  return "unknown";
}

std::string_view to_string(IncludeKind value) {
  switch (value) {
    case IncludeKind::user:
      return "user";
    case IncludeKind::quote:
      return "quote";
    case IncludeKind::system:
      return "system";
    case IncludeKind::internal_system:
      return "internal_system";
    case IncludeKind::after:
      return "after";
  }
  return "unknown";
}

std::string_view to_string(OptionAction value) {
  switch (value) {
    case OptionAction::compiler:
      return "compiler";
    case OptionAction::source_input:
      return "source_input";
    case OptionAction::kept:
      return "kept";
    case OptionAction::kept_unknown:
      return "kept_unknown";
    case OptionAction::transformed:
      return "transformed";
    case OptionAction::dropped:
      return "dropped";
    case OptionAction::dropped_unsupported:
      return "dropped_unsupported";
  }
  return "unknown";
}

std::string_view to_string(OptionCategory value) {
  switch (value) {
    case OptionCategory::compiler:
      return "compiler";
    case OptionCategory::source:
      return "source";
    case OptionCategory::define:
      return "define";
    case OptionCategory::undefine:
      return "undefine";
    case OptionCategory::include:
      return "include";
    case OptionCategory::forced_include:
      return "forced_include";
    case OptionCategory::language_standard:
      return "language_standard";
    case OptionCategory::language:
      return "language";
    case OptionCategory::runtime_library:
      return "runtime_library";
    case OptionCategory::semantics:
      return "semantics";
    case OptionCategory::optimization:
      return "optimization";
    case OptionCategory::output:
      return "output";
    case OptionCategory::dependency_generation:
      return "dependency_generation";
    case OptionCategory::diagnostics:
      return "diagnostics";
    case OptionCategory::debug_info:
      return "debug_info";
    case OptionCategory::build_process:
      return "build_process";
    case OptionCategory::codegen:
      return "codegen";
    case OptionCategory::precompiled_header:
      return "precompiled_header";
    case OptionCategory::response_file:
      return "response_file";
    case OptionCategory::linker:
      return "linker";
    case OptionCategory::structural:
      return "structural";
    case OptionCategory::unknown:
      return "unknown";
  }
  return "unknown";
}

std::string_view to_string(NormalizationStatus value) {
  switch (value) {
    case NormalizationStatus::ok:
      return "ok";
    case NormalizationStatus::degraded:
      return "degraded";
    case NormalizationStatus::unusable:
      return "unusable";
  }
  return "unknown";
}

namespace {

// Lower-cased executable name without directory, `.exe` or a trailing
// version suffix such as clang-18 / g++-13.
std::string canonical_compiler_name(std::string_view compiler_argv0) {
  std::string name = ascii_lower(path_to_utf8_generic(path_from_utf8(compiler_argv0).filename()));
  if (name.size() > 4 && name.compare(name.size() - 4, 4, ".exe") == 0) name.resize(name.size() - 4);
  const std::size_t dash = name.find_last_of('-');
  if (dash != std::string::npos && dash + 1 < name.size() &&
      std::all_of(name.begin() + static_cast<std::ptrdiff_t>(dash) + 1, name.end(),
                  [](unsigned char c) { return std::isdigit(c) || c == '.'; })) {
    name.resize(dash);
  }
  return name;
}

}  // namespace

CompilerKind detect_compiler(std::string_view compiler_argv0) {
  const std::string name = canonical_compiler_name(compiler_argv0);
  if (name == "cl") return CompilerKind::native_cl;
  if (name == "clang-cl") return CompilerKind::clang_cl;
  if (name == "clang" || name == "clang++") return CompilerKind::clang;
  if (name == "gcc" || name == "g++" || name == "cc" || name == "c++") return CompilerKind::gnu_alias;
  return CompilerKind::unknown;
}

DriverFlavor detect_driver(std::string_view compiler_argv0) {
  switch (detect_compiler(compiler_argv0)) {
    case CompilerKind::native_cl:
    case CompilerKind::clang_cl:
      return DriverFlavor::msvc;
    case CompilerKind::clang:
    case CompilerKind::gnu_alias:
      return DriverFlavor::gnu;
    case CompilerKind::unknown:
      break;
  }
  return DriverFlavor::unknown;
}

NormalizedCompileContext normalize_compile_context(const CompileCommand& command) {
  Normalizer n(command);
  if (command.arguments.empty()) {
    n.out.status = NormalizationStatus::unusable;
    n.out.notes.push_back("empty argument list");
    return std::move(n.out);
  }
  n.out.compiler = command.arguments.front();
  n.out.compiler_kind = detect_compiler(n.out.compiler);
  n.out.driver = detect_driver(n.out.compiler);
  n.add_disposition(0, OptionAction::compiler, OptionCategory::compiler, "compiler driver");

  switch (n.out.driver) {
    case DriverFlavor::msvc:
      n.run_msvc();
      break;
    case DriverFlavor::gnu:
      n.run_gnu();
      break;
    case DriverFlavor::unknown:
      n.out.status = NormalizationStatus::unusable;
      n.out.notes.push_back("compiler driver '" + n.out.compiler + "' not recognised; options cannot be classified");
      for (std::size_t i = 1; i < command.arguments.size(); ++i) {
        n.add_disposition(i, OptionAction::kept_unknown, OptionCategory::unknown, "driver unknown; forwarded unchanged",
                          {command.arguments[i]});
      }
      break;
  }
  if (n.out.driver != DriverFlavor::unknown && !n.out.source_identified) {
    // Without an identified source the argv does not describe the entry's
    // compilation; appending the file later would invent a compile context.
    n.out.status = NormalizationStatus::degraded;
    n.out.notes.push_back("entry source file was not identified among the arguments; the analyzer must not append it");
  }
  return std::move(n.out);
}

}  // namespace lcm
