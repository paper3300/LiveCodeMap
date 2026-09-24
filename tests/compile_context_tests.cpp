#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "lcm/compile_context.hpp"
#include "lcm/compile_db.hpp"
#include "lcm/source.hpp"

#include "support/case_sensitive_dir.hpp"

using namespace lcm;
namespace fs = std::filesystem;

namespace {

CompileCommand command_for(std::vector<std::string> arguments, const char* directory = "C:/proj/build",
                           const char* file = "C:/proj/src/a.cpp") {
  CompileCommand c;
  c.directory = path_from_utf8(directory);
  c.file = path_from_utf8(file);
  c.arguments = std::move(arguments);
  return c;
}

const OptionDisposition& disposition_of(const NormalizedCompileContext& ctx, const std::string& raw) {
  auto it = std::find_if(ctx.dispositions.begin(), ctx.dispositions.end(),
                         [&](const OptionDisposition& d) { return d.raw == raw; });
  REQUIRE_MESSAGE(it != ctx.dispositions.end(), "no disposition for " << raw);
  return *it;
}

void check_one_disposition_per_argument(const CompileCommand& c, const NormalizedCompileContext& ctx) {
  REQUIRE(ctx.dispositions.size() == c.arguments.size());
  for (std::size_t i = 0; i < c.arguments.size(); ++i) {
    CHECK(ctx.dispositions[i].index == i);
    CHECK(ctx.dispositions[i].raw == c.arguments[i]);
  }
}

}  // namespace

TEST_CASE("driver detection by argv[0] spelling") {
  CHECK(detect_driver("cl.exe") == DriverFlavor::msvc);
  CHECK(detect_driver("C:/VS/bin/CL.EXE") == DriverFlavor::msvc);
  CHECK(detect_driver("clang-cl") == DriverFlavor::msvc);
  CHECK(detect_driver("clang++") == DriverFlavor::gnu);
  CHECK(detect_driver("/usr/bin/g++-13") == DriverFlavor::gnu);
  CHECK(detect_driver("clang-18") == DriverFlavor::gnu);
  CHECK(detect_driver("cc") == DriverFlavor::gnu);
  CHECK(detect_driver("weird-cc") == DriverFlavor::unknown);
  CHECK(detect_driver("cl-wrapper.exe") == DriverFlavor::unknown);
}

TEST_CASE("compiler executable identity is separate from the argument syntax flavor") {
  CHECK(detect_compiler("cl.exe") == CompilerKind::native_cl);
  CHECK(detect_compiler("C:/VS/bin/CL.EXE") == CompilerKind::native_cl);
  CHECK(detect_compiler("clang-cl") == CompilerKind::clang_cl);
  CHECK(detect_compiler("clang-cl.exe") == CompilerKind::clang_cl);
  CHECK(detect_compiler("clang") == CompilerKind::clang);
  CHECK(detect_compiler("clang++") == CompilerKind::clang);
  CHECK(detect_compiler("clang-18") == CompilerKind::clang);
  CHECK(detect_compiler("gcc") == CompilerKind::gnu_alias);
  CHECK(detect_compiler("/usr/bin/g++-13") == CompilerKind::gnu_alias);
  CHECK(detect_compiler("cc") == CompilerKind::gnu_alias);
  CHECK(detect_compiler("c++") == CompilerKind::gnu_alias);
  CHECK(detect_compiler("weird-cc") == CompilerKind::unknown);
  CHECK(detect_compiler("sccache.exe") == CompilerKind::unknown);
  CHECK(to_string(CompilerKind::native_cl) == "native_cl");
  CHECK(to_string(IncludeKind::internal_system) == "internal_system");
  CHECK(to_string(OptionCategory::structural) == "structural");
}

// Driver regression contracts (execution plan, "Adapter compatibility correction").
TEST_CASE("contract 1: clang-cl `--` is the end of options; an option-shaped source after it is the input") {
  const CompileCommand c = command_for({"clang-cl.exe", "-TP", "-c", "--", "-dash.cc"}, "C:/p", "C:/p/-dash.cc");
  const NormalizedCompileContext ctx = normalize_compile_context(c);
  check_one_disposition_per_argument(c, ctx);
  CHECK(ctx.compiler_kind == CompilerKind::clang_cl);
  CHECK(disposition_of(ctx, "--").action == OptionAction::dropped);
  CHECK(disposition_of(ctx, "--").category == OptionCategory::structural);
  CHECK(disposition_of(ctx, "-dash.cc").action == OptionAction::source_input);
  CHECK(ctx.source_identified);
  CHECK(ctx.analyzer_arguments == std::vector<std::string>{"/TP"});  // neither `--` nor the source is forwarded
  CHECK(ctx.status == NormalizationStatus::ok);

  SUBCASE("native cl.exe ignores `--` (D9002): the same command has no verified source") {
    const CompileCommand n = command_for({"cl.exe", "-TP", "-c", "--", "-dash.cc"}, "C:/p", "C:/p/-dash.cc");
    const NormalizedCompileContext nctx = normalize_compile_context(n);
    CHECK(nctx.compiler_kind == CompilerKind::native_cl);
    CHECK(disposition_of(nctx, "--").action == OptionAction::kept_unknown);
    CHECK(disposition_of(nctx, "-dash.cc").action == OptionAction::kept_unknown);
    CHECK_FALSE(nctx.source_identified);
    CHECK(nctx.status == NormalizationStatus::degraded);
    CHECK(nctx.analyzer_arguments == std::vector<std::string>{"/TP", "--", "-dash.cc"});  // rejected by the gate
  }
  SUBCASE("clang++ honours `--`; the GNU aliases are not verified and keep rejecting it") {
    const CompileCommand ok = command_for({"clang++", "-c", "--", "-dash.cc"}, "C:/p", "C:/p/-dash.cc");
    const NormalizedCompileContext octx = normalize_compile_context(ok);
    CHECK(disposition_of(octx, "--").category == OptionCategory::structural);
    CHECK(octx.source_identified);
    CHECK(octx.analyzer_arguments.empty());
    for (const char* alias : {"g++", "gcc", "cc", "c++"}) {
      CAPTURE(alias);
      const CompileCommand g = command_for({alias, "-c", "--", "-dash.cc"}, "C:/p", "C:/p/-dash.cc");
      const NormalizedCompileContext gctx = normalize_compile_context(g);
      CHECK(gctx.compiler_kind == CompilerKind::gnu_alias);
      CHECK(disposition_of(gctx, "--").action == OptionAction::kept_unknown);
      CHECK_FALSE(gctx.source_identified);
      CHECK(gctx.status == NormalizationStatus::degraded);
    }
  }
  SUBCASE("a second `--` after the delimiter is an additional input, not a delimiter") {
    const CompileCommand two = command_for({"clang-cl.exe", "-c", "--", "--", "main.cc"}, "C:/p", "C:/p/main.cc");
    const NormalizedCompileContext tctx = normalize_compile_context(two);
    CHECK(tctx.dispositions[2].category == OptionCategory::structural);
    CHECK(tctx.dispositions[3].action == OptionAction::dropped_unsupported);  // an input named `--`, not the entry
    CHECK(tctx.dispositions[3].category == OptionCategory::source);
    CHECK(tctx.dispositions[4].action == OptionAction::source_input);
    CHECK(tctx.analyzer_arguments.empty());
    CHECK(tctx.status == NormalizationStatus::degraded);
  }
}

