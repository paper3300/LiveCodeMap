#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include "lcm/query/mcp_session.hpp"
#include "lcm/query/query.hpp"
#include "query_fixture.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

using namespace lcm;
using namespace lcm::query;
using namespace lcm::query::test;
namespace fs = std::filesystem;

namespace {

fs::path scratch(const std::string& name) {
  static std::atomic<int> counter{0};
  const fs::path p = path_from_utf8(LCM_TEST_SCRATCH_DIR) /
                     path_from_utf8("mcp-" + name + "-" +
                                    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                                    std::to_string(counter++));
  fs::create_directories(p);
  return p;
}

Json reply(McpSession& s, const Json& request) {
  const auto line = s.handle(request.dump());
  REQUIRE(line.has_value());
  CHECK(line->find('\n') == std::string::npos);
  return Json::parse(*line);
}

Json call(McpSession& s, int id, const std::string& tool, Json arguments) {
  return reply(s, Json{{"jsonrpc", "2.0"},
                       {"id", id},
                       {"method", "tools/call"},
                       {"params", Json{{"name", tool}, {"arguments", std::move(arguments)}}}});
}

int error_code(const Json& r) { return r["error"]["code"].get<int>(); }

std::string read_all(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string digest(const fs::path& p) {
  std::error_code ec;
  return fs::exists(p, ec) ? sha256(read_all(p)).hex() : "absent";
}

#ifdef _WIN32
// Runs livecodemap.exe with stdin/stdout/stderr redirected to files; returns the exit code.
int run_exe(const std::vector<std::string>& args, const fs::path& in, const fs::path& out, const fs::path& err) {
  std::wstring command = L"\"" + path_from_utf8(LCM_LIVECODEMAP_EXE).wstring() + L"\"";
  for (const auto& a : args) command += L" \"" + path_from_utf8(a).wstring() + L"\"";
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE hin = CreateFileW(in.c_str(), GENERIC_READ, FILE_SHARE_READ, &sa, OPEN_EXISTING, 0, nullptr);
  HANDLE hout = CreateFileW(out.c_str(), GENERIC_WRITE, 0, &sa, CREATE_ALWAYS, 0, nullptr);
  HANDLE herr = CreateFileW(err.c_str(), GENERIC_WRITE, 0, &sa, CREATE_ALWAYS, 0, nullptr);
  REQUIRE(hin != INVALID_HANDLE_VALUE);
  REQUIRE(hout != INVALID_HANDLE_VALUE);
  REQUIRE(herr != INVALID_HANDLE_VALUE);
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = hin;
  si.hStdOutput = hout;
  si.hStdError = herr;
  PROCESS_INFORMATION pi{};
  std::vector<wchar_t> buffer(command.begin(), command.end());
  buffer.push_back(L'\0');
  const BOOL ok = CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  CloseHandle(hin);
  CloseHandle(hout);
  CloseHandle(herr);
  REQUIRE(ok);
  WaitForSingleObject(pi.hProcess, 60000);
  DWORD code = 0;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  return static_cast<int>(code);
}
#endif

}  // namespace

TEST_CASE("mcp: initialize negotiates the protocol revision; notifications get no response") {
  QueryEngine engine{EngineOptions{}};
  McpSession s(engine, "9.9.9");
  for (const char* v : {"2025-11-25", "2025-06-18", "2025-03-26"}) {
    const Json r = reply(s, Json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
                                 {"params", Json{{"protocolVersion", v}, {"capabilities", Json::object()},
                                                 {"clientInfo", Json{{"name", "t"}, {"version", "1"}}}}}});
    CHECK(r["jsonrpc"] == "2.0");
    CHECK(r["id"] == 1);
    CHECK(r["result"]["protocolVersion"] == v);
    CHECK(r["result"]["capabilities"] == Json{{"tools", Json::object()}});
    CHECK(r["result"]["serverInfo"]["name"] == "livecodemap");
    CHECK(r["result"]["serverInfo"]["version"] == "9.9.9");
    CHECK(s.protocol_version() == v);
  }
  for (const Json& p : {Json{{"protocolVersion", "2024-11-05"}}, Json{{"protocolVersion", "next"}}, Json::object()}) {
    const Json r = reply(s, Json{{"jsonrpc", "2.0"}, {"id", "init"}, {"method", "initialize"}, {"params", p}});
    CHECK(r["id"] == "init");
    CHECK(r["result"]["protocolVersion"] == "2025-11-25");
  }
  // No valid notification gets a response, known or not.
  CHECK_FALSE(s.handle(R"({"jsonrpc":"2.0","method":"notifications/initialized"})").has_value());
  CHECK_FALSE(s.handle(R"({"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":3}})").has_value());
  CHECK_FALSE(s.handle(R"({"jsonrpc":"2.0","method":"tools/call","params":{"name":"find_code"}})").has_value());
  CHECK(reply(s, Json{{"jsonrpc", "2.0"}, {"id", 7}, {"method", "ping"}})["result"] == Json::object());
}

