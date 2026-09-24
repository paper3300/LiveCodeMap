// Minimal single-TU replay tool (stage 4).
//
// Runs ONE translation unit through the public analysis API and writes a
// durable JSON report of everything that decided the outcome: the raw command
// exactly as given, the response files that were read and their hashes, the
// transformation each argument received, the identities, the PCH replay verdict,
// diagnostics, limits, timing and optional structural checks.
//
// It performs no repair of its own. There is no manual token deletion and no
// compiler-name substitution: whatever the command says is what is analysed, and
// if the public contract rejects it the tool reports why and exits non-zero.
#include <chrono>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "lcm/analyzer/analyzer.hpp"
#include "lcm/facts.hpp"
#include "lcm/identity.hpp"
#include "lcm/compile_db.hpp"
#include "lcm/response_file.hpp"
#include "lcm/source.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace lcm;
using namespace lcm::analyzer;

namespace {

// Distinct exit codes so an automated check can tell the failures apart.
enum ExitCode : int {
  exit_ok = 0,
  exit_usage = 2,
  exit_bad_input = 3,
  exit_rejected_context = 4,
  exit_rejected_pch = 5,
  exit_completed_with_errors = 6,
  exit_structure_mismatch = 7,
  exit_report_write_failed = 8,
};

struct Options {
  fs::path repository_root;
  fs::path resource_dir;
  fs::path directory;
  fs::path source;
  std::vector<std::string> arguments;
  fs::path producer_source;
  std::vector<std::string> producer_arguments;
  fs::path out;
  std::string expect_type;
  long long expect_direct_bases = -1;
  long long expect_virtual_bases = -1;
  long long expect_direct_includes = -1;
  long long evidence_limit = 200;
};

void usage() {
  std::cerr << "usage: lcm_replay --repo <dir> --resource-dir <dir> --directory <dir> --source <file>\n"
               "                  --arg <token> [--arg <token> ...]\n"
               "                  [--producer-source <file> --producer-arg <token> ...]\n"
               "                  --out <report.json>\n"
               "                  [--expect-type <name>] [--expect-direct-bases <n>]\n"
               "                  [--expect-virtual-bases <n>] [--expect-direct-includes <n>]\n"
               "                  [--evidence-limit <n>]   (0 = unlimited)\n";
}

CompileCommand build(const fs::path& directory, const fs::path& source, std::vector<std::string> argv) {
  CompileCommand c;
  c.directory = fs::absolute(directory).lexically_normal();
  c.file = fs::absolute(source).lexically_normal();
  c.directory_as_written = path_to_utf8_generic(c.directory);
  c.file_as_written = path_to_utf8_generic(c.file);
  c.arguments = std::move(argv);
  c.command_id = compute_command_id(c);
  return c;
}

json command_json(const CompileCommand& command) {
  json j;
  j["directory"] = path_to_utf8_generic(command.directory);
  j["file"] = path_to_utf8_generic(command.file);
  j["command_id"] = command.command_id;
  j["raw_arguments"] = command.arguments;  // preserved verbatim, never edited
  return j;
}

json expansion_json(const ResponseExpansion& e) {
  json j;
  j["attempted"] = e.attempted;
  j["ok"] = e.ok;
  j["errors"] = e.errors;
  j["replay_id"] = e.replay_id;
  j["expanded_argument_count"] = e.arguments.size();
  j["expanded_arguments"] = e.arguments;
  json files = json::array();
  for (const auto& f : e.files) {
    files.push_back({{"as_written", f.as_written},
                     {"resolved", f.resolved_utf8},
                     {"alias_spellings", f.alias_spellings},
                     {"encoding", std::string(to_string(f.encoding))},
                     {"bytes", f.bytes},
                     {"sha256", f.content_sha256},
                     {"first_depth", f.first_depth}});
  }
  j["response_files"] = files;
  json origins = json::array();
  for (std::size_t i = 0; i < e.origins.size(); ++i) {
    const auto& o = e.origins[i];
    json entry{{"token_index", i}, {"raw_index", o.raw_index}, {"from_response_file", o.from_response_file}};
    if (o.from_response_file) {
      entry["response_file"] = e.files[o.snapshot].resolved_utf8;
      entry["index_in_file"] = o.index_in_file;
    }
    origins.push_back(std::move(entry));
  }
  j["token_origins"] = origins;
  return j;
}

json capture_json(const PchPrefixCapture& c) {
  return json{{"event_digest", c.event_digest}, {"token_digest", c.token_digest},
              {"macro_digest", c.macro_digest}, {"file_digest", c.file_digest},
              {"counter", c.counter},           {"events", c.events},
              {"files", c.files},               {"window_seen", c.window_seen},
              {"boundary_reached", c.boundary_reached}, {"diagnostics", c.diagnostics}};
}

json pch_json(const PchReplay& p) {
  json j;
  j["mode"] = std::string(to_string(p.mode));
  j["reject_reason"] = p.reject_reason;
  j["producer_command_id"] = p.producer_command_id;
  j["producer_replay_id"] = p.producer_replay_id;
  j["consumer_command_id"] = p.consumer_command_id;
  j["wrapper_header"] = p.wrapper_header;
  j["producer_wrapper_include"] = p.producer_wrapper_include;
  j["pch_binary_recorded_never_consumed"] = p.pch_binary;
  j["order_guard_virtual_path"] = p.guard_path;
  j["prefix_identity"] = p.prefix_identity;
  j["prefix_file_count"] = p.prefix_files.size();
  j["prefix_files"] = p.prefix_files;
  j["mismatches"] = p.mismatches;
  j["producer_capture"] = capture_json(p.producer);
  j["consumer_capture"] = capture_json(p.consumer);
  j["final_parse_capture"] = capture_json(p.final_parse);
  j["final_prefix_axes_compared"] = p.final_axes_compared;
  j["final_prefix_drift"] = p.final_drift;
  j["replay_arguments"] = p.replay_arguments;
  j["claim"] =
      "existing-input textual snapshot: equivalence of the producer and consumer prefixes as they are on disk "
      "now. No claim about the historical binary precompiled header, and none about input currency.";
  return j;
}

json dispositions_json(const NormalizedCompileContext& context) {
  json out = json::array();
  for (const auto& d : context.dispositions) {
    out.push_back({{"index", d.index},
                   {"raw", d.raw},
                   {"action", std::string(to_string(d.action))},
                   {"category", std::string(to_string(d.category))},
                   {"reason", d.reason},
                   {"replacement", d.replacement}});
  }
  return out;
}


json span_json(const SourceSpan& span) {
  return json{{"begin_line", span.begin_line}, {"begin_column", span.begin_column},
              {"end_line", span.end_line},     {"end_column", span.end_column},
              {"begin_offset", span.begin_offset}, {"end_offset", span.end_offset}};
}

json location_json(const SourceLocation& location) {
  return json{{"file", location.file.generic},
              {"content_sha256", location.content_hash.digest.hex()},
              {"content_bytes", location.content_hash.size},
              {"span", span_json(location.span)},
              {"span_sha256", location.span_hash ? location.span_hash->hex() : std::string()}};
}

std::string qualified_name(const CanonicalKey& key) {
  std::string out;
  for (const OwnerComponent& owner : key.owner_chain) {
    if (!owner.name.empty()) out += owner.name + "::";
  }
  return out + key.canonical_name;
}

json symbol_ref_json(const FactSet& facts, StableId id) {
  const auto it = facts.symbols().find(id);
  if (it == facts.symbols().end()) return json{{"unknown_symbol", true}};
  const Symbol& symbol = it->second;
  json out{{"qualified_name", qualified_name(symbol.key)},
           {"canonical_name", symbol.key.canonical_name},
           {"kind", std::string(to_string(symbol.key.kind))},
           {"normalized_signature", symbol.key.normalized_signature},
           {"presence", std::string(to_string(symbol.presence))}};
  json definitions = json::array();
  for (const SymbolLocation& location : symbol.locations) {
    if (location.role != LocationRole::definition) continue;
    definitions.push_back(location_json(location.location));
  }
  out["definitions"] = definitions;
  return out;
}

// Everything a reviewer needs to check the unit against its ORIGINAL source,
// exported generically: no expectation and no project-specific fact is compiled
// in. Scoped to the unit's own main file so it stays bounded.
json fact_evidence_json(const AnalysisResult& result, const std::optional<RepoRelativePath>& main_file,
                        std::size_t limit, const std::string& guard_path) {
  const auto capped = [limit](const json& array) { return limit != 0 && array.size() >= limit; };
  bool truncated = false;
  json evidence;
  evidence["main_file"] = main_file ? main_file->generic : std::string();

  json symbols = json::array();
  json bases_by_type = json::array();
  for (const auto& [id, symbol] : result.facts.symbols()) {
    bool in_main = false;
    json locations = json::array();
    for (const SymbolLocation& location : symbol.locations) {
      if (!main_file || location.location.file != *main_file) continue;
      in_main = true;
      json entry = location_json(location.location);
      entry["role"] = std::string(to_string(location.role));
      entry["body_sha256"] = location.body_hash ? location.body_hash->hex() : std::string();
      locations.push_back(std::move(entry));
    }
    if (!in_main) continue;
    if (capped(symbols)) { truncated = true; continue; }
    json owner = json::array();
    for (const OwnerComponent& o : symbol.key.owner_chain) owner.push_back(o.name);
    symbols.push_back({{"qualified_name", qualified_name(symbol.key)},
                       {"canonical_name", symbol.key.canonical_name},
                       {"kind", std::string(to_string(symbol.key.kind))},
                       {"owner_chain", owner},
                       {"normalized_signature", symbol.key.normalized_signature},
                       {"locations", locations}});
  }

  // Direct bases, grouped per type and ORDERED by the written base list, with
  // the access actually spelled kept separate from the effective access.
  std::map<std::string, std::vector<json>> bases;
  for (const auto& [key, relation] : result.facts.relations()) {
    if (key.kind != RelationKind::extends) continue;
    for (const Evidence& evidence_entry : relation.evidence) {
      if (!evidence_entry.base) continue;
      const auto source = result.facts.symbols().find(key.source);
      if (source == result.facts.symbols().end()) continue;
      json entry{{"lexical_ordinal", evidence_entry.base->lexical_ordinal},
                 {"effective_access", std::string(to_string(evidence_entry.base->effective_access))},
                 {"access_written", evidence_entry.base->access_written},
                 {"is_virtual", evidence_entry.base->is_virtual},
                 {"confidence", std::string(to_string(key.confidence))},
                 {"subobject_spelling", evidence_entry.subobject},
                 {"base", symbol_ref_json(result.facts, key.target)},
                 {"evidence_location", location_json(evidence_entry.location)}};
      bases[qualified_name(source->second.key)].push_back(std::move(entry));
    }
  }
  for (auto& [type, list] : bases) {
    std::sort(list.begin(), list.end(), [](const json& a, const json& b) {
      return a["lexical_ordinal"].get<std::uint32_t>() < b["lexical_ordinal"].get<std::uint32_t>();
    });
    bases_by_type.push_back({{"type", type}, {"direct_bases", list}});
  }

  json calls = json::array();
  json unresolved = json::array();
  for (const auto& [key, relation] : result.facts.relations()) {
    if (key.kind != RelationKind::calls) continue;
    for (const Evidence& evidence_entry : relation.evidence) {
      if (!main_file || evidence_entry.location.file != *main_file) continue;
      if (capped(calls)) { truncated = true; continue; }
      calls.push_back({{"caller", symbol_ref_json(result.facts, key.source)},
                       {"callee", symbol_ref_json(result.facts, key.target)},
                       {"dispatch", std::string(to_string(evidence_entry.dispatch))},
                       {"evaluation", std::string(to_string(evidence_entry.evaluation))},
                       {"confidence", std::string(to_string(key.confidence))},
                       {"call_location", location_json(evidence_entry.location)}});
    }
  }
  for (const UnresolvedSite& site : result.facts.unresolved_sites()) {
    if (main_file && site.location.file != *main_file) continue;
    if (capped(unresolved)) { truncated = true; continue; }
    unresolved.push_back({{"enclosing", symbol_ref_json(result.facts, site.enclosing)},
                          {"expression", site.expression},
                          {"reason", std::string(to_string(site.reason))},
                          {"location", location_json(site.location)}});
  }

  json includes = json::array();
  for (const DirectIncludeFact& include : result.facts.direct_includes()) {
    if (main_file && include.includer != *main_file) continue;
    if (capped(includes)) { truncated = true; continue; }
    json conditions = json::array();
    for (const ConditionalBranch& branch : include.condition_path) {
      conditions.push_back({{"is_else", branch.is_else},
                            {"has_own_condition", branch.own_condition.has_value()},
                            {"preceding_branches", branch.preceding_branches.size()}});
    }
    includes.push_back({{"operand_as_written", include.operand_as_written},
                        {"directive_kind", std::string(to_string(include.directive_kind))},
                        {"operand_kind", std::string(to_string(include.operand_kind))},
                        {"activity", std::string(to_string(include.activity))},
                        {"resolution", std::string(to_string(include.resolution))},
                        {"resolved_repo_target",
                         include.resolved_repo_target ? include.resolved_repo_target->generic : std::string()},
                        {"external_dependency_key", include.external_dependency_key},
                        {"directive_location", location_json(include.directive_location)},
                        {"condition_path", conditions}});
  }
  std::sort(includes.begin(), includes.end(), [](const json& a, const json& b) {
    return a["directive_location"]["span"]["begin_offset"].get<std::uint32_t>() <
           b["directive_location"]["span"]["begin_offset"].get<std::uint32_t>();
  });

  // The order guard is the only synthetic input; it is named rather than
  // filtered away, so a reviewer discounts exactly it and nothing else.
  json synthetic = json::array();
  if (!guard_path.empty()) {
    synthetic.push_back({{"path", guard_path}, {"role", "pch_order_guard"}, {"bytes", 0},
                         {"note", "empty in-memory header; keeps the /Yu wrapper out of first /FI position"}});
  }

  evidence["symbols_in_main"] = symbols;
  evidence["bases"] = bases_by_type;
  evidence["calls_from_main"] = calls;
  evidence["direct_includes_from_main"] = includes;
  evidence["direct_include_count"] = includes.size();
  evidence["unresolved_in_main"] = unresolved;
  evidence["synthetic_inputs"] = synthetic;
  evidence["evidence_limit"] = limit;
  evidence["truncated"] = truncated;
  return evidence;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  // A malformed numeric value is a usage error with a message, never an
  // uncaught exception that loses the reason.
  const auto number = [](const std::string& flag, const std::string& text) -> long long {
    try {
      std::size_t consumed = 0;
      const long long value = std::stoll(text, &consumed);
      if (consumed != text.size() || value < 0) throw std::invalid_argument("");
      return value;
    } catch (const std::exception&) {
      std::cerr << flag << " expects a non-negative integer, got '" << text << "'\n";
      std::exit(exit_bad_input);
    }
  };
  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    auto next = [&](const char* what) -> std::string {
      if (i + 1 >= argc) {
        std::cerr << "missing value for " << what << "\n";
        std::exit(exit_usage);
      }
      return argv[++i];
    };
    if (flag == "--repo") options.repository_root = path_from_utf8(next("--repo"));
    else if (flag == "--resource-dir") options.resource_dir = path_from_utf8(next("--resource-dir"));
    else if (flag == "--directory") options.directory = path_from_utf8(next("--directory"));
    else if (flag == "--source") options.source = path_from_utf8(next("--source"));
    else if (flag == "--arg") options.arguments.push_back(next("--arg"));
    else if (flag == "--producer-source") options.producer_source = path_from_utf8(next("--producer-source"));
    else if (flag == "--producer-arg") options.producer_arguments.push_back(next("--producer-arg"));
    else if (flag == "--out") options.out = path_from_utf8(next("--out"));
    else if (flag == "--expect-type") options.expect_type = next("--expect-type");
    else if (flag == "--expect-direct-bases") options.expect_direct_bases = number(flag, next(flag.c_str()));
    else if (flag == "--expect-virtual-bases") options.expect_virtual_bases = number(flag, next(flag.c_str()));
    else if (flag == "--expect-direct-includes") options.expect_direct_includes = number(flag, next(flag.c_str()));
    else if (flag == "--evidence-limit") options.evidence_limit = number(flag, next(flag.c_str()));
    else {
      std::cerr << "unknown option: " << flag << "\n";
      usage();
      return exit_usage;
    }
  }
  if (options.repository_root.empty() || options.resource_dir.empty() || options.directory.empty() ||
      options.source.empty() || options.arguments.empty() || options.out.empty()) {
    usage();
    return exit_usage;
  }