TEST_CASE("contract 2: every non-selected input after `--` degrades the context and is never forwarded") {
  // Option-shaped extra inputs are the dangerous case: the original driver compiles `-DSTEALTH=1` as a
  // FILE, and a string allowlist would happily accept the same token as a define once `--` is gone.
  for (const char* extra : {"extra.cc", "-DSTEALTH=1", "/Iextra", "-Wno-deprecated-declarations", "--"}) {
    CAPTURE(extra);
    const CompileCommand c = command_for({"clang-cl.exe", "-TP", "-c", "--", "main.cc", extra}, "C:/p", "C:/p/main.cc");
    const NormalizedCompileContext ctx = normalize_compile_context(c);
    check_one_disposition_per_argument(c, ctx);
    CHECK(disposition_of(ctx, "main.cc").action == OptionAction::source_input);
    CHECK(ctx.dispositions[5].action == OptionAction::dropped_unsupported);
    CHECK(ctx.dispositions[5].category == OptionCategory::source);
    CHECK(ctx.dispositions[5].reason.find("additional input") != std::string::npos);
    CHECK(ctx.analyzer_arguments == std::vector<std::string>{"/TP"});
    CHECK(ctx.defines.empty());
    CHECK(ctx.includes.empty());
    CHECK(ctx.source_identified);
    CHECK(ctx.status == NormalizationStatus::degraded);
  }
  // Order does not matter: the extra input may precede the entry source.
  const CompileCommand first = command_for({"clang++", "-c", "--", "-DSTEALTH=1", "main.cc"}, "C:/p", "C:/p/main.cc");
  const NormalizedCompileContext fctx = normalize_compile_context(first);
  CHECK(fctx.dispositions[3].action == OptionAction::dropped_unsupported);
  CHECK(fctx.dispositions[4].action == OptionAction::source_input);
  CHECK(fctx.analyzer_arguments.empty());
  CHECK(fctx.status == NormalizationStatus::degraded);
}

TEST_CASE("contract 3: `--` consumed as an include operand is a literal directory; only the free one delimits") {
  const CompileCommand c =
      command_for({"clang-cl.exe", "-TP", "/I", "--", "-imsvc", "--", "-c", "--", "main.cc"}, "C:/p", "C:/p/main.cc");
  const NormalizedCompileContext ctx = normalize_compile_context(c);
  check_one_disposition_per_argument(c, ctx);
  REQUIRE(ctx.includes.size() == 2);
  CHECK(ctx.includes[0].as_written == "--");
  CHECK(ctx.includes[0].kind == IncludeKind::user);
  CHECK(ctx.includes[1].as_written == "--");
  CHECK(ctx.includes[1].kind == IncludeKind::internal_system);
  CHECK(ctx.dispositions[3].reason == "value of the preceding option");
  CHECK(ctx.dispositions[5].reason == "value of the preceding option");
  CHECK(ctx.dispositions[7].category == OptionCategory::structural);
  CHECK(ctx.dispositions[8].action == OptionAction::source_input);
  CHECK(ctx.analyzer_arguments == std::vector<std::string>{"/TP", "/I--", "/imsvc--"});
  CHECK(ctx.status == NormalizationStatus::ok);
}

TEST_CASE("contract 4: joined and separated -imsvc keep atomic paths, their search group and their order") {
  const CompileCommand c =
      command_for({"clang-cl.exe", "-TP", "-c", "-imsvcsys one", "-I", "user mid", "-imsvc", "sys two", "main.cc"},
                  "C:/p", "C:/p/main.cc");
  const NormalizedCompileContext ctx = normalize_compile_context(c);
  check_one_disposition_per_argument(c, ctx);
  REQUIRE(ctx.includes.size() == 3);
  CHECK(ctx.includes[0].as_written == "sys one");
  CHECK(ctx.includes[0].kind == IncludeKind::internal_system);
  CHECK(path_key(ctx.includes[0].resolved) == path_key(path_from_utf8("C:/p/sys one")));
  CHECK(ctx.includes[1].as_written == "user mid");
  CHECK(ctx.includes[1].kind == IncludeKind::user);
  CHECK(ctx.includes[2].as_written == "sys two");
  CHECK(ctx.includes[2].kind == IncludeKind::internal_system);
  CHECK(path_key(ctx.includes[2].resolved) == path_key(path_from_utf8("C:/p/sys two")));
  // Within the system group the metadata order is the argv order; the user group is separate.
  std::vector<std::string> system_group;
  for (const auto& inc : ctx.includes) {
    if (is_system_include(inc.kind)) system_group.push_back(inc.as_written);
  }
  CHECK(system_group == std::vector<std::string>{"sys one", "sys two"});
  CHECK(ctx.analyzer_arguments == std::vector<std::string>{"/TP", "/imsvcsys one", "/Iuser mid", "/imsvcsys two"});
  CHECK(disposition_of(ctx, "-imsvcsys one").action == OptionAction::kept);
  CHECK(disposition_of(ctx, "sys two").reason == "value of the preceding option");
  CHECK(ctx.status == NormalizationStatus::ok);

  SUBCASE("native cl.exe has no -imsvc: the token stays unknown and its separated value is a stray input") {
    const CompileCommand n = command_for({"cl.exe", "-c", "-imsvc", "sys two", "-imsvcsys one", "main.cc"}, "C:/p", "C:/p/main.cc");
    const NormalizedCompileContext nctx = normalize_compile_context(n);
    CHECK(disposition_of(nctx, "-imsvc").action == OptionAction::kept_unknown);
    CHECK(disposition_of(nctx, "sys two").action == OptionAction::kept_unknown);  // cl.exe would treat it as a source
    CHECK(disposition_of(nctx, "-imsvcsys one").action == OptionAction::kept_unknown);
    CHECK(nctx.includes.empty());
    CHECK(nctx.source_identified);
    CHECK(nctx.analyzer_arguments == std::vector<std::string>{"-imsvc", "sys two", "-imsvcsys one"});
  }
}

TEST_CASE("contract 5: a response file after `--` is still expanded by the driver, so it still degrades") {
  const CompileCommand c = command_for({"clang-cl.exe", "-TP", "-c", "--", "@harmless.rsp", "main.cc"}, "C:/p", "C:/p/main.cc");
  const NormalizedCompileContext ctx = normalize_compile_context(c);
  check_one_disposition_per_argument(c, ctx);
  CHECK(disposition_of(ctx, "@harmless.rsp").action == OptionAction::dropped_unsupported);
  CHECK(disposition_of(ctx, "@harmless.rsp").category == OptionCategory::response_file);
  CHECK(ctx.unexpanded_response_files == std::vector<std::string>{"harmless.rsp"});
  CHECK(ctx.status == NormalizationStatus::degraded);
  CHECK(disposition_of(ctx, "main.cc").action == OptionAction::source_input);  // never reclassified as the input
}

