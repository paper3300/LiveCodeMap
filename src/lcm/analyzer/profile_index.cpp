#include "lcm/analyzer/profile_index.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <exception>
#include <fstream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

#include "lcm/source.hpp"

namespace lcm::analyzer {

namespace fs = std::filesystem;

std::string_view to_string(IndexOutcome value) {
  switch (value) {
    case IndexOutcome::committed: return "committed";
    case IndexOutcome::unchanged: return "unchanged";
    case IndexOutcome::rejected: return "rejected";
    case IndexOutcome::failed: return "failed";
    case IndexOutcome::busy: return "busy";
  }
  return "unknown";
}

std::string_view to_string(SlotPlan value) {
  switch (value) {
    case SlotPlan::new_action: return "new";
    case SlotPlan::changed: return "changed";
    case SlotPlan::unchanged: return "unchanged";
    case SlotPlan::retired: return "retired";
  }
  return "unknown";
}

std::string_view to_string(SlotResult value) {
  switch (value) {
    case SlotResult::installed: return "installed";
    case SlotResult::unchanged: return "unchanged";
    case SlotResult::retired: return "retired";
    case SlotResult::failed: return "failed";
    case SlotResult::not_published: return "not_published";
    case SlotResult::not_analyzed: return "not_analyzed";
  }
  return "unknown";
}

const SlotReport* ProfileIndexReport::slot(std::string_view name) const {
  for (const auto& s : slots) {
    if (s.slot == name) return &s;
  }
  return nullptr;
}

namespace {

std::optional<std::string> read_bytes(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (in.bad()) return std::nullopt;
  return bytes;
}

std::optional<FileContentHash> hash_path(const fs::path& path) {
  auto bytes = read_bytes(path);
  if (!bytes) return std::nullopt;
  return hash_file_content(*bytes);
}

std::string utf8_key(const std::string& path_utf8) { return path_key(path_from_utf8(path_utf8)); }

// The bytes of each distinct file, read at most once per pass: many units
// record the same dependency, and one pass must not hash a file per unit.
// Keyed by the exact recorded spelling (no case folding), so two spellings are
// simply read twice. One cache per pass: the locked revalidation reads again.
class FileHashes {
 public:
  std::optional<FileContentHash> get(const std::string& path_utf8) {
    auto [it, fresh] = cache_.try_emplace(path_utf8);
    if (fresh) it->second = hash_path(path_from_utf8(path_utf8));
    return it->second;
  }

  // Hashes every path of `inputs` (rehashed roles only) not known yet, spread
  // over worker threads; get() then answers from the cache.
  void prefetch(const std::vector<const std::vector<store::InputObservation>*>& inputs) {
    std::vector<std::pair<const std::string, std::optional<FileContentHash>>*> todo;
    for (const auto* list : inputs) {
      for (const auto& in : *list) {
        if (store::is_context_input(in.role) && in.role != store::InputRole::response_file) continue;
        auto [it, fresh] = cache_.try_emplace(in.path_utf8);
        if (fresh) todo.push_back(&*it);  // elements stay put when the map rehashes
      }
    }
    const std::size_t workers =
        std::min<std::size_t>({std::max(1u, std::thread::hardware_concurrency()), 16, todo.size() / 32 + 1});
    std::atomic<std::size_t> next{0};
    const auto work = [&] {
      for (std::size_t n = next.fetch_add(1); n < todo.size(); n = next.fetch_add(1)) {
        todo[n]->second = hash_path(path_from_utf8(todo[n]->first));
      }
    };
    std::vector<std::thread> pool;
    for (std::size_t j = 1; j < workers; ++j) pool.emplace_back(work);
    work();
    for (auto& t : pool) t.join();
  }

