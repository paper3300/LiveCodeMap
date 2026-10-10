// livecodemap: read-only query CLI and MCP stdio server (Clang-free).
// Arguments, the LIVECODEMAP_DB variable and all output are UTF-8.

#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "lcm/query/cli.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <fcntl.h>
#include <io.h>
#include <windows.h>

namespace {

std::string utf8(const wchar_t* text) {
  const int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return {};
  std::string out(static_cast<std::size_t>(n - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), n, nullptr, nullptr);
  return out;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  // Raw bytes on the standard streams: no CRLF translation of protocol lines.
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
  SetConsoleOutputCP(CP_UTF8);
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) args.push_back(utf8(argv[i]));
  std::optional<std::string> env_db;
  if (const wchar_t* v = _wgetenv(L"LIVECODEMAP_DB")) env_db = utf8(v);
  return lcm::query::run_cli(args, env_db, std::cin, std::cout, std::cerr);
}

#else

#include <cstdlib>

int main(int argc, char** argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
  std::optional<std::string> env_db;
  if (const char* v = std::getenv("LIVECODEMAP_DB")) env_db = v;
  return lcm::query::run_cli(args, env_db, std::cin, std::cout, std::cerr);
}

#endif