TEST_CASE("a standalone response token consumed as an operand is unexpanded response evidence, not a literal path") {
  // Root's pinned `clang-cl -### /I @harmless.rsp -c main.cc` shows cc1 `-I extra.cc`: expansion precedes
  // operand consumption. The normalizer must not invent a directory named `@harmless.rsp`.
  struct Case {
    std::vector<std::string> argv;
    const char* option;
  };
  for (const Case& k : {Case{{"clang-cl.exe", "/I", "@harmless.rsp", "-c", "main.cc"}, "/I"},
                        Case{{"clang-cl.exe", "-imsvc", "@harmless.rsp", "-c", "main.cc"}, "-imsvc"},
                        Case{{"clang++", "-I", "@harmless.rsp", "-c", "main.cc"}, "-I"},
                        Case{{"clang++", "-include", "@harmless.rsp", "-c", "main.cc"}, "-include"},
                        Case{{"cl.exe", "/I", "@harmless.rsp", "/c", "main.cc"}, "/I"}}) {
    CAPTURE(k.option);
    const CompileCommand c = command_for(k.argv, "C:/p", "C:/p/main.cc");
    const NormalizedCompileContext ctx = normalize_compile_context(c);
    check_one_disposition_per_argument(c, ctx);
    CHECK(disposition_of(ctx, k.option).action == OptionAction::dropped_unsupported);
    CHECK(disposition_of(ctx, k.option).reason.find("response-file token") != std::string::npos);
    CHECK(disposition_of(ctx, "@harmless.rsp").action == OptionAction::dropped_unsupported);
    CHECK(disposition_of(ctx, "@harmless.rsp").category == OptionCategory::response_file);
    CHECK(ctx.unexpanded_response_files == std::vector<std::string>{"harmless.rsp"});
    CHECK(ctx.includes.empty());
    CHECK(ctx.forced_includes.empty());
    CHECK(ctx.analyzer_arguments.empty());
    CHECK(ctx.source_identified);  // main.cc is still the entry; the request is rejected for being degraded
    CHECK(ctx.status == NormalizationStatus::degraded);
  }
  // Joined spellings are single tokens: the driver does not expand them, so they are literal paths.
  const CompileCommand joined = command_for({"clang-cl.exe", "/I@literal", "-imsvc@sys", "-c", "main.cc"}, "C:/p", "C:/p/main.cc");
  const NormalizedCompileContext jctx = normalize_compile_context(joined);
  REQUIRE(jctx.includes.size() == 2);
  CHECK(jctx.includes[0].as_written == "@literal");
  CHECK(jctx.includes[0].kind == IncludeKind::user);
  CHECK(jctx.includes[1].as_written == "@sys");
  CHECK(jctx.includes[1].kind == IncludeKind::internal_system);
  CHECK(jctx.unexpanded_response_files.empty());
  CHECK(jctx.analyzer_arguments == std::vector<std::string>{"/I@literal", "/imsvc@sys"});
  CHECK(jctx.status == NormalizationStatus::ok);
}

TEST_CASE("contract 6: only the exact audited clang-cl warning token is dropped; everything else stays rejected") {
  const CompileCommand c = command_for({"clang-cl.exe", "-TP", "-c", "-Wno-deprecated-declarations",
                                        "-Wno-bogus-unknown-thing", "/clang:-load", "/clang:plugin.dll", "main.cc"},
                                       "C:/p", "C:/p/main.cc");
  const NormalizedCompileContext ctx = normalize_compile_context(c);
  check_one_disposition_per_argument(c, ctx);
  CHECK(disposition_of(ctx, "-Wno-deprecated-declarations").action == OptionAction::dropped);
  CHECK(disposition_of(ctx, "-Wno-deprecated-declarations").category == OptionCategory::diagnostics);
  CHECK(disposition_of(ctx, "-Wno-bogus-unknown-thing").action == OptionAction::kept_unknown);
  CHECK(disposition_of(ctx, "/clang:-load").action == OptionAction::kept_unknown);
  CHECK(disposition_of(ctx, "/clang:plugin.dll").action == OptionAction::kept_unknown);
  CHECK(ctx.analyzer_arguments ==
        std::vector<std::string>{"/TP", "-Wno-bogus-unknown-thing", "/clang:-load", "/clang:plugin.dll"});
  CHECK(ctx.status == NormalizationStatus::ok);  // the gate, not the normalizer, rejects the forwarded unknowns

  SUBCASE("near misses and other compilers are not covered by the audited rule") {
    for (const char* spelling : {"/Wno-deprecated-declarations", "-Wno-deprecated-declarationsX", "-Wno-deprecated",
                                 "-Wdeprecated-declarations"}) {
      CAPTURE(spelling);
      const CompileCommand miss = command_for({"clang-cl.exe", "-c", spelling, "main.cc"}, "C:/p", "C:/p/main.cc");
      CHECK(disposition_of(normalize_compile_context(miss), spelling).action == OptionAction::kept_unknown);
    }
    const CompileCommand native = command_for({"cl.exe", "-c", "-Wno-deprecated-declarations", "main.cc"}, "C:/p", "C:/p/main.cc");
    CHECK(disposition_of(normalize_compile_context(native), "-Wno-deprecated-declarations").action ==
          OptionAction::kept_unknown);
  }
}

TEST_CASE("`/link` still swallows the rest of a clang-cl line, `--` included") {
  const CompileCommand c = command_for({"clang-cl.exe", "-c", "/link", "foo.lib", "--", "main.cc"}, "C:/p", "C:/p/main.cc");
  const NormalizedCompileContext ctx = normalize_compile_context(c);
  CHECK(disposition_of(ctx, "--").category == OptionCategory::linker);
  CHECK(disposition_of(ctx, "main.cc").category == OptionCategory::linker);
  CHECK_FALSE(ctx.source_identified);
  CHECK(ctx.status == NormalizationStatus::degraded);
}

TEST_CASE("MSVC command: raw argv preserved, normalized argv exact, every option accounted for") {
  const CompileCommand c = command_for({
      "cl.exe", "/nologo", "/c", "/W4", "/WX", "/EHsc", "/MDd", "/Zi", "/Od", "/std:c++20", "/permissive-", "/utf-8",
      "/DUNICODE", "/D", "_DEBUG", "/I..\\include", "/I", "C:\\ext\\inc", "/external:I", "C:\\sdk\\inc",
      "/FIforced.h", "/Yustdafx.h", "/Fpbuild\\pch.pch", "/Foobj\\a.obj", "/Fdobj\\vc.pdb", "/showIncludes", "/MP4",
      "..\\src\\a.cpp", "/link", "/LIBPATH:x"});
  const std::vector<std::string> raw_before = c.arguments;
  const NormalizedCompileContext ctx = normalize_compile_context(c);

  CHECK(c.arguments == raw_before);  // raw input untouched
  CHECK(ctx.driver == DriverFlavor::msvc);
  CHECK(ctx.compiler == "cl.exe");
  check_one_disposition_per_argument(c, ctx);

  CHECK(ctx.analyzer_arguments == std::vector<std::string>{
                                      "/EHsc", "/MDd", "/Od", "/std:c++20", "/permissive-", "/utf-8", "/DUNICODE",
                                      "/D_DEBUG", "/I..\\include", "/IC:\\ext\\inc", "/external:IC:\\sdk\\inc",
                                      "/FIforced.h", "/FIstdafx.h"});

  CHECK(ctx.defines == std::vector<std::string>{"UNICODE", "_DEBUG"});
  REQUIRE(ctx.includes.size() == 3);
  CHECK(ctx.includes[0].as_written == "..\\include");
  CHECK(path_key(ctx.includes[0].resolved) == path_key(path_from_utf8("C:/proj/include")));
  CHECK(ctx.includes[0].kind == IncludeKind::user);
  CHECK_FALSE(is_system_include(ctx.includes[0].kind));
  CHECK(path_key(ctx.includes[1].resolved) == path_key(path_from_utf8("C:/ext/inc")));
  CHECK(ctx.includes[2].kind == IncludeKind::system);
  CHECK(is_system_include(ctx.includes[2].kind));
  CHECK(ctx.forced_includes == std::vector<std::string>{"forced.h", "stdafx.h"});
  CHECK(ctx.language_standard == "c++20");
  CHECK(ctx.runtime_library == "MDd");
  CHECK(ctx.pch_emulated);
  CHECK(ctx.status == NormalizationStatus::degraded);  // PCH emulation is a degradation, reported as such

  CHECK(disposition_of(ctx, "/nologo").action == OptionAction::dropped);
  CHECK(disposition_of(ctx, "/nologo").category == OptionCategory::build_process);
  CHECK(disposition_of(ctx, "/W4").category == OptionCategory::diagnostics);
  CHECK(disposition_of(ctx, "/Zi").category == OptionCategory::debug_info);
  CHECK(disposition_of(ctx, "/Foobj\\a.obj").category == OptionCategory::output);
  CHECK(disposition_of(ctx, "/Fpbuild\\pch.pch").category == OptionCategory::precompiled_header);
  CHECK(disposition_of(ctx, "/showIncludes").category == OptionCategory::dependency_generation);
  CHECK(disposition_of(ctx, "/MP4").category == OptionCategory::build_process);
  CHECK(disposition_of(ctx, "/Od").action == OptionAction::kept);
  CHECK(disposition_of(ctx, "/Od").category == OptionCategory::optimization);
  CHECK(disposition_of(ctx, "/EHsc").category == OptionCategory::semantics);
  CHECK(disposition_of(ctx, "/Yustdafx.h").action == OptionAction::transformed);
  CHECK(disposition_of(ctx, "/Yustdafx.h").replacement == std::vector<std::string>{"/FIstdafx.h"});
  CHECK(disposition_of(ctx, "..\\src\\a.cpp").action == OptionAction::source_input);
  CHECK(disposition_of(ctx, "/link").category == OptionCategory::linker);
  CHECK(disposition_of(ctx, "/LIBPATH:x").action == OptionAction::dropped);
  CHECK(disposition_of(ctx, "_DEBUG").reason == "value of the preceding option");
  for (const auto& d : ctx.dispositions) CHECK_FALSE(d.reason.empty());
}