TEST_CASE("mcp: JSON-RPC error codes and id preservation") {
  QueryEngine engine{EngineOptions{}};
  McpSession s(engine, "1");
  const auto raw = [&](const std::string& line) { return Json::parse(*s.handle(line)); };
  Json r = raw("{not json");
  CHECK(error_code(r) == -32700);
  CHECK(r["id"].is_null());
  CHECK(error_code(raw(R"([{"jsonrpc":"2.0","id":1,"method":"ping"}])")) == -32600);  // batch
  CHECK(error_code(raw("42")) == -32600);
  CHECK(error_code(raw(R"({"jsonrpc":"1.0","id":1,"method":"ping"})")) == -32600);
  CHECK(error_code(raw(R"({"jsonrpc":"2.0","id":1})")) == -32600);
  CHECK(error_code(raw(R"({"jsonrpc":"2.0","id":{"x":1},"method":"ping"})")) == -32600);
  r = raw(R"({"jsonrpc":"2.0","id":"abc","method":"resources/list"})");
  CHECK(error_code(r) == -32601);
  CHECK(r["id"] == "abc");
  r = raw(R"({"jsonrpc":"2.0","id":12345678901,"method":"tools/call","params":{"name":"nope","arguments":{}}})");
  CHECK(error_code(r) == -32602);
  CHECK(r["id"] == 12345678901LL);
  CHECK(error_code(raw(R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"arguments":{}}})")) == -32602);
  CHECK(error_code(raw(R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"find_code","arguments":[]}})")) == -32602);
  CHECK(error_code(call(s, 3, "find_code", Json::object())) == -32602);                          // missing query
  CHECK(error_code(call(s, 3, "find_code", Json{{"query", 5}})) == -32602);                       // wrong type
  CHECK(error_code(call(s, 3, "find_code", Json{{"query", "x"}, {"limit", "5"}})) == -32602);     // wrong type
  CHECK(error_code(call(s, 3, "find_code", Json{{"query", "x"}, {"bogus", 1}})) == -32602);       // unknown argument
  CHECK(error_code(call(s, 3, "trace_calls", Json{{"selector", "x"}, {"direction", "up"}})) == -32602);
  CHECK(error_code(call(s, 3, "trace_calls", Json{{"selector", "x"}, {"all", 1}})) == -32602);
  CHECK(error_code(call(s, 3, "check_freshness", Json{{"budget_ms", 1.5}})) == -32602);
}

TEST_CASE("mcp: tools/list exposes exactly the four tools with object input schemas") {
  QueryEngine engine{EngineOptions{}};
  McpSession s(engine, "1");
  const Json r = reply(s, Json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}});
  const Json& tools = r["result"]["tools"];
  REQUIRE(tools.size() == 4);
  std::vector<std::string> names;
  for (const auto& t : tools) {
    names.push_back(t["name"].get<std::string>());
    CHECK(t["description"].get<std::string>().size() > 80);
    CHECK(t["inputSchema"]["type"] == "object");
    CHECK(t["inputSchema"]["properties"].is_object());
  }
  CHECK(names == std::vector<std::string>{"find_code", "trace_calls", "file_api", "check_freshness"});
  CHECK(tools[0]["inputSchema"]["properties"]["limit"]["default"] == 5);
  CHECK(tools[0]["inputSchema"]["required"] == Json::array({"query"}));
  CHECK(tools[1]["inputSchema"]["properties"]["limit"]["default"] == 20);
  CHECK(tools[1]["inputSchema"]["properties"]["node_budget"]["default"] == 20000);
  CHECK(tools[1]["inputSchema"]["properties"]["time_budget_ms"]["default"] == 2000);
  CHECK(tools[1]["inputSchema"]["properties"]["depth"]["maximum"] == 5);
  CHECK(tools[2]["inputSchema"]["properties"]["limit"]["default"] == 100);
  CHECK(tools[3]["inputSchema"]["properties"]["budget_ms"]["default"] == 2000);
}

