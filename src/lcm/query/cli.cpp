#include "lcm/query/cli.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <initializer_list>
#include <istream>
#include <ostream>

#include "lcm/query/mcp_session.hpp"
#include "lcm/query/query.hpp"
#include "lcm/query/render.hpp"
#include "lcm/source.hpp"

#ifndef LCM_QUERY_VERSION
#define LCM_QUERY_VERSION "0.0.0"
#endif

namespace lcm::query {

namespace {

constexpr const char* kUsage =
    "usage: livecodemap [--db <index.sqlite>] [--profile <key>] [--root <workspace>] [--json] <command> ...\n"
    "\n"
    "  ask <identifier...>       find symbols and files by identifier   [--limit N (8)] [--cursor C]\n"
    "  callers <selector>        callers of a symbol (stable ID or name) [--outgoing] [--depth N (1)] [--all]\n"
    "                            [--limit N (20)] [--node-budget N (20000)] [--time-budget MS (2000)] [--cursor C]\n"
    "  skeleton <path>           API of one source file                 [--limit N (100)] [--cursor C]\n"
    "  check                     read-only freshness check              [--budget MS (2000)]\n"
    "  mcp                       MCP server on stdin/stdout\n"
    "\n"
    "--db defaults to LIVECODEMAP_DB. The database is opened read-only. --json prints the response object.\n";

struct UsageError {
  std::string message;
};

struct Parsed {
  std::optional<std::string> db, profile, root;
  bool json = false;
  std::string command;
  std::vector<std::string> positional;
  std::vector<std::pair<std::string, std::optional<std::string>>> options;  // command options in order
};

const std::vector<std::string>& valued_options() {
  static const std::vector<std::string> v{"--limit", "--cursor", "--depth", "--node-budget", "--time-budget", "--budget"};
  return v;
}

Parsed parse(const std::vector<std::string>& args) {
  Parsed p;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    const auto value = [&]() -> std::string {
      if (i + 1 >= args.size()) throw UsageError{a + " needs a value"};
      return args[++i];
    };
    if (a == "--db") {
      p.db = value();
    } else if (a == "--profile") {
      p.profile = value();
    } else if (a == "--root") {
      p.root = value();
    } else if (a == "--json") {
      p.json = true;
    } else if (a == "--help" || a == "-h") {
      p.command = "help";
    } else if (a.rfind("--", 0) == 0) {
      const bool valued = std::find(valued_options().begin(), valued_options().end(), a) != valued_options().end();
      p.options.emplace_back(a, valued ? std::optional<std::string>(value()) : std::nullopt);
    } else if (p.command.empty()) {
      p.command = a;
    } else {
      p.positional.push_back(a);
    }
  }
  return p;
}

std::int64_t number(const std::string& option, const std::string& text) {
  std::int64_t v = 0;
  const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
  if (ec != std::errc() || ptr != text.data() + text.size()) throw UsageError{option + " needs an integer, got '" + text + "'"};
  return v;
}

std::size_t count(const std::string& option, const std::string& text) {
  const std::int64_t v = number(option, text);
  return v < 0 ? 0 : static_cast<std::size_t>(v);
}

std::string joined(const std::vector<std::string>& words) {
  std::string out;
  for (const auto& w : words) out += (out.empty() ? "" : " ") + w;
  return out;
}

void allow(const Parsed& p, std::initializer_list<const char*> names) {
  for (const auto& [name, value] : p.options) {
    bool ok = false;
    for (const char* n : names) ok = ok || name == n;
    if (!ok) throw UsageError{"unknown option " + name + " for " + p.command};
  }
}

}  // namespace

int run_cli(const std::vector<std::string>& args, const std::optional<std::string>& env_db, std::istream& in,
            std::ostream& out, std::ostream& err) {
  Parsed p;
  try {
    p = parse(args);
    if (p.command.empty() || p.command == "help") {
      (p.command.empty() ? err : out) << kUsage;
      return p.command.empty() ? 2 : 0;
    }
    EngineOptions options;
    if (p.db) {
      options.db = path_from_utf8(*p.db);
    } else if (env_db && !env_db->empty()) {
      options.db = path_from_utf8(*env_db);
    }
    options.profile = p.profile;
    if (p.root) options.root = path_from_utf8(*p.root);
    QueryEngine engine(options);

    Json response;
    if (p.command == "ask") {
      allow(p, {"--limit", "--cursor"});
      FindCodeArgs a;
      a.limit = 8;
      a.query = joined(p.positional);
      for (const auto& [name, value] : p.options) {
        if (name == "--limit") a.limit = count(name, *value);
        if (name == "--cursor") a.cursor = *value;
      }
      response = engine.find_code(a);
    } else if (p.command == "callers") {
      allow(p, {"--outgoing", "--incoming", "--depth", "--all", "--limit", "--node-budget", "--time-budget", "--cursor"});
      TraceArgs a;
      a.selector = joined(p.positional);
      for (const auto& [name, value] : p.options) {
        if (name == "--outgoing") a.direction = Direction::outgoing;
        if (name == "--incoming") a.direction = Direction::incoming;
        if (name == "--depth") a.depth = static_cast<int>(std::clamp<std::int64_t>(number(name, *value), -1, 1'000'000));
        if (name == "--all") a.all = true;
        if (name == "--limit") a.limit = count(name, *value);
        if (name == "--node-budget") a.node_budget = count(name, *value);
        if (name == "--time-budget") a.time_budget = std::chrono::milliseconds(number(name, *value));
        if (name == "--cursor") a.cursor = *value;
      }
      response = engine.trace_calls(a);
    } else if (p.command == "skeleton") {
      allow(p, {"--limit", "--cursor"});
      FileApiArgs a;
      a.path = joined(p.positional);
      for (const auto& [name, value] : p.options) {
        if (name == "--limit") a.limit = count(name, *value);
        if (name == "--cursor") a.cursor = *value;
      }
      response = engine.file_api(a);
    } else if (p.command == "check") {
      allow(p, {"--budget"});
      if (!p.positional.empty()) throw UsageError{"check takes no arguments"};
      CheckArgs a;
      for (const auto& [name, value] : p.options) {
        if (name == "--budget") a.budget = std::chrono::milliseconds(number(name, *value));
      }
      response = engine.check_freshness(a);
    } else if (p.command == "mcp") {
      allow(p, {});
      if (!p.positional.empty()) throw UsageError{"mcp takes no arguments"};
      McpSession session(engine, LCM_QUERY_VERSION);
      session.serve(in, out);
      return 0;
    } else {
      throw UsageError{"unknown command '" + p.command + "'"};
    }
    if (p.json) {
      out << response.dump(2, ' ', false, Json::error_handler_t::replace) << '\n';
    } else {
      out << render_text(response);
    }
    out.flush();
    return is_error(response) ? 1 : 0;
  } catch (const UsageError& e) {
    err << "livecodemap: " << e.message << "\n\n" << kUsage;
    return 2;
  }
}

}  // namespace lcm::query