TEST_CASE("optimization switches are exact spellings: /OUT: is not an optimization level") {
  const CompileCommand c = command_for({"cl.exe", "/c", "/O2", "/Ob1", "/OUT:side-effect.exe", "../src/a.cpp"});
  const NormalizedCompileContext ctx = normalize_compile_context(c);
  CHECK(disposition_of(ctx, "/O2").category == OptionCategory::optimization);
  CHECK(disposition_of(ctx, "/Ob1").category == OptionCategory::optimization);
  CHECK(disposition_of(ctx, "/OUT:side-effect.exe").action == OptionAction::kept_unknown);
  CHECK(disposition_of(ctx, "/OUT:side-effect.exe").category == OptionCategory::unknown);
}

TEST_CASE("unknown options are kept and reported, never silently discarded") {
  const CompileCommand c = command_for({"cl.exe", "/c", "/Qsomething-new", "/DX", "../src/a.cpp"});
  const NormalizedCompileContext ctx = normalize_compile_context(c);
  check_one_disposition_per_argument(c, ctx);
  const auto& d = disposition_of(ctx, "/Qsomething-new");
  CHECK(d.action == OptionAction::kept_unknown);
  CHECK(d.category == OptionCategory::unknown);
  CHECK(ctx.analyzer_arguments == std::vector<std::string>{"/Qsomething-new", "/DX"});
  CHECK(ctx.status == NormalizationStatus::ok);
}

TEST_CASE("unsupported semantics and response files degrade the context explicitly") {
  const CompileCommand c = command_for({"cl.exe", "/clr", "@opts.rsp", "/DX", "../src/a.cpp"});
  const NormalizedCompileContext ctx = normalize_compile_context(c);
  check_one_disposition_per_argument(c, ctx);
  CHECK(disposition_of(ctx, "/clr").action == OptionAction::dropped_unsupported);
  CHECK(disposition_of(ctx, "@opts.rsp").action == OptionAction::dropped_unsupported);
  CHECK(disposition_of(ctx, "@opts.rsp").category == OptionCategory::response_file);
  CHECK(ctx.unexpanded_response_files == std::vector<std::string>{"opts.rsp"});
  CHECK(ctx.status == NormalizationStatus::degraded);
  CHECK(ctx.analyzer_arguments == std::vector<std::string>{"/DX"});
}

TEST_CASE("option whose value is missing at the end of the line is reported, not guessed") {
  const CompileCommand c = command_for({"cl.exe", "../src/a.cpp", "/I"});
  const NormalizedCompileContext ctx = normalize_compile_context(c);
  check_one_disposition_per_argument(c, ctx);
  CHECK(disposition_of(ctx, "/I").action == OptionAction::dropped_unsupported);
  CHECK(ctx.includes.empty());
  CHECK(ctx.status == NormalizationStatus::degraded);
}

TEST_CASE("GNU command: defines, includes, forced includes, standard; outputs and dependency files dropped") {
  const CompileCommand c = command_for(
      {"clang++", "-std=c++20", "-O2", "-g", "-Wall", "-Werror", "-fno-exceptions", "-DFOO=1", "-D", "BAR", "-I",
       "inc", "-isystem", "/usr/x", "-include", "pre.h", "-MD", "-MF", "a.d", "-o", "a.o", "-c", "../src/a.cpp"},
      "/home/u/proj/build", "/home/u/proj/src/a.cpp");
  const NormalizedCompileContext ctx = normalize_compile_context(c);
  CHECK(ctx.driver == DriverFlavor::gnu);
  check_one_disposition_per_argument(c, ctx);

  CHECK(ctx.analyzer_arguments == std::vector<std::string>{"-std=c++20", "-O2", "-fno-exceptions", "-DFOO=1", "-DBAR",
                                                           "-Iinc", "-isystem/usr/x", "-include", "pre.h"});
  CHECK(ctx.defines == std::vector<std::string>{"FOO=1", "BAR"});
  REQUIRE(ctx.includes.size() == 2);
  CHECK(path_key(ctx.includes[0].resolved) == path_key(path_from_utf8("/home/u/proj/build/inc")));
  CHECK(ctx.includes[0].kind == IncludeKind::user);
  CHECK(ctx.includes[1].kind == IncludeKind::system);
  CHECK(ctx.forced_includes == std::vector<std::string>{"pre.h"});
  CHECK(ctx.language_standard == "c++20");
  CHECK_FALSE(ctx.runtime_library);
  CHECK(ctx.status == NormalizationStatus::ok);

  CHECK(disposition_of(ctx, "-o").category == OptionCategory::output);
  CHECK(disposition_of(ctx, "a.o").action == OptionAction::dropped);
  CHECK(disposition_of(ctx, "-MD").category == OptionCategory::dependency_generation);
  CHECK(disposition_of(ctx, "-MF").category == OptionCategory::dependency_generation);
  CHECK(disposition_of(ctx, "a.d").action == OptionAction::dropped);
  CHECK(disposition_of(ctx, "-g").category == OptionCategory::debug_info);
  CHECK(disposition_of(ctx, "-Wall").category == OptionCategory::diagnostics);
  CHECK(disposition_of(ctx, "-c").category == OptionCategory::build_process);
  CHECK(disposition_of(ctx, "-fno-exceptions").action == OptionAction::kept);
  CHECK(disposition_of(ctx, "../src/a.cpp").action == OptionAction::source_input);
}

TEST_CASE("GNU precompiled header binary is unsupported and degrades the context") {
  const CompileCommand c = command_for({"clang", "-include-pch", "pre.pch", "-x", "c++", "src/a.cpp"}, "/p", "/p/src/a.cpp");
  const NormalizedCompileContext ctx = normalize_compile_context(c);
  check_one_disposition_per_argument(c, ctx);
  CHECK(disposition_of(ctx, "-include-pch").action == OptionAction::dropped_unsupported);
  CHECK(disposition_of(ctx, "pre.pch").action == OptionAction::dropped_unsupported);
  CHECK(ctx.language == "c++");
  CHECK(ctx.analyzer_arguments == std::vector<std::string>{"-x", "c++"});
  CHECK(ctx.status == NormalizationStatus::degraded);
  CHECK_FALSE(ctx.pch_emulated);
}

