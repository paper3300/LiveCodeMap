#include "lcm/query/render.hpp"

namespace lcm::query {

namespace {

std::string str(const Json& j) {
  if (j.is_string()) return j.get<std::string>();
  if (j.is_null()) return "-";
  return j.dump();
}

std::string where(const Json& location) {
  if (!location.is_object()) return "(no location)";
  return location["path"].get<std::string>() + ":" + std::to_string(location["span"]["begin_line"].get<std::uint64_t>()) +
         ":" + std::to_string(location["span"]["begin_column"].get<std::uint64_t>());
}

std::string symbol_line(const Json& s) {
  std::string line = s["qualified_name"].get<std::string>();
  const std::string signature = s["signature"].get<std::string>();
  if (!signature.empty()) line += signature;
  line += "  [" + s["kind"].get<std::string>();
  if (s["presence"] != "indexed") line += ", " + s["presence"].get<std::string>();
  line += "]  " + where(s["primary_location"]) + "  " + s["id"].get<std::string>();
  return line;
}

void render_find(const Json& r, std::string& out) {
  for (const auto& hit : r["results"]) {
    out += "  " + std::to_string(hit["tier"].get<int>()) + " " + hit["match"].get<std::string>() + "  ";
    out += hit["kind"] == "file" ? "file " + hit["file"]["path"].get<std::string>() : symbol_line(hit["symbol"]);
    out += "\n";
  }
}

void render_sites(const Json& sites, const char* label, std::string& out) {
  for (const auto& s : sites) {
    out += std::string("      ") + label + " " + where(s["location"]) + "  (" + s["dispatch"].get<std::string>() + ", " +
           std::to_string(s["units"].size()) + " unit(s))\n";
  }
}

void render_trace(const Json& r, std::string& out) {
  if (r.contains("candidates")) {
    out += "  candidates for '" + r["selector"].get<std::string>() + "':\n";
    for (const auto& c : r["candidates"]) out += "    " + symbol_line(c) + "\n";
    return;
  }
  out += "  " + r["direction"].get<std::string>() + " of " + symbol_line(r["selected"]) + "\n";
  const Json& counts = r["counts"];
  for (const auto& [k, v] : counts.items()) {
    if (k != "exact") out += "  " + k + ": " + str(v) + (counts["exact"].get<bool>() ? "" : " (discovered so far)") + "\n";
  }
  for (const auto& rec : r["records"]) {
    out += "  hop " + std::to_string(rec["hop"].get<int>()) + "  " + symbol_line(rec["symbol"]) + "  (path " +
           rec["path_confidence"].get<std::string>() + ")\n";
    render_sites(rec["confirmed"], "confirmed", out);
    render_sites(rec["possible"], "possible ", out);
  }
  if (r.contains("unresolved")) {
    for (const auto& u : r["unresolved"]) {
      out += "  unresolved " + u["expression"].get<std::string>() + "  " + where(u["location"]) + "  (" +
             u["reason"].get<std::string>() + ")\n";
    }
  }
}

void render_file(const Json& r, std::string& out) {
  out += "  file " + r["file"]["path"].get<std::string>() + "  (module: " + r["module_reason"].get<std::string>() + ")\n";
  for (const auto& inc : r["includes"]) {
    out += "  #" + inc["directive"].get<std::string>() + " " + inc["operand"].get<std::string>() + "  line " +
           std::to_string(inc["location"]["span"]["begin_line"].get<std::uint64_t>()) + "  " +
           inc["activity"].get<std::string>() + ", " + inc["resolution"].get<std::string>();
    if (!inc["target"].is_null()) out += " -> " + inc["target"].get<std::string>();
    out += "\n";
  }
  for (const auto& e : r["entries"]) {
    out += "  " + symbol_line(e["symbol"]) + "\n";
    for (const auto& h : e["here"]) out += "      here      " + h["role"].get<std::string>() + " " + where(h) + "\n";
    for (const auto& o : e["elsewhere"]) out += "      elsewhere " + o["role"].get<std::string>() + " " + where(o) + "\n";
  }
}

void render_check(const Json& r, std::string& out) {
  out += "  outcome " + r["outcome"].get<std::string>() + " in " + str(r["elapsed_ms"]) + " ms (budget " +
         str(r["budget_ms"]) + " ms); inputs checked " + str(r["inputs_checked"]) + " of " + str(r["inputs_total"]) +
         "; nothing persisted\n";
  for (const auto& [k, v] : r["live"].items()) out += "  live " + k + ": " + str(v) + "\n";
  out += "  units: " + r["units"].dump() + "\n";
  constexpr std::size_t kShownChanges = 20;  // the JSON response lists every change
  std::size_t shown = 0;
  for (const auto& c : r["changes"]) {
    if (shown++ == kShownChanges) {
      out += "  ... " + std::to_string(r["changes"].size() - kShownChanges) + " more change(s); see --json\n";
      break;
    }
    out += "  " + c["change"].get<std::string>() + " " + c["path"].get<std::string>() + " (" + c["role"].get<std::string>() +
           ", " + str(c["units"]) + " unit(s))\n";
  }
  if (!r["guidance"].get<std::string>().empty()) out += "  " + r["guidance"].get<std::string>() + "\n";
}

}  // namespace

std::string render_text(const Json& response) {
  std::string out = response["tool"].get<std::string>() + ": " + response["status"].get<std::string>();
  if (!response["profile"].is_null()) {
    out += "  profile " + response["profile"].get<std::string>() + "  generation " + str(response["fact_generation"]);
  }
  out += "\n";
  for (const auto& d : response["diagnostics"]) {
    out += "  ! " + d["code"].get<std::string>() + ": " + d["message"].get<std::string>() + "\n";
  }
  const Json& result = response["result"];
  if (result.is_object()) {
    const std::string tool = response["tool"].get<std::string>();
    if (tool == "find_code") render_find(result, out);
    if (tool == "trace_calls") render_trace(result, out);
    if (tool == "file_api") render_file(result, out);
    if (tool == "check_freshness") render_check(result, out);
  }
  const Json& p = response["pagination"];
  if (response["status"] != "error" || (result.is_object() && result.contains("candidates"))) {
    out += "  shown " + str(p["shown"]) + ", returned " + str(p["returned_count"]) + " of " +
           (p["complete"].get<bool>() ? str(p["total"]) : "? (" + str(p["discovered"]) + " discovered so far)");
    if (!p["limit_reason"].is_null()) out += "; limited by " + str(p["limit_reason"]);
    out += "\n";
    if (!p["next_cursor"].is_null()) out += "  next page: --cursor " + p["next_cursor"].get<std::string>() + "\n";
  }
  return out;
}

}  // namespace lcm::query
