#include "lcm/analyzer/store_adapter.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>

#include "clang/Basic/Version.h"
#include "lcm/hash.hpp"
#include "lcm/source.hpp"

namespace lcm::analyzer {

namespace {

std::optional<Sha256Digest> digest_from_hex(const std::string& hex) {
  if (hex.size() != 64) return std::nullopt;
  Sha256Digest d;
  for (std::size_t i = 0; i < 32; ++i) {
    unsigned value = 0;
    for (std::size_t j = 0; j < 2; ++j) {
      const char c = hex[2 * i + j];
      value <<= 4;
      if (c >= '0' && c <= '9') {
        value |= static_cast<unsigned>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        value |= static_cast<unsigned>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        value |= static_cast<unsigned>(c - 'A' + 10);
      } else {
        return std::nullopt;
      }
    }
    d.bytes[i] = static_cast<std::uint8_t>(value);
  }
  return d;
}

std::string level_name(AnalysisDiagnostic::Level level) {
  switch (level) {
    case AnalysisDiagnostic::Level::note: return "note";
    case AnalysisDiagnostic::Level::remark: return "remark";
    case AnalysisDiagnostic::Level::warning: return "warning";
    case AnalysisDiagnostic::Level::error: return "error";
    case AnalysisDiagnostic::Level::fatal: return "fatal";
  }
  return "error";
}

bool is_inl(const std::string& path) {
  if (path.size() < 4) return false;
  std::string tail = path.substr(path.size() - 4);
  std::transform(tail.begin(), tail.end(), tail.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return tail == ".inl";
}

// Identity of the normalized context that reached the front end.
std::string context_identity(const NormalizedCompileContext& context) {
  std::string out;
  for (const auto& a : context.analyzer_arguments) {
    out += std::to_string(a.size());
    out.push_back(':');
    out += a;
    out.push_back(';');
  }
  return "ctx:" + sha256(out).hex();
}

}  // namespace

std::string analyzer_identity() { return "clang " CLANG_VERSION_STRING; }

std::string_view to_string(PublishPolicy value) {
  switch (value) {
    case PublishPolicy::accepted_only: return "accepted_only";
    case PublishPolicy::allow_errors_and_degraded: return "allow_errors_and_degraded";
  }
  return "unknown";
}

ContributionConversion to_contribution(const AnalysisResult& result, const ContributionOptions& options) {
  ContributionConversion out;
  if (options.slot.empty()) {
    out.reason = "no slot supplied";
    return out;
  }
  if (result.status == AnalysisStatus::rejected) {
    out.reason = "unit was rejected; nothing to store";
    return out;
  }
  if (options.policy == PublishPolicy::accepted_only) {
    if (result.status != AnalysisStatus::ok) {
      out.reason = "unit completed with errors; provisional facts are not stored under accepted_only";
      return out;
    }
    if (!result.context_complete) {
      out.reason = "compile context was degraded; facts are not stored under accepted_only";
      return out;
    }
  }

  store::Contribution c;
  c.slot = options.slot;
  c.fact_label = result.analysis_unit;
  c.provenance.command_id = result.command_id;
  c.provenance.replay_id = result.replay_id;
  c.provenance.context_id = context_identity(result.context);
  if (result.pch.mode == PchReplayMode::textual_snapshot) {
    c.provenance.pch_producer_command_id = result.pch.producer_command_id;
    c.provenance.pch_producer_replay_id = result.pch.producer_replay_id;
  }
  c.analysis_status = result.status == AnalysisStatus::ok ? "ok" : "completed_with_errors";
  c.build_currency = options.build_currency;
  c.settings.analyzer = analyzer_identity();
  c.settings.resource_dir = options.resource_dir.empty() ? std::string() : path_to_utf8_generic(options.resource_dir.lexically_normal());
  c.settings.repository_member = options.repository_member;
  c.settings.publish_policy = std::string(to_string(options.policy));
  c.facts = result.facts;
  c.limits = result.limits;
  for (const auto& d : result.diagnostics) {
    c.diagnostics.push_back(store::UnitDiagnostic{level_name(d.level), d.message, d.file, d.line, d.column});
  }
  for (const auto& inc : result.includes) {
    store::IncludeEdge e;
    // A command-forced include has no includer; keep that instead of inventing an owner.
    if (!inc.includer_path_utf8.empty()) e.includer_path_utf8 = inc.includer_path_utf8;
    e.spelling = inc.spelling;
    e.angled = inc.angled;
    e.resolved_path_utf8 = inc.resolved_path_utf8;
    e.line = inc.line;
    c.includes.push_back(std::move(e));
  }

  // One input per file identity. Repeated observations must agree on bytes.
  std::map<std::string, std::size_t> by_key;
  const auto add = [&](store::InputObservation o) -> bool {
    const std::string key = path_key(path_from_utf8(o.path_utf8));
    auto it = by_key.find(key);
    if (it == by_key.end()) {
      by_key.emplace(key, c.inputs.size());
      c.inputs.push_back(std::move(o));
      return true;
    }
    store::InputObservation& existing = c.inputs[it->second];
    if (existing.content_hash != o.content_hash) {
      out.reason = "inconsistent observations of " + o.path_utf8;
      return false;
    }
    if (o.role == store::InputRole::source) existing.role = store::InputRole::source;
    existing.is_system = existing.is_system && o.is_system;
    return true;
  };
  // The textual-PCH order guard is an in-memory file named after its own
  // digest (part of the replay identity); it is not a filesystem dependency.
  const std::string guard_key = result.pch.mode == PchReplayMode::textual_snapshot && !result.pch.guard_path.empty()
                                    ? path_key(path_from_utf8(result.pch.guard_path))
                                    : std::string();
  for (const auto& f : result.files) {
    if (!guard_key.empty() && path_key(path_from_utf8(f.path_utf8)) == guard_key) continue;
    store::InputObservation o;
    o.path_utf8 = f.path_utf8;
    o.repo_path = f.in_root ? f.repo_path : std::nullopt;
    o.content_hash = f.content_hash;
    o.is_system = f.is_system;
    if (f.is_main_file) {
      o.role = store::InputRole::source;
    } else if (!f.in_root) {
      o.role = store::InputRole::external_dependency;
    } else if (auto ov = f.repo_path ? options.role_overrides.find(f.repo_path->generic) : options.role_overrides.end();
               ov != options.role_overrides.end()) {
      o.role = ov->second;
    } else if (is_inl(f.path_utf8)) {
      o.role = store::InputRole::inline_file;
    } else {
      o.role = store::InputRole::header;
    }
    if (!add(std::move(o))) return out;
  }
  const auto add_response_files = [&](const ResponseExpansion& expansion) -> bool {
    if (!expansion.ok) return true;
    for (const auto& snap : expansion.files) {
      const auto digest = digest_from_hex(snap.content_sha256);
      if (!digest) {
        out.reason = "response file without a content hash: " + snap.resolved_utf8;
        return false;
      }
      store::InputObservation o;
      o.path_utf8 = snap.resolved_utf8;
      o.role = store::InputRole::response_file;
      o.content_hash = FileContentHash{*digest, snap.bytes};
      if (!add(std::move(o))) return false;
    }
    return true;
  };
  if (!add_response_files(result.expansion)) return out;
  if (result.pch.mode == PchReplayMode::textual_snapshot && !add_response_files(result.pch.producer_expansion)) return out;
  for (const auto& ctx : options.context_inputs) {
    store::InputObservation o;
    o.path_utf8 = path_to_utf8_generic(ctx.path);
    o.role = ctx.role;
    if (ctx.content_hash) {
      o.content_hash = *ctx.content_hash;
    } else {
      std::ifstream in(ctx.path, std::ios::binary);
      if (!in) {
        out.reason = "context input not readable: " + path_to_utf8_generic(ctx.path);
        return out;
      }
      const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      o.content_hash = hash_file_content(bytes);
    }
    if (!add(std::move(o))) return out;
  }
  out.contribution = std::move(c);
  return out;
}

}  // namespace lcm::analyzer