TEST_CASE("-Wp, preprocessor passthrough is unpacked when understood and degraded otherwise") {
  SUBCASE("defines and includes inside -Wp are real semantics, not warning control") {
    const CompileCommand c = command_for({"gcc", "-Wp,-DLCM_PROBE=1", "-Wp,-D,OTHER", "-Wp,-Iinc,-UGONE", "-c", "src/a.cpp"},
                                         "/p", "/p/src/a.cpp");
    const NormalizedCompileContext ctx = normalize_compile_context(c);
    check_one_disposition_per_argument(c, ctx);
    CHECK(disposition_of(ctx, "-Wp,-DLCM_PROBE=1").action == OptionAction::transformed);
    CHECK(ctx.analyzer_arguments == std::vector<std::string>{"-DLCM_PROBE=1", "-DOTHER", "-Iinc", "-UGONE"});
    CHECK(ctx.defines == std::vector<std::string>{"LCM_PROBE=1", "OTHER"});
    CHECK(ctx.undefines == std::vector<std::string>{"GONE"});
    REQUIRE(ctx.includes.size() == 1);
    CHECK(ctx.status == NormalizationStatus::ok);
  }
  SUBCASE("anything else inside -Wp degrades the context instead of being dropped as a warning") {
    const CompileCommand c = command_for({"gcc", "-Wp,-MD,dep.d", "-Wall", "-c", "src/a.cpp"}, "/p", "/p/src/a.cpp");
    const NormalizedCompileContext ctx = normalize_compile_context(c);
    check_one_disposition_per_argument(c, ctx);
    CHECK(disposition_of(ctx, "-Wp,-MD,dep.d").action == OptionAction::dropped_unsupported);
    CHECK(disposition_of(ctx, "-Wall").action == OptionAction::dropped);
    CHECK(ctx.status == NormalizationStatus::degraded);
  }
}

TEST_CASE("joined GNU value spellings are classified like their separated forms") {
  const CompileCommand c = command_for({"clang", "-ofoo.o", "-MFfoo.d", "-MTfoo.o", "-xc++", "-Iinc", "src/a.cpp"}, "/p",
                                       "/p/src/a.cpp");
  const NormalizedCompileContext ctx = normalize_compile_context(c);
  check_one_disposition_per_argument(c, ctx);
  CHECK(disposition_of(ctx, "-ofoo.o").category == OptionCategory::output);
  CHECK(disposition_of(ctx, "-ofoo.o").action == OptionAction::dropped);
  CHECK(disposition_of(ctx, "-MFfoo.d").category == OptionCategory::dependency_generation);
  CHECK(disposition_of(ctx, "-MTfoo.o").category == OptionCategory::dependency_generation);
  CHECK(ctx.language == "c++");
  CHECK(ctx.analyzer_arguments == std::vector<std::string>{"-x", "c++", "-Iinc"});
  CHECK(ctx.status == NormalizationStatus::ok);
}

TEST_CASE("separated MSVC attached-value options are reported as unsupported, not guessed") {
  const CompileCommand c = command_for({"cl.exe", "/c", "/Fo", "output.obj", "/DX", "../src/a.cpp"});
  const NormalizedCompileContext ctx = normalize_compile_context(c);
  check_one_disposition_per_argument(c, ctx);
  CHECK(disposition_of(ctx, "/Fo").action == OptionAction::dropped_unsupported);
  CHECK(disposition_of(ctx, "output.obj").action == OptionAction::dropped_unsupported);
  CHECK(ctx.analyzer_arguments == std::vector<std::string>{"/DX"});  // output.obj is not forwarded as an input
  CHECK(ctx.status == NormalizationStatus::degraded);

  // With the value attached the option is an ordinary dropped output.
  const CompileCommand attached = command_for({"cl.exe", "/c", "/Fooutput.obj", "../src/a.cpp"});
  const NormalizedCompileContext ctx2 = normalize_compile_context(attached);
  CHECK(disposition_of(ctx2, "/Fooutput.obj").action == OptionAction::dropped);
  CHECK(ctx2.status == NormalizationStatus::ok);
}

TEST_CASE("/Tp and /Tc language overrides reach the analyzer invocation") {
  const CompileCommand c = command_for({"cl.exe", "/c", "/Tp", "../src/a.cpp"});
  const NormalizedCompileContext ctx = normalize_compile_context(c);
  check_one_disposition_per_argument(c, ctx);
  CHECK(disposition_of(ctx, "/Tp").action == OptionAction::source_input);
  CHECK(ctx.language == "c++");
  CHECK(ctx.analyzer_arguments == std::vector<std::string>{"/TP"});

  const CompileCommand joined = command_for({"cl.exe", "/c", "/Tc../src/a.cpp"});
  const NormalizedCompileContext ctx2 = normalize_compile_context(joined);
  CHECK(ctx2.language == "c");
  CHECK(ctx2.analyzer_arguments == std::vector<std::string>{"/TC"});
}

TEST_CASE("operand consumption mirrors the driver; a source swallowed as an operand leaves no source") {
  SUBCASE("-D followed by the source path: the driver treats it as a macro, so no source remains") {
    const CompileCommand c = command_for({"clang", "-D", "src/a.cpp"}, "C:/p", "C:/p/src/a.cpp");
    const NormalizedCompileContext ctx = normalize_compile_context(c);
    check_one_disposition_per_argument(c, ctx);
    CHECK(disposition_of(ctx, "-D").action == OptionAction::kept);
    CHECK(disposition_of(ctx, "src/a.cpp").reason == "value of the preceding option");
    CHECK(ctx.defines == std::vector<std::string>{"src/a.cpp"});
    CHECK(ctx.analyzer_arguments == std::vector<std::string>{"-Dsrc/a.cpp"});
    CHECK_FALSE(ctx.source_identified);
    CHECK(ctx.status == NormalizationStatus::degraded);  // an analyzer must not append the source itself
  }
  SUBCASE("MSVC /I followed by the source path likewise") {
    const CompileCommand c = command_for({"cl.exe", "/c", "/I", "../src/a.cpp"});
    const NormalizedCompileContext ctx = normalize_compile_context(c);
    REQUIRE(ctx.includes.size() == 1);
    CHECK(ctx.includes.front().as_written == "../src/a.cpp");
    CHECK_FALSE(ctx.source_identified);
    CHECK(ctx.status == NormalizationStatus::degraded);
  }
}

TEST_CASE("operands are never reinterpreted from their spelling") {
  SUBCASE("an include directory literally named -DNAME stays an include directory") {
    const CompileCommand c = command_for({"clang", "-I", "-DNAME", "src/a.cpp"}, "C:/p", "C:/p/src/a.cpp");
    const NormalizedCompileContext ctx = normalize_compile_context(c);
    check_one_disposition_per_argument(c, ctx);
    REQUIRE(ctx.includes.size() == 1);
    CHECK(ctx.includes.front().as_written == "-DNAME");
    CHECK(ctx.defines.empty());
    CHECK(disposition_of(ctx, "-DNAME").reason == "value of the preceding option");
    CHECK(ctx.analyzer_arguments == std::vector<std::string>{"-I-DNAME"});
    CHECK(ctx.source_identified);
    CHECK(ctx.status == NormalizationStatus::ok);
  }
  SUBCASE("a forced include equal to the source path plus the real source input is fine") {
    const CompileCommand c = command_for({"clang", "-include", "src/a.cpp", "src/a.cpp"}, "C:/p", "C:/p/src/a.cpp");
    const NormalizedCompileContext ctx = normalize_compile_context(c);
    check_one_disposition_per_argument(c, ctx);
    CHECK(ctx.forced_includes == std::vector<std::string>{"src/a.cpp"});
    CHECK(ctx.dispositions[3].action == OptionAction::source_input);
    CHECK(ctx.source_identified);
    CHECK(ctx.status == NormalizationStatus::ok);
  }
  SUBCASE("a define whose value merely looks like the source path is still a define") {
    const CompileCommand c = command_for({"clang", "-DPATH=src/a.cpp", "src/a.cpp"}, "C:/p", "C:/p/src/a.cpp");
    const NormalizedCompileContext ctx = normalize_compile_context(c);
    CHECK(ctx.defines == std::vector<std::string>{"PATH=src/a.cpp"});
    CHECK(ctx.source_identified);
    CHECK(ctx.status == NormalizationStatus::ok);
  }
  SUBCASE("MSVC /D consuming a token that looks like an option is preserved as the driver does") {
    const CompileCommand c = command_for({"cl.exe", "/c", "/D", "/Ifoo", "../src/a.cpp"});
    const NormalizedCompileContext ctx = normalize_compile_context(c);
    CHECK(ctx.defines == std::vector<std::string>{"/Ifoo"});
    CHECK(ctx.includes.empty());
    CHECK(ctx.source_identified);
    CHECK(ctx.status == NormalizationStatus::ok);
  }
}