TEST_CASE("mcp: tools/call returns the response model as structured content and text; tool errors set isError") {
  const QueryFx fx = QueryFx::create(scratch("calls"));
  EngineOptions o;
  o.db = fx.db;
  QueryEngine engine(o);
  McpSession s(engine, "1");
  QueryEngine direct(o);

  Json r = call(s, 1, "find_code", Json{{"query", "run_A"}});
  REQUIRE(r.contains("result"));
  const Json& res = r["result"];
  CHECK(res["isError"] == false);
  REQUIRE(res["content"].size() == 2);
  CHECK(res["content"][0]["type"] == "text");
  CHECK(Json::parse(res["content"][0]["text"].get<std::string>()) == res["structuredContent"]);
  CHECK(res["content"][1]["text"].get<std::string>().find("find_code: success") == 0);
  FindCodeArgs a;
  a.query = "run_A";  // MCP default limit 5
  CHECK(res["structuredContent"] == direct.find_code(a));

  // Many calls in one session, each on its own snapshot; ids preserved.
  for (int i = 0; i < 20; ++i) {
    const Json t = call(s, 100 + i, "trace_calls", Json{{"selector", "geo::Shape::area"}, {"limit", 1}});
    CHECK(t["id"] == 100 + i);
    CHECK(t["result"]["structuredContent"]["status"] == "success");
  }
  const Json page = call(s, 2, "find_code", Json{{"query", "f"}, {"limit", 5}});
  const std::string cursor = page["result"]["structuredContent"]["pagination"]["next_cursor"].get<std::string>();
  const Json next = call(s, 3, "find_code", Json{{"query", "f"}, {"limit", 5}, {"cursor", cursor}});
  CHECK(next["result"]["structuredContent"]["pagination"]["returned_count"] == 10);
  CHECK(call(s, 4, "file_api", Json{{"path", "include/shapes.h"}})["result"]["structuredContent"]["pagination"]["shown"] == 20);
  CHECK(call(s, 5, "check_freshness", Json::object())["result"]["isError"] == false);
  CHECK(call(s, 6, "trace_calls", Json{{"selector", "run_A"}, {"direction", "outgoing"}, {"depth", 2}, {"all", false},
                                       {"node_budget", 10}, {"time_budget_ms", 500}})["result"]["isError"] == false);

  // Tool-level errors are results with isError, not JSON-RPC errors.
  for (const Json& bad : {call(s, 7, "find_code", Json{{"query", ""}}), call(s, 8, "trace_calls", Json{{"selector", "dup"}}),
                          call(s, 9, "find_code", Json{{"query", "f"}, {"cursor", "garbage"}}),
                          call(s, 10, "find_code", Json{{"query", "f"}, {"limit", 0}}),
                          call(s, 11, "file_api", Json{{"path", "../x"}})}) {
    REQUIRE(bad.contains("result"));
    CHECK(bad["result"]["isError"] == true);
    CHECK(bad["result"]["structuredContent"]["status"] == "error");
  }
  QueryEngine missing{EngineOptions{}};
  McpSession no_db(missing, "1");
  const Json m = call(no_db, 1, "find_code", Json{{"query", "x"}});
  CHECK(m["result"]["isError"] == true);
  CHECK(m["result"]["structuredContent"]["diagnostics"][0]["code"] == "db_missing");
}

