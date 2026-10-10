// `livecodemap` command line: ask, callers, skeleton, check and mcp over the
// query engine. Testable without a process; tools/livecodemap/main.cpp only
// converts the Windows command line and standard streams.
#pragma once

#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace lcm::query {

// `args` excludes the program name. `env_db` is LIVECODEMAP_DB when set.
// Exit code: 0 for success/partial/stale, 1 for an error response, 2 for a
// usage error.
int run_cli(const std::vector<std::string>& args, const std::optional<std::string>& env_db, std::istream& in,
            std::ostream& out, std::ostream& err);

}  // namespace lcm::query