TEST_CASE("a command without an identifiable source is degraded and says so") {
  const CompileCommand c = command_for({"cl.exe", "/c", "/DFOO"});
  const NormalizedCompileContext ctx = normalize_compile_context(c);
  CHECK_FALSE(ctx.source_identified);
  CHECK(ctx.status == NormalizationStatus::degraded);
  CHECK(std::any_of(ctx.notes.begin(), ctx.notes.end(),
                    [](const std::string& n) { return n.find("not identified") != std::string::npos; }));
  CHECK(ctx.analyzer_arguments == std::vector<std::string>{"/DFOO"});

  // The source appearing twice is a second compilation job for the driver: degraded, never forwarded.
  const CompileCommand twice = command_for({"cl.exe", "/c", "../src/a.cpp", "../src/a.cpp"});
  const NormalizedCompileContext ctx2 = normalize_compile_context(twice);
  CHECK(ctx2.source_identified);
  CHECK(ctx2.dispositions[2].action == OptionAction::source_input);
  CHECK(ctx2.dispositions[3].action == OptionAction::dropped_unsupported);
  CHECK(ctx2.dispositions[3].category == OptionCategory::source);
  CHECK(ctx2.analyzer_arguments.empty());
  CHECK(ctx2.status == NormalizationStatus::degraded);
}

