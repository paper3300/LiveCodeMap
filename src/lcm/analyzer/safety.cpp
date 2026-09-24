#include "lcm/analyzer/safety.hpp"

#include <cctype>
#include <string_view>

namespace lcm::analyzer {
namespace {

bool starts_with(std::string_view s, std::string_view prefix) { return s.substr(0, prefix.size()) == prefix; }

// A value made only of identifier-ish characters (letters, digits, '_', '-',
// '+', '.', ':'): used for switch values such as /std:c++20 or /Zc:__cplusplus.
bool plain_value(std::string_view v) {
  if (v.empty()) return false;
  for (char c : v) {
    const auto uc = static_cast<unsigned char>(c);
    if (!(std::isalnum(uc) || c == '_' || c == '-' || c == '+' || c == '.' || c == ':')) return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// MSVC (clang-cl driver mode). `body` is the argument without its leading
// '/' or '-'. Every accepted form is spelled out; no open-ended prefixes.
const char* check_msvc(std::string_view body, CompilerKind compiler) {
  // Value-taking options: value must be attached and non-empty.
  for (std::string_view opt : {"D", "U", "I", "FI"}) {
    if (starts_with(body, opt)) return body.size() > opt.size() ? nullptr : "value-taking option without a value";
  }
  if (starts_with(body, "external:I")) return body.size() > 10 ? nullptr : "value-taking option without a value";
  if (starts_with(body, "imsvc")) {
    // clang-cl system include (as if in %INCLUDE%). Native cl.exe has no such
    // option, so the same spelling in a cl.exe command is an unknown token.
    if (compiler != CompilerKind::clang_cl) return "clang-cl-only option is not accepted for this compiler";
    return body.size() > 5 ? nullptr : "value-taking option without a value";
  }
  if (starts_with(body, "std:")) {
    const std::string_view v = body.substr(4);
    for (std::string_view ok : {"c++14", "c++17", "c++20", "c++23", "c++latest", "c11", "c17", "c23", "clatest"}) {
      if (v == ok) return nullptr;
    }
    return "unsupported language standard value";
  }
  if (starts_with(body, "Zc:")) return plain_value(body.substr(3)) ? nullptr : "malformed conformance switch";
  if (starts_with(body, "arch:")) {
    const std::string_view v = body.substr(5);
    for (std::string_view ok : {"IA32", "SSE", "SSE2", "AVX", "AVX2", "AVX512", "AVX10.1", "ARMv7VE", "VFPv4", "armv8.0",
                                "armv8.1", "armv8.2", "armv8.3", "armv8.4", "armv8.5", "armv8.6", "armv8.7", "armv8.8"}) {
      if (v == ok) return nullptr;
    }
    return "unsupported architecture value";
  }
  if (starts_with(body, "fp:")) {
    const std::string_view v = body.substr(3);
    for (std::string_view ok : {"precise", "fast", "strict", "except", "except-", "contract"}) {
      if (v == ok) return nullptr;
    }
    return "unsupported floating-point model value";
  }
  if (starts_with(body, "source-charset:") || starts_with(body, "execution-charset:")) {
    const std::string_view v = body.substr(body.find(':') + 1);
    return plain_value(v) ? nullptr : "malformed charset value";
  }
  if (starts_with(body, "Zp")) {
    const std::string_view v = body.substr(2);
    for (std::string_view ok : {"", "1", "2", "4", "8", "16"}) {
      if (v == ok) return nullptr;
    }
    return "unsupported packing value";
  }
  if (starts_with(body, "vm")) {
    const std::string_view v = body.substr(2);
    if (v.empty()) return "malformed pointer-to-member switch";
    for (char c : v) {
      if (c != 'b' && c != 'g' && c != 'm' && c != 's' && c != 'v') return "malformed pointer-to-member switch";
    }
    return nullptr;
  }
  if (starts_with(body, "EH")) {
    const std::string_view v = body.substr(2);
    if (v.empty()) return "malformed exception-handling switch";
    for (std::size_t i = 0; i < v.size(); ++i) {
      const char c = v[i];
      if (c == '-' && i == v.size() - 1) continue;
      if (c != 'a' && c != 's' && c != 'c' && c != 'r') return "malformed exception-handling switch";
    }
    return nullptr;
  }
  for (std::string_view exact :
       {"TP", "TC", "GR", "GR-", "permissive", "permissive-", "utf-8", "MD", "MDd", "MT", "MTd", "LD", "LDd", "Za", "Ze",
        "J", "openmp", "openmp-", "openmp:experimental", "openmp:llvm", "await", "await:strict", "O1", "O2", "Ox", "Od",
        "Og", "Oi", "Oi-", "Os", "Ot", "Ob0", "Ob1", "Ob2", "Ob3", "Oy", "Oy-"}) {
    if (body == exact) return nullptr;
  }
  return "not in the analyzer allowlist";
}

// ---------------------------------------------------------------------------
// GNU (g++ driver mode). Exact spellings, or `name=` prefixes with a
// non-empty value, or explicit two-token forms.
constexpr std::string_view kGnuExact[] = {
    "-ansi", "-nostdinc", "-nostdinc++", "-nostdlib", "-nostdlib++", "-pthread",
    "-O0", "-O1", "-O2", "-O3", "-Os", "-Oz", "-Og", "-Ofast", "-O",
    "-m32", "-m64", "-msse", "-msse2", "-msse3", "-mssse3", "-msse4", "-msse4.1", "-msse4.2", "-mavx", "-mavx2",
    "-mavx512f", "-mfma", "-mbmi", "-mbmi2", "-mpopcnt", "-mno-sse", "-mno-sse2", "-mno-avx", "-mno-avx2",
    "-mno-red-zone", "-mred-zone",
    // Front-end feature flags (exact).
    "-fexceptions", "-fno-exceptions", "-frtti", "-fno-rtti", "-fms-extensions", "-fno-ms-extensions",
    "-fms-compatibility", "-fno-ms-compatibility", "-fchar8_t", "-fno-char8_t", "-fsigned-char", "-funsigned-char",
    "-fshort-wchar", "-fno-short-wchar", "-fPIC", "-fpic", "-fPIE", "-fpie", "-fno-pic", "-fno-pie",
    "-fstack-protector", "-fstack-protector-strong", "-fstack-protector-all", "-fno-stack-protector",
    "-fstrict-aliasing", "-fno-strict-aliasing", "-fopenmp", "-fno-openmp", "-fcoroutines", "-fno-coroutines",
    "-fsized-deallocation", "-fno-sized-deallocation", "-faligned-new", "-fno-aligned-new", "-fgnu-keywords",
    "-fno-gnu-keywords", "-fdollars-in-identifiers", "-fno-dollars-in-identifiers", "-foperator-names",
    "-fno-operator-names", "-fms-volatile", "-fdelayed-template-parsing", "-fno-delayed-template-parsing",
    "-fno-builtin", "-fbuiltin", "-ffreestanding", "-fno-freestanding", "-fcommon", "-fno-common", "-fwrapv",
    "-fno-wrapv", "-ffast-math", "-fno-fast-math", "-fno-elide-constructors", "-felide-constructors", "-fasm-blocks",
    "-fno-asm-blocks", "-fdeclspec", "-fno-declspec", "-fborland-extensions", "-fno-plt", "-fomit-frame-pointer",
    "-fno-omit-frame-pointer", "-finline", "-fno-inline", "-fdata-sections", "-ffunction-sections", "-funwind-tables",
    "-fno-unwind-tables", "-fasynchronous-unwind-tables", "-fextended-identifiers", "-fpermissive",
    "-fno-access-control", "-fsigned-zeros", "-fno-signed-zeros", "-ftrapping-math", "-fno-trapping-math",
    "-fvisibility-inlines-hidden", "-fno-semantic-interposition", "-fstack-clash-protection", "-fno-rtti-data",
    "-fthreadsafe-statics", "-fno-threadsafe-statics", "-fuse-cxa-atexit", "-fno-use-cxa-atexit",
    "-fno-new-alignment",
};

// `name=value` forms with a non-empty plain value.
constexpr std::string_view kGnuValued[] = {
    "-std=", "--target=", "--sysroot=", "-stdlib=", "-march=", "-mtune=", "-mcpu=", "-mfpmath=", "-mfloat-abi=",
    "-mthread-model=", "-mcmodel=", "-mstack-alignment=", "-fwchar-type=", "-fvisibility=", "-ffp-contract=",
    "-ffp-model=", "-fdenormal-fp-math=", "-fexec-charset=", "-finput-charset=", "-fnew-alignment=",
    "-ftemplate-depth=", "-fconstexpr-depth=", "-fconstexpr-steps=", "-fbracket-depth=", "-fcf-protection=",
};

// Attached-value options (value may contain anything, must be non-empty).
constexpr std::string_view kGnuAttached[] = {"-D", "-U", "-I", "-isystem", "-iquote", "-idirafter"};

// Two-token options: the next argument is the value.
constexpr std::string_view kGnuTwoToken[] = {"-include", "-imacros", "-x", "-target", "--sysroot", "-isysroot"};

const char* check_gnu(std::string_view arg, bool& consumes_next) {
  consumes_next = false;
  for (std::string_view exact : kGnuExact) {
    if (arg == exact) return nullptr;
  }
  for (std::string_view prefix : kGnuValued) {
    if (starts_with(arg, prefix)) return arg.size() > prefix.size() ? nullptr : "value-taking option without a value";
  }
  for (std::string_view prefix : kGnuAttached) {
    if (starts_with(arg, prefix)) return arg.size() > prefix.size() ? nullptr : "value-taking option without a value";
  }
  for (std::string_view two : kGnuTwoToken) {
    if (arg == two) {
      consumes_next = true;
      return nullptr;
    }
  }
  if (starts_with(arg, "-f")) return "feature flag not in the analyzer allowlist";
  if (starts_with(arg, "-m")) return "machine option not in the analyzer allowlist";
  return "not in the analyzer allowlist";
}

}  // namespace

SafetyVerdict check_analyzer_arguments(const std::vector<std::string>& analyzer_arguments, DriverFlavor driver,
                                       CompilerKind compiler) {
  SafetyVerdict verdict;
  if (driver == DriverFlavor::unknown) {
    verdict.rejected.push_back("<driver>: unknown compiler driver; no argument can be classified");
    verdict.safe = false;
    return verdict;
  }
  for (std::size_t i = 0; i < analyzer_arguments.size(); ++i) {
    const std::string& arg = analyzer_arguments[i];
    if (arg.empty()) {
      verdict.rejected.push_back("<empty>: empty argument");
      continue;
    }
    bool consumes_next = false;
    const char* reason = nullptr;
    if (driver == DriverFlavor::msvc) {
      if (arg[0] != '/' && arg[0] != '-' || arg.size() < 2) {
        reason = "positional or unknown token is not forwarded";
      } else {
        reason = check_msvc(std::string_view(arg).substr(1), compiler);
      }
    } else {
      if (arg[0] != '-') {
        reason = "positional or unknown token is not forwarded";
      } else {
        reason = check_gnu(arg, consumes_next);
      }
    }
    if (reason) {
      verdict.rejected.push_back(arg + ": " + reason);
      continue;
    }
    if (consumes_next) {
      if (i + 1 >= analyzer_arguments.size()) {
        verdict.rejected.push_back(arg + ": value-taking option without a value");
      } else if (analyzer_arguments[i + 1].empty() || analyzer_arguments[i + 1][0] == '-') {
        verdict.rejected.push_back(arg + " " + analyzer_arguments[i + 1] + ": option-shaped or empty value is not forwarded");
        ++i;
      } else {
        ++i;  // the value token belongs to this option
      }
    }
  }
  verdict.safe = verdict.rejected.empty();
  return verdict;
}

}  // namespace lcm::analyzer