 private:
  std::unordered_map<std::string, std::optional<FileContentHash>> cache_;
};

fs::path absolute_under(const fs::path& root, const fs::path& p) {
  return (p.is_absolute() ? p : root / p).lexically_normal();
}

bool is_inl(const std::string& path) {
  if (path.size() < 4) return false;
  std::string tail = path.substr(path.size() - 4);
  std::transform(tail.begin(), tail.end(), tail.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return tail == ".inl";
}

bool same_file(const fs::path& a, const fs::path& b) { return path_key(a) == path_key(b) || same_existing_file(a, b); }

bool located_in(const SourceLocation& l, const std::string& generic) { return l.file.generic == generic; }

// A translation-unit source by extension (.gen.cpp included), case-insensitive.
bool is_translation_unit_source(const std::string& path_utf8) {
  std::string ext = path_to_utf8_generic(path_from_utf8(path_utf8).extension());
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return ext == ".cpp" || ext == ".cc" || ext == ".cxx" || ext == ".c";
}

// A validated selected action.
struct ContextKey {
  store::InputRole role;
  FileContentHash hash;
  friend bool operator==(const ContextKey&, const ContextKey&) = default;
};

struct Resolved {
  const SelectedAction* action = nullptr;
  const CompileCommand* command = nullptr;
  const CompileCommand* producer = nullptr;
  fs::path source;                                // absolute, lexically normal
  std::vector<fs::path> members;                  // absolute, lexically normal
  std::map<std::string, std::string> member_repo;  // path key -> repository-relative generic
  std::set<std::string> source_keys;              // source and members
  std::optional<std::string> unity_repo;          // synthetic unity file inside the root
  std::vector<ContextInput> context;              // request plus action inputs, with captured hashes
  std::map<std::string, ContextKey> context_keys;  // the same by path key
  store::BuildCurrency currency = store::BuildCurrency::unknown;
};

bool tracked_by_rehash(store::InputRole role) {
  return !store::is_context_input(role) || role == store::InputRole::response_file;
}

// Why a committed unit does not describe the selected action any more; empty
// when it still does. Compiler-free: identities, roles and file hashes only.
std::vector<std::string> change_reasons(const store::UnitRecord& unit, const Resolved& action, FileHashes& hashes,
                                        const std::map<std::string, store::InputRole>& overrides,
                                        const store::AnalysisSettings& settings) {
  const std::map<std::string, ContextKey>& context = action.context_keys;
  std::vector<std::string> out;
  if (unit.provenance.command_id != action.command->command_id) out.push_back("selected command changed");
  const std::optional<std::string> producer =
      action.producer ? std::optional<std::string>(action.producer->command_id) : std::nullopt;
  if (unit.provenance.pch_producer_command_id != producer) out.push_back("PCH producer selection changed");
  if (unit.build_currency != action.currency) out.push_back("build currency evidence changed");
  const auto setting = [&](const std::string& committed, const std::string& requested, const char* what) {
    if (committed != requested) {
      out.push_back(std::string(what) + " changed: '" + committed + "' -> '" + requested + "'");
    }
  };
  setting(unit.settings.analyzer, settings.analyzer, "analyzer");
  setting(unit.settings.resource_dir, settings.resource_dir, "resource directory");
  setting(unit.settings.repository_member, settings.repository_member, "repository member");
  setting(unit.settings.publish_policy, settings.publish_policy, "publish policy");

  std::set<std::string> sources;
  std::map<std::string, ContextKey> committed_context;
  for (const auto& in : unit.inputs) {
    if (in.role == store::InputRole::source) sources.insert(utf8_key(in.path_utf8));
    if (!tracked_by_rehash(in.role)) committed_context.emplace(utf8_key(in.path_utf8), ContextKey{in.role, in.content_hash});
  }
  if (sources != action.source_keys) out.push_back("source/unity membership changed");
  for (const auto& [key, c] : context) {
    auto it = committed_context.find(key);
    if (it == committed_context.end()) {
      out.push_back("context input added: " + key);
    } else if (c.role == store::InputRole::build_database && it->second.role == c.role) {
      // Same database path: its bytes do not schedule the slot. The selected
      // entry (command ID), the producer entry and their response files are
      // compared above and below; the whole database was validated this run.
    } else if (!(it->second == c)) {
      out.push_back("context input changed: " + key);
    }
  }
  for (const auto& [key, c] : committed_context) {
    if (!context.count(key)) out.push_back("context input removed: " + key);
  }

  for (const auto& in : unit.inputs) {
    if (!tracked_by_rehash(in.role)) continue;
    switch (in.role) {
      case store::InputRole::header:
      case store::InputRole::inline_file:
      case store::InputRole::generated:
      case store::InputRole::reflected_header:
      case store::InputRole::ignored_dependency:
        if (in.repo_path) {
          auto ov = overrides.find(in.repo_path->generic);
          const store::InputRole expected =
              ov != overrides.end() ? ov->second
                                    : (is_inl(in.path_utf8) ? store::InputRole::inline_file : store::InputRole::header);
          if (expected != in.role) out.push_back("input role changed: " + in.repo_path->generic);
        }
        break;
      default:
        break;
    }
    const auto now = hashes.get(in.path_utf8);
    if (!now) {
      out.push_back("input deleted: " + in.path_utf8);
    } else if (*now != in.content_hash) {
      out.push_back("input modified: " + in.path_utf8);
    }
  }
  return out;
}

// Inputs whose current bytes differ from the recorded ones.
std::vector<std::string> drifted_inputs(const std::vector<store::InputObservation>& inputs, FileHashes& hashes) {
  std::vector<std::string> out;
  for (const auto& in : inputs) {
    if (!tracked_by_rehash(in.role)) continue;
    const auto now = hashes.get(in.path_utf8);
    if (!now || *now != in.content_hash) out.push_back(in.path_utf8);
  }
  return out;
}

// The same facts without the direct includes written in `includer`.
FactSet without_includes_from(const FactSet& in, const std::string& includer) {
  FactSet out;
  for (const auto& [id, symbol] : in.symbols()) {
    if (!symbol.locations.empty()) {
      for (const auto& l : symbol.locations) {
        if (l.role == LocationRole::implicit_declaration) {
          out.add_hidden_member(symbol.key, l);
        } else {
          out.add_symbol_location(symbol.key, l);
        }
      }
    } else if (symbol.presence == SymbolPresence::external_placeholder) {
      out.add_external_placeholder(symbol.key);
    } else {
      out.add_endpoint_only(symbol.key);
    }
  }
  for (const auto& [key, relation] : in.relations()) {
    for (const auto& e : relation.evidence) out.add_relation_evidence(key, e);
  }
  for (const auto& s : in.unresolved_sites()) out.add_unresolved_site(s);
  for (const auto& c : in.captures()) out.add_capture(c);
  for (const auto& inc : in.direct_includes()) {
    if (inc.includer.generic != includer) out.add_direct_include(inc);
  }
  return out;
}

// Checks the declared unity membership against the includes the analyzer
// observed in the synthetic unity file and keeps that file out of the facts.
std::string apply_unity(const AnalysisResult& result, const Resolved& action, store::Contribution& contribution) {
  const std::string unity_key = path_key(action.source);
  std::set<std::string> observed;
  for (const auto& inc : result.includes) {
    if (utf8_key(inc.includer_path_utf8) != unity_key) continue;
    if (!inc.resolved_path_utf8) return "unity file include '" + inc.spelling + "' was not resolved";
    observed.insert(utf8_key(*inc.resolved_path_utf8));
  }
  std::set<std::string> declared;
  for (const auto& m : action.members) declared.insert(path_key(m));
  if (observed != declared) {
    std::string why = "declared unity members differ from the analyzer's observation;";
    for (const auto& k : declared) {
      if (!observed.count(k)) why += " not included: " + k + ";";
    }
    for (const auto& k : observed) {
      if (!declared.count(k)) why += " undeclared: " + k + ";";
    }
    return why;
  }
  const FactSet& facts = contribution.facts;
  // Nested/intermediate unity is unsupported. A member is a fragment when the
  // analyzer observed it directly including another translation-unit source
  // (active, resolved #include, macro operands included), whatever else it
  // contains. Every other member is an original, also one that consists only
  // of includes: it keeps its source role and its own direct-include facts.
  for (const auto& inc : result.includes) {
    if (!inc.resolved_path_utf8) continue;
    const auto member = action.member_repo.find(utf8_key(inc.includer_path_utf8));
    if (member == action.member_repo.end()) continue;
    if (is_translation_unit_source(*inc.resolved_path_utf8)) {
      return "unity member " + member->second + " directly includes the translation-unit source " +
             path_to_utf8_generic(path_from_utf8(*inc.resolved_path_utf8)) +
             " (nested unity fragments are unsupported)";
    }
  }
  if (!action.unity_repo) return {};
  const std::string& unity = *action.unity_repo;
  for (const auto& [id, symbol] : facts.symbols()) {
    for (const auto& l : symbol.locations) {
      if (located_in(l.location, unity)) return "synthetic unity file declares " + symbol.key.canonical_name;
    }
  }
  for (const auto& [key, relation] : facts.relations()) {
    for (const auto& e : relation.evidence) {
      if (located_in(e.location, unity)) return "synthetic unity file contains relation evidence";
    }
  }
  for (const auto& s : facts.unresolved_sites()) {
    if (located_in(s.location, unity)) return "synthetic unity file contains an unresolved site";
  }
  for (const auto& c : facts.captures()) {
    if (located_in(c.location, unity)) return "synthetic unity file contains a capture";
  }
  const bool any = std::any_of(facts.direct_includes().begin(), facts.direct_includes().end(),
                               [&](const DirectIncludeFact& inc) { return inc.includer.generic == unity; });
  if (any) contribution.facts = without_includes_from(facts, unity);
  return {};
}

std::string first_errors(const AnalysisResult& result) {
  std::string out;
  std::size_t n = 0;
  for (const auto& d : result.diagnostics) {
    if (d.level != AnalysisDiagnostic::Level::error && d.level != AnalysisDiagnostic::Level::fatal &&
        result.status != AnalysisStatus::rejected) {
      continue;
    }
    if (n++ == 3) break;
    out += "; " + d.text();
  }
  return out;
}

bool profile_exists(const store::ReadSnapshot& snap, const std::string& profile) {
  const auto all = snap.profiles();
  return std::find(all.begin(), all.end(), profile) != all.end();
}

}  // namespace

ProfileIndexReport index_profile(store::Store& store, const ProfileIndexRequest& request) {
  ProfileIndexReport report;
  const fs::path root = store.workspace_root();
  std::vector<std::string>& errors = report.errors;

  // Records a failed attempt for `slots`, creating a new (empty) profile with
  // the requested UHT applicability first; an existing profile keeps its own.
  // Never touches committed facts. Nothing is recorded for an empty profile key.
  const auto record_failure = [&](const std::optional<store::WriterLock>& held, const std::vector<std::string>& slots,
                                  const std::string& message) {
    std::optional<store::WriterLock> own;
    const store::WriterLock* lock = held ? &*held : nullptr;
    if (!lock) {
      own = store.acquire_writer(request.lock_wait);
      if (!own) {
        report.message += " (failure not recorded: writer lock busy)";
        return;
      }
      lock = &*own;
    }
    if (request.profile.empty()) return;
    try {
      if (!profile_exists(store.read(), request.profile)) store.ensure_profile(*lock, request.profile, request.uht_applicable);
      report.attempt_id = store.record_rejected(*lock, request.profile, slots, message);
      report.failure_recorded = true;
    } catch (const std::exception& e) {
      report.message += std::string(" (failure not recorded: ") + e.what() + ")";
    }
  };
  const auto finish_generation = [&] {
    const auto snap = store.read();
    report.generation_after = profile_exists(snap, request.profile) ? snap.fact_generation(request.profile) : 0;
  };

  // 1. Request, compile database and context inputs (captured before analysis).
  if (request.profile.empty()) errors.push_back("empty profile key");
  if (request.repository_member.empty()) errors.push_back("empty repository member");
  if (request.resource_dir.empty() || !request.resource_dir.is_absolute()) {
    errors.push_back("a pinned absolute Clang resource directory is required");
  }
  const fs::path db_path = absolute_under(root, request.compile_database);
  std::optional<CompilationDatabase> database;
  std::map<std::string, ContextInput> captured;  // every context input by path key, hashed once
  std::vector<std::string> global_context;        // keys applied to every action
  // Hashes a context input once per run; its role must agree wherever it is named.
  const auto capture = [&](const ContextInput& ctx) -> std::optional<std::string> {
    const fs::path p = absolute_under(root, ctx.path);
    const std::string key = path_key(p);
    if (!store::is_context_input(ctx.role) || ctx.role == store::InputRole::response_file) {
      errors.push_back("context input " + path_to_utf8_generic(p) + " has a non-context role " +
                       std::string(store::to_string(ctx.role)));
      return std::nullopt;
    }
    if (auto it = captured.find(key); it != captured.end()) {
      if (it->second.role != ctx.role) {
        errors.push_back("context input " + path_to_utf8_generic(p) + " is named with two roles");
        return std::nullopt;
      }
      return key;
    }
    const auto h = hash_path(p);
    if (!h) {
      errors.push_back("context input missing or unreadable: " + path_to_utf8_generic(p));
      return std::nullopt;
    }
    captured.emplace(key, ContextInput{p, ctx.role, *h});
    return key;
  };
  if (request.compile_database.empty()) {
    errors.push_back("no compile database selected");
  } else if (auto bytes = read_bytes(db_path); !bytes) {
    errors.push_back("compile database missing or unreadable: " + path_to_utf8_generic(db_path));
  } else {
    CompileDbParseOptions po;
    po.syntax = request.syntax;
    po.database_directory = db_path.parent_path();
    auto parsed = parse_compilation_database(*bytes, po);
    for (const auto& d : parsed.diagnostics) {
      if (d.severity == DiagnosticSeverity::error) {
        errors.push_back("malformed compile database" +
                         (d.entry_index ? " entry " + std::to_string(*d.entry_index) : std::string()) + ": " + d.message);
      }
    }
    if (!parsed.database && errors.empty()) errors.push_back("malformed compile database");
    database = std::move(parsed.database);
    // The hash of the bytes that were parsed, not of a second read.
    captured.emplace(path_key(db_path),
                     ContextInput{db_path, store::InputRole::build_database, hash_file_content(*bytes)});
    global_context.push_back(path_key(db_path));
  }
  for (const auto& ctx : request.context_inputs) {
    const auto key = capture(ctx);
    if (!key) continue;
    if (std::find(global_context.begin(), global_context.end(), *key) != global_context.end()) {
      errors.push_back("context input listed twice: " + path_to_utf8_generic(ctx.path));
      continue;
    }
    global_context.push_back(*key);
  }

  // 2. The authoritative selection.
  if (request.actions.empty() && !request.empty_selection_intended) {
    errors.push_back("empty selection without empty_selection_intended; nothing is retired on missing input");
  }
  if (!request.actions.empty() && request.empty_selection_intended) {
    errors.push_back("empty_selection_intended set for a non-empty selection");
  }
  std::map<std::string, store::InputRole> overrides;
  for (const auto& [path, role] : request.role_overrides) {
    if (!is_canonical_repo_relative(path)) {
      errors.push_back("role override for non-canonical path " + path);
    } else if (role != store::InputRole::generated && role != store::InputRole::reflected_header &&
               role != store::InputRole::ignored_dependency) {
      errors.push_back("role override for " + path + " must be generated, reflected_header or ignored_dependency");
    } else {
      overrides.emplace(path, role);
    }
  }
  std::vector<Resolved> actions;
  std::set<std::string> slots;
  RepoPathResolver paths(root);  // one filesystem answer per path for this selection
  std::map<std::string, std::string> command_owner;  // command id -> slot
  std::map<std::string, std::string> file_owner;     // path key -> slot
  const auto claim = [&](const fs::path& file, const std::string& slot) {
    auto [it, fresh] = file_owner.emplace(path_key(file), slot);
    if (!fresh) {
      errors.push_back("file " + path_to_utf8_generic(file) + " is claimed by slots '" + it->second + "' and '" + slot +
                       "'");
    }
  };
  for (const auto& a : request.actions) {
    const std::string where = "slot '" + a.slot + "': ";
    if (a.slot.empty()) {
      errors.push_back("selected action with an empty slot");
      continue;
    }
    if (!slots.insert(a.slot).second) {
      errors.push_back("slot '" + a.slot + "' is selected more than once");
      continue;
    }
    if (a.source.empty()) {
      errors.push_back(where + "no source");
      continue;
    }
    Resolved r;
    r.action = &a;
    r.source = absolute_under(root, a.source);
    r.currency = a.build_currency.value_or(request.build_currency);
    std::vector<std::string> keys = global_context;
    for (const auto& ctx : a.context_inputs) {
      const auto key = capture(ctx);
      if (!key) continue;
      if (std::find(keys.begin(), keys.end(), *key) != keys.end()) {
        errors.push_back(where + "context input listed twice: " + path_to_utf8_generic(ctx.path));
        continue;
      }
      keys.push_back(*key);
    }
    for (const auto& key : keys) {
      if (auto it = captured.find(key); it != captured.end()) {
        r.context.push_back(it->second);
        r.context_keys.emplace(key, ContextKey{it->second.role, *it->second.content_hash});
      }
    }
    if (database) {
      if (a.command_id) {
        for (const auto& c : database->commands) {
          if (c.command_id == *a.command_id) {
            r.command = &c;
            break;
          }
        }
        if (!r.command) {
          errors.push_back(where + "command " + *a.command_id + " is not in the compile database");
        } else if (!same_file(r.command->file, r.source)) {
          errors.push_back(where + "command " + *a.command_id + " compiles " + path_to_utf8_generic(r.command->file) +
                           ", not " + path_to_utf8_generic(r.source));
          r.command = nullptr;
        }
      } else {
        const CommandSelection choice = database->select_unique(r.source);
        if (choice.kind == CommandSelection::Kind::none) {
          errors.push_back(where + "no compile command for " + path_to_utf8_generic(r.source));
        } else if (choice.kind == CommandSelection::Kind::ambiguous) {
          std::string ids;
          for (const auto* c : choice.candidates) ids += " " + c->command_id;
          errors.push_back(where + "several commands for " + path_to_utf8_generic(r.source) +
                           "; select one explicitly:" + ids);
        } else {
          r.command = choice.command;
        }
      }
      if (a.pch_producer_command_id) {
        for (const auto& c : database->commands) {
          if (c.command_id == *a.pch_producer_command_id) {
            r.producer = &c;
            break;
          }
        }
        if (!r.producer) errors.push_back(where + "PCH producer " + *a.pch_producer_command_id + " is not in the database");
      }
    }
    if (r.command) {
      auto [it, fresh] = command_owner.emplace(r.command->command_id, a.slot);
      if (!fresh) errors.push_back("command " + r.command->command_id + " is selected by slots '" + it->second + "' and '" + a.slot + "'");
    }
    claim(r.source, a.slot);
    r.source_keys.insert(path_key(r.source));
    if (a.unity_members.empty()) {
      if (!paths.resolve(r.source)) {
        errors.push_back(where + "source " + path_to_utf8_generic(r.source) + " is outside the workspace root");
      }
    } else {
      if (auto rel = paths.resolve(r.source)) r.unity_repo = rel->generic;
      for (const auto& m : a.unity_members) {
        const fs::path p = absolute_under(root, m);
        const auto rel = paths.resolve(p);
        if (!rel) {
          errors.push_back(where + "unity member " + path_to_utf8_generic(p) + " is outside the workspace root");
          continue;
        }
        if (overrides.count(rel->generic)) errors.push_back(where + "unity member " + rel->generic + " has a role override");
        claim(p, a.slot);
        r.members.push_back(p);
        r.member_repo.emplace(path_key(p), rel->generic);
        r.source_keys.insert(path_key(p));
      }
    }
    actions.push_back(std::move(r));
  }

  // 3. Committed state of the profile, from one snapshot.
  bool exists = false;
  std::map<std::string, store::UnitRecord> committed;
  {
    const auto snap = store.read();
    exists = !request.profile.empty() && profile_exists(snap, request.profile);
    if (exists) {
      report.generation_before = snap.fact_generation(request.profile);
      const auto status = snap.status(request.profile);
      for (const auto& layer : status.layers) {
        if (layer.layer == Layer::uht_context && layer.applicable != request.uht_applicable) {
          errors.push_back("profile '" + request.profile + "' exists with UHT applicability " +
                           (layer.applicable ? "true" : "false") + "; it cannot be switched");
        }
      }
      for (auto& u : snap.units(request.profile, store::UnitDetail::inputs)) committed.emplace(u.slot, std::move(u));
    }
  }
  report.generation_after = report.generation_before;

  std::vector<std::string> declared_slots(slots.begin(), slots.end());
  if (!errors.empty()) {
    report.outcome = IndexOutcome::rejected;
    report.message = "selection rejected before analysis: " + errors.front();
    record_failure(std::nullopt, declared_slots, report.message);
    finish_generation();
    return report;
  }

  // 4. Plan against the committed units.
  store::AnalysisSettings settings;
  settings.analyzer = analyzer_identity();
  settings.resource_dir = path_to_utf8_generic(request.resource_dir.lexically_normal());
  settings.repository_member = request.repository_member;
  settings.publish_policy = std::string(to_string(request.policy));
  std::vector<std::size_t> planned;  // indexes into actions (and report.slots)
  FileHashes planning_hashes;
  {
    std::vector<const std::vector<store::InputObservation>*> recorded;
    for (const auto& [slot, unit] : committed) recorded.push_back(&unit.inputs);
    planning_hashes.prefetch(recorded);
  }
  for (std::size_t i = 0; i < actions.size(); ++i) {
    const Resolved& r = actions[i];
    SlotReport s;
    s.slot = r.action->slot;
    s.command_id = r.command->command_id;
    auto it = committed.find(s.slot);
    if (it == committed.end()) {
      s.plan = SlotPlan::new_action;
      s.reasons.push_back("no committed unit");
    } else {
      s.reasons = change_reasons(it->second, r, planning_hashes, overrides, settings);
      s.plan = s.reasons.empty() ? SlotPlan::unchanged : SlotPlan::changed;
    }
    s.result = s.plan == SlotPlan::unchanged ? SlotResult::unchanged : SlotResult::not_published;
    if (s.plan != SlotPlan::unchanged) planned.push_back(i);
    report.slots.push_back(std::move(s));
  }
  std::vector<std::string> retire;
  for (const auto& [slot, unit] : committed) {
    if (slots.count(slot)) continue;
    retire.push_back(slot);
    SlotReport s;
    s.slot = slot;
    s.plan = SlotPlan::retired;
    s.result = SlotResult::not_published;
    s.command_id = unit.provenance.command_id;
    s.reasons.push_back("absent from the authoritative selection");
    report.slots.push_back(std::move(s));
  }
  report.planned_analyses = planned.size();
  if (request.observer) request.observer(IndexPhase::planned);
  std::vector<std::string> involved;
  for (std::size_t i : planned) involved.push_back(report.slots[i].slot);
  involved.insert(involved.end(), retire.begin(), retire.end());
  if (involved.empty()) involved = declared_slots;  // an unchanged run that fails revalidation

  // 5. Analyse the new and changed actions without the writer lock. Each
  // analysis only reads the request and its own action; the outcome of every
  // planned slot is kept in selection order, whatever order jobs finish in.
  // A finished contribution goes to the spool at once (validated as the store
  // would validate it), so memory holds only the analyses in flight plus each
  // slot's input list, which the locked revalidation needs.
  struct Analysed {
    bool done = false;
    std::optional<std::size_t> entry;          // spool entry of a good contribution
    std::vector<store::InputObservation> inputs;
    std::string why;  // non-empty: this slot failed
  };
  std::vector<Analysed> analysed(planned.size());
  std::optional<store::ContributionSpool> spool;
  if (!planned.empty()) spool.emplace(store.path().parent_path());
  const auto analyze_one = [&](std::size_t n) {
    const std::size_t i = planned[n];
    const Resolved& r = actions[i];
    const std::string& slot = report.slots[i].slot;
    Analysed& out = analysed[n];
    AnalysisRequest ar;
    ar.command = r.command;
    ar.pch_producer = r.producer;
    ar.repository_root = root;
    ar.repository_member = request.repository_member;
    ar.resource_dir = request.resource_dir;
    ar.analysis_unit = slot;  // fact label == logical slot
    const AnalysisResult result = analyze_translation_unit(ar);
    out.done = true;

    std::string why;
    if (result.status == AnalysisStatus::rejected) {
      why = "analysis rejected" + first_errors(result);
    } else if (r.producer && result.pch.mode != PchReplayMode::textual_snapshot) {
      why = "PCH producer " + r.producer->command_id + " was selected but not used (" +
            std::string(to_string(result.pch.mode)) + (result.pch.reject_reason.empty() ? "" : ": " + result.pch.reject_reason) + ")";
    }
    std::optional<store::Contribution> contribution;
    if (why.empty()) {
      ContributionOptions co;
      co.slot = slot;
      co.policy = request.policy;
      co.role_overrides = overrides;
      for (const auto& [key, generic] : r.member_repo) co.role_overrides[generic] = store::InputRole::source;
      co.context_inputs = r.context;
      co.build_currency = r.currency;
      co.repository_member = request.repository_member;
      co.resource_dir = request.resource_dir;
      auto converted = to_contribution(result, co);
      if (!converted.contribution) {
        why = converted.reason + first_errors(result);
      } else {
        contribution = std::move(converted.contribution);
      }
    }
    if (why.empty() && !r.members.empty()) why = apply_unity(result, r, *contribution);
    std::size_t entry = 0;
    if (why.empty()) {
      if (auto refused = spool->add(*contribution, &entry); !refused.empty()) why = "contribution refused: " + refused;
    }
    if (why.empty()) {
      out.entry = entry;
      out.inputs = std::move(contribution->inputs);
    } else {
      out.why = std::move(why);
    }
  };
  const std::size_t jobs = std::min<std::size_t>(std::max<std::size_t>(request.jobs, 1), planned.size());
  if (jobs <= 1) {
    for (std::size_t n = 0; n < planned.size(); ++n) {
      analyze_one(n);
      if (!analysed[n].why.empty()) break;  // the first failure stops analysis
    }
  } else {
    // Bounded parallel analysis. After a failure no further analysis starts;
    // analyses already running finish. An exception is rethrown after every
    // job has stopped, the first one in selection order.
    std::atomic<std::size_t> next{0};
    std::atomic<bool> stop{false};
    std::vector<std::exception_ptr> thrown(planned.size());
    const auto worker = [&] {
      for (;;) {
        if (stop.load()) return;
        const std::size_t n = next.fetch_add(1);
        if (n >= planned.size()) return;
        try {
          analyze_one(n);
          if (!analysed[n].why.empty()) stop.store(true);
        } catch (...) {
          thrown[n] = std::current_exception();
          stop.store(true);
        }
      }
    };
    std::vector<std::thread> pool;
    pool.reserve(jobs);
    for (std::size_t j = 0; j < jobs; ++j) pool.emplace_back(worker);
    for (auto& t : pool) t.join();
    for (const auto& e : thrown) {
      if (e) std::rethrow_exception(e);
    }
  }
  // Per-slot results in selection order: a failed slot is `failed`, a slot
  // that was not started is `not_analyzed`, a good analysis stays
  // `not_published` unless the batch is published. The run's message names the
  // first failed slot in selection order.
  std::vector<std::size_t> spooled;             // spool entries to install, in selection order
  std::vector<const Analysed*> installed;       // the same analyses, for revalidation
  std::string failure;
  for (std::size_t n = 0; n < planned.size(); ++n) {
    SlotReport& s = report.slots[planned[n]];
    Analysed& a = analysed[n];
    if (!a.done) {
      s.result = SlotResult::not_analyzed;
      continue;
    }
    ++report.actual_analyses;
    if (!a.why.empty()) {
      s.result = SlotResult::failed;
      s.reasons.push_back(a.why);
      if (failure.empty()) failure = "slot '" + s.slot + "': " + a.why;
      continue;
    }
    spooled.push_back(*a.entry);
    installed.push_back(&a);
  }
  if (request.observer) request.observer(IndexPhase::analyzed);
  if (!failure.empty()) {
    report.outcome = IndexOutcome::failed;
    report.message = "nothing published: " + failure;
    record_failure(std::nullopt, involved, report.message);
    finish_generation();
    return report;
  }

  // 6. Revalidate under the writer lock (also when nothing changed), then publish once.
  std::optional<store::WriterLock> lock = store.acquire_writer(request.lock_wait);
  if (!lock) {
    report.outcome = IndexOutcome::busy;
    report.message = planned.empty() && retire.empty() ? "writer lock busy; unchanged state not revalidated"
                                                       : "writer lock busy; analyses discarded, nothing published";
    finish_generation();
    return report;
  }
  const auto fail_locked = [&](const std::string& why) {
    report.outcome = IndexOutcome::failed;
    report.message = "nothing published: " + why;
    record_failure(lock, involved, report.message);
    finish_generation();
    return report;
  };
  if (request.observer) request.observer(IndexPhase::locked);
  {
    bool now_exists = false;
    std::int64_t gen = 0;
    {
      // The snapshot must be closed before a failure is recorded: an open
      // read transaction blocks the writer's commit in rollback-journal mode.
      const auto snap = store.read();
      now_exists = profile_exists(snap, request.profile);
      gen = now_exists ? snap.fact_generation(request.profile) : 0;
    }
    if (now_exists != exists || gen != report.generation_before) {
      return fail_locked("the profile was published concurrently (generation " + std::to_string(report.generation_before) +
                         " -> " + std::to_string(gen) + "); rerun");
    }
  }
  FileHashes locked_hashes;  // the locked pass reads every file again, each once
  {
    std::vector<const std::vector<store::InputObservation>*> recorded;
    for (const Analysed* a : installed) recorded.push_back(&a->inputs);
    for (const auto& s : report.slots) {
      if (s.plan == SlotPlan::unchanged) recorded.push_back(&committed.at(s.slot).inputs);
    }
    locked_hashes.prefetch(recorded);
  }
  for (const auto& [key, ctx] : captured) {
    const auto now = locked_hashes.get(path_to_utf8_generic(ctx.path));
    if (!now || *now != *ctx.content_hash) {
      return fail_locked("context input changed during the run: " + path_to_utf8_generic(ctx.path));
    }
  }
  for (std::size_t k = 0; k < installed.size(); ++k) {
    if (auto drift = drifted_inputs(installed[k]->inputs, locked_hashes); !drift.empty()) {
      return fail_locked("slot '" + spool->slot(spooled[k]) + "': input changed after analysis: " + drift.front());
    }
  }
  for (const auto& s : report.slots) {
    if (s.plan != SlotPlan::unchanged) continue;
    if (auto drift = drifted_inputs(committed.at(s.slot).inputs, locked_hashes); !drift.empty()) {
      return fail_locked("unchanged slot '" + s.slot + "': input changed during the run: " + drift.front());
    }
  }
  try {
    store.ensure_profile(*lock, request.profile, request.uht_applicable);
  } catch (const std::exception& e) {
    return fail_locked(e.what());
  }
  if (spooled.empty() && retire.empty()) {
    report.outcome = IndexOutcome::unchanged;
    report.message = exists ? "every selected action matches its committed unit"
                            : "intended empty selection; empty profile created";
    return report;
  }
  store::Batch batch;
  batch.profile = request.profile;
  if (spool) batch.spool = &*spool;
  batch.spooled = spooled;
  batch.retire = retire;
  const store::PublishResult published = store.publish(*lock, batch, request.publish_observer);
  report.attempt_id = published.attempt_id;
  report.generation_after = published.fact_generation;
  switch (published.outcome) {
    case store::PublishOutcome::committed:
      report.outcome = IndexOutcome::committed;
      report.message = "published " + std::to_string(batch.spooled.size()) + " contribution(s), retired " +
                       std::to_string(retire.size()) + " slot(s)";
      for (auto& s : report.slots) {
        if (s.plan == SlotPlan::retired) {
          s.result = SlotResult::retired;
        } else if (s.plan != SlotPlan::unchanged) {
          s.result = SlotResult::installed;
        }
      }
      break;
    case store::PublishOutcome::no_op:
      report.outcome = IndexOutcome::unchanged;
      report.message = "publication was a no-op";
      break;
    case store::PublishOutcome::rejected:
    case store::PublishOutcome::failed:
      report.outcome = IndexOutcome::failed;
      report.message = "nothing published: store " + std::string(store::to_string(published.outcome)) + ": " +
                       published.message;
      report.failure_recorded = published.attempt_id.has_value();
      break;
  }
  return report;
}

}  // namespace lcm::analyzer