TEST_CASE("duplicate selected source: every second actual occurrence degrades the context (adapter audit)") {
  // Agent3's three failing cases: same spelling, lexical alias, duplicate across the delimiter.
  struct Case {
    std::vector<std::string> argv;
    std::size_t first;
    std::size_t second;
  };
  for (const Case& k : {Case{{"clang-cl.exe", "-TP", "-c", "main.cc", "main.cc"}, 3, 4},
                        Case{{"clang-cl.exe", "-TP", "-c", "main.cc", "./main.cc"}, 3, 4},
                        Case{{"clang-cl.exe", "-TP", "-c", "main.cc", "--", "main.cc"}, 3, 5},
                        Case{{"clang-cl.exe", "-TP", "-c", "--", "main.cc", "./main.cc"}, 4, 5},
                        Case{{"clang++", "-c", "main.cc", "./main.cc"}, 2, 3},
                        // Source-option forms: /Tp joined, separated, and mixed with a positional.
                        Case{{"clang-cl.exe", "-c", "/Tpmain.cc", "main.cc"}, 2, 3},
                        Case{{"clang-cl.exe", "-c", "main.cc", "/Tpmain.cc"}, 2, 3},
                        Case{{"clang-cl.exe", "-c", "/Tp", "main.cc", "/Tc", "./main.cc"}, 2, 4},
                        Case{{"clang-cl.exe", "-c", "/Tpmain.cc", "--", "main.cc"}, 2, 4}}) {
    CAPTURE(k.argv[k.second]);
    const CompileCommand c = command_for(k.argv, "C:/p", "C:/p/main.cc");
    const NormalizedCompileContext ctx = normalize_compile_context(c);
    check_one_disposition_per_argument(c, ctx);
    CHECK(ctx.source_identified);  // the first occurrence is still the entry
    CHECK(ctx.dispositions[k.first].action == OptionAction::source_input);
    CHECK(ctx.dispositions[k.second].action == OptionAction::dropped_unsupported);
    CHECK(ctx.dispositions[k.second].category == OptionCategory::source);
    CHECK(ctx.dispositions[k.second].reason.find("second time") != std::string::npos);
    CHECK(ctx.status == NormalizationStatus::degraded);
    for (const auto& arg : ctx.analyzer_arguments) CHECK(arg.find("main.cc") == std::string::npos);
  }
  // A separated /Tp value token inherits the unsupported disposition (no forwarding of the value).
  const CompileCommand sep = command_for({"clang-cl.exe", "-c", "main.cc", "/Tp", "main.cc"}, "C:/p", "C:/p/main.cc");
  const NormalizedCompileContext sctx = normalize_compile_context(sep);
  CHECK(sctx.dispositions[3].action == OptionAction::dropped_unsupported);
  CHECK(sctx.dispositions[4].action == OptionAction::dropped_unsupported);
  CHECK(sctx.dispositions[4].reason == "value of the preceding option");
  CHECK(sctx.analyzer_arguments.empty());
  CHECK(sctx.status == NormalizationStatus::degraded);

  SUBCASE("positive controls: operands equal to the file name are not source occurrences") {
    for (const std::vector<std::string> argv : {std::vector<std::string>{"clang-cl.exe", "-c", "/I", "main.cc", "main.cc"},
                                                 std::vector<std::string>{"clang-cl.exe", "-c", "/Imain.cc", "main.cc"},
                                                 std::vector<std::string>{"clang-cl.exe", "-c", "/D", "main.cc", "main.cc"},
                                                 std::vector<std::string>{"clang-cl.exe", "-c", "/FImain.cc", "main.cc"},
                                                 std::vector<std::string>{"clang-cl.exe", "-c", "-imsvc", "main.cc", "main.cc"},
                                                 std::vector<std::string>{"clang++", "-c", "-include", "main.cc", "main.cc"},
                                                 std::vector<std::string>{"clang++", "-c", "-DNAME=main.cc", "main.cc"}}) {
      CAPTURE(argv[2]);
      const CompileCommand c = command_for(argv, "C:/p", "C:/p/main.cc");
      const NormalizedCompileContext ctx = normalize_compile_context(c);
      check_one_disposition_per_argument(c, ctx);
      CHECK(ctx.source_identified);
      CHECK(ctx.dispositions.back().action == OptionAction::source_input);
      CHECK(ctx.status == NormalizationStatus::ok);
    }
    // A single source through /Tp with an ordinary include operand stays a single source.
    const CompileCommand tp = command_for({"clang-cl.exe", "-c", "/Imain.cc", "/Tpmain.cc"}, "C:/p", "C:/p/main.cc");
    const NormalizedCompileContext tctx = normalize_compile_context(tp);
    CHECK(tctx.source_identified);
    CHECK(tctx.status == NormalizationStatus::ok);
    CHECK(tctx.analyzer_arguments == std::vector<std::string>{"/Imain.cc", "/TP"});
  }
  SUBCASE("real filesystem alias on the default volume: a case alias of one existing file is a duplicate") {
    const fs::path root = fs::temp_directory_path() /
                          path_from_utf8("lcm-ctx-dup-" +
                                         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    {
      std::ofstream src(root / "main.cc", std::ios::binary);
      src << "int m;\n";
    }
#ifdef _WIN32
    if (same_existing_file(root / "main.cc", root / "MAIN.CC")) {
      CompileCommand c;
      c.directory = root;
      c.file = root / "main.cc";
      c.arguments = {"clang-cl.exe", "-c", "main.cc", "MAIN.CC"};
      const NormalizedCompileContext ctx = normalize_compile_context(c);
      CHECK(ctx.dispositions[2].action == OptionAction::source_input);
      CHECK(ctx.dispositions[3].action == OptionAction::dropped_unsupported);
      CHECK(ctx.status == NormalizationStatus::degraded);
    } else {
      MESSAGE("LIMITATION: case-insensitive alias not available on this volume; alias duplicate not executed");
    }
#endif
    std::error_code ec;
    fs::remove_all(root, ec);
  }
}

TEST_CASE("-Wp with an empty or option-shaped payload never forwards a bare value-taking flag") {
  for (const char* bad : {"-Wp,-D,", "-Wp,-D", "-Wp,-U,", "-Wp,-I,", "-Wp,-D,-Ifoo", "-Wp,-I,-DX"}) {
    const CompileCommand c = command_for({"clang", bad, "src/a.cpp"}, "C:/p", "C:/p/src/a.cpp");
    const NormalizedCompileContext ctx = normalize_compile_context(c);
    CAPTURE(bad);
    check_one_disposition_per_argument(c, ctx);
    CHECK(disposition_of(ctx, bad).action == OptionAction::dropped_unsupported);
    CHECK(ctx.analyzer_arguments.empty());
    CHECK(ctx.defines.empty());
    CHECK(ctx.undefines.empty());
    CHECK(ctx.includes.empty());
    CHECK(ctx.status == NormalizationStatus::degraded);
  }
}

TEST_CASE("entry source identification uses filesystem identity for real Unicode case aliases") {
  const fs::path root = fs::temp_directory_path() /
                        path_from_utf8("lcm-ctx-Ünïcode-" +
                                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directories(root / path_from_utf8("Ünïcode"));
  const fs::path file = root / path_from_utf8("Ünïcode/файл.cpp");
  const fs::path other = root / path_from_utf8("Ünïcode/файл2.cpp");
  {
    std::ofstream out(file, std::ios::binary);
    out << "int x;\n";
    std::ofstream out2(other, std::ios::binary);
    out2 << "int y;\n";
  }
  REQUIRE(fs::exists(file));

  CompileCommand c;
  c.directory = root;
  c.file = file;
  c.arguments = {"cl.exe", "/c", "ÜNÏCODE/ФАЙЛ.CPP", "Ünïcode/файл2.cpp", "Ünïcode/ДРУГОЙ.cpp"};
  const NormalizedCompileContext ctx = normalize_compile_context(c);
  check_one_disposition_per_argument(c, ctx);
#ifdef _WIN32
  // NTFS says the differently cased spelling is the same existing file: one source, no duplicate input.
  CHECK(disposition_of(ctx, "ÜNÏCODE/ФАЙЛ.CPP").action == OptionAction::source_input);
  CHECK(ctx.source_identified);
#endif
  // A genuinely different existing file and a non-existing spelling are not the entry source.
  CHECK(disposition_of(ctx, "Ünïcode/файл2.cpp").action == OptionAction::kept_unknown);
  CHECK(disposition_of(ctx, "Ünïcode/ДРУГОЙ.cpp").action == OptionAction::kept_unknown);

  std::error_code ec;
  fs::remove_all(root, ec);
}

#ifdef _WIN32
using lcm::test_support::try_enable_case_sensitive_directory;

TEST_CASE("distinct a.cpp and A.cpp in a case-sensitive directory are not conflated by the ASCII path key") {
  const fs::path root = fs::temp_directory_path() /
                        path_from_utf8("lcm-ctx-cs-" +
                                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directories(root);
  if (!try_enable_case_sensitive_directory(root)) {
    MESSAGE("LIMITATION: per-directory case sensitivity is not available on this system/volume; "
            "the case-sensitive conflation regression could not be executed");
    std::error_code ec;
    fs::remove_all(root, ec);
    return;
  }
  const fs::path lower = root / "a.cpp";
  const fs::path upper = root / "A.cpp";
  {
    std::ofstream l(lower, std::ios::binary);
    l << "int lower;\n";
    std::ofstream u(upper, std::ios::binary);
    u << "int upper;\n";
  }
  REQUIRE(fs::exists(lower));
  REQUIRE(fs::exists(upper));
  // Precondition of the finding: both exist, are distinct, and share the ASCII-folded key.
  REQUIRE_FALSE(same_existing_file(lower, upper));
  REQUIRE(path_key(lower) == path_key(upper));

  CompileCommand c;
  c.directory = root;
  c.file = lower;
  c.arguments = {"cl.exe", "/c", "A.cpp", "a.cpp"};
  const NormalizedCompileContext ctx = normalize_compile_context(c);
  check_one_disposition_per_argument(c, ctx);
  CHECK(disposition_of(ctx, "A.cpp").action == OptionAction::kept_unknown);  // a different file, not the entry
  CHECK(disposition_of(ctx, "a.cpp").action == OptionAction::source_input);
  CHECK(ctx.source_identified);
  CHECK(ctx.analyzer_arguments == std::vector<std::string>{"A.cpp"});

  std::error_code ec;
  fs::remove_all(root, ec);
}
#endif

TEST_CASE("unknown driver makes the context unusable while preserving every argument") {
  const CompileCommand c = command_for({"weird-cc", "-DX", "../src/a.cpp"});
  const NormalizedCompileContext ctx = normalize_compile_context(c);
  CHECK(ctx.driver == DriverFlavor::unknown);
  CHECK(ctx.status == NormalizationStatus::unusable);
  check_one_disposition_per_argument(c, ctx);
  CHECK(ctx.analyzer_arguments == std::vector<std::string>{"-DX", "../src/a.cpp"});
  CHECK(std::all_of(ctx.dispositions.begin() + 1, ctx.dispositions.end(),
                    [](const OptionDisposition& d) { return d.action == OptionAction::kept_unknown; }));
  CHECK_FALSE(ctx.notes.empty());
}

TEST_CASE("a positional argument that is not the entry file is kept and flagged") {
  const CompileCommand c = command_for({"cl.exe", "/c", "../src/a.cpp", "../src/other.cpp"});
  const NormalizedCompileContext ctx = normalize_compile_context(c);
  CHECK(disposition_of(ctx, "../src/a.cpp").action == OptionAction::source_input);
  CHECK(disposition_of(ctx, "../src/other.cpp").action == OptionAction::kept_unknown);
  CHECK(ctx.analyzer_arguments == std::vector<std::string>{"../src/other.cpp"});
}

TEST_CASE("normalization works on a parsed database entry and leaves the entry unchanged") {
  const std::string json = R"([{"directory": "C:/proj/build", "file": "../src/a.cpp",
    "command": "cl.exe /nologo /c /DFOO=\"a b\" /I\"C:\\inc dir\" ../src/a.cpp /Foa.obj"}])";
  CompileDbParseOptions options;
  options.syntax = CommandSyntax::windows;
  const auto parsed = parse_compilation_database(json, options);
  REQUIRE(parsed.database);
  REQUIRE(parsed.database->commands.size() == 1);
  const CompileCommand& entry = parsed.database->commands.front();
  const std::string id_before = entry.command_id;
  const NormalizedCompileContext ctx = normalize_compile_context(entry);
  CHECK(entry.command_id == id_before);
  CHECK(entry.arguments == std::vector<std::string>{"cl.exe", "/nologo", "/c", "/DFOO=a b", "/IC:\\inc dir",
                                                    "../src/a.cpp", "/Foa.obj"});
  CHECK(ctx.analyzer_arguments == std::vector<std::string>{"/DFOO=a b", "/IC:\\inc dir"});
  CHECK(ctx.defines == std::vector<std::string>{"FOO=a b"});
  CHECK(ctx.status == NormalizationStatus::ok);
}

// --- stage 3: the two argument forms the real Temppal command carries --------
//
// Both were recovered from the object's own recorded command line, which shows
// cl.exe consuming the .sarif path as the VALUE of /experimental:log rather
// than as a second source input. Only those two exact spellings are audited,
// and only for native cl.exe.