TEST_CASE("mcp: serve reads newline-delimited requests and writes one JSON object per line") {
  const QueryFx fx = QueryFx::create(scratch("serve"));
  EngineOptions o;
  o.db = fx.db;
  QueryEngine engine(o);
  McpSession s(engine, "1");
  std::istringstream in(
      "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-06-18\"}}\r\n"
      "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"
      "\n"
      "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"find_code\",\"arguments\":{\"query\":\"조회_함수\"}}}\n"
      "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"ping\"}\n");
  std::ostringstream out;
  s.serve(in, out);
  std::istringstream lines(out.str());
  std::vector<Json> replies;
  for (std::string line; std::getline(lines, line);) replies.push_back(Json::parse(line));
  REQUIRE(replies.size() == 3);
  CHECK(replies[0]["result"]["protocolVersion"] == "2025-06-18");
  CHECK(replies[1]["id"] == 2);
  CHECK(out.str().find("src/한글.cpp") != std::string::npos);  // UTF-8 kept raw
  CHECK(replies[2]["id"] == 3);
}

#ifdef _WIN32
TEST_CASE("livecodemap.exe: CLI JSON and an MCP session over pipes; the database bytes are unchanged") {
  const fs::path dir = scratch("exe");
  const QueryFx fx = QueryFx::create(dir);
  const std::string db = path_to_utf8_generic(fx.db);
  const std::string before = digest(fx.db);
  write_text(dir / "empty.txt", "");

  CHECK(run_exe({"--db", db, "--json", "ask", "run_A"}, dir / "empty.txt", dir / "out.json", dir / "err.txt") == 0);
  const Json cli = Json::parse(read_all(dir / "out.json"));
  CHECK(cli["status"] == "success");
  CHECK(cli["result"]["results"][0]["symbol"]["qualified_name"] == "run_A");
  CHECK(run_exe({"--db", db, "--json", "callers", "dup"}, dir / "empty.txt", dir / "out2.json", dir / "err2.txt") == 1);
  CHECK(run_exe({"--db", db, "nonsense"}, dir / "empty.txt", dir / "out3.txt", dir / "err3.txt") == 2);
  CHECK(read_all(dir / "out3.txt").empty());  // usage goes to stderr
  CHECK(run_exe({"--db", db, "--json", "skeleton", "src/한글.cpp"}, dir / "empty.txt", dir / "out4.json", dir / "err4.txt") == 0);
  CHECK(Json::parse(read_all(dir / "out4.json"))["result"]["file"]["path"] == "src/한글.cpp");

  write_text(dir / "session.jsonl",
             "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-11-25\"}}\n"
             "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"
             "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}\n"
             "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"trace_calls\",\"arguments\":{\"selector\":\"geo::Circle::draw\"}}}\n"
             "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":{\"name\":\"file_api\",\"arguments\":{\"path\":\"include/shapes.h\",\"limit\":2}}}\n"
             "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\",\"params\":{\"name\":\"check_freshness\",\"arguments\":{}}}\n"
             "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"bogus\"}\n");
  CHECK(run_exe({"--db", db, "mcp"}, dir / "session.jsonl", dir / "session.out", dir / "session.err") == 0);
  const std::string out = read_all(dir / "session.out");
  CHECK(out.find('\r') == std::string::npos);  // binary stdout: LF-only protocol lines
  std::istringstream lines(out);
  std::vector<Json> replies;
  for (std::string line; std::getline(lines, line);) replies.push_back(Json::parse(line));
  REQUIRE(replies.size() == 6);
  CHECK(replies[0]["result"]["protocolVersion"] == "2025-11-25");
  CHECK(replies[1]["result"]["tools"].size() == 4);
  CHECK(replies[2]["result"]["structuredContent"]["result"]["records"].size() == 2);
  CHECK(replies[3]["result"]["structuredContent"]["pagination"]["next_cursor"].is_string());
  CHECK(replies[4]["result"]["isError"] == false);
  CHECK(replies[5]["error"]["code"] == -32601);
  CHECK(read_all(dir / "session.err").empty());  // nothing but protocol on stdout, no noise on stderr
  CHECK(digest(fx.db) == before);
}
#endif