  // An incomplete producer pair is refused rather than silently treated as
  // "no producer": a caller that supplied half of it did intend PCH replay, and
  // quietly rejecting for the wrong reason would hide their mistake.
  if (options.producer_arguments.empty() != options.producer_source.empty()) {
    std::cerr << "--producer-source and --producer-arg must be given together (got "
              << (options.producer_source.empty() ? "arguments without a source" : "a source without arguments")
              << ")\n";
    return exit_bad_input;
  }
  const CompileCommand consumer = build(options.directory, options.source, options.arguments);
  CompileCommand producer;
  const bool has_producer = !options.producer_arguments.empty();
  if (has_producer) producer = build(options.directory, options.producer_source, options.producer_arguments);

  AnalysisRequest request;
  request.command = &consumer;
  request.pch_producer = has_producer ? &producer : nullptr;
  request.repository_root = fs::absolute(options.repository_root).lexically_normal();
  request.resource_dir = fs::absolute(options.resource_dir).lexically_normal();

  const auto started = std::chrono::steady_clock::now();
  const AnalysisResult result = analyze_translation_unit(request);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);

  // --- structural checks against the original source -------------------------
  json structure = json::object();
  bool structure_ok = true;
  if (!options.expect_type.empty()) {
    std::set<StableId> ids;
    for (const auto& [id, symbol] : result.facts.symbols()) {
      if (symbol.key.canonical_name == options.expect_type) ids.insert(id);
    }
    long long direct = 0;
    long long virtual_bases = 0;
    json bases = json::array();
    for (const auto& [key, relation] : result.facts.relations()) {
      if (key.kind != RelationKind::extends || !ids.contains(key.source)) continue;
      for (const auto& evidence : relation.evidence) {
        if (!evidence.base) continue;
        ++direct;
        if (evidence.base->is_virtual) ++virtual_bases;
        bases.push_back({{"is_virtual", evidence.base->is_virtual},
                         {"file", evidence.location.file.generic},
                         {"line", evidence.location.span.begin_line}});
      }
    }
    structure["type"] = options.expect_type;
    structure["found_symbols"] = ids.size();
    structure["direct_bases"] = direct;
    structure["virtual_bases"] = virtual_bases;
    structure["base_evidence"] = bases;
    if (options.expect_direct_bases >= 0 && direct != options.expect_direct_bases) structure_ok = false;
    if (options.expect_virtual_bases >= 0 && virtual_bases != options.expect_virtual_bases) structure_ok = false;
    if (ids.empty()) structure_ok = false;
  }
  if (options.expect_direct_includes >= 0) {
    const auto main_repo = make_repo_relative_on_disk(request.repository_root, consumer.file);
    long long includes = 0;
    if (main_repo) {
      for (const auto& include : result.facts.direct_includes()) {
        if (include.includer == *main_repo) ++includes;
      }
    }
    structure["direct_includes_from_main"] = includes;
    if (includes != options.expect_direct_includes) structure_ok = false;
  }
  structure["checked"] = !structure.empty();
  structure["passed"] = structure_ok;

  // --- the report ------------------------------------------------------------
  json report;
  report["schema"] = "lcm.replay-report.v1";
  report["tool"] = "lcm_replay";
  report["elapsed_ms"] = elapsed.count();
  report["repository_root"] = path_to_utf8_generic(request.repository_root);
  report["resource_dir"] = path_to_utf8_generic(request.resource_dir);
  report["consumer_command"] = command_json(consumer);
  if (has_producer) report["pch_producer_command"] = command_json(producer);
  report["identities"] = {{"command_id", result.command_id},
                          {"replay_id", result.replay_id},
                          {"analysis_unit", result.analysis_unit}};
  report["response_expansion"] = expansion_json(result.expansion);
  if (has_producer) {
    // The producer's own response files decided PCH acceptance just as much as
    // the consumer's. This is the expansion the analyzer actually consumed,
    // carried out of the validation -- not a second read of the filesystem,
    // which could describe different bytes than the decision was made on.
    report["producer_response_expansion"] = expansion_json(result.pch.producer_expansion);
  }
  report["normalization"] = {{"driver", std::string(to_string(result.context.driver))},
                             {"compiler_kind", std::string(to_string(result.context.compiler_kind))},
                             {"status", std::string(to_string(result.context_status))},
                             {"context_complete", result.context_complete},
                             {"source_identified", result.context.source_identified},
                             {"pch_emulated", result.context.pch_emulated},
                             {"analyzer_argument_count", result.context.analyzer_arguments.size()},
                             {"analyzer_arguments", result.context.analyzer_arguments},
                             {"notes", result.context.notes},
                             {"dispositions", dispositions_json(result.context)}};
  report["pch_replay"] = pch_json(result.pch);
  report["status"] = result.status == AnalysisStatus::ok               ? "ok"
                     : result.status == AnalysisStatus::completed_with_errors ? "completed_with_errors"
                                                                             : "rejected";
  report["rejected_arguments"] = result.rejected_arguments;
  report["limits"] = result.limits;
  json diagnostics = json::array();
  std::map<std::string, std::size_t> levels;
  for (const auto& d : result.diagnostics) {
    const char* level = d.level == AnalysisDiagnostic::Level::note      ? "note"
                        : d.level == AnalysisDiagnostic::Level::remark  ? "remark"
                        : d.level == AnalysisDiagnostic::Level::warning ? "warning"
                        : d.level == AnalysisDiagnostic::Level::error   ? "error"
                                                                        : "fatal";
    ++levels[level];
    diagnostics.push_back({{"level", level}, {"text", d.text()}});
  }
  report["diagnostics"] = diagnostics;
  report["diagnostic_counts"] = levels;
  report["facts"] = {{"file_observations", result.files.size()},
                     {"include_observations", result.includes.size()},
                     {"symbols", result.facts.symbols().size()},
                     {"relations", result.facts.relations().size()},
                     {"unresolved_sites", result.facts.unresolved_sites().size()},
                     {"direct_includes", result.facts.direct_includes().size()}};
  // Actual-read input identity: every file the front end entered with the hash
  // and size of the bytes it saw, and every #include with its resolution. Both
  // are serialised as observed and never capped by --evidence-limit.
  const auto repo_path_json = [](const std::optional<RepoRelativePath>& p) {
    return p ? json(p->generic) : json(nullptr);
  };
  json observed_files = json::array();
  for (const FileObservation& f : result.files) {
    observed_files.push_back({{"path", f.path_utf8},
                              {"repo_path", repo_path_json(f.repo_path)},
                              {"in_root", f.in_root},
                              {"is_main_file", f.is_main_file},
                              {"is_system", f.is_system},
                              {"content_sha256", f.content_hash.digest.hex()},
                              {"content_bytes", f.content_hash.size}});
  }
  std::sort(observed_files.begin(), observed_files.end(), [](const json& a, const json& b) {
    return a["path"].get<std::string>() < b["path"].get<std::string>();
  });
  report["file_observations"] = observed_files;
  json observed_includes = json::array();  // in directive order
  for (const IncludeObservation& i : result.includes) {
    observed_includes.push_back({{"includer_path", i.includer_path_utf8},
                                 {"includer_repo_path", repo_path_json(i.includer_repo_path)},
                                 {"spelling", i.spelling},
                                 {"angled", i.angled},
                                 {"line", i.line},
                                 {"resolved_path", i.resolved_path_utf8 ? json(*i.resolved_path_utf8) : json(nullptr)},
                                 {"resolved_repo_path", repo_path_json(i.resolved_repo_path)},
                                 {"resolved_in_root", i.resolved_in_root}});
  }
  report["include_observations"] = observed_includes;
  report["structure"] = structure;
  report["fact_evidence"] =
      fact_evidence_json(result, make_repo_relative_on_disk(request.repository_root, consumer.file),
                         static_cast<std::size_t>(options.evidence_limit), result.pch.guard_path);

  std::ofstream out(options.out, std::ios::binary);
  if (!out) {
    std::cerr << "cannot write report to " << path_to_utf8_generic(options.out) << "\n";
    return exit_report_write_failed;
  }
  out << report.dump(2) << "\n";
  if (!out) return exit_report_write_failed;

  std::cout << "status=" << report["status"].get<std::string>() << " elapsed_ms=" << elapsed.count()
            << " pch=" << to_string(result.pch.mode) << " replay_id=" << result.replay_id << "\n";

  if (result.status == AnalysisStatus::rejected) {
    return result.pch.mode == PchReplayMode::rejected ? exit_rejected_pch : exit_rejected_context;
  }
  if (result.status == AnalysisStatus::completed_with_errors) return exit_completed_with_errors;
  if (!structure_ok) return exit_structure_mismatch;
  return exit_ok;
}