TEST_CASE("native cl.exe /experimental:log drops the option together with its separated value") {
  const CompileCommand c = command_for(
      {"cl.exe", "/c", "/experimental:log", "C:/build/a.sarif", "/DKEEP", "../src/a.cpp"});
  const NormalizedCompileContext ctx = normalize_compile_context(c);
  check_one_disposition_per_argument(c, ctx);
  CHECK(ctx.status == NormalizationStatus::ok);
  CHECK(ctx.source_identified);
  CHECK(disposition_of(ctx, "/experimental:log").action == OptionAction::dropped);
  CHECK(disposition_of(ctx, "/experimental:log").category == OptionCategory::diagnostics);
  // The value is consumed by the option: it is neither forwarded nor mistaken
  // for an extra input.
  CHECK(disposition_of(ctx, "C:/build/a.sarif").action == OptionAction::dropped);
  CHECK(disposition_of(ctx, "C:/build/a.sarif").reason == "value of the preceding option");
  CHECK(ctx.analyzer_arguments == std::vector<std::string>{"/DKEEP"});
}

TEST_CASE("native cl.exe /d2ExtendedWarningInfo is dropped as a back-end switch") {
  const CompileCommand c = command_for({"cl.exe", "/c", "/d2ExtendedWarningInfo", "/DKEEP", "../src/a.cpp"});
  const NormalizedCompileContext ctx = normalize_compile_context(c);
  check_one_disposition_per_argument(c, ctx);
  CHECK(ctx.status == NormalizationStatus::ok);
  CHECK(disposition_of(ctx, "/d2ExtendedWarningInfo").action == OptionAction::dropped);
  CHECK(disposition_of(ctx, "/d2ExtendedWarningInfo").category == OptionCategory::codegen);
  CHECK(ctx.analyzer_arguments == std::vector<std::string>{"/DKEEP"});
}

TEST_CASE("the two audited forms stay unknown for every other compiler") {
  for (const char* compiler : {"clang-cl.exe", "clang++", "gcc"}) {
    for (const char* option : {"/experimental:log", "/d2ExtendedWarningInfo"}) {
      CAPTURE(compiler);
      CAPTURE(option);
      const bool msvc_syntax = std::string(compiler) == "clang-cl.exe";
      const CompileCommand c =
          msvc_syntax ? command_for({compiler, "/c", option, "C:/build/a.sarif", "../src/a.cpp"})
                      : command_for({compiler, "-c", option, "C:/build/a.sarif", "../src/a.cpp"});
      const NormalizedCompileContext ctx = normalize_compile_context(c);
      // Never dropped: the option is unknown there, so it survives to the
      // safety gate, which rejects the unit instead of ignoring the switch.
      CHECK(disposition_of(ctx, option).action == OptionAction::kept_unknown);
      CHECK(std::find(ctx.analyzer_arguments.begin(), ctx.analyzer_arguments.end(), option) !=
            ctx.analyzer_arguments.end());
    }
  }
}

TEST_CASE("lookalike spellings of the two audited forms are not covered") {
  for (const char* option : {"/experimental:module", "/experimental:deterministic", "/experimental:",
                             "/experimental:logg", "/experimental:logC:/a.sarif", "/d2", "/d2ExtendedWarningInfo2",
                             "/d2ExtendedWarning", "/d2somethingelse"}) {
    CAPTURE(option);
    const CompileCommand c = command_for({"cl.exe", "/c", option, "../src/a.cpp"});
    const NormalizedCompileContext ctx = normalize_compile_context(c);
    CHECK(disposition_of(ctx, option).action == OptionAction::kept_unknown);
    CHECK(std::find(ctx.analyzer_arguments.begin(), ctx.analyzer_arguments.end(), option) !=
          ctx.analyzer_arguments.end());
  }
}

TEST_CASE("/experimental:log without a usable value degrades instead of dropping silently") {
  SUBCASE("missing at the end of the command line") {
    const CompileCommand c = command_for({"cl.exe", "/c", "../src/a.cpp", "/experimental:log"});
    const NormalizedCompileContext ctx = normalize_compile_context(c);
    CHECK(ctx.status == NormalizationStatus::degraded);
    CHECK(disposition_of(ctx, "/experimental:log").action == OptionAction::dropped_unsupported);
  }
  SUBCASE("empty separated value") {
    const CompileCommand c = command_for({"cl.exe", "/c", "/experimental:log", "", "../src/a.cpp"});
    const NormalizedCompileContext ctx = normalize_compile_context(c);
    CHECK(ctx.status == NormalizationStatus::degraded);
    CHECK(disposition_of(ctx, "/experimental:log").action == OptionAction::dropped_unsupported);
  }
  SUBCASE("a response-file token as the value keeps the existing expansion boundary") {
    const CompileCommand c = command_for({"cl.exe", "/c", "/experimental:log", "@log.rsp", "../src/a.cpp"});
    const NormalizedCompileContext ctx = normalize_compile_context(c);
    CHECK(ctx.status == NormalizationStatus::degraded);
    CHECK(ctx.unexpanded_response_files == std::vector<std::string>{"log.rsp"});
  }
}

TEST_CASE("/experimental:log consumes whatever follows, so it can swallow the source") {
  // cl.exe takes the next argument as the value whatever it looks like. When
  // that argument is the entry's source the command no longer names its source,
  // which the existing post-parse check turns into a degraded context.
  const CompileCommand c = command_for({"cl.exe", "/c", "/experimental:log", "../src/a.cpp"});
  const NormalizedCompileContext ctx = normalize_compile_context(c);
  CHECK_FALSE(ctx.source_identified);
  CHECK(ctx.status == NormalizationStatus::degraded);
}

TEST_CASE("an extra source next to the audited forms still rejects as an extra input") {
  const CompileCommand c =
      command_for({"cl.exe", "/c", "/experimental:log", "C:/build/a.sarif", "/d2ExtendedWarningInfo",
                   "../src/a.cpp", "../src/other.cpp"});
  const NormalizedCompileContext ctx = normalize_compile_context(c);
  CHECK(ctx.source_identified);
  // The second source is not the entry's file: it stays an unknown positional
  // and the safety gate refuses it.
  CHECK(disposition_of(ctx, "../src/other.cpp").action == OptionAction::kept_unknown);
  CHECK(std::find(ctx.analyzer_arguments.begin(), ctx.analyzer_arguments.end(), "../src/other.cpp") !=
        ctx.analyzer_arguments.end());
}

TEST_CASE("the two audited forms are the slash spelling only; the dash spelling stays unknown") {
  // MSVC syntax accepts a leading '-' for both, and the normalizer strips either
  // prefix before matching rules. The audited exception is narrower than the
  // driver on purpose: only the reviewed '/' spelling is dropped.
  for (const char* option : {"-experimental:log", "-d2ExtendedWarningInfo"}) {
    CAPTURE(option);
    const CompileCommand c = command_for({"cl.exe", "/c", option, "C:/build/a.sarif", "../src/a.cpp"});
    const NormalizedCompileContext ctx = normalize_compile_context(c);
    CHECK(disposition_of(ctx, option).action == OptionAction::kept_unknown);
    CHECK(std::find(ctx.analyzer_arguments.begin(), ctx.analyzer_arguments.end(), option) !=
          ctx.analyzer_arguments.end());
    // Its operand is not consumed either: an unknown switch takes no value, so
    // the following token stays a separate unknown positional.
    CHECK(disposition_of(ctx, "C:/build/a.sarif").action == OptionAction::kept_unknown);
  }
  // The audited slash spellings still behave as reviewed.
  const CompileCommand ok = command_for({"cl.exe", "/c", "/experimental:log", "C:/build/a.sarif",
                                         "/d2ExtendedWarningInfo", "../src/a.cpp"});
  const NormalizedCompileContext octx = normalize_compile_context(ok);
  CHECK(octx.status == NormalizationStatus::ok);
  CHECK(octx.analyzer_arguments.empty());
}
