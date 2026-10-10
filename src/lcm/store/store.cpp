#include "lcm/store/store.hpp"

#include "lcm/store/contribution_codec.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <atomic>
#include <chrono>
#include <fstream>
#include <future>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <optional>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>

#include "lcm/hash.hpp"
#include "lcm/identity.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace lcm::store {

namespace fs = std::filesystem;

namespace {

// 'LCMS' in the SQLite header: identifies a LiveCodeMap store file.
constexpr int kApplicationId = 0x4C434D53;

// ---------------------------------------------------------------------------
// Minimal SQLite wrapper.

[[noreturn]] void throw_sqlite(sqlite3* db, std::string_view what) {
  throw StoreError(std::string(what) + ": " + (db ? sqlite3_errmsg(db) : "out of memory"));
}

class Stmt {
 public:
  Stmt(sqlite3* db, std::string_view sql) : db_(db) {
    if (sqlite3_prepare_v2(db, sql.data(), static_cast<int>(sql.size()), &stmt_, nullptr) != SQLITE_OK) {
      throw_sqlite(db, "prepare");
    }
  }
  Stmt(const Stmt&) = delete;
  Stmt& operator=(const Stmt&) = delete;
  ~Stmt() { sqlite3_finalize(stmt_); }

  Stmt& reset() {
    sqlite3_reset(stmt_);
    sqlite3_clear_bindings(stmt_);
    next_ = 1;
    return *this;
  }

  Stmt& b(std::nullptr_t) {
    check(sqlite3_bind_null(stmt_, next_++));
    return *this;
  }
  Stmt& b(std::string_view v) {
    check(sqlite3_bind_text(stmt_, next_++, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT));
    return *this;
  }
  Stmt& b(const std::string& v) { return b(std::string_view(v)); }
  Stmt& b(const char* v) { return b(std::string_view(v)); }
  Stmt& b(bool v) { return b(static_cast<std::int64_t>(v ? 1 : 0)); }
  Stmt& b(std::int64_t v) {
    check(sqlite3_bind_int64(stmt_, next_++, v));
    return *this;
  }
  Stmt& b(std::uint64_t v) { return b(static_cast<std::int64_t>(v)); }
  Stmt& b(std::uint32_t v) { return b(static_cast<std::int64_t>(v)); }
  Stmt& b(int v) { return b(static_cast<std::int64_t>(v)); }
  Stmt& b(const std::optional<std::string>& v) { return v ? b(*v) : b(nullptr); }
  Stmt& b(const std::optional<std::int64_t>& v) { return v ? b(*v) : b(nullptr); }
  // A SHA-256 digest is stored as its 32 raw bytes.
  Stmt& b(const Sha256Digest& d) {
    check(sqlite3_bind_blob(stmt_, next_++, d.bytes.data(), static_cast<int>(d.bytes.size()), SQLITE_TRANSIENT));
    return *this;
  }
  Stmt& b(const std::optional<Sha256Digest>& d) { return d ? b(*d) : b(nullptr); }

  bool step() {
    const int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;
    throw_sqlite(db_, "step");
  }
  void run() {
    while (step()) {
    }
  }

  [[nodiscard]] bool is_null(int col) const { return sqlite3_column_type(stmt_, col) == SQLITE_NULL; }
  [[nodiscard]] std::int64_t i64(int col) const { return sqlite3_column_int64(stmt_, col); }
  [[nodiscard]] std::uint32_t u32(int col) const { return static_cast<std::uint32_t>(sqlite3_column_int64(stmt_, col)); }
  [[nodiscard]] bool flag(int col) const { return sqlite3_column_int64(stmt_, col) != 0; }
  [[nodiscard]] std::string text(int col) const {
    const auto* p = reinterpret_cast<const char*>(sqlite3_column_text(stmt_, col));
    return p ? std::string(p, static_cast<std::size_t>(sqlite3_column_bytes(stmt_, col))) : std::string{};
  }
  [[nodiscard]] std::optional<std::string> opt_text(int col) const {
    if (is_null(col)) return std::nullopt;
    return text(col);
  }
  [[nodiscard]] Sha256Digest digest(int col) const {
    Sha256Digest d;
    const auto* p = static_cast<const std::uint8_t*>(sqlite3_column_blob(stmt_, col));
    if (sqlite3_column_type(stmt_, col) != SQLITE_BLOB || !p ||
        sqlite3_column_bytes(stmt_, col) != static_cast<int>(d.bytes.size())) {
      throw StoreError("corrupt store: digest is not 32 bytes");
    }
    std::copy(p, p + d.bytes.size(), d.bytes.begin());
    return d;
  }
  [[nodiscard]] std::optional<Sha256Digest> opt_digest(int col) const {
    if (is_null(col)) return std::nullopt;
    return digest(col);
  }

 private:
  void check(int rc) {
    if (rc != SQLITE_OK) throw_sqlite(db_, "bind");
  }
  sqlite3* db_ = nullptr;
  sqlite3_stmt* stmt_ = nullptr;
  int next_ = 1;
};

sqlite3* open_connection(const fs::path& path, bool create, std::chrono::milliseconds busy) {
  sqlite3* db = nullptr;
  const int flags = SQLITE_OPEN_READWRITE | (create ? SQLITE_OPEN_CREATE : 0) | SQLITE_OPEN_NOMUTEX;
  const int rc = sqlite3_open_v2(path_to_utf8_generic(path).c_str(), &db, flags, nullptr);
  if (rc != SQLITE_OK) {
    std::string message = db ? sqlite3_errmsg(db) : "out of memory";
    sqlite3_close_v2(db);
    throw StoreError("cannot open store " + path_to_utf8_generic(path) + ": " + message);
  }
  sqlite3_extended_result_codes(db, 1);
  sqlite3_busy_timeout(db, static_cast<int>(busy.count()));
  return db;
}

void exec(sqlite3* db, const char* sql) {
  char* error = nullptr;
  if (sqlite3_exec(db, sql, nullptr, nullptr, &error) != SQLITE_OK) {
    std::string message = error ? error : sqlite3_errmsg(db);
    sqlite3_free(error);
    throw StoreError(std::string("sql: ") + message + " (" + std::string(sql).substr(0, 48) + ")");
  }
}

void exec_noexcept(sqlite3* db, const char* sql) { sqlite3_exec(db, sql, nullptr, nullptr, nullptr); }

std::int64_t pragma_int(sqlite3* db, const char* sql) {
  Stmt s(db, sql);
  return s.step() ? s.i64(0) : 0;
}

std::string pragma_text(sqlite3* db, const char* sql) {
  Stmt s(db, sql);
  return s.step() ? s.text(0) : std::string{};
}

// ---------------------------------------------------------------------------
// Schema. Location columns are shared by every located fact. The observed
// spelling, content digest and size of the located file are one `file_version`
// row shared by every location in those bytes; digests are 32-byte BLOBs.

#define LCM_LOC_COLUMNS                                                                                   \
  "file_id INTEGER REFERENCES file(id), version_id INTEGER NOT NULL REFERENCES file_version(id), "         \
  "b_off INTEGER NOT NULL, e_off INTEGER NOT NULL, b_line INTEGER NOT NULL, "                              \
  "b_col INTEGER NOT NULL, e_line INTEGER NOT NULL, e_col INTEGER NOT NULL, span_hash BLOB"

constexpr const char* kLocInsertColumns = "file_id, version_id, b_off, e_off, b_line, b_col, e_line, e_col, span_hash";
constexpr const char* kLocPlaceholders = "?,?,?,?,?,?,?,?,?";

constexpr const char* kSchema =
    "CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT NOT NULL) WITHOUT ROWID;"
    "CREATE TABLE profile(id INTEGER PRIMARY KEY, key TEXT NOT NULL UNIQUE, uht_applicable INTEGER NOT NULL,"
    " fact_generation INTEGER NOT NULL DEFAULT 0, ranking_generation INTEGER NOT NULL DEFAULT 0);"
    "CREATE TABLE attempt(id INTEGER PRIMARY KEY, profile_id INTEGER NOT NULL REFERENCES profile(id),"
    " layer TEXT NOT NULL, state TEXT NOT NULL, started_at INTEGER NOT NULL, finished_at INTEGER,"
    " message TEXT NOT NULL);"
    "CREATE INDEX attempt_by_profile ON attempt(profile_id, layer, id);"
    "CREATE TABLE slot_attempt(attempt_id INTEGER NOT NULL REFERENCES attempt(id), slot TEXT NOT NULL,"
    " action TEXT NOT NULL, PRIMARY KEY(attempt_id, slot)) WITHOUT ROWID;"
    "CREATE TABLE file(id INTEGER PRIMARY KEY, profile_id INTEGER NOT NULL REFERENCES profile(id),"
    " path TEXT NOT NULL, UNIQUE(profile_id, path));"
    // One observed state of a located file: spelling as written, content digest, size.
    "CREATE TABLE file_version(id INTEGER PRIMARY KEY, profile_id INTEGER NOT NULL REFERENCES profile(id),"
    " file_id INTEGER REFERENCES file(id), path_as_written TEXT NOT NULL, digest BLOB NOT NULL, size INTEGER NOT NULL);"
    "CREATE INDEX file_version_key ON file_version(profile_id, file_id, digest);"
    "CREATE TABLE excluded_file(profile_id INTEGER NOT NULL REFERENCES profile(id), path TEXT NOT NULL,"
    " PRIMARY KEY(profile_id, path)) WITHOUT ROWID;"
    "CREATE TABLE unit(id INTEGER PRIMARY KEY, profile_id INTEGER NOT NULL REFERENCES profile(id),"
    " slot TEXT NOT NULL, fact_label TEXT NOT NULL, command_id TEXT NOT NULL, context_id TEXT,"
    " replay_id TEXT NOT NULL, pch_producer_command_id TEXT, pch_producer_replay_id TEXT,"
    " analyzer TEXT NOT NULL, resource_dir TEXT NOT NULL, repository_member TEXT NOT NULL,"
    " publish_policy TEXT NOT NULL, analysis_status TEXT NOT NULL, build_currency TEXT NOT NULL, committed_generation INTEGER NOT NULL,"
    " attempt_id INTEGER NOT NULL REFERENCES attempt(id), UNIQUE(profile_id, slot), UNIQUE(profile_id, fact_label));"
    "CREATE TABLE unit_diagnostic(unit_id INTEGER NOT NULL REFERENCES unit(id) ON DELETE CASCADE,"
    " ordinal INTEGER NOT NULL, level TEXT NOT NULL, message TEXT NOT NULL, file TEXT, line INTEGER NOT NULL,"
    " col INTEGER NOT NULL, PRIMARY KEY(unit_id, ordinal)) WITHOUT ROWID;"
    "CREATE TABLE unit_limit(unit_id INTEGER NOT NULL REFERENCES unit(id) ON DELETE CASCADE,"
    " ordinal INTEGER NOT NULL, text TEXT NOT NULL, PRIMARY KEY(unit_id, ordinal)) WITHOUT ROWID;"
    "CREATE TABLE unit_fingerprint(unit_id INTEGER NOT NULL REFERENCES unit(id) ON DELETE CASCADE,"
    " kind TEXT NOT NULL, value TEXT, PRIMARY KEY(unit_id, kind)) WITHOUT ROWID;"
    "CREATE TABLE unit_freshness(unit_id INTEGER PRIMARY KEY REFERENCES unit(id) ON DELETE CASCADE,"
    " semantic TEXT NOT NULL, compile_context TEXT NOT NULL, uht TEXT, checked_at INTEGER NOT NULL,"
    " reasons TEXT NOT NULL);"
    // Dependency observations are shared rows: every unit that observed the same
    // file in the same role with the same bytes (for example the files of one
    // precompiled-header prefix) references one `dependency` row, and every
    // identical include edge one `include_site` row. The per-unit rows keep
    // only the reference (and, for edges, the unit's order).
    "CREATE TABLE dependency(id INTEGER PRIMARY KEY, path_key TEXT NOT NULL, path TEXT NOT NULL, repo_path TEXT,"
    " repo_path_as_written TEXT, role TEXT NOT NULL, digest BLOB NOT NULL, size INTEGER NOT NULL,"
    " is_system INTEGER NOT NULL);"
    "CREATE INDEX dependency_key ON dependency(path_key, digest);"
    "CREATE TABLE input(unit_id INTEGER NOT NULL REFERENCES unit(id) ON DELETE CASCADE,"
    " dependency_id INTEGER NOT NULL REFERENCES dependency(id), PRIMARY KEY(unit_id, dependency_id)) WITHOUT ROWID;"
    "CREATE INDEX input_by_dependency ON input(dependency_id);"
    "CREATE TABLE include_site(id INTEGER PRIMARY KEY, includer TEXT, spelling TEXT NOT NULL, angled INTEGER NOT NULL,"
    " resolved TEXT, line INTEGER NOT NULL);"
    "CREATE INDEX include_site_key ON include_site(spelling, line, includer);"
    "CREATE TABLE include_edge(unit_id INTEGER NOT NULL REFERENCES unit(id) ON DELETE CASCADE,"
    " ordinal INTEGER NOT NULL, site_id INTEGER NOT NULL REFERENCES include_site(id), PRIMARY KEY(unit_id, ordinal))"
    " WITHOUT ROWID;"
    "CREATE INDEX include_edge_by_site ON include_edge(site_id);"
    "CREATE TABLE symbol(id INTEGER PRIMARY KEY, profile_id INTEGER NOT NULL REFERENCES profile(id),"
    " stable_id TEXT NOT NULL, key_serialized TEXT NOT NULL, repository_member TEXT NOT NULL,"
    " language TEXT NOT NULL, kind TEXT NOT NULL, canonical_name TEXT NOT NULL,"
    " normalized_signature TEXT NOT NULL, template_role TEXT NOT NULL, template_parameters TEXT NOT NULL,"
    " template_arguments TEXT NOT NULL, linkage TEXT NOT NULL, linkage_file TEXT, linkage_file_as_written TEXT,"
    " linkage_enclosing TEXT, linkage_anchor TEXT, linkage_ordinal INTEGER, UNIQUE(profile_id, stable_id));"
    "CREATE INDEX symbol_by_name ON symbol(profile_id, canonical_name);"
    "CREATE TABLE symbol_owner(symbol_id INTEGER NOT NULL REFERENCES symbol(id) ON DELETE CASCADE,"
    " ordinal INTEGER NOT NULL, kind TEXT NOT NULL, name TEXT NOT NULL, normalized_signature TEXT NOT NULL,"
    " template_role TEXT NOT NULL, template_parameters TEXT NOT NULL, template_arguments TEXT NOT NULL,"
    " PRIMARY KEY(symbol_id, ordinal)) WITHOUT ROWID;"
    "CREATE TABLE unit_symbol(unit_id INTEGER NOT NULL REFERENCES unit(id) ON DELETE CASCADE,"
    " symbol_id INTEGER NOT NULL REFERENCES symbol(id), placeholder INTEGER NOT NULL,"
    " PRIMARY KEY(unit_id, symbol_id)) WITHOUT ROWID;"
    "CREATE INDEX unit_symbol_by_symbol ON unit_symbol(symbol_id);"
    "CREATE TABLE symbol_location(id INTEGER PRIMARY KEY,"
    " unit_id INTEGER NOT NULL REFERENCES unit(id) ON DELETE CASCADE,"
    " symbol_id INTEGER NOT NULL REFERENCES symbol(id), role TEXT NOT NULL, " LCM_LOC_COLUMNS ","
    " body_hash BLOB, explicitly_defaulted INTEGER NOT NULL, deleted_as_written INTEGER NOT NULL,"
    " implicitly_deleted INTEGER NOT NULL, trivial INTEGER NOT NULL);"
    "CREATE INDEX symbol_location_by_unit ON symbol_location(unit_id);"
    "CREATE INDEX symbol_location_by_symbol ON symbol_location(symbol_id);"
    "CREATE INDEX symbol_location_by_file ON symbol_location(file_id, version_id);"
    "CREATE TABLE relation(id INTEGER PRIMARY KEY, profile_id INTEGER NOT NULL REFERENCES profile(id),"
    " source_id INTEGER NOT NULL REFERENCES symbol(id), target_id INTEGER NOT NULL REFERENCES symbol(id),"
    " kind TEXT NOT NULL, confidence TEXT NOT NULL, UNIQUE(profile_id, source_id, target_id, kind, confidence));"
    "CREATE INDEX relation_by_source ON relation(source_id);"
    "CREATE INDEX relation_by_target ON relation(target_id);"
    "CREATE TABLE evidence(id INTEGER PRIMARY KEY, relation_id INTEGER NOT NULL REFERENCES relation(id),"
    " unit_id INTEGER NOT NULL REFERENCES unit(id) ON DELETE CASCADE, kind TEXT NOT NULL, " LCM_LOC_COLUMNS ","
    " provider_key TEXT, evaluation TEXT NOT NULL, dispatch TEXT NOT NULL, template_use TEXT NOT NULL,"
    " immediately_invoked_lambda INTEGER NOT NULL, template_arguments TEXT NOT NULL,"
    " subobject_role TEXT NOT NULL, subobject TEXT NOT NULL, lifetime TEXT NOT NULL,"
    " potentially_elided INTEGER NOT NULL, base_access TEXT, base_access_written INTEGER,"
    " base_virtual INTEGER, base_ordinal INTEGER);"
    "CREATE INDEX evidence_by_unit ON evidence(unit_id);"
    "CREATE INDEX evidence_by_relation ON evidence(relation_id);"
    "CREATE INDEX evidence_by_file ON evidence(file_id, version_id);"
    "CREATE TABLE unresolved(id INTEGER PRIMARY KEY, unit_id INTEGER NOT NULL REFERENCES unit(id) ON DELETE CASCADE,"
    " enclosing_id INTEGER NOT NULL REFERENCES symbol(id), expression TEXT NOT NULL, " LCM_LOC_COLUMNS ","
    " reason TEXT NOT NULL);"
    "CREATE INDEX unresolved_by_unit ON unresolved(unit_id);"
    "CREATE INDEX unresolved_by_enclosing ON unresolved(enclosing_id);"
    "CREATE INDEX unresolved_by_file ON unresolved(file_id, version_id);"
    "CREATE TABLE capture(id INTEGER PRIMARY KEY, unit_id INTEGER NOT NULL REFERENCES unit(id) ON DELETE CASCADE,"
    " lambda_id INTEGER NOT NULL REFERENCES symbol(id), kind TEXT NOT NULL, explicit_capture INTEGER NOT NULL,"
    " init_capture INTEGER NOT NULL, pack_expansion INTEGER NOT NULL, name TEXT NOT NULL,"
    " target_descriptor TEXT NOT NULL, " LCM_LOC_COLUMNS ");"
    "CREATE INDEX capture_by_unit ON capture(unit_id);"
    "CREATE INDEX capture_by_lambda ON capture(lambda_id);"
    "CREATE INDEX capture_by_file ON capture(file_id, version_id);"
    "CREATE TABLE direct_include(id INTEGER PRIMARY KEY,"
    " unit_id INTEGER NOT NULL REFERENCES unit(id) ON DELETE CASCADE,"
    " includer_file_id INTEGER REFERENCES file(id), includer_as_written TEXT NOT NULL,"
    " directive_kind TEXT NOT NULL, operand_kind TEXT NOT NULL, operand TEXT NOT NULL, expanded TEXT,"
    " activity TEXT NOT NULL, resolution TEXT NOT NULL, target_file_id INTEGER REFERENCES file(id),"
    " target_as_written TEXT, dependency_key TEXT NOT NULL, " LCM_LOC_COLUMNS ");"
    "CREATE INDEX direct_include_by_unit ON direct_include(unit_id);"
    "CREATE INDEX direct_include_by_file ON direct_include(file_id, version_id);"
    "CREATE INDEX direct_include_by_includer ON direct_include(includer_file_id);"
    "CREATE INDEX direct_include_by_target ON direct_include(target_file_id);"
    "CREATE TABLE include_branch(include_id INTEGER NOT NULL REFERENCES direct_include(id) ON DELETE CASCADE,"
    " ordinal INTEGER NOT NULL, is_else INTEGER NOT NULL, PRIMARY KEY(include_id, ordinal)) WITHOUT ROWID;"
    // term_ordinal -1 is the branch's own condition; 0.. are preceding branches in order.
    "CREATE TABLE include_term(include_id INTEGER NOT NULL REFERENCES direct_include(id) ON DELETE CASCADE,"
    " branch_ordinal INTEGER NOT NULL, term_ordinal INTEGER NOT NULL, kind TEXT NOT NULL,"
    " expression TEXT NOT NULL, " LCM_LOC_COLUMNS ", PRIMARY KEY(include_id, branch_ordinal, term_ordinal))"
    " WITHOUT ROWID;"
    "CREATE INDEX include_term_by_file ON include_term(file_id, version_id);";

// Per-connection scratch sets used while removing a unit.
constexpr const char* kTempSchema =
    "CREATE TEMP TABLE IF NOT EXISTS touched_symbol(id INTEGER PRIMARY KEY);"
    "CREATE TEMP TABLE IF NOT EXISTS touched_relation(id INTEGER PRIMARY KEY);"
    "CREATE TEMP TABLE IF NOT EXISTS touched_file(id INTEGER PRIMARY KEY);"
    "CREATE TEMP TABLE IF NOT EXISTS touched_version(id INTEGER PRIMARY KEY);"
    "CREATE TEMP TABLE IF NOT EXISTS touched_dependency(id INTEGER PRIMARY KEY);"
    "CREATE TEMP TABLE IF NOT EXISTS touched_site(id INTEGER PRIMARY KEY);";

// ---------------------------------------------------------------------------
// Enum text tags. Values are stored as the same stable text tags the fact
// hash uses, so renumbering an enum never reinterprets a stored row.

template <typename E>
E parse_enum(std::string_view text, int count, const char* what) {
  for (int i = 0; i < count; ++i) {
    if (to_string(static_cast<E>(i)) == text) return static_cast<E>(i);
  }
  throw StoreError(std::string("corrupt store: unknown ") + what + " '" + std::string(text) + "'");
}

std::int64_t to_epoch(Timestamp t) { return t.time_since_epoch().count(); }
Timestamp from_epoch(std::int64_t s) { return Timestamp{std::chrono::seconds{s}}; }
Timestamp now_seconds() { return std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()); }

void put(std::string& out, std::string_view value) {
  out.append(std::to_string(value.size()));
  out.push_back(':');
  out.append(value);
  out.push_back(';');
}

std::string input_path_key(std::string_view path_utf8) { return path_key(path_from_utf8(path_utf8)); }

std::string join_lines(const std::vector<std::string>& lines) {
  std::string out;
  for (const auto& l : lines) {
    if (!out.empty()) out.push_back('\n');
    out += l;
  }
  return out;
}

std::vector<std::string> split_lines(const std::string& text) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t end = text.find('\n', start);
    out.push_back(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return out;
}

// ---------------------------------------------------------------------------
// Contribution validation (before any mutation).

bool acceptable_repo_path(const RepoRelativePath& p, bool allow_empty) {
  if (p.generic.empty()) return allow_empty;
  return is_canonical_repo_relative(p.generic);
}

std::string validate_contribution(const Contribution& c) {
  const std::string where = "slot '" + c.slot + "': ";
  if (c.fact_label.empty()) return where + "empty fact label";
  if (c.provenance.command_id.empty()) return where + "missing command identity";
  if (c.provenance.replay_id.empty()) return where + "missing replay identity";
  if (c.analysis_status != "ok" && c.analysis_status != "completed_with_errors") {
    return where + "analysis status must be ok or completed_with_errors, not '" + c.analysis_status + "'";
  }
  const auto owned = [&](const std::string& unit) { return unit == c.fact_label; };
  const auto& symbols = c.facts.symbols();
  for (const auto& [id, symbol] : symbols) {
    if (symbol.id != id) return where + "symbol map key differs from its id " + id.value;
    try {
      if (make_stable_id(symbol.key) != id) return where + "stable id does not match its canonical key: " + id.value;
    } catch (const std::exception& e) {
      return where + "invalid canonical key for " + id.value + ": " + e.what();
    }
    if (symbol.presence == SymbolPresence::external_placeholder && !symbol.locations.empty()) {
      return where + "placeholder with locations: " + id.value;
    }
    for (const auto& l : symbol.locations) {
      if (!owned(l.analysis_unit)) return where + "location of " + id.value + " owned by '" + l.analysis_unit + "'";
      if (!acceptable_repo_path(l.location.file, true)) return where + "non-canonical location file " + l.location.file.generic;
    }
  }
  const auto known = [&](const StableId& id) { return symbols.count(id) != 0; };
  for (const auto& [key, relation] : c.facts.relations()) {
    if (!known(key.source) || !known(key.target)) {
      return where + "relation endpoint without a symbol in the contribution: " + key.source.value + " -> " +
             key.target.value + " (" + std::string(to_string(key.kind)) + ", " +
             (!known(key.source) ? "source missing" : "source present") + ", " +
             (!known(key.target) ? "target missing" : "target present") +
             (relation.evidence.empty() ? "" : ", " + relation.evidence.front().location.file.generic) + ")";
    }
    for (const auto& e : relation.evidence) {
      if (!owned(e.analysis_unit)) return where + "evidence owned by '" + e.analysis_unit + "'";
      if (!acceptable_repo_path(e.location.file, true)) return where + "non-canonical evidence file " + e.location.file.generic;
    }
  }
  for (const auto& s : c.facts.unresolved_sites()) {
    if (!owned(s.analysis_unit)) return where + "unresolved site owned by '" + s.analysis_unit + "'";
    if (!known(s.enclosing)) return where + "unresolved owner without a symbol: " + s.enclosing.value;
  }
  for (const auto& cap : c.facts.captures()) {
    if (!owned(cap.analysis_unit)) return where + "capture owned by '" + cap.analysis_unit + "'";
    if (!known(cap.lambda)) return where + "capture lambda without a symbol: " + cap.lambda.value;
  }
  for (const auto& inc : c.facts.direct_includes()) {
    if (!owned(inc.analysis_unit)) return where + "direct include owned by '" + inc.analysis_unit + "'";
    if (!acceptable_repo_path(inc.includer, true)) return where + "non-canonical includer " + inc.includer.generic;
    if (inc.resolved_repo_target && !acceptable_repo_path(*inc.resolved_repo_target, false)) {
      return where + "non-canonical include target " + inc.resolved_repo_target->generic;
    }
  }
  std::set<std::string> keys;
  for (const auto& in : c.inputs) {
    if (in.path_utf8.empty()) return where + "input without a path";
    if (!keys.insert(input_path_key(in.path_utf8)).second) return where + "duplicate input " + in.path_utf8;
    if (in.repo_path && !acceptable_repo_path(*in.repo_path, false)) return where + "non-canonical input " + in.path_utf8;
  }
  return {};
}

std::string validate_batch(const Batch& batch) {
  std::set<std::string> slots;
  std::set<std::string> labels;
  for (const auto& c : batch.install) {
    if (c.slot.empty()) return "empty slot";
    if (!slots.insert(c.slot).second) return "slot '" + c.slot + "' appears more than once in the batch";
    if (!labels.insert(c.fact_label).second) return "fact label '" + c.fact_label + "' is used by two slots";
    if (auto why = validate_contribution(c); !why.empty()) return why;
  }
  // Spooled contributions were validated when they were added.
  if (!batch.spooled.empty() && !batch.spool) return "spooled installs without a spool";
  for (const std::size_t entry : batch.spooled) {
    if (entry >= batch.spool->size()) return "unknown spool entry " + std::to_string(entry);
    const std::string slot = batch.spool->slot(entry);
    if (!slots.insert(slot).second) return "slot '" + slot + "' appears more than once in the batch";
    if (!labels.insert(batch.spool->fact_label(entry)).second) {
      return "fact label '" + batch.spool->fact_label(entry) + "' is used by two slots";
    }
  }
  for (const auto& s : batch.retire) {
    if (s.empty()) return "empty slot";
    if (!slots.insert(s).second) return "slot '" + s + "' is both installed/retired or retired twice";
  }
  return {};
}

// ---------------------------------------------------------------------------
// Fingerprints.

std::vector<Fingerprint> derive_fingerprints(const Contribution& c) {
  std::vector<Fingerprint> out;
  {
    std::vector<std::string> rows;
    for (const auto& in : c.inputs) {
      std::string r;
      put(r, input_path_key(in.path_utf8));
      put(r, in.content_hash.digest.hex());
      put(r, std::to_string(in.content_hash.size));
      rows.push_back(std::move(r));
    }
    std::sort(rows.begin(), rows.end());
    std::string all;
    for (const auto& r : rows) all += r;
    out.push_back({FingerprintKind::file_bytes, sha256(all).hex()});
  }
  {
    std::string api;
    std::string body;
    for (const auto& [id, symbol] : c.facts.symbols()) {
      put(api, id.value);
      put(api, to_string(symbol.presence));
      std::set<std::string> decl;
      for (const auto& l : symbol.locations) {
        decl.insert(std::string(to_string(l.role)) + "@" + l.location.file.generic);
        if (l.body_hash) {
          put(body, id.value);
          put(body, l.body_hash->hex());
        }
      }
      for (const auto& d : decl) put(api, d);
    }
    out.push_back({FingerprintKind::declaration_api, sha256(api).hex()});
    out.push_back({FingerprintKind::symbol_body, sha256(body).hex()});
  }
  {
    std::string action;
    put(action, c.provenance.command_id);
    put(action, c.provenance.context_id.value_or(""));
    put(action, c.provenance.replay_id);
    put(action, c.provenance.pch_producer_command_id.value_or(""));
    put(action, c.provenance.pch_producer_replay_id.value_or(""));
    put(action, c.settings.analyzer);
    put(action, c.settings.resource_dir);
    put(action, c.settings.repository_member);
    put(action, c.settings.publish_policy);
    out.push_back({FingerprintKind::compile_action, sha256(action).hex()});
  }
  out.push_back({FingerprintKind::retrieval_document, std::nullopt});
  out.push_back({FingerprintKind::deep_input, std::nullopt});
  return out;
}

// ---------------------------------------------------------------------------
// File hashing for freshness checks.

struct CurrentFile {
  bool present = false;
  FileContentHash hash;
  friend bool operator==(const CurrentFile&, const CurrentFile&) = default;
};

// Hashes the file in chunks and gives up (nullopt) as soon as the deadline
// passes, so one large file cannot overrun the budget unobserved.
std::optional<CurrentFile> probe_file(const std::string& path_utf8, std::chrono::steady_clock::time_point deadline) {
  if (std::chrono::steady_clock::now() >= deadline) return std::nullopt;
  CurrentFile out;
  const fs::path p = path_from_utf8(path_utf8);
  std::error_code ec;
  if (!fs::is_regular_file(p, ec)) return out;
  std::ifstream in(p, std::ios::binary);
  if (!in) return out;
  Sha256 hash;
  std::uint64_t size = 0;
  std::vector<char> buffer(1u << 20);
  while (in) {
    in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto got = static_cast<std::size_t>(in.gcount());
    if (got == 0) break;
    hash.update(std::string_view(buffer.data(), got));
    size += got;
    if (std::chrono::steady_clock::now() >= deadline) return std::nullopt;
  }
  if (in.bad()) return out;  // unreadable counts as absent, never as unchanged
  out.present = true;
  out.hash = FileContentHash{hash.finish(), size};
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Enum names.

std::string_view to_string(JournalMode value) { return value == JournalMode::wal ? "wal" : "rollback"; }

// ---------------------------------------------------------------------------
// Contribution spool.

namespace {

bool is_spool_name(const fs::path& p) {
  const std::string name = path_to_utf8_generic(p.filename());
  return name.rfind("lcm-spool-", 0) == 0 && name.size() > 14 && name.compare(name.size() - 4, 4, ".bin") == 0;
}

// A spool file whose owner is gone is removed. A live owner holds its file
// exclusively (Windows: the only handle, no sharing; POSIX: an flock, and the
// name is already unlinked), so the exclusive open that proves the owner gone
// cannot succeed on a live spool. The OS deletes the spool of a process that
// exits or is killed; what remains after a power loss is reclaimed here.
void reclaim_orphan_spools(const fs::path& directory) {
  std::error_code ec;
  for (auto it = fs::directory_iterator(directory, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
    const fs::path p = it->path();
    if (!is_spool_name(p)) continue;
#ifdef _WIN32
    const HANDLE h = CreateFileW(p.c_str(), DELETE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);  // deleted as this, the only handle, closes
#else
    const int fd = ::open(p.c_str(), O_RDWR);
    if (fd < 0) continue;
    if (::flock(fd, LOCK_EX | LOCK_NB) == 0) ::unlink(p.c_str());
    ::close(fd);
#endif
  }
}

}  // namespace

struct ContributionSpool::Impl {
  struct Entry {
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
    Sha256Digest digest;
    std::string slot;
    std::string label;
  };
  fs::path path;
  mutable std::mutex mutex;
#ifdef _WIN32
  HANDLE file = INVALID_HANDLE_VALUE;
#else
  int file = -1;
#endif
  std::uint64_t end = 0;
  std::vector<Entry> entries;

  // Positioned I/O on the spool; false on any failure.
  bool write_at(std::uint64_t offset, const char* data, std::size_t size) {
#ifdef _WIN32
    LARGE_INTEGER at;
    at.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(file, at, nullptr, FILE_BEGIN)) return false;
    while (size > 0) {
      const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(size, 1u << 30));
      DWORD done = 0;
      if (!WriteFile(file, data, chunk, &done, nullptr) || done != chunk) return false;
      data += chunk;
      size -= chunk;
    }
    return true;
#else
    while (size > 0) {
      const ssize_t done = ::pwrite(file, data, size, static_cast<off_t>(offset));
      if (done <= 0) return false;
      data += done;
      size -= static_cast<std::size_t>(done);
      offset += static_cast<std::uint64_t>(done);
    }
    return true;
#endif
  }
  bool read_at(std::uint64_t offset, char* data, std::size_t size) const {
#ifdef _WIN32
    LARGE_INTEGER at;
    at.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(file, at, nullptr, FILE_BEGIN)) return false;
    while (size > 0) {
      const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(size, 1u << 30));
      DWORD done = 0;
      if (!ReadFile(file, data, chunk, &done, nullptr) || done != chunk) return false;
      data += chunk;
      size -= chunk;
    }
    return true;
#else
    while (size > 0) {
      const ssize_t done = ::pread(file, data, size, static_cast<off_t>(offset));
      if (done <= 0) return false;
      data += done;
      size -= static_cast<std::size_t>(done);
      offset += static_cast<std::uint64_t>(done);
    }
    return true;
#endif
  }
};

ContributionSpool::ContributionSpool(const fs::path& directory) : impl_(std::make_unique<Impl>()) {
  reclaim_orphan_spools(directory);
  static std::atomic<std::uint64_t> counter{0};
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  impl_->path = directory / path_from_utf8("lcm-spool-" + std::to_string(stamp) + "-" +
                                           std::to_string(counter.fetch_add(1)) + ".bin");
#ifdef _WIN32
  // The only handle, shared with nobody, deleted by the OS when it closes --
  // also when the owning process is killed.
  impl_->file = CreateFileW(impl_->path.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW,
                            FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
  if (impl_->file == INVALID_HANDLE_VALUE) {
    throw StoreError("cannot create the contribution spool " + path_to_utf8_generic(impl_->path));
  }
#else
  // Locked, then unlinked at once: the data lives exactly as long as the descriptor.
  impl_->file = ::open(impl_->path.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
  if (impl_->file < 0) throw StoreError("cannot create the contribution spool " + path_to_utf8_generic(impl_->path));
  ::flock(impl_->file, LOCK_EX);
  ::unlink(impl_->path.c_str());
#endif
}

ContributionSpool::~ContributionSpool() {
#ifdef _WIN32
  if (impl_->file != INVALID_HANDLE_VALUE) CloseHandle(impl_->file);  // deletes the file
#else
  if (impl_->file >= 0) ::close(impl_->file);
#endif
}

std::string ContributionSpool::add(const Contribution& contribution, std::size_t* entry) {
  if (auto why = validate_contribution(contribution); !why.empty()) return why;
  const std::string bytes = encode_contribution(contribution);
  Impl::Entry e;
  e.size = bytes.size();
  e.digest = sha256(bytes);
  e.slot = contribution.slot;
  e.label = contribution.fact_label;
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  e.offset = impl_->end;
  if (!impl_->write_at(e.offset, bytes.data(), bytes.size())) {
    throw StoreError("cannot write the contribution spool " + path_to_utf8_generic(impl_->path));
  }
  impl_->end += bytes.size();
  if (entry) *entry = impl_->entries.size();
  impl_->entries.push_back(std::move(e));
  return {};
}

std::size_t ContributionSpool::size() const {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->entries.size();
}

std::string ContributionSpool::slot(std::size_t entry) const {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->entries.at(entry).slot;
}

std::string ContributionSpool::fact_label(std::size_t entry) const {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->entries.at(entry).label;
}

Contribution ContributionSpool::load(std::size_t entry) const {
  std::string bytes;
  Sha256Digest digest;
  {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    const Impl::Entry& e = impl_->entries.at(entry);
    bytes.resize(static_cast<std::size_t>(e.size));
    if (!impl_->read_at(e.offset, bytes.data(), bytes.size())) {
      throw StoreError("cannot read the contribution spool " + path_to_utf8_generic(impl_->path));
    }
    digest = e.digest;
  }
  if (sha256(bytes) != digest) throw StoreError("the contribution spool changed after it was written");
  return decode_contribution(bytes);
}

const fs::path& ContributionSpool::path() const { return impl_->path; }

std::string_view to_string(InputRole value) {
  switch (value) {
    case InputRole::source: return "source";
    case InputRole::header: return "header";
    case InputRole::inline_file: return "inline_file";
    case InputRole::generated: return "generated";
    case InputRole::reflected_header: return "reflected_header";
    case InputRole::ignored_dependency: return "ignored_dependency";
    case InputRole::external_dependency: return "external_dependency";
    case InputRole::response_file: return "response_file";
    case InputRole::compiler: return "compiler";
    case InputRole::build_database: return "build_database";
    case InputRole::build_configuration: return "build_configuration";
    case InputRole::project_descriptor: return "project_descriptor";
  }
  return "unknown";
}

bool is_context_input(InputRole role) {
  switch (role) {
    case InputRole::response_file:
    case InputRole::compiler:
    case InputRole::build_database:
    case InputRole::build_configuration:
    case InputRole::project_descriptor:
      return true;
    default:
      return false;
  }
}

std::string_view to_string(BuildCurrency value) { return value == BuildCurrency::verified ? "verified" : "unknown"; }

std::string_view to_string(FingerprintKind value) {
  switch (value) {
    case FingerprintKind::file_bytes: return "file_bytes";
    case FingerprintKind::declaration_api: return "declaration_api";
    case FingerprintKind::symbol_body: return "symbol_body";
    case FingerprintKind::compile_action: return "compile_action";
    case FingerprintKind::retrieval_document: return "retrieval_document";
    case FingerprintKind::deep_input: return "deep_input";
  }
  return "unknown";
}

std::string_view to_string(AttemptState value) {
  switch (value) {
    case AttemptState::in_progress: return "in_progress";
    case AttemptState::succeeded: return "succeeded";
    case AttemptState::failed: return "failed";
    case AttemptState::interrupted: return "interrupted";
  }
  return "unknown";
}

std::string_view to_string(PublishOutcome value) {
  switch (value) {
    case PublishOutcome::committed: return "committed";
    case PublishOutcome::no_op: return "no_op";
    case PublishOutcome::rejected: return "rejected";
    case PublishOutcome::failed: return "failed";
  }
  return "unknown";
}

std::string_view to_string(InspectionOutcome value) {
  switch (value) {
    case InspectionOutcome::complete: return "complete";
    case InspectionOutcome::budget_exhausted: return "budget_exhausted";
    case InspectionOutcome::lock_busy: return "lock_busy";
    case InspectionOutcome::generation_changed: return "generation_changed";
  }
  return "unknown";
}

std::string_view to_string(ChangeKind value) {
  switch (value) {
    case ChangeKind::modified: return "modified";
    case ChangeKind::deleted: return "deleted";
    case ChangeKind::changed_during_check: return "changed_during_check";
  }
  return "unknown";
}

// ---------------------------------------------------------------------------
// Writer lock.

struct WriterLock::Handle {
#ifdef _WIN32
  HANDLE file = INVALID_HANDLE_VALUE;
  ~Handle() {
    if (file != INVALID_HANDLE_VALUE) {
      OVERLAPPED ov{};
      UnlockFileEx(file, 0, 1, 0, &ov);
      CloseHandle(file);
    }
  }
#else
  int fd = -1;
  ~Handle() {
    if (fd >= 0) {
      flock(fd, LOCK_UN);
      close(fd);
    }
  }
#endif
};

WriterLock::WriterLock(std::unique_ptr<Handle> handle, const void* owner) : handle_(std::move(handle)), owner_(owner) {}
WriterLock::WriterLock(WriterLock&&) noexcept = default;
WriterLock& WriterLock::operator=(WriterLock&&) noexcept = default;
WriterLock::~WriterLock() = default;

namespace {

// One non-blocking attempt to take the exclusive byte-range lock.
std::unique_ptr<WriterLock::Handle> try_lock_file(const fs::path& lock_path);

}  // namespace

// ---------------------------------------------------------------------------
// Store implementation.

struct Store::Impl {
  fs::path db_path;
  fs::path lock_path;
  std::string workspace_root;
  StoreOptions options;
  JournalMode mode = JournalMode::wal;
  sqlite3* db = nullptr;
  std::unordered_map<std::string, std::unique_ptr<Stmt>> cache;

  // Rows the insert phase of one publication looked up or created, so a row
  // shared by many units (symbol, relation, file, file version, dependency,
  // include site) is asked of SQLite once per publication instead of once per
  // unit. Valid from the first insert to the end of that transaction: the
  // insert phase never deletes. Reset by publish() on every exit.
  struct InsertCache {
    std::unordered_map<std::string, std::int64_t> files;  // file paths and file_version keys
    std::unordered_map<std::string, std::pair<std::int64_t, CanonicalKey>> symbols;  // stable id -> row, key
    std::map<std::tuple<std::int64_t, std::int64_t, int, int>, std::int64_t> relations;
    std::unordered_map<std::string, std::int64_t> dependencies;
    std::unordered_map<std::string, std::int64_t> sites;
  };
  std::optional<InsertCache> insert_cache;
  InsertCache& inserts() {
    if (!insert_cache) insert_cache.emplace();
    return *insert_cache;
  }
  // Unambiguous key of several fields (length-prefixed).
  static void key_part(std::string& key, std::string_view v) {
    key += std::to_string(v.size());
    key += ':';
    key += v;
  }
  static void key_optional(std::string& key, const std::optional<std::string>& v) {
    if (v) {
      key_part(key, *v);
    } else {
      key += '-';
    }
  }

  ~Impl() {
    cache.clear();
    if (db) sqlite3_close_v2(db);
  }

  Stmt& q(const std::string& sql) {
    auto it = cache.find(sql);
    if (it == cache.end()) it = cache.emplace(sql, std::make_unique<Stmt>(db, sql)).first;
    return it->second->reset();
  }

  // A cached statement left on a row keeps an implicit read transaction open:
  // a SHARED lock in rollback mode and a pinned, aging snapshot in WAL mode.
  // Every public operation starts and ends with no active statement.
  void quiesce() {
    for (sqlite3_stmt* p = sqlite3_next_stmt(db, nullptr); p; p = sqlite3_next_stmt(db, p)) {
      if (sqlite3_stmt_busy(p)) sqlite3_reset(p);
    }
  }

  struct Quiet {
    Impl& impl;
    explicit Quiet(Impl& i) : impl(i) { impl.quiesce(); }
    Quiet(const Quiet&) = delete;
    Quiet& operator=(const Quiet&) = delete;
    ~Quiet() { impl.quiesce(); }
  };

  std::int64_t profile_id(std::string_view profile) {
    auto& s = q("SELECT id FROM profile WHERE key=?");
    s.b(profile);
    if (!s.step()) throw std::invalid_argument("unknown profile '" + std::string(profile) + "'; call ensure_profile first");
    return s.i64(0);
  }

  std::int64_t generation(std::int64_t profile) {
    auto& s = q("SELECT fact_generation FROM profile WHERE id=?");
    s.b(profile);
    return s.step() ? s.i64(0) : 0;
  }

  // Attempt bookkeeping in its own transaction.
  std::int64_t record_attempt(std::int64_t profile, Layer layer, AttemptState state, std::string_view message,
                              const std::vector<std::pair<std::string, std::string>>& slots) {
    exec(db, "BEGIN IMMEDIATE");
    try {
      const Timestamp now = now_seconds();
      auto& a = q("INSERT INTO attempt(profile_id, layer, state, started_at, finished_at, message) VALUES(?,?,?,?,?,?)");
      a.b(profile).b(to_string(layer)).b(to_string(state)).b(to_epoch(now));
      if (state == AttemptState::in_progress) {
        a.b(nullptr);
      } else {
        a.b(to_epoch(now));
      }
      a.b(message).run();
      const std::int64_t id = sqlite3_last_insert_rowid(db);
      for (const auto& [slot, action] : slots) {
        q("INSERT INTO slot_attempt(attempt_id, slot, action) VALUES(?,?,?)").b(id).b(slot).b(action).run();
      }
      exec(db, "COMMIT");
      return id;
    } catch (...) {
      exec_noexcept(db, "ROLLBACK");
      throw;
    }
  }

  void finish_attempt(std::int64_t attempt, AttemptState state, std::string_view message) {
    q("UPDATE attempt SET state=?, finished_at=?, message=? WHERE id=?")
        .b(to_string(state))
        .b(to_epoch(now_seconds()))
        .b(message)
        .b(attempt)
        .run();
  }

  std::optional<std::int64_t> file_id(std::int64_t profile, const std::string& generic,
                                      std::unordered_map<std::string, std::int64_t>& memo) {
    if (generic.empty()) return std::nullopt;
    if (auto it = memo.find(generic); it != memo.end()) return it->second;
    auto& sel = q("SELECT id FROM file WHERE profile_id=? AND path=?");
    sel.b(profile).b(generic);
    std::int64_t id = 0;
    if (sel.step()) {
      id = sel.i64(0);
    } else {
      q("INSERT INTO file(profile_id, path) VALUES(?,?)").b(profile).b(generic).run();
      id = sqlite3_last_insert_rowid(db);
    }
    memo.emplace(generic, id);
    return id;
  }

  // The file_version row of (file, spelling as written, digest, size), created on first use.
  std::int64_t version_id(std::int64_t profile, std::optional<std::int64_t> file, const SourceLocation& l,
                          std::unordered_map<std::string, std::int64_t>& memo) {
    // Raw bytes, marked so it can never equal a file path in the same memo.
    std::string key;
    key.reserve(1 + 8 + 32 + 8 + l.file.as_written.size());
    key.push_back('\x01');
    const std::int64_t file_part = file.value_or(-1);
    const std::uint64_t size_part = l.content_hash.size;
    key.append(reinterpret_cast<const char*>(&file_part), sizeof(file_part));
    key.append(reinterpret_cast<const char*>(l.content_hash.digest.bytes.data()), l.content_hash.digest.bytes.size());
    key.append(reinterpret_cast<const char*>(&size_part), sizeof(size_part));
    key += l.file.as_written;
    if (auto it = memo.find(key); it != memo.end()) return it->second;
    auto& sel = q("SELECT id FROM file_version WHERE profile_id=? AND file_id IS ? AND digest=? AND size=?"
                  " AND path_as_written=?");
    sel.b(profile).b(file).b(l.content_hash.digest).b(l.content_hash.size).b(l.file.as_written);
    std::int64_t id = 0;
    if (sel.step()) {
      id = sel.i64(0);
    } else {
      q("INSERT INTO file_version(profile_id, file_id, path_as_written, digest, size) VALUES(?,?,?,?,?)")
          .b(profile).b(file).b(l.file.as_written).b(l.content_hash.digest).b(l.content_hash.size).run();
      id = sqlite3_last_insert_rowid(db);
    }
    memo.emplace(std::move(key), id);
    return id;
  }

  void bind_location(Stmt& s, std::int64_t profile, const SourceLocation& l,
                     std::unordered_map<std::string, std::int64_t>& memo) {
    const std::optional<std::int64_t> file = file_id(profile, l.file.generic, memo);
    s.b(file)
        .b(version_id(profile, file, l, memo))
        .b(l.span.begin_offset)
        .b(l.span.end_offset)
        .b(l.span.begin_line)
        .b(l.span.begin_column)
        .b(l.span.end_line)
        .b(l.span.end_column)
        .b(l.span_hash);
  }

  std::int64_t symbol_row(std::int64_t profile, const StableId& id, const CanonicalKey& key) {
    // A key already seen in this publication is compared as a key (equal keys
    // serialize equally); only a first sighting is serialized and checked
    // against the stored serialization.
    auto& known = inserts().symbols;
    if (const auto it = known.find(id.value); it != known.end()) {
      if (!(it->second.second == key)) throw StoreError("stable id collision between different canonical keys: " + id.value);
      return it->second.first;
    }
    const std::string serialized = serialize_canonical_key(key);
    auto& sel = q("SELECT id, key_serialized FROM symbol WHERE profile_id=? AND stable_id=?");
    sel.b(profile).b(id.value);
    if (sel.step()) {
      if (sel.text(1) != serialized) throw StoreError("stable id collision between different canonical keys: " + id.value);
      const std::int64_t found = sel.i64(0);
      known.emplace(id.value, std::make_pair(found, key));
      return found;
    }
    auto& ins = q(
        "INSERT INTO symbol(profile_id, stable_id, key_serialized, repository_member, language, kind, canonical_name,"
        " normalized_signature, template_role, template_parameters, template_arguments, linkage, linkage_file,"
        " linkage_file_as_written, linkage_enclosing, linkage_anchor, linkage_ordinal)"
        " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
    ins.b(profile)
        .b(id.value)
        .b(serialized)
        .b(key.repository_member)
        .b(to_string(key.language))
        .b(to_string(key.kind))
        .b(key.canonical_name)
        .b(key.normalized_signature)
        .b(to_string(key.template_role))
        .b(key.template_parameters)
        .b(key.template_arguments);
    if (const auto* internal = std::get_if<InternalLinkage>(&key.linkage)) {
      ins.b("internal").b(internal->file_scope.generic).b(internal->file_scope.as_written).b(nullptr).b(nullptr).b(nullptr);
    } else if (const auto* local = std::get_if<LocalScope>(&key.linkage)) {
      ins.b("local").b(nullptr).b(nullptr).b(local->enclosing.value).b(local->anchor).b(local->ordinal);
    } else {
      ins.b("external").b(nullptr).b(nullptr).b(nullptr).b(nullptr).b(nullptr);
    }
    ins.run();
    const std::int64_t row = sqlite3_last_insert_rowid(db);
    for (std::size_t i = 0; i < key.owner_chain.size(); ++i) {
      const auto& o = key.owner_chain[i];
      q("INSERT INTO symbol_owner(symbol_id, ordinal, kind, name, normalized_signature, template_role,"
        " template_parameters, template_arguments) VALUES(?,?,?,?,?,?,?,?)")
          .b(row)
          .b(static_cast<std::int64_t>(i))
          .b(to_string(o.kind))
          .b(o.name)
          .b(o.normalized_signature)
          .b(to_string(o.template_role))
          .b(o.template_parameters)
          .b(o.template_arguments)
          .run();
    }
    known.emplace(id.value, std::make_pair(row, key));
    return row;
  }

  // Deletes one unit's contribution and prunes common rows it was the last
  // owner of. Returns false when the slot has no committed unit.
  bool remove_unit(std::int64_t profile, const std::string& slot) {
    auto& sel = q("SELECT id FROM unit WHERE profile_id=? AND slot=?");
    sel.b(profile).b(slot);
    if (!sel.step()) return false;
    const std::int64_t unit = sel.i64(0);
    exec(db,
         "DELETE FROM temp.touched_symbol; DELETE FROM temp.touched_relation; DELETE FROM temp.touched_file;"
         " DELETE FROM temp.touched_version; DELETE FROM temp.touched_dependency; DELETE FROM temp.touched_site;");
    q("INSERT OR IGNORE INTO temp.touched_version"
      " SELECT version_id FROM symbol_location WHERE unit_id=?1"
      " UNION SELECT version_id FROM evidence WHERE unit_id=?1"
      " UNION SELECT version_id FROM unresolved WHERE unit_id=?1"
      " UNION SELECT version_id FROM capture WHERE unit_id=?1"
      " UNION SELECT version_id FROM direct_include WHERE unit_id=?1"
      " UNION SELECT t.version_id FROM include_term t JOIN direct_include d ON d.id=t.include_id WHERE d.unit_id=?1")
        .b(unit)
        .run();
    q("INSERT OR IGNORE INTO temp.touched_dependency SELECT dependency_id FROM input WHERE unit_id=?").b(unit).run();
    q("INSERT OR IGNORE INTO temp.touched_site SELECT site_id FROM include_edge WHERE unit_id=?").b(unit).run();
    q("INSERT OR IGNORE INTO temp.touched_symbol SELECT symbol_id FROM unit_symbol WHERE unit_id=?").b(unit).run();
    q("INSERT OR IGNORE INTO temp.touched_relation SELECT relation_id FROM evidence WHERE unit_id=?").b(unit).run();
    q("INSERT OR IGNORE INTO temp.touched_file"
      " SELECT file_id FROM symbol_location WHERE unit_id=?1 AND file_id IS NOT NULL"
      " UNION SELECT file_id FROM evidence WHERE unit_id=?1 AND file_id IS NOT NULL"
      " UNION SELECT file_id FROM unresolved WHERE unit_id=?1 AND file_id IS NOT NULL"
      " UNION SELECT file_id FROM capture WHERE unit_id=?1 AND file_id IS NOT NULL"
      " UNION SELECT file_id FROM direct_include WHERE unit_id=?1 AND file_id IS NOT NULL"
      " UNION SELECT includer_file_id FROM direct_include WHERE unit_id=?1 AND includer_file_id IS NOT NULL"
      " UNION SELECT target_file_id FROM direct_include WHERE unit_id=?1 AND target_file_id IS NOT NULL"
      " UNION SELECT t.file_id FROM include_term t JOIN direct_include d ON d.id=t.include_id"
      "   WHERE d.unit_id=?1 AND t.file_id IS NOT NULL")
        .b(unit)
        .run();
    q("DELETE FROM unit WHERE id=?").b(unit).run();  // cascades to every unit-owned row
    exec(db,
         "DELETE FROM relation WHERE id IN (SELECT id FROM temp.touched_relation)"
         " AND NOT EXISTS (SELECT 1 FROM evidence e WHERE e.relation_id=relation.id);"
         "DELETE FROM symbol WHERE id IN (SELECT id FROM temp.touched_symbol)"
         " AND NOT EXISTS (SELECT 1 FROM unit_symbol us WHERE us.symbol_id=symbol.id);"
         "DELETE FROM file_version WHERE id IN (SELECT id FROM temp.touched_version)"
         " AND NOT EXISTS (SELECT 1 FROM symbol_location x WHERE x.file_id IS file_version.file_id"
         "   AND x.version_id=file_version.id)"
         " AND NOT EXISTS (SELECT 1 FROM evidence x WHERE x.file_id IS file_version.file_id"
         "   AND x.version_id=file_version.id)"
         " AND NOT EXISTS (SELECT 1 FROM unresolved x WHERE x.file_id IS file_version.file_id"
         "   AND x.version_id=file_version.id)"
         " AND NOT EXISTS (SELECT 1 FROM capture x WHERE x.file_id IS file_version.file_id"
         "   AND x.version_id=file_version.id)"
         " AND NOT EXISTS (SELECT 1 FROM direct_include x WHERE x.file_id IS file_version.file_id"
         "   AND x.version_id=file_version.id)"
         " AND NOT EXISTS (SELECT 1 FROM include_term x WHERE x.file_id IS file_version.file_id"
         "   AND x.version_id=file_version.id);"
         "DELETE FROM dependency WHERE id IN (SELECT id FROM temp.touched_dependency)"
         " AND NOT EXISTS (SELECT 1 FROM input x WHERE x.dependency_id=dependency.id);"
         "DELETE FROM include_site WHERE id IN (SELECT id FROM temp.touched_site)"
         " AND NOT EXISTS (SELECT 1 FROM include_edge x WHERE x.site_id=include_site.id);"
         "DELETE FROM file WHERE id IN (SELECT id FROM temp.touched_file)"
         " AND NOT EXISTS (SELECT 1 FROM symbol_location x WHERE x.file_id=file.id)"
         " AND NOT EXISTS (SELECT 1 FROM evidence x WHERE x.file_id=file.id)"
         " AND NOT EXISTS (SELECT 1 FROM unresolved x WHERE x.file_id=file.id)"
         " AND NOT EXISTS (SELECT 1 FROM capture x WHERE x.file_id=file.id)"
         " AND NOT EXISTS (SELECT 1 FROM direct_include x WHERE x.file_id=file.id OR x.includer_file_id=file.id"
         "   OR x.target_file_id=file.id)"
         " AND NOT EXISTS (SELECT 1 FROM include_term x WHERE x.file_id=file.id);");
    return true;
  }

  void insert_unit(std::int64_t profile, std::int64_t generation, std::int64_t attempt, const Contribution& c) {
    {
      auto& lab = q("SELECT slot FROM unit WHERE profile_id=? AND fact_label=?");
      lab.b(profile).b(c.fact_label);
      if (lab.step()) {
        throw StoreError("fact label '" + c.fact_label + "' is already owned by live slot '" + lab.text(0) + "'");
      }
    }
    q("INSERT INTO unit(profile_id, slot, fact_label, command_id, context_id, replay_id, pch_producer_command_id,"
      " pch_producer_replay_id, analyzer, resource_dir, repository_member, publish_policy, analysis_status,"
      " build_currency, committed_generation, attempt_id)"
      " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)")
        .b(profile)
        .b(c.slot)
        .b(c.fact_label)
        .b(c.provenance.command_id)
        .b(c.provenance.context_id)
        .b(c.provenance.replay_id)
        .b(c.provenance.pch_producer_command_id)
        .b(c.provenance.pch_producer_replay_id)
        .b(c.settings.analyzer)
        .b(c.settings.resource_dir)
        .b(c.settings.repository_member)
        .b(c.settings.publish_policy)
        .b(c.analysis_status)
        .b(to_string(c.build_currency))
        .b(generation)
        .b(attempt)
        .run();
    const std::int64_t unit = sqlite3_last_insert_rowid(db);

    for (std::size_t i = 0; i < c.diagnostics.size(); ++i) {
      const auto& d = c.diagnostics[i];
      q("INSERT INTO unit_diagnostic(unit_id, ordinal, level, message, file, line, col) VALUES(?,?,?,?,?,?,?)")
          .b(unit).b(static_cast<std::int64_t>(i)).b(d.level).b(d.message).b(d.file)
          .b(static_cast<std::int64_t>(d.line)).b(static_cast<std::int64_t>(d.column)).run();
    }
    for (std::size_t i = 0; i < c.limits.size(); ++i) {
      q("INSERT INTO unit_limit(unit_id, ordinal, text) VALUES(?,?,?)")
          .b(unit).b(static_cast<std::int64_t>(i)).b(c.limits[i]).run();
    }
    for (const auto& f : derive_fingerprints(c)) {
      q("INSERT INTO unit_fingerprint(unit_id, kind, value) VALUES(?,?,?)").b(unit).b(to_string(f.kind)).b(f.value).run();
    }
    for (const auto& in : c.inputs) {
      const std::string path_key = input_path_key(in.path_utf8);
      const std::optional<std::string> repo = in.repo_path ? std::optional<std::string>(in.repo_path->generic) : std::nullopt;
      const std::optional<std::string> repo_written =
          in.repo_path ? std::optional<std::string>(in.repo_path->as_written) : std::nullopt;
      std::string cache_key;
      key_part(cache_key, path_key);
      key_part(cache_key, in.path_utf8);
      key_optional(cache_key, repo);
      key_optional(cache_key, repo_written);
      key_part(cache_key, to_string(in.role));
      key_part(cache_key, in.content_hash.digest.hex());
      key_part(cache_key, std::to_string(in.content_hash.size) + (in.is_system ? "s" : "-"));
      auto [known, fresh] = inserts().dependencies.try_emplace(std::move(cache_key), 0);
      if (fresh) {
        auto& sel = q("SELECT id FROM dependency WHERE path_key=? AND digest=? AND path=? AND repo_path IS ?"
                      " AND repo_path_as_written IS ? AND role=? AND size=? AND is_system=?");
        sel.b(path_key).b(in.content_hash.digest).b(in.path_utf8).b(repo).b(repo_written).b(to_string(in.role))
            .b(in.content_hash.size).b(in.is_system);
        if (sel.step()) {
          known->second = sel.i64(0);
        } else {
          q("INSERT INTO dependency(path_key, path, repo_path, repo_path_as_written, role, digest, size, is_system)"
            " VALUES(?,?,?,?,?,?,?,?)")
              .b(path_key).b(in.path_utf8).b(repo).b(repo_written).b(to_string(in.role)).b(in.content_hash.digest)
              .b(in.content_hash.size).b(in.is_system).run();
          known->second = sqlite3_last_insert_rowid(db);
        }
      }
      q("INSERT INTO input(unit_id, dependency_id) VALUES(?,?)").b(unit).b(known->second).run();
    }
    for (std::size_t i = 0; i < c.includes.size(); ++i) {
      const auto& e = c.includes[i];
      std::string cache_key;
      key_optional(cache_key, e.includer_path_utf8);
      key_part(cache_key, e.spelling);
      key_optional(cache_key, e.resolved_path_utf8);
      key_part(cache_key, std::to_string(e.line) + (e.angled ? "a" : "q"));
      auto [known, fresh] = inserts().sites.try_emplace(std::move(cache_key), 0);
      if (fresh) {
        auto& sel = q("SELECT id FROM include_site WHERE spelling=? AND line=? AND includer IS ? AND angled=?"
                      " AND resolved IS ?");
        sel.b(e.spelling).b(static_cast<std::int64_t>(e.line)).b(e.includer_path_utf8).b(e.angled).b(e.resolved_path_utf8);
        if (sel.step()) {
          known->second = sel.i64(0);
        } else {
          q("INSERT INTO include_site(includer, spelling, angled, resolved, line) VALUES(?,?,?,?,?)")
              .b(e.includer_path_utf8).b(e.spelling).b(e.angled).b(e.resolved_path_utf8)
              .b(static_cast<std::int64_t>(e.line)).run();
          known->second = sqlite3_last_insert_rowid(db);
        }
      }
      q("INSERT INTO include_edge(unit_id, ordinal, site_id) VALUES(?,?,?)")
          .b(unit).b(static_cast<std::int64_t>(i)).b(known->second).run();
    }

    auto& files = inserts().files;
    std::map<StableId, std::int64_t> rows;
    for (const auto& [id, symbol] : c.facts.symbols()) {
      const std::int64_t row = symbol_row(profile, id, symbol.key);
      rows.emplace(id, row);
      q("INSERT INTO unit_symbol(unit_id, symbol_id, placeholder) VALUES(?,?,?)")
          .b(unit).b(row).b(symbol.presence == SymbolPresence::external_placeholder).run();
      for (const auto& l : symbol.locations) {
        static const std::string sql = std::string("INSERT INTO symbol_location(unit_id, symbol_id, role, ") +
                                       kLocInsertColumns +
                                       ", body_hash, explicitly_defaulted, deleted_as_written, implicitly_deleted,"
                                       " trivial) VALUES(?,?,?," +
                                       kLocPlaceholders + ",?,?,?,?,?)";
        auto& s = q(sql);
        s.b(unit).b(row).b(to_string(l.role));
        bind_location(s, profile, l.location, files);
        s.b(l.body_hash)
            .b(l.callable.explicitly_defaulted)
            .b(l.callable.deleted_as_written)
            .b(l.callable.implicitly_deleted)
            .b(l.callable.trivial)
            .run();
      }
    }
    for (const auto& [key, relation] : c.facts.relations()) {
      const std::int64_t source = rows.at(key.source);
      const std::int64_t target = rows.at(key.target);
      auto [known, fresh] = inserts().relations.try_emplace(
          std::make_tuple(source, target, static_cast<int>(key.kind), static_cast<int>(key.confidence)), 0);
      if (fresh) {
        auto& sel =
            q("SELECT id FROM relation WHERE profile_id=? AND source_id=? AND target_id=? AND kind=? AND confidence=?");
        sel.b(profile).b(source).b(target).b(to_string(key.kind)).b(to_string(key.confidence));
        if (sel.step()) {
          known->second = sel.i64(0);
        } else {
          q("INSERT INTO relation(profile_id, source_id, target_id, kind, confidence) VALUES(?,?,?,?,?)")
              .b(profile).b(source).b(target).b(to_string(key.kind)).b(to_string(key.confidence)).run();
          known->second = sqlite3_last_insert_rowid(db);
        }
      }
      const std::int64_t rel = known->second;
      for (const auto& e : relation.evidence) {
        static const std::string sql =
            std::string("INSERT INTO evidence(relation_id, unit_id, kind, ") + kLocInsertColumns +
            ", provider_key, evaluation, dispatch, template_use, immediately_invoked_lambda, template_arguments,"
            " subobject_role, subobject, lifetime, potentially_elided, base_access, base_access_written,"
            " base_virtual, base_ordinal) VALUES(?,?,?," +
            kLocPlaceholders + ",?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
        auto& s = q(sql);
        s.b(rel).b(unit).b(to_string(e.kind));
        bind_location(s, profile, e.location, files);
        s.b(e.provider_key)
            .b(to_string(e.evaluation))
            .b(to_string(e.dispatch))
            .b(to_string(e.template_use))
            .b(e.immediately_invoked_lambda)
            .b(e.template_arguments)
            .b(to_string(e.subobject_role))
            .b(e.subobject)
            .b(to_string(e.lifetime))
            .b(e.potentially_elided);
        if (e.base) {
          s.b(to_string(e.base->effective_access)).b(e.base->access_written).b(e.base->is_virtual).b(e.base->lexical_ordinal);
        } else {
          s.b(nullptr).b(nullptr).b(nullptr).b(nullptr);
        }
        s.run();
      }
    }
    for (const auto& site : c.facts.unresolved_sites()) {
      static const std::string sql = std::string("INSERT INTO unresolved(unit_id, enclosing_id, expression, ") +
                                     kLocInsertColumns + ", reason) VALUES(?,?,?," + kLocPlaceholders + ",?)";
      auto& s = q(sql);
      s.b(unit).b(rows.at(site.enclosing)).b(site.expression);
      bind_location(s, profile, site.location, files);
      s.b(to_string(site.reason)).run();
    }
    for (const auto& cap : c.facts.captures()) {
      static const std::string sql = std::string("INSERT INTO capture(unit_id, lambda_id, kind, explicit_capture,"
                                                 " init_capture, pack_expansion, name, target_descriptor, ") +
                                     kLocInsertColumns + ") VALUES(?,?,?,?,?,?,?,?," + kLocPlaceholders + ")";
      auto& s = q(sql);
      s.b(unit).b(rows.at(cap.lambda)).b(to_string(cap.kind)).b(cap.explicit_capture).b(cap.init_capture)
          .b(cap.pack_expansion).b(cap.name).b(cap.target_descriptor);
      bind_location(s, profile, cap.location, files);
      s.run();
    }
    for (const auto& inc : c.facts.direct_includes()) {
      static const std::string sql =
          std::string("INSERT INTO direct_include(unit_id, includer_file_id, includer_as_written, directive_kind,"
                      " operand_kind, operand, expanded, activity, resolution, target_file_id,"
                      " target_as_written, dependency_key, ") +
          kLocInsertColumns + ") VALUES(?,?,?,?,?,?,?,?,?,?,?,?," + kLocPlaceholders + ")";
      auto& s = q(sql);
      s.b(unit)
          .b(file_id(profile, inc.includer.generic, files))
          .b(inc.includer.as_written)
          .b(to_string(inc.directive_kind))
          .b(to_string(inc.operand_kind))
          .b(inc.operand_as_written)
          .b(inc.expanded_spelling)
          .b(to_string(inc.activity))
          .b(to_string(inc.resolution));
      if (inc.resolved_repo_target) {
        s.b(file_id(profile, inc.resolved_repo_target->generic, files)).b(inc.resolved_repo_target->as_written);
      } else {
        s.b(nullptr).b(nullptr);
      }
      s.b(inc.external_dependency_key);
      bind_location(s, profile, inc.directive_location, files);
      s.run();
      const std::int64_t include = sqlite3_last_insert_rowid(db);
      for (std::size_t b = 0; b < inc.condition_path.size(); ++b) {
        const auto& branch = inc.condition_path[b];
        q("INSERT INTO include_branch(include_id, ordinal, is_else) VALUES(?,?,?)")
            .b(include).b(static_cast<std::int64_t>(b)).b(branch.is_else).run();
        const auto term = [&](const ConditionTerm& t, std::int64_t ordinal) {
          static const std::string term_sql =
              std::string("INSERT INTO include_term(include_id, branch_ordinal, term_ordinal, kind, expression, ") +
              kLocInsertColumns + ") VALUES(?,?,?,?,?," + kLocPlaceholders + ")";
          auto& ts = q(term_sql);
          ts.b(include).b(static_cast<std::int64_t>(b)).b(ordinal).b(to_string(t.kind)).b(t.expression_as_written);
          bind_location(ts, profile, t.location, files);
          ts.run();
        };
        if (branch.own_condition) term(*branch.own_condition, -1);
        for (std::size_t t = 0; t < branch.preceding_branches.size(); ++t) {
          term(branch.preceding_branches[t], static_cast<std::int64_t>(t));
        }
      }
    }
  }

  void recover_interrupted() {
    {
      auto& any = q("SELECT 1 FROM attempt WHERE state=? LIMIT 1");
      any.b(to_string(AttemptState::in_progress));
      const bool found = any.step();
      any.reset();
      if (!found) return;  // nothing to recover: take no write lock
    }
    exec(db, "BEGIN IMMEDIATE");
    try {
      q("UPDATE attempt SET state=?, message=CASE WHEN message='' THEN ? ELSE message || ' | ' || ? END"
        " WHERE state=?")
          .b(to_string(AttemptState::interrupted))
          .b("writer terminated before completion (detected by writer recovery)")
          .b("writer terminated before completion (detected by writer recovery)")
          .b(to_string(AttemptState::in_progress))
          .run();
      exec(db, "COMMIT");
    } catch (...) {
      exec_noexcept(db, "ROLLBACK");
      throw;
    }
  }
};

namespace {

std::unique_ptr<WriterLock::Handle> try_lock_file(const fs::path& lock_path) {
  auto handle = std::make_unique<WriterLock::Handle>();
#ifdef _WIN32
  handle->file = CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle->file == INVALID_HANDLE_VALUE) {
    throw StoreError("cannot open writer lock file " + path_to_utf8_generic(lock_path));
  }
  OVERLAPPED ov{};
  if (!LockFileEx(handle->file, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov)) return nullptr;
#else
  handle->fd = ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (handle->fd < 0) throw StoreError("cannot open writer lock file " + path_to_utf8_generic(lock_path));
  if (flock(handle->fd, LOCK_EX | LOCK_NB) != 0) return nullptr;
#endif
  return handle;
}

}  // namespace

Store::Store(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Store::Store(Store&&) noexcept = default;
Store& Store::operator=(Store&&) noexcept = default;
Store::~Store() = default;

JournalMode Store::journal_mode() const { return impl_->mode; }
const fs::path& Store::path() const { return impl_->db_path; }
fs::path Store::workspace_root() const { return path_from_utf8(impl_->workspace_root); }

Store Store::open(const fs::path& db_path, const fs::path& workspace_root, const StoreOptions& options) {
  auto impl = std::make_unique<Impl>();
  impl->db_path = fs::absolute(db_path).lexically_normal();
  impl->lock_path = impl->db_path;
  impl->lock_path += ".writer.lock";
  impl->workspace_root = path_to_utf8_generic(fs::absolute(workspace_root).lexically_normal());
  impl->options = options;
  impl->db = open_connection(impl->db_path, true, options.busy_timeout);
  sqlite3* db = impl->db;

  // Identity check before anything writes to the file: a foreign or newer
  // database is refused and left exactly as it was.
  const auto check_identity = [&]() -> bool {  // true: initialized store of this schema
    const std::int64_t app = pragma_int(db, "PRAGMA application_id");
    const std::int64_t version = pragma_int(db, "PRAGMA user_version");
    const std::int64_t objects = pragma_int(db, "SELECT count(*) FROM sqlite_schema");
    if (app == 0 && version == 0 && objects == 0) return false;
    if (app != kApplicationId) throw StoreError("not a LiveCodeMap store (application id mismatch); left unchanged");
    if (version != kSchemaVersion) {
      throw StoreError("unsupported store schema version " + std::to_string(version) + " (this build supports " +
                       std::to_string(kSchemaVersion) + "); left unchanged");
    }
    return true;
  };
  if (!check_identity()) {
    exec(db, "BEGIN IMMEDIATE");
    try {
      if (!check_identity()) {
        exec(db, kSchema);
        Stmt meta(db, "INSERT INTO meta(key, value) VALUES(?,?)");
        meta.reset().b("workspace_root").b(impl->workspace_root).run();
        meta.reset().b("schema").b("lcm-store").run();
        exec(db, ("PRAGMA application_id=" + std::to_string(kApplicationId)).c_str());
        exec(db, ("PRAGMA user_version=" + std::to_string(kSchemaVersion)).c_str());
      }
      exec(db, "COMMIT");
    } catch (...) {
      exec_noexcept(db, "ROLLBACK");
      throw;
    }
  }
  {
    Stmt s(db, "SELECT value FROM meta WHERE key='workspace_root'");
    if (!s.step() || path_key(path_from_utf8(s.text(0))) != path_key(path_from_utf8(impl->workspace_root))) {
      throw StoreError("store belongs to another workspace root; left unchanged");
    }
  }

  // Durability: every commit is synced (FULL) in both journal modes.
  if (options.journal == JournalMode::wal && pragma_text(db, "PRAGMA journal_mode=WAL") == "wal") {
    impl->mode = JournalMode::wal;
  } else {
    const std::string mode = pragma_text(db, "PRAGMA journal_mode=DELETE");
    impl->mode = mode == "wal" ? JournalMode::wal : JournalMode::rollback;
  }
  exec(db, "PRAGMA synchronous=FULL; PRAGMA foreign_keys=ON;");
  exec(db, kTempSchema);
  return Store(std::move(impl));
}

std::optional<WriterLock> Store::acquire_writer(std::chrono::milliseconds wait) {
  const auto deadline = std::chrono::steady_clock::now() + wait;
  for (;;) {
    if (auto handle = try_lock_file(impl_->lock_path)) {
      WriterLock lock(std::move(handle), impl_.get());
      Impl::Quiet quiet(*impl_);
      impl_->recover_interrupted();
      reclaim_orphan_spools(impl_->db_path.parent_path());
      return lock;
    }
    if (std::chrono::steady_clock::now() >= deadline) return std::nullopt;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

void Store::require_lock(const WriterLock& lock) const {
  if (!lock.handle_ || lock.owner_ != impl_.get()) {
    throw std::logic_error("the writer lock does not belong to this store or was moved from");
  }
}

void Store::ensure_profile(const WriterLock& lock, std::string_view profile, bool uht_applicable) {
  require_lock(lock);
  Impl::Quiet quiet(*impl_);
  if (profile.empty()) throw std::invalid_argument("empty profile key");
  sqlite3* db = impl_->db;
  {
    auto& sel = impl_->q("SELECT uht_applicable FROM profile WHERE key=?");
    sel.b(profile);
    if (sel.step()) {
      const bool existing = sel.flag(0);
      sel.reset();
      if (existing != uht_applicable) {
        throw std::invalid_argument("profile '" + std::string(profile) + "' exists with a different UHT applicability");
      }
      return;  // the writer lock excludes a concurrent creation
    }
  }
  exec(db, "BEGIN IMMEDIATE");
  try {
    impl_->q("INSERT INTO profile(key, uht_applicable) VALUES(?,?)").b(profile).b(uht_applicable).run();
    exec(db, "COMMIT");
  } catch (...) {
    exec_noexcept(db, "ROLLBACK");
    throw;
  }
}

PublishResult Store::publish(const WriterLock& lock, const Batch& batch, const PublishObserver& observer) {
  require_lock(lock);
  Impl::Quiet quiet(*impl_);
  Impl& s = *impl_;
  const std::int64_t profile = s.profile_id(batch.profile);
  PublishResult result;
  result.fact_generation = s.generation(profile);

  std::vector<std::pair<std::string, std::string>> slots;
  for (const auto& c : batch.install) slots.emplace_back(c.slot, "install");
  if (batch.spool) {
    for (const std::size_t entry : batch.spooled) {
      if (entry < batch.spool->size()) slots.emplace_back(batch.spool->slot(entry), "install");
    }
  }
  for (const auto& r : batch.retire) slots.emplace_back(r, "retire");

  std::string why = validate_batch(batch);
  if (why.empty()) {
    // A fact label names exactly one live slot per profile: a label still
    // owned by a slot this batch neither replaces nor retires would merge two
    // units' ownership, so it is refused before anything is mutated.
    std::set<std::string> leaving;
    for (const auto& [slot, action] : slots) leaving.insert(slot);
    std::vector<std::string> installed_labels;
    for (const auto& c : batch.install) installed_labels.push_back(c.fact_label);
    for (const std::size_t entry : batch.spooled) installed_labels.push_back(batch.spool->fact_label(entry));
    for (const auto& label : installed_labels) {
      auto& sel = s.q("SELECT slot FROM unit WHERE profile_id=? AND fact_label=?");
      sel.b(profile).b(label);
      if (sel.step() && !leaving.count(sel.text(0))) {
        why = "fact label '" + label + "' is owned by live slot '" + sel.text(0) + "'";
        break;
      }
    }
  }
  if (!why.empty()) {
    std::vector<std::pair<std::string, std::string>> rejected;
    std::set<std::string> seen;
    for (const auto& [slot, action] : slots) {
      if (seen.insert(slot).second) rejected.emplace_back(slot, "rejected");
    }
    result.outcome = PublishOutcome::rejected;
    result.message = why;
    result.attempt_id = s.record_attempt(profile, Layer::semantic_graph, AttemptState::failed, why, rejected);
    return result;
  }

  // No-op: nothing to install and none of the retired slots exists.
  if (batch.install.empty() && batch.spooled.empty()) {
    bool any = false;
    for (const auto& r : batch.retire) {
      auto& sel = s.q("SELECT 1 FROM unit WHERE profile_id=? AND slot=?");
      sel.b(profile).b(r);
      any = any || sel.step();
    }
    if (!any) {
      result.outcome = PublishOutcome::no_op;
      result.message = "nothing to install and no committed slot to retire";
      return result;
    }
  }

  // The in-progress attempt is durable before replacement starts, so a
  // terminated writer is identified honestly by the next writer's recovery.
  std::int64_t attempt = 0;
  try {
    attempt = s.record_attempt(profile, Layer::semantic_graph, AttemptState::in_progress, "", slots);
  } catch (const StoreError& e) {
    // Nothing was written (e.g. a rollback-journal reader kept the commit busy).
    result.outcome = PublishOutcome::failed;
    result.message = std::string("could not record the attempt: ") + e.what();
    return result;
  }
  result.attempt_id = attempt;
  try {
    if (observer) observer(PublishPhase::attempt_recorded);
    // A large page cache for the one big transaction (index pages of shared
    // rows are revisited by every unit), restored afterwards; lookup caches
    // live exactly as long as this transaction.
    struct PublicationScope {
      Impl& impl;
      std::int64_t page_cache;
      explicit PublicationScope(Impl& i) : impl(i), page_cache(pragma_int(i.db, "PRAGMA cache_size")) {
        impl.insert_cache.reset();
        exec(impl.db, "PRAGMA cache_size=-524288");
      }
      ~PublicationScope() {
        impl.insert_cache.reset();
        exec_noexcept(impl.db, ("PRAGMA cache_size=" + std::to_string(page_cache)).c_str());
      }
    } scope(s);
    exec(s.db, "BEGIN IMMEDIATE");
    try {
      for (const auto& c : batch.install) s.remove_unit(profile, c.slot);
      for (const std::size_t entry : batch.spooled) s.remove_unit(profile, batch.spool->slot(entry));
      for (const auto& r : batch.retire) s.remove_unit(profile, r);
      if (observer) observer(PublishPhase::old_removed);
      const std::int64_t next = result.fact_generation + 1;
      for (const auto& c : batch.install) s.insert_unit(profile, next, attempt, c);
      // Spooled contributions: the next one is read and decoded on another
      // thread while the current one is inserted, so at most two are in memory.
      if (!batch.spooled.empty()) {
        const auto load = [&](std::size_t k) {
          return std::async(std::launch::async, [&batch, k] { return batch.spool->load(batch.spooled[k]); });
        };
        std::future<Contribution> pending = load(0);
        for (std::size_t k = 0; k < batch.spooled.size(); ++k) {
          Contribution current = pending.get();
          if (k + 1 < batch.spooled.size()) pending = load(k + 1);
          s.insert_unit(profile, next, attempt, current);
        }
      }
      s.q("UPDATE profile SET fact_generation=? WHERE id=?").b(next).b(profile).run();
      s.finish_attempt(attempt, AttemptState::succeeded, "");
      if (observer) observer(PublishPhase::before_commit);
      exec(s.db, "COMMIT");
      result.fact_generation = next;
    } catch (...) {
      exec_noexcept(s.db, "ROLLBACK");
      throw;
    }
  } catch (const std::exception& e) {
    result.outcome = PublishOutcome::failed;
    result.message = e.what();
    try {
      s.finish_attempt(attempt, AttemptState::failed, result.message);
    } catch (...) {
      // Left in progress; the next writer recovery marks it interrupted.
    }
    result.fact_generation = s.generation(profile);
    return result;
  }
  result.outcome = PublishOutcome::committed;
  return result;
}

std::int64_t Store::record_rejected(const WriterLock& lock, std::string_view profile,
                                    const std::vector<std::string>& slots, std::string_view message) {
  require_lock(lock);
  Impl::Quiet quiet(*impl_);
  std::vector<std::pair<std::string, std::string>> rows;
  std::set<std::string> seen;
  for (const auto& slot : slots) {
    if (seen.insert(slot).second) rows.emplace_back(slot, "rejected");
  }
  return impl_->record_attempt(impl_->profile_id(profile), Layer::semantic_graph, AttemptState::failed, message, rows);
}

std::int64_t Store::record_auxiliary_failure(const WriterLock& lock, std::string_view profile, Layer layer,
                                             std::string_view message) {
  require_lock(lock);
  Impl::Quiet quiet(*impl_);
  if (layer != Layer::retrieval && layer != Layer::deep) {
    throw std::invalid_argument("only retrieval and deep are auxiliary layers");
  }
  return impl_->record_attempt(impl_->profile_id(profile), layer, AttemptState::failed, message, {});
}

void Store::set_excluded_files(const WriterLock& lock, std::string_view profile,
                               const std::vector<RepoRelativePath>& files) {
  require_lock(lock);
  Impl::Quiet quiet(*impl_);
  for (const auto& f : files) {
    if (!is_canonical_repo_relative(f.generic)) throw std::invalid_argument("non-canonical excluded file " + f.generic);
  }
  Impl& s = *impl_;
  const std::int64_t id = s.profile_id(profile);
  exec(s.db, "BEGIN IMMEDIATE");
  try {
    s.q("DELETE FROM excluded_file WHERE profile_id=?").b(id).run();
    for (const auto& f : files) {
      s.q("INSERT OR IGNORE INTO excluded_file(profile_id, path) VALUES(?,?)").b(id).b(f.generic).run();
    }
    exec(s.db, "COMMIT");
  } catch (...) {
    exec_noexcept(s.db, "ROLLBACK");
    throw;
  }
}

ReadSnapshot Store::read() const {
  sqlite3* db = open_connection(impl_->db_path, false, impl_->options.busy_timeout);
  try {
    exec(db, "BEGIN");
    // The first read binds the transaction to the latest committed state.
    pragma_int(db, "SELECT count(*) FROM profile");
  } catch (...) {
    sqlite3_close_v2(db);
    throw;
  }
  return ReadSnapshot(db, path_from_utf8(impl_->workspace_root));
}

// ---------------------------------------------------------------------------
// Freshness inspection.

namespace {

struct Dependency {
  std::string path_key;
  std::string path;
  std::optional<RepoRelativePath> repo_path;
  InputRole role = InputRole::header;
  FileContentHash recorded;
};

// One unit's reference to a shared dependency row.
struct DependencyRow {
  std::int64_t unit = 0;
  const Dependency* dep = nullptr;
};

struct UnitRow {
  std::int64_t id = 0;
  std::string slot;
  BuildCurrency currency = BuildCurrency::unknown;
};

// The committed state one inspection checks, read from one snapshot. Not
// movable once loaded: `deps` points into `dependencies`.
struct InspectionPlan {
  std::int64_t profile = 0;
  bool uht_applicable = false;
  std::int64_t generation = 0;
  std::vector<UnitRow> units;
  std::map<std::int64_t, Dependency> dependencies;  // shared rows, each decoded once
  std::vector<DependencyRow> deps;                  // sorted by path key, then unit
  std::map<std::string, std::string> distinct;      // path key -> path
};

using Probe = std::map<std::string, CurrentFile>;

void load_inspection_plan(sqlite3* db, const std::string& profile_key, InspectionPlan& plan) {
  Stmt p(db, "SELECT id, uht_applicable, fact_generation FROM profile WHERE key=?");
  p.b(profile_key);
  if (!p.step()) throw std::invalid_argument("unknown profile '" + profile_key + "'");
  plan.profile = p.i64(0);
  plan.uht_applicable = p.flag(1);
  plan.generation = p.i64(2);
  Stmt u(db, "SELECT id, slot, build_currency FROM unit WHERE profile_id=? ORDER BY slot");
  u.b(plan.profile);
  while (u.step()) {
    plan.units.push_back({u.i64(0), u.text(1), parse_enum<BuildCurrency>(u.text(2), 2, "build currency")});
  }
  // Set-based: the distinct shared rows once, then the units' references.
  Stmt d(db,
         "SELECT d.id, d.path_key, d.path, d.repo_path, d.repo_path_as_written, d.role, d.digest, d.size"
         " FROM dependency d WHERE d.id IN (SELECT i.dependency_id FROM input i JOIN unit u ON u.id=i.unit_id"
         " WHERE u.profile_id=?)");
  d.b(plan.profile);
  while (d.step()) {
    Dependency dep;
    dep.path_key = d.text(1);
    dep.path = d.text(2);
    if (!d.is_null(3)) dep.repo_path = RepoRelativePath{d.text(3), d.text(4)};
    dep.role = parse_enum<InputRole>(d.text(5), 12, "input role");
    dep.recorded = FileContentHash{d.digest(6), static_cast<std::uint64_t>(d.i64(7))};
    plan.dependencies.emplace(d.i64(0), std::move(dep));
  }
  Stmt i(db, "SELECT i.unit_id, i.dependency_id FROM input i JOIN unit u ON u.id=i.unit_id WHERE u.profile_id=?");
  i.b(plan.profile);
  while (i.step()) plan.deps.push_back(DependencyRow{i.i64(0), &plan.dependencies.at(i.i64(1))});
  std::sort(plan.deps.begin(), plan.deps.end(), [](const DependencyRow& a, const DependencyRow& b) {
    if (a.dep->path_key != b.dep->path_key) return a.dep->path_key < b.dep->path_key;
    return a.unit < b.unit;
  });
  for (const auto& row : plan.deps) plan.distinct.emplace(row.dep->path_key, row.dep->path);
}

// One hashing pass over every distinct input; false when the deadline passed.
bool probe_inputs(const InspectionPlan& plan, const InspectRequest& request, int pass,
                  std::chrono::steady_clock::time_point deadline, Probe& out) {
  for (const auto& [key, path] : plan.distinct) {
    if (request.before_probe) request.before_probe(pass, path);
    auto current = probe_file(path, deadline);
    if (!current) return false;
    out[key] = *current;
  }
  return true;
}

// Per-unit result from what is known. A unit is fresh only when every
// dependency was verified unchanged in BOTH passes and its build-action
// currency was verified by the caller.
void conclude_inspection(const InspectionPlan& plan, const std::string& profile_key, const Probe& first,
                         const Probe* second, FreshnessReport& report) {
  std::map<std::int64_t, UnitFreshness> by_unit;
  std::map<std::int64_t, bool> complete;
  for (const auto& u : plan.units) {
    UnitFreshness f;
    f.slot = u.slot;
    if (plan.uht_applicable) f.uht = Freshness::unknown;
    by_unit.emplace(u.id, std::move(f));
    complete[u.id] = true;
  }
  std::map<std::string, InputChange> changes;
  for (const auto& row : plan.deps) {
    const Dependency& d = *row.dep;
    UnitFreshness& f = by_unit.at(row.unit);
    const auto a = first.find(d.path_key);
    const auto* b = second ? &second->at(d.path_key) : nullptr;
    if (a == first.end()) {
      complete[row.unit] = false;
      continue;
    }
    std::optional<ChangeKind> kind;
    const CurrentFile& now = b ? *b : a->second;
    if (!now.present) {
      kind = ChangeKind::deleted;
    } else if (now.hash != d.recorded) {
      kind = ChangeKind::modified;
    } else if (b && !(a->second == *b)) {
      kind = ChangeKind::changed_during_check;
    }
    if (!kind) continue;
    auto [it, inserted] = changes.try_emplace(d.path_key + "|" + std::string(to_string(*kind)));
    if (inserted) it->second = InputChange{d.path, d.repo_path, d.role, *kind, {}};
    it->second.slots.push_back(f.slot);
    f.semantic = Freshness::stale;
    if (is_context_input(d.role)) f.compile_context = Freshness::stale;
    if (d.role == InputRole::reflected_header && f.uht) f.uht = Freshness::stale;
    f.reasons.push_back(d.path + " " + std::string(to_string(*kind)) + " (" + std::string(to_string(d.role)) + ")");
  }
  for (const auto& u : plan.units) {
    UnitFreshness& f = by_unit.at(u.id);
    if (!second || !complete[u.id]) {
      if (!complete[u.id]) f.reasons.push_back("not every dependency was checked within the budget");
      continue;  // anything not stale stays unknown
    }
    if (f.compile_context != Freshness::stale) {
      if (u.currency == BuildCurrency::verified) {
        f.compile_context = Freshness::fresh;
      } else {
        f.reasons.push_back("build-action currency not verified");
      }
    }
    if (f.semantic != Freshness::stale && f.compile_context == Freshness::fresh) f.semantic = Freshness::fresh;
    if (f.uht && *f.uht != Freshness::stale) f.reasons.push_back("UHT currency not verified");
  }
  report.changes.clear();
  for (auto& [key, change] : changes) report.changes.push_back(std::move(change));
  report.units.clear();
  for (const auto& u : plan.units) report.units.push_back(by_unit.at(u.id));
  std::size_t not_fresh = 0;
  for (const auto& f : report.units) not_fresh += f.semantic != Freshness::fresh ? 1 : 0;
  if (not_fresh != 0) {
    report.guidance = std::to_string(not_fresh) + " unit(s) of profile '" + profile_key +
                      "' are not verified fresh; run `livecodemap build` for this profile to re-analyse them. "
                      "This check ran no compiler, project build, UBT/UHT or deep analysis.";
  }
}

}  // namespace

FreshnessReport Store::inspect_freshness(const InspectRequest& request) {
  Impl::Quiet quiet(*impl_);
  const auto start = std::chrono::steady_clock::now();
  const auto deadline = start + request.budget;
  FreshnessReport report;
  const auto elapsed = [&] {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
  };

  // Committed snapshot of the profile's units and dependencies.
  InspectionPlan plan;
  {
    ReadSnapshot snap = read();
    load_inspection_plan(snap.db_, request.profile, plan);
  }
  const std::vector<UnitRow>& units = plan.units;
  const std::int64_t profile = plan.profile;
  report.fact_generation = plan.generation;
  report.inputs_total = plan.distinct.size();

  const auto conclude = [&](const Probe& first, const Probe* second) {
    conclude_inspection(plan, request.profile, first, second, report);
  };

  Impl& s = *impl_;
  const auto write_state = [&](std::int64_t unit, const UnitFreshness& f, std::int64_t now) {
    s.q("INSERT OR REPLACE INTO unit_freshness(unit_id, semantic, compile_context, uht, checked_at, reasons)"
        " VALUES(?,?,?,?,?,?)")
        .b(unit)
        .b(to_string(f.semantic))
        .b(to_string(f.compile_context))
        .b(f.uht ? std::optional<std::string>(std::string(to_string(*f.uht))) : std::nullopt)
        .b(now)
        .b(join_lines(f.reasons))
        .run();
  };

  // An inspection that does not complete never leaves an older verification
  // looking current: with the writer lock (already held, or free right now)
  // every persisted `fresh` state of the profile becomes `unknown` and the
  // stale units detected so far are recorded. Without the lock nothing is
  // written and the report says so (persisted_downgrade == false).
  const auto incomplete = [&](InspectionOutcome outcome, const Probe& first, std::optional<WriterLock> lock) {
    report.outcome = outcome;
    conclude(first, nullptr);
    if (!lock) lock = acquire_writer(std::chrono::milliseconds(0));
    if (lock) {
      exec(s.db, "BEGIN IMMEDIATE");
      try {
        const std::string reason = "last inspection incomplete: " + std::string(to_string(outcome));
        s.q("UPDATE unit_freshness SET"
            " semantic=CASE semantic WHEN 'fresh' THEN 'unknown' ELSE semantic END,"
            " compile_context=CASE compile_context WHEN 'fresh' THEN 'unknown' ELSE compile_context END,"
            " uht=CASE uht WHEN 'fresh' THEN 'unknown' ELSE uht END,"
            " reasons=CASE WHEN reasons='' THEN ?1 ELSE reasons || char(10) || ?1 END"
            " WHERE unit_id IN (SELECT id FROM unit WHERE profile_id=?2)")
            .b(reason)
            .b(profile)
            .run();
        if (s.generation(profile) == report.fact_generation) {
          const std::int64_t now = to_epoch(now_seconds());
          for (std::size_t i = 0; i < units.size(); ++i) {
            if (report.units[i].semantic == Freshness::stale) write_state(units[i].id, report.units[i], now);
          }
        }
        exec(s.db, "COMMIT");
        report.persisted_downgrade = true;
      } catch (...) {
        exec_noexcept(s.db, "ROLLBACK");
        throw;
      }
    }
    report.elapsed = elapsed();
    return report;
  };

  Probe first;
  if (std::chrono::steady_clock::now() >= deadline) return incomplete(InspectionOutcome::budget_exhausted, first, {});
  const bool first_complete = probe_inputs(plan, request, 1, deadline, first);
  report.inputs_checked = first.size();
  if (!first_complete) return incomplete(InspectionOutcome::budget_exhausted, first, {});
  if (request.after_first_pass) request.after_first_pass();

  const auto remaining =
      std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
  auto lock = acquire_writer(std::max(std::chrono::milliseconds(0), std::min(request.lock_wait, remaining)));
  if (!lock) return incomplete(InspectionOutcome::lock_busy, first, {});
  if (s.generation(profile) != report.fact_generation) {
    return incomplete(InspectionOutcome::generation_changed, first, std::move(lock));
  }
  Probe second;
  const bool second_complete = probe_inputs(plan, request, 2, deadline, second);
  report.inputs_checked = second.size();
  if (!second_complete) return incomplete(InspectionOutcome::budget_exhausted, first, std::move(lock));
  if (request.before_publish) request.before_publish();
  if (std::chrono::steady_clock::now() >= deadline) {
    return incomplete(InspectionOutcome::budget_exhausted, first, std::move(lock));
  }
  conclude(first, &second);

  exec(s.db, "BEGIN IMMEDIATE");
  try {
    const Timestamp now = now_seconds();
    for (std::size_t i = 0; i < units.size(); ++i) write_state(units[i].id, report.units[i], to_epoch(now));
    // Nothing is published as verified once the budget is spent.
    if (std::chrono::steady_clock::now() >= deadline) {
      exec(s.db, "ROLLBACK");
      return incomplete(InspectionOutcome::budget_exhausted, first, std::move(lock));
    }
    exec(s.db, "COMMIT");
    for (auto& f : report.units) f.checked_at = now;
  } catch (...) {
    exec_noexcept(s.db, "ROLLBACK");
    throw;
  }
  report.outcome = InspectionOutcome::complete;
  report.published = true;
  report.elapsed = elapsed();
  return report;
}

// ---------------------------------------------------------------------------
// Read side.

// Prepared statements of one snapshot, finalized before its transaction ends.
struct ReadSnapshot::Cache {
  std::unordered_map<std::string, std::unique_ptr<Stmt>> statements;
  Stmt& get(sqlite3* db, const std::string& sql) {
    auto it = statements.find(sql);
    if (it == statements.end()) it = statements.emplace(sql, std::make_unique<Stmt>(db, sql)).first;
    return it->second->reset();
  }
};

ReadSnapshot::ReadSnapshot(sqlite3* db, fs::path workspace_root, bool owns_connection)
    : db_(db), workspace_root_(std::move(workspace_root)), owns_(owns_connection), cache_(std::make_unique<Cache>()) {}

ReadSnapshot::ReadSnapshot(ReadSnapshot&& other) noexcept
    : db_(std::exchange(other.db_, nullptr)),
      workspace_root_(std::move(other.workspace_root_)),
      owns_(other.owns_),
      cache_(std::move(other.cache_)) {}

namespace {

// Ends a snapshot's read transaction; a borrowed connection stays open.
void end_snapshot(sqlite3* db, bool owns) {
  for (sqlite3_stmt* p = sqlite3_next_stmt(db, nullptr); p; p = sqlite3_next_stmt(db, p)) sqlite3_reset(p);
  exec_noexcept(db, "ROLLBACK");
  if (owns) sqlite3_close_v2(db);
}

}  // namespace

ReadSnapshot& ReadSnapshot::operator=(ReadSnapshot&& other) noexcept {
  if (this != &other) {
    cache_.reset();
    if (db_) end_snapshot(db_, owns_);
    db_ = std::exchange(other.db_, nullptr);
    workspace_root_ = std::move(other.workspace_root_);
    owns_ = other.owns_;
    cache_ = std::move(other.cache_);
  }
  return *this;
}

ReadSnapshot::~ReadSnapshot() {
  cache_.reset();
  if (db_) end_snapshot(db_, owns_);
}

namespace {

std::int64_t profile_row(sqlite3* db, std::string_view profile) {
  Stmt s(db, "SELECT id FROM profile WHERE key=?");
  s.b(profile);
  if (!s.step()) throw std::invalid_argument("unknown profile '" + std::string(profile) + "'");
  return s.i64(0);
}

// Reads the 11 location columns starting at `col`, where the first one is the
// joined file path (NULL for an empty path) and the next three come from the
// joined file_version row (see loc_select / loc_join).
SourceLocation read_location(const Stmt& s, int col) {
  SourceLocation l;
  l.file.generic = s.text(col);
  l.file.as_written = s.text(col + 1);
  l.content_hash = FileContentHash{s.digest(col + 2), static_cast<std::uint64_t>(s.i64(col + 3))};
  l.span = SourceSpan{s.u32(col + 4), s.u32(col + 5), s.u32(col + 6), s.u32(col + 7), s.u32(col + 8), s.u32(col + 9)};
  l.span_hash = s.opt_digest(col + 10);
  return l;
}

std::string loc_select(const char* alias, const char* file_alias) {
  const std::string a = alias;
  const std::string v = "v_" + a;
  return std::string(file_alias) + ".path, " + v + ".path_as_written, " + v + ".digest, " + v + ".size, " + a +
         ".b_off, " + a + ".e_off, " + a + ".b_line, " + a + ".b_col, " + a + ".e_line, " + a + ".e_col, " + a +
         ".span_hash";
}

// The join that loc_select's version columns need.
std::string loc_join(const char* alias) {
  const std::string a = alias;
  return " JOIN file_version v_" + a + " ON v_" + a + ".id=" + a + ".version_id";
}

OwnerComponent read_owner(const Stmt& s, int col) {
  OwnerComponent o;
  o.kind = parse_enum<SymbolKind>(s.text(col), 19, "symbol kind");
  o.name = s.text(col + 1);
  o.normalized_signature = s.text(col + 2);
  o.template_role = parse_enum<TemplateRole>(s.text(col + 3), 4, "template role");
  o.template_parameters = s.text(col + 4);
  o.template_arguments = s.text(col + 5);
  return o;
}

}  // namespace

std::vector<std::string> ReadSnapshot::profiles() const {
  std::vector<std::string> out;
  Stmt s(db_, "SELECT key FROM profile ORDER BY key");
  while (s.step()) out.push_back(s.text(0));
  return out;
}

std::int64_t ReadSnapshot::fact_generation(std::string_view profile) const {
  Stmt s(db_, "SELECT fact_generation FROM profile WHERE key=?");
  s.b(profile);
  if (!s.step()) throw std::invalid_argument("unknown profile '" + std::string(profile) + "'");
  return s.i64(0);
}

std::vector<RepoRelativePath> ReadSnapshot::withheld_files(std::string_view profile) const {
  const std::int64_t id = profile_row(db_, profile);
  std::set<std::string> out;
  Stmt ex(db_, "SELECT path FROM excluded_file WHERE profile_id=?");
  ex.b(id);
  while (ex.step()) out.insert(ex.text(0));
  Stmt files(db_, "SELECT path FROM file WHERE profile_id=?");
  files.b(id);
  while (files.step()) {
    const std::string generic = files.text(0);
    std::error_code ec;
    if (!fs::exists(workspace_root_ / path_from_utf8(generic), ec)) out.insert(generic);
  }
  std::vector<RepoRelativePath> result;
  for (const auto& g : out) result.push_back(RepoRelativePath{g, g});
  return result;
}

FactSet ReadSnapshot::load_facts(std::string_view profile, Projection projection) const {
  const std::int64_t id = profile_row(db_, profile);
  std::set<std::string> withheld;
  if (projection == Projection::public_) {
    for (const auto& f : withheld_files(profile)) withheld.insert(f.generic);
  }
  const auto hidden = [&](const RepoRelativePath& p) { return !p.generic.empty() && withheld.count(p.generic) != 0; };

  std::map<std::int64_t, std::string> labels;
  {
    Stmt s(db_, "SELECT id, fact_label FROM unit WHERE profile_id=?");
    s.b(id);
    while (s.step()) labels.emplace(s.i64(0), s.text(1));
  }

  struct Row {
    StableId id;
    CanonicalKey key;
    bool placeholder = false;
    std::vector<SymbolLocation> locations;
    std::size_t committed_locations = 0;  // before the projection
    bool referenced = false;
  };
  std::map<std::int64_t, Row> symbols;
  {
    Stmt s(db_,
           "SELECT id, stable_id, repository_member, language, kind, canonical_name, normalized_signature,"
           " template_role, template_parameters, template_arguments, linkage, linkage_file, linkage_file_as_written,"
           " linkage_enclosing, linkage_anchor, linkage_ordinal FROM symbol WHERE profile_id=?");
    s.b(id);
    while (s.step()) {
      Row r;
      r.id = StableId{s.text(1)};
      r.key.repository_member = s.text(2);
      r.key.language = parse_enum<Language>(s.text(3), 1, "language");
      r.key.kind = parse_enum<SymbolKind>(s.text(4), 19, "symbol kind");
      r.key.canonical_name = s.text(5);
      r.key.normalized_signature = s.text(6);
      r.key.template_role = parse_enum<TemplateRole>(s.text(7), 4, "template role");
      r.key.template_parameters = s.text(8);
      r.key.template_arguments = s.text(9);
      const std::string linkage = s.text(10);
      if (linkage == "internal") {
        r.key.linkage = InternalLinkage{RepoRelativePath{s.text(11), s.text(12)}};
      } else if (linkage == "local") {
        r.key.linkage = LocalScope{StableId{s.text(13)}, s.text(14), s.u32(15)};
      } else if (linkage == "external") {
        r.key.linkage = ExternalLinkage{};
      } else {
        throw StoreError("corrupt store: linkage '" + linkage + "'");
      }
      symbols.emplace(s.i64(0), std::move(r));
    }
  }
  {
    Stmt s(db_,
           "SELECT o.symbol_id, o.kind, o.name, o.normalized_signature, o.template_role, o.template_parameters,"
           " o.template_arguments FROM symbol_owner o JOIN symbol y ON y.id=o.symbol_id WHERE y.profile_id=?"
           " ORDER BY o.symbol_id, o.ordinal");
    s.b(id);
    while (s.step()) symbols.at(s.i64(0)).key.owner_chain.push_back(read_owner(s, 1));
  }
  {
    Stmt s(db_,
           "SELECT DISTINCT us.symbol_id FROM unit_symbol us JOIN unit u ON u.id=us.unit_id"
           " WHERE u.profile_id=? AND us.placeholder=1");
    s.b(id);
    while (s.step()) symbols.at(s.i64(0)).placeholder = true;
  }
  {
    Stmt s(db_, "SELECT sl.symbol_id, sl.unit_id, sl.role, " + loc_select("sl", "f") +
                    ", sl.body_hash, sl.explicitly_defaulted, sl.deleted_as_written, sl.implicitly_deleted, sl.trivial"
                    " FROM symbol_location sl JOIN unit u ON u.id=sl.unit_id LEFT JOIN file f ON f.id=sl.file_id" +
                    loc_join("sl") + " WHERE u.profile_id=?");
    s.b(id);
    while (s.step()) {
      SymbolLocation l;
      l.role = parse_enum<LocationRole>(s.text(2), 3, "location role");
      l.location = read_location(s, 3);
      l.body_hash = s.opt_digest(14);
      l.analysis_unit = labels.at(s.i64(1));
      l.callable = CallableFlags{s.flag(15), s.flag(16), s.flag(17), s.flag(18)};
      Row& r = symbols.at(s.i64(0));
      ++r.committed_locations;
      if (hidden(l.location.file)) continue;
      r.locations.push_back(std::move(l));
    }
  }

  // Only symbols with a visible location, or pure external placeholders
  // (no committed location at all), are public. Everything else is withheld
  // as a whole -- key, name and every incident relation, unresolved site and
  // capture: a symbol owned only by withheld files, and a location-less
  // endpoint whose former owners were retired or replaced, since nothing
  // committed proves its origin was visible. So is an internal-linkage symbol
  // scoped to a withheld file, and a local symbol whose enclosing symbol is
  // withheld (its key names that symbol).
  std::set<std::int64_t> withheld_symbols;
  if (projection == Projection::public_) {
    std::map<StableId, std::int64_t> row_of;
    for (const auto& [row, r] : symbols) {
      row_of.emplace(r.id, row);
      bool w = r.locations.empty() && (r.committed_locations != 0 || !r.placeholder);
      if (const auto* internal = std::get_if<InternalLinkage>(&r.key.linkage)) w = w || hidden(internal->file_scope);
      if (w) withheld_symbols.insert(row);
    }
    for (bool changed = true; changed;) {
      changed = false;
      for (const auto& [row, r] : symbols) {
        const auto* local = std::get_if<LocalScope>(&r.key.linkage);
        if (!local || withheld_symbols.count(row)) continue;
        const auto enclosing = row_of.find(local->enclosing);
        if (enclosing != row_of.end() && withheld_symbols.count(enclosing->second)) {
          withheld_symbols.insert(row);
          changed = true;
        }
      }
    }
  }
  const auto symbol_withheld = [&](std::int64_t row) { return withheld_symbols.count(row) != 0; };

  FactSet out;
  // Relations first only to learn which endpoints survive; facts are added below.
  std::vector<std::pair<RelationKey, Evidence>> evidence;
  std::map<std::int64_t, StableId> ids;
  for (const auto& [row, r] : symbols) ids.emplace(row, r.id);
  {
    Stmt s(db_, "SELECT r.source_id, r.target_id, r.kind, r.confidence, e.unit_id, e.kind, " + loc_select("e", "f") +
                    ", e.provider_key, e.evaluation, e.dispatch, e.template_use, e.immediately_invoked_lambda,"
                    " e.template_arguments, e.subobject_role, e.subobject, e.lifetime, e.potentially_elided,"
                    " e.base_access, e.base_access_written, e.base_virtual, e.base_ordinal"
                    " FROM evidence e JOIN relation r ON r.id=e.relation_id LEFT JOIN file f ON f.id=e.file_id" +
                    loc_join("e") + " WHERE r.profile_id=?");
    s.b(id);
    while (s.step()) {
      RelationKey key{ids.at(s.i64(0)), ids.at(s.i64(1)), parse_enum<RelationKind>(s.text(2), 7, "relation kind"),
                      parse_enum<Confidence>(s.text(3), 3, "confidence")};
      Evidence e;
      e.analysis_unit = labels.at(s.i64(4));
      e.kind = parse_enum<EvidenceKind>(s.text(5), 3, "evidence kind");
      e.location = read_location(s, 6);
      e.provider_key = s.opt_text(17);
      e.evaluation = parse_enum<EvaluationContext>(s.text(18), 7, "evaluation context");
      e.dispatch = parse_enum<DispatchKind>(s.text(19), 4, "dispatch");
      e.template_use = parse_enum<TemplateUse>(s.text(20), 6, "template use");
      e.immediately_invoked_lambda = s.flag(21);
      e.template_arguments = s.text(22);
      e.subobject_role = parse_enum<SubobjectRole>(s.text(23), 4, "subobject role");
      e.subobject = s.text(24);
      e.lifetime = parse_enum<LifetimeKind>(s.text(25), 5, "lifetime");
      e.potentially_elided = s.flag(26);
      if (!s.is_null(27)) {
        e.base = BaseEdgeDetails{parse_enum<BaseAccess>(s.text(27), 3, "base access"), s.flag(28), s.flag(29), s.u32(30)};
      }
      if (hidden(e.location.file) || symbol_withheld(s.i64(0)) || symbol_withheld(s.i64(1))) continue;
      symbols.at(s.i64(0)).referenced = true;
      symbols.at(s.i64(1)).referenced = true;
      evidence.emplace_back(std::move(key), std::move(e));
    }
  }
  std::vector<UnresolvedSite> unresolved;
  {
    Stmt s(db_, "SELECT x.enclosing_id, x.unit_id, x.expression, " + loc_select("x", "f") +
                    ", x.reason FROM unresolved x JOIN unit u ON u.id=x.unit_id LEFT JOIN file f ON f.id=x.file_id" +
                    loc_join("x") + " WHERE u.profile_id=?");
    s.b(id);
    while (s.step()) {
      UnresolvedSite site;
      site.enclosing = ids.at(s.i64(0));
      site.analysis_unit = labels.at(s.i64(1));
      site.expression = s.text(2);
      site.location = read_location(s, 3);
      site.reason = parse_enum<UnresolvedReason>(s.text(14), 8, "unresolved reason");
      if (hidden(site.location.file) || symbol_withheld(s.i64(0))) continue;
      symbols.at(s.i64(0)).referenced = true;
      unresolved.push_back(std::move(site));
    }
  }
  std::vector<CaptureFact> captures;
  {
    Stmt s(db_, "SELECT x.lambda_id, x.unit_id, x.kind, x.explicit_capture, x.init_capture, x.pack_expansion, x.name,"
                " x.target_descriptor, " + loc_select("x", "f") +
                    " FROM capture x JOIN unit u ON u.id=x.unit_id LEFT JOIN file f ON f.id=x.file_id" +
                    loc_join("x") + " WHERE u.profile_id=?");
    s.b(id);
    while (s.step()) {
      CaptureFact c;
      c.lambda = ids.at(s.i64(0));
      c.analysis_unit = labels.at(s.i64(1));
      c.kind = parse_enum<CaptureKind>(s.text(2), 4, "capture kind");
      c.explicit_capture = s.flag(3);
      c.init_capture = s.flag(4);
      c.pack_expansion = s.flag(5);
      c.name = s.text(6);
      c.target_descriptor = s.text(7);
      c.location = read_location(s, 8);
      if (hidden(c.location.file) || symbol_withheld(s.i64(0))) continue;
      symbols.at(s.i64(0)).referenced = true;
      captures.push_back(std::move(c));
    }
  }
  std::map<std::int64_t, DirectIncludeFact> includes;
  {
    Stmt s(db_, "SELECT x.id, x.unit_id, fi.path, x.includer_as_written, x.directive_kind, x.operand_kind, x.operand,"
                " x.expanded, x.activity, x.resolution, ft.path, x.target_as_written, x.target_file_id,"
                " x.dependency_key, " + loc_select("x", "f") +
                    " FROM direct_include x JOIN unit u ON u.id=x.unit_id LEFT JOIN file f ON f.id=x.file_id"
                    " LEFT JOIN file fi ON fi.id=x.includer_file_id LEFT JOIN file ft ON ft.id=x.target_file_id" +
                    loc_join("x") + " WHERE u.profile_id=?");
    s.b(id);
    while (s.step()) {
      DirectIncludeFact inc;
      inc.analysis_unit = labels.at(s.i64(1));
      inc.includer = RepoRelativePath{s.text(2), s.text(3)};
      inc.directive_kind = parse_enum<IncludeDirectiveKind>(s.text(4), 3, "directive kind");
      inc.operand_kind = parse_enum<IncludeOperandKind>(s.text(5), 3, "operand kind");
      inc.operand_as_written = s.text(6);
      inc.expanded_spelling = s.opt_text(7);
      inc.activity = parse_enum<IncludeActivity>(s.text(8), 3, "include activity");
      inc.resolution = parse_enum<IncludeResolution>(s.text(9), 4, "include resolution");
      if (!s.is_null(12)) inc.resolved_repo_target = RepoRelativePath{s.text(10), s.text(11)};
      inc.external_dependency_key = s.text(13);
      inc.directive_location = read_location(s, 14);
      if (hidden(inc.includer) || hidden(inc.directive_location.file) ||
          (inc.resolved_repo_target && hidden(*inc.resolved_repo_target))) {
        continue;
      }
      includes.emplace(s.i64(0), std::move(inc));
    }
  }
  {
    Stmt s(db_, "SELECT b.include_id, b.ordinal, b.is_else FROM include_branch b JOIN direct_include d ON d.id=b.include_id"
                " JOIN unit u ON u.id=d.unit_id WHERE u.profile_id=? ORDER BY b.include_id, b.ordinal");
    s.b(id);
    while (s.step()) {
      auto it = includes.find(s.i64(0));
      if (it == includes.end()) continue;
      ConditionalBranch branch;
      branch.is_else = s.flag(2);
      it->second.condition_path.push_back(std::move(branch));
    }
  }
  {
    Stmt s(db_, "SELECT t.include_id, t.branch_ordinal, t.term_ordinal, t.kind, t.expression, " + loc_select("t", "f") +
                    " FROM include_term t JOIN direct_include d ON d.id=t.include_id JOIN unit u ON u.id=d.unit_id"
                    " LEFT JOIN file f ON f.id=t.file_id" +
                    loc_join("t") + " WHERE u.profile_id=?"
                    " ORDER BY t.include_id, t.branch_ordinal, t.term_ordinal");
    s.b(id);
    while (s.step()) {
      auto it = includes.find(s.i64(0));
      if (it == includes.end()) continue;
      ConditionTerm term;
      term.kind = parse_enum<ConditionTermKind>(s.text(3), 7, "condition kind");
      term.expression_as_written = s.text(4);
      term.location = read_location(s, 5);
      auto& branch = it->second.condition_path.at(static_cast<std::size_t>(s.i64(1)));
      if (s.i64(2) < 0) {
        branch.own_condition = std::move(term);
      } else {
        branch.preceding_branches.push_back(std::move(term));
      }
    }
  }

  // Presence follows what survives (the projection may withhold locations).
  for (const auto& [row, r] : symbols) {
    if (symbol_withheld(row)) continue;
    if (!r.locations.empty()) {
      for (const auto& l : r.locations) {
        if (l.role == LocationRole::implicit_declaration) {
          out.add_hidden_member(r.key, l);
        } else {
          out.add_symbol_location(r.key, l);
        }
      }
    } else if (r.placeholder) {
      out.add_external_placeholder(r.key);
    } else if (r.referenced || projection == Projection::all) {
      out.add_endpoint_only(r.key);
    }
  }
  for (auto& [key, e] : evidence) out.add_relation_evidence(key, std::move(e));
  for (auto& site : unresolved) out.add_unresolved_site(std::move(site));
  for (auto& c : captures) out.add_capture(std::move(c));
  for (auto& [row, inc] : includes) out.add_direct_include(std::move(inc));
  return out;
}

namespace {

std::vector<Fingerprint> read_fingerprints(sqlite3* db, std::int64_t unit) {
  std::vector<Fingerprint> out;
  for (int k = 0; k < 6; ++k) out.push_back({static_cast<FingerprintKind>(k), std::nullopt});
  Stmt s(db, "SELECT kind, value FROM unit_fingerprint WHERE unit_id=?");
  s.b(unit);
  while (s.step()) {
    const auto kind = parse_enum<FingerprintKind>(s.text(0), 6, "fingerprint kind");
    out[static_cast<std::size_t>(kind)].value = s.opt_text(1);
  }
  return out;
}

}  // namespace

std::vector<UnitRecord> ReadSnapshot::units(std::string_view profile, UnitDetail detail) const {
  const std::int64_t id = profile_row(db_, profile);
  // Dependency observations, set-based: each distinct shared row is decoded
  // once and every unit references it (sorted by path key like the rows).
  std::map<std::int64_t, std::pair<std::string, InputObservation>> dependencies;  // id -> (path key, observation)
  std::map<std::int64_t, std::vector<const std::pair<std::string, InputObservation>*>> unit_inputs;
  if (detail != UnitDetail::summary) {
    Stmt d(db_,
           "SELECT d.id, d.path_key, d.path, d.repo_path, d.repo_path_as_written, d.role, d.digest, d.size, d.is_system"
           " FROM dependency d WHERE d.id IN (SELECT i.dependency_id FROM input i JOIN unit u ON u.id=i.unit_id"
           " WHERE u.profile_id=?)");
    d.b(id);
    while (d.step()) {
      InputObservation o;
      o.path_utf8 = d.text(2);
      if (!d.is_null(3)) o.repo_path = RepoRelativePath{d.text(3), d.text(4)};
      o.role = parse_enum<InputRole>(d.text(5), 12, "input role");
      o.content_hash = FileContentHash{d.digest(6), static_cast<std::uint64_t>(d.i64(7))};
      o.is_system = d.flag(8);
      dependencies.emplace(d.i64(0), std::make_pair(d.text(1), std::move(o)));
    }
    Stmt i(db_, "SELECT i.unit_id, i.dependency_id FROM input i JOIN unit u ON u.id=i.unit_id WHERE u.profile_id=?");
    i.b(id);
    while (i.step()) unit_inputs[i.i64(0)].push_back(&dependencies.at(i.i64(1)));
    for (auto& [unit, rows] : unit_inputs) {
      std::sort(rows.begin(), rows.end(), [](const auto* a, const auto* b) { return a->first < b->first; });
    }
  }
  bool uht_applicable = false;
  {
    Stmt p(db_, "SELECT uht_applicable FROM profile WHERE id=?");
    p.b(id);
    if (p.step()) uht_applicable = p.flag(0);
  }
  std::vector<UnitRecord> out;
  Stmt s(db_,
         "SELECT id, slot, fact_label, command_id, context_id, replay_id, pch_producer_command_id,"
         " pch_producer_replay_id, analysis_status, build_currency, committed_generation, attempt_id,"
         " analyzer, resource_dir, repository_member, publish_policy"
         " FROM unit WHERE profile_id=? ORDER BY slot");
  s.b(id);
  while (s.step()) {
    const std::int64_t unit = s.i64(0);
    UnitRecord r;
    r.slot = s.text(1);
    r.fact_label = s.text(2);
    r.provenance = Provenance{s.text(3), s.opt_text(4), s.text(5), s.opt_text(6), s.opt_text(7)};
    r.analysis_status = s.text(8);
    r.build_currency = parse_enum<BuildCurrency>(s.text(9), 2, "build currency");
    r.committed_generation = s.i64(10);
    r.attempt_id = s.i64(11);
    r.settings = AnalysisSettings{s.text(12), s.text(13), s.text(14), s.text(15)};
    if (const auto rows = unit_inputs.find(unit); rows != unit_inputs.end()) {
      r.inputs.reserve(rows->second.size());
      for (const auto* row : rows->second) r.inputs.push_back(row->second);
    }
    if (detail == UnitDetail::inputs) {
      r.freshness.slot = r.slot;
      out.push_back(std::move(r));
      continue;
    }
    if (detail == UnitDetail::full) {
      Stmt e(db_, "SELECT s.includer, s.spelling, s.angled, s.resolved, s.line FROM include_edge e"
                  " JOIN include_site s ON s.id=e.site_id WHERE e.unit_id=? ORDER BY e.ordinal");
      e.b(unit);
      while (e.step()) r.includes.push_back(IncludeEdge{e.opt_text(0), e.text(1), e.flag(2), e.opt_text(3), e.u32(4)});
      Stmt d(db_, "SELECT level, message, file, line, col FROM unit_diagnostic WHERE unit_id=? ORDER BY ordinal");
      d.b(unit);
      while (d.step()) r.diagnostics.push_back(UnitDiagnostic{d.text(0), d.text(1), d.opt_text(2), d.u32(3), d.u32(4)});
      Stmt l(db_, "SELECT text FROM unit_limit WHERE unit_id=? ORDER BY ordinal");
      l.b(unit);
      while (l.step()) r.limits.push_back(l.text(0));
      r.fingerprints = read_fingerprints(db_, unit);
    }
    r.freshness.slot = r.slot;
    if (uht_applicable) r.freshness.uht = Freshness::unknown;
    {
      Stmt f(db_, "SELECT semantic, compile_context, uht, reasons, checked_at FROM unit_freshness WHERE unit_id=?");
      f.b(unit);
      if (f.step()) {
        r.freshness.semantic = parse_enum<Freshness>(f.text(0), 3, "freshness");
        r.freshness.compile_context = parse_enum<Freshness>(f.text(1), 3, "freshness");
        if (!f.is_null(2)) r.freshness.uht = parse_enum<Freshness>(f.text(2), 3, "freshness");
        r.freshness.reasons = split_lines(f.text(3));
        r.freshness.checked_at = from_epoch(f.i64(4));
      }
    }
    out.push_back(std::move(r));
  }
  return out;
}

std::optional<UnitRecord> ReadSnapshot::unit(std::string_view profile, std::string_view slot) const {
  for (auto& r : units(profile)) {
    if (r.slot == slot) return std::move(r);
  }
  return std::nullopt;
}

namespace {

std::optional<AttemptInfo> read_attempt(sqlite3* db, std::int64_t profile, Layer layer, bool succeeded_only) {
  Stmt s(db, std::string("SELECT id, state, started_at, finished_at, message FROM attempt WHERE profile_id=? AND layer=?") +
                 (succeeded_only ? " AND state='succeeded'" : "") + " ORDER BY id DESC LIMIT 1");
  s.b(profile).b(to_string(layer));
  if (!s.step()) return std::nullopt;
  AttemptInfo a;
  a.id = s.i64(0);
  a.layer = layer;
  a.state = parse_enum<AttemptState>(s.text(1), 4, "attempt state");
  a.started = from_epoch(s.i64(2));
  if (!s.is_null(3)) a.finished = from_epoch(s.i64(3));
  a.message = s.text(4);
  return a;
}

}  // namespace

ProfileStatus ReadSnapshot::status(std::string_view profile) const {
  const std::int64_t id = profile_row(db_, profile);
  ProfileStatus st;
  st.profile = std::string(profile);
  bool uht_applicable = false;
  {
    Stmt p(db_, "SELECT uht_applicable, fact_generation, ranking_generation FROM profile WHERE id=?");
    p.b(id);
    p.step();
    uht_applicable = p.flag(0);
    st.fact_generation = p.i64(1);
    st.ranking_generation = p.i64(2);
  }
  st.last_attempt = read_attempt(db_, id, Layer::semantic_graph, false);
  st.last_success = read_attempt(db_, id, Layer::semantic_graph, true);

  // Latest semantic attempt per slot decides whether the slot is failing.
  std::map<std::string, AttemptState> latest;
  {
    Stmt s(db_, "SELECT sa.slot, a.state FROM slot_attempt sa JOIN attempt a ON a.id=sa.attempt_id"
                " WHERE a.profile_id=? AND a.layer='semantic_graph' ORDER BY a.id");
    s.b(id);
    while (s.step()) latest[s.text(0)] = parse_enum<AttemptState>(s.text(1), 4, "attempt state");
  }
  for (const auto& [slot, state] : latest) {
    if (state == AttemptState::failed || state == AttemptState::interrupted) st.failed_slots.push_back(slot);
  }
  const std::vector<UnitRecord> records = units(profile, UnitDetail::summary);
  std::set<std::string> committed;
  for (const auto& r : records) committed.insert(r.slot);

  UnitCounts counts;
  std::set<std::string> all(committed.begin(), committed.end());
  all.insert(st.failed_slots.begin(), st.failed_slots.end());
  counts.total = static_cast<std::uint32_t>(all.size());
  counts.failed = static_cast<std::uint32_t>(st.failed_slots.size());
  const std::set<std::string> failed(st.failed_slots.begin(), st.failed_slots.end());
  for (const auto& r : records) {
    if (!failed.count(r.slot)) ++counts.succeeded;
  }
  UnitCounts context_counts = counts;
  for (const auto& r : records) {
    if (r.freshness.semantic == Freshness::stale) ++counts.stale;
    if (r.freshness.compile_context == Freshness::stale) ++context_counts.stale;
  }

  const auto availability = [&]() {
    if (records.empty()) return failed.empty() ? Availability::missing : Availability::failed;
    return failed.empty() ? Availability::ready : Availability::partial;
  }();
  const auto combine = [&](auto field) {
    if (records.empty()) return Freshness::unknown;
    bool all_fresh = true;
    for (const auto& r : records) {
      const Freshness f = field(r);
      if (f == Freshness::stale) return Freshness::stale;
      all_fresh = all_fresh && f == Freshness::fresh;
    }
    return all_fresh ? Freshness::fresh : Freshness::unknown;
  };
  const auto stamp = [](const std::optional<AttemptInfo>& a, bool finished) -> std::optional<Timestamp> {
    if (!a) return std::nullopt;
    return finished ? a->finished : std::optional<Timestamp>(a->started);
  };

  for (int i = 0; i < 6; ++i) {
    const auto layer = static_cast<Layer>(i);
    LayerState state;
    state.layer = layer;
    switch (layer) {
      case Layer::structure:
        state.reason = "structure indexing is not produced by this increment";
        break;
      case Layer::compile_context:
        state.availability = availability;
        state.freshness = combine([](const UnitRecord& r) { return r.freshness.compile_context; });
        state.last_success = stamp(st.last_success, true);
        state.last_attempt = stamp(st.last_attempt, false);
        state.units = context_counts;
        if (state.freshness != Freshness::fresh) state.reason = "build-action currency not verified fresh";
        break;
      case Layer::uht_context:
        if (!uht_applicable) {
          state = not_applicable(layer, "UHT does not apply to this profile");
        } else {
          state.reason = "no UHT context evidence is produced by this increment";
          state.freshness = combine([](const UnitRecord& r) { return r.freshness.uht.value_or(Freshness::unknown); });
          if (state.freshness == Freshness::fresh) state.freshness = Freshness::unknown;
        }
        break;
      case Layer::semantic_graph:
        state.availability = availability;
        state.freshness = combine([](const UnitRecord& r) { return r.freshness.semantic; });
        state.last_success = stamp(st.last_success, true);
        state.last_attempt = stamp(st.last_attempt, false);
        state.units = counts;
        if (!failed.empty()) state.reason = std::to_string(failed.size()) + " slot(s) failed in their latest attempt";
        break;
      case Layer::retrieval:
      case Layer::deep: {
        const auto attempt = read_attempt(db_, id, layer, false);
        state.last_attempt = stamp(attempt, false);
        // Only failures can be recorded; nothing is ever produced here.
        state.availability = attempt ? Availability::failed : Availability::missing;
        state.reason = attempt ? attempt->message : "not produced by this increment";
        break;
      }
    }
    st.layers.push_back(std::move(state));
  }
  return st;
}

// ---------------------------------------------------------------------------
// Read-side row lookups for the query layer. Every lookup uses an existing
// index; projection is applied by the caller through a PublicView.

namespace {

constexpr const char* kSymbolColumns =
    "y.id, y.stable_id, y.repository_member, y.language, y.kind, y.canonical_name, y.normalized_signature,"
    " y.template_role, y.template_parameters, y.template_arguments, y.linkage, y.linkage_file,"
    " y.linkage_file_as_written, y.linkage_enclosing, y.linkage_anchor, y.linkage_ordinal";

// Reads kSymbolColumns from column 0; the owner chain is read separately.
SymbolRow read_symbol_row(const Stmt& s) {
  SymbolRow r;
  r.row = s.i64(0);
  r.id = StableId{s.text(1)};
  r.key.repository_member = s.text(2);
  r.key.language = parse_enum<Language>(s.text(3), 1, "language");
  r.key.kind = parse_enum<SymbolKind>(s.text(4), 19, "symbol kind");
  r.key.canonical_name = s.text(5);
  r.key.normalized_signature = s.text(6);
  r.key.template_role = parse_enum<TemplateRole>(s.text(7), 4, "template role");
  r.key.template_parameters = s.text(8);
  r.key.template_arguments = s.text(9);
  const std::string linkage = s.text(10);
  if (linkage == "internal") {
    r.key.linkage = InternalLinkage{RepoRelativePath{s.text(11), s.text(12)}};
  } else if (linkage == "local") {
    r.key.linkage = LocalScope{StableId{s.text(13)}, s.text(14), s.u32(15)};
  } else if (linkage == "external") {
    r.key.linkage = ExternalLinkage{};
  } else {
    throw StoreError("corrupt store: linkage '" + linkage + "'");
  }
  return r;
}

constexpr const char* kOwnerSql =
    "SELECT kind, name, normalized_signature, template_role, template_parameters, template_arguments"
    " FROM symbol_owner WHERE symbol_id=? ORDER BY ordinal";

void read_owner_chain(Stmt& owners, SymbolRow& r) {
  owners.b(r.row);
  while (owners.step()) r.key.owner_chain.push_back(read_owner(owners, 0));
}

std::string location_sql(const char* where) {
  return "SELECT sl.unit_id, sl.file_id, u.fact_label, sl.role, " + loc_select("sl", "f") +
         ", sl.body_hash, sl.explicitly_defaulted, sl.deleted_as_written, sl.implicitly_deleted, sl.trivial"
         " FROM symbol_location sl JOIN unit u ON u.id=sl.unit_id LEFT JOIN file f ON f.id=sl.file_id" +
         loc_join("sl") + " WHERE " + where;
}

LocationRow read_location_row(const Stmt& s) {
  LocationRow r;
  r.unit_row = s.i64(0);
  if (!s.is_null(1)) r.file_row = s.i64(1);
  r.location.analysis_unit = s.text(2);
  r.location.role = parse_enum<LocationRole>(s.text(3), 3, "location role");
  r.location.location = read_location(s, 4);
  r.location.body_hash = s.opt_digest(15);
  r.location.callable = CallableFlags{s.flag(16), s.flag(17), s.flag(18), s.flag(19)};
  return r;
}

std::vector<RelationRow> read_relations(Stmt& s) {
  std::vector<RelationRow> out;
  while (s.step()) {
    out.push_back(RelationRow{s.i64(0), s.i64(1), s.i64(2), parse_enum<RelationKind>(s.text(3), 7, "relation kind"),
                              parse_enum<Confidence>(s.text(4), 3, "confidence")});
  }
  return out;
}

}  // namespace

std::optional<ProfileInfo> ReadSnapshot::profile_info(std::string_view profile) const {
  Stmt& s = cache_->get(db_, "SELECT id, key, uht_applicable, fact_generation, ranking_generation FROM profile WHERE key=?");
  s.b(profile);
  if (!s.step()) return std::nullopt;
  ProfileInfo p{s.i64(0), s.text(1), s.flag(2), s.i64(3), s.i64(4)};
  s.reset();
  return p;
}

std::vector<std::string> ReadSnapshot::distinct_symbol_names(std::string_view profile) const {
  const std::int64_t id = profile_row(db_, profile);
  Stmt& s = cache_->get(db_, "SELECT DISTINCT canonical_name FROM symbol WHERE profile_id=? ORDER BY canonical_name");
  s.b(id);
  std::vector<std::string> out;
  while (s.step()) out.push_back(s.text(0));
  return out;
}

std::vector<SymbolRow> ReadSnapshot::symbols_named(std::string_view profile, std::string_view canonical_name) const {
  const std::int64_t id = profile_row(db_, profile);
  Stmt& s = cache_->get(db_, std::string("SELECT ") + kSymbolColumns +
                                 " FROM symbol y WHERE y.profile_id=? AND y.canonical_name=? ORDER BY y.id");
  s.b(id).b(canonical_name);
  std::vector<SymbolRow> out;
  while (s.step()) out.push_back(read_symbol_row(s));
  for (auto& r : out) read_owner_chain(cache_->get(db_, kOwnerSql), r);
  return out;
}

std::optional<SymbolRow> ReadSnapshot::symbol_by_stable_id(std::string_view profile, std::string_view stable_id) const {
  const std::int64_t id = profile_row(db_, profile);
  Stmt& s = cache_->get(db_, std::string("SELECT ") + kSymbolColumns + " FROM symbol y WHERE y.profile_id=? AND y.stable_id=?");
  s.b(id).b(stable_id);
  if (!s.step()) return std::nullopt;
  SymbolRow r = read_symbol_row(s);
  s.reset();
  read_owner_chain(cache_->get(db_, kOwnerSql), r);
  return r;
}

SymbolRow ReadSnapshot::symbol(std::int64_t symbol_row) const {
  Stmt& s = cache_->get(db_, std::string("SELECT ") + kSymbolColumns + " FROM symbol y WHERE y.id=?");
  s.b(symbol_row);
  if (!s.step()) throw std::invalid_argument("unknown symbol row " + std::to_string(symbol_row));
  SymbolRow r = read_symbol_row(s);
  s.reset();
  read_owner_chain(cache_->get(db_, kOwnerSql), r);
  return r;
}

std::vector<LocationRow> ReadSnapshot::symbol_locations(std::int64_t symbol_row) const {
  Stmt& s = cache_->get(db_, location_sql("sl.symbol_id=? ORDER BY sl.id"));
  s.b(symbol_row);
  std::vector<LocationRow> out;
  while (s.step()) out.push_back(read_location_row(s));
  return out;
}

std::vector<FileRow> ReadSnapshot::files(std::string_view profile) const {
  const std::int64_t id = profile_row(db_, profile);
  Stmt& s = cache_->get(db_, "SELECT id, path FROM file WHERE profile_id=? ORDER BY path");
  s.b(id);
  std::vector<FileRow> out;
  while (s.step()) out.push_back(FileRow{s.i64(0), s.text(1)});
  return out;
}

std::optional<FileRow> ReadSnapshot::file_by_path(std::string_view profile, std::string_view generic_path) const {
  const std::int64_t id = profile_row(db_, profile);
  Stmt& s = cache_->get(db_, "SELECT id, path FROM file WHERE profile_id=? AND path=?");
  s.b(id).b(generic_path);
  if (!s.step()) return std::nullopt;
  FileRow r{s.i64(0), s.text(1)};
  s.reset();
  return r;
}

void ReadSnapshot::for_each_location_site_in_file(std::int64_t file_row,
                                                  const std::function<void(const FileSiteRow&)>& visit) const {
  Stmt& s = cache_->get(db_, "SELECT symbol_id, unit_id, version_id, role, b_off, e_off, b_line, b_col, e_line, e_col,"
                             " span_hash FROM symbol_location WHERE file_id=?");
  s.b(file_row);
  FileSiteRow r;
  while (s.step()) {
    r.symbol_row = s.i64(0);
    r.unit_row = s.i64(1);
    r.version_row = s.i64(2);
    r.role = parse_enum<LocationRole>(s.text(3), 3, "location role");
    r.span = SourceSpan{s.u32(4), s.u32(5), s.u32(6), s.u32(7), s.u32(8), s.u32(9)};
    r.span_hash = s.opt_digest(10);
    visit(r);
  }
}

std::vector<IncludeRow> ReadSnapshot::includes_from(std::int64_t file_row) const {
  Stmt& s = cache_->get(
      db_, "SELECT x.id, x.unit_id, u.fact_label, x.includer_file_id, fi.path, x.includer_as_written, x.directive_kind,"
           " x.operand_kind, x.operand, x.expanded, x.activity, x.resolution, x.target_file_id, ft.path,"
           " x.target_as_written, x.dependency_key, x.file_id, " +
               loc_select("x", "f") +
               " FROM direct_include x JOIN unit u ON u.id=x.unit_id LEFT JOIN file f ON f.id=x.file_id"
               " LEFT JOIN file fi ON fi.id=x.includer_file_id LEFT JOIN file ft ON ft.id=x.target_file_id" +
               loc_join("x") + " WHERE x.includer_file_id=? ORDER BY x.id");
  s.b(file_row);
  std::vector<IncludeRow> out;
  while (s.step()) {
    IncludeRow r;
    r.row = s.i64(0);
    r.unit_row = s.i64(1);
    if (!s.is_null(3)) r.includer_file_row = s.i64(3);
    if (!s.is_null(12)) r.target_file_row = s.i64(12);
    if (!s.is_null(16)) r.location_file_row = s.i64(16);
    DirectIncludeFact& inc = r.include;
    inc.analysis_unit = s.text(2);
    inc.includer = RepoRelativePath{s.text(4), s.text(5)};
    inc.directive_kind = parse_enum<IncludeDirectiveKind>(s.text(6), 3, "directive kind");
    inc.operand_kind = parse_enum<IncludeOperandKind>(s.text(7), 3, "operand kind");
    inc.operand_as_written = s.text(8);
    inc.expanded_spelling = s.opt_text(9);
    inc.activity = parse_enum<IncludeActivity>(s.text(10), 3, "include activity");
    inc.resolution = parse_enum<IncludeResolution>(s.text(11), 4, "include resolution");
    if (!s.is_null(12)) inc.resolved_repo_target = RepoRelativePath{s.text(13), s.text(14)};
    inc.external_dependency_key = s.text(15);
    inc.directive_location = read_location(s, 17);
    out.push_back(std::move(r));
  }
  return out;
}

std::vector<ConditionalBranch> ReadSnapshot::include_condition_path(std::int64_t include_row) const {
  std::vector<ConditionalBranch> path;
  Stmt& b = cache_->get(db_, "SELECT is_else FROM include_branch WHERE include_id=? ORDER BY ordinal");
  b.b(include_row);
  while (b.step()) {
    ConditionalBranch branch;
    branch.is_else = b.flag(0);
    path.push_back(std::move(branch));
  }
  Stmt& t = cache_->get(db_, "SELECT t.branch_ordinal, t.term_ordinal, t.kind, t.expression, " + loc_select("t", "f") +
                                 " FROM include_term t LEFT JOIN file f ON f.id=t.file_id" + loc_join("t") +
                                 " WHERE t.include_id=? ORDER BY t.branch_ordinal, t.term_ordinal");
  t.b(include_row);
  while (t.step()) {
    ConditionTerm term;
    term.kind = parse_enum<ConditionTermKind>(t.text(2), 7, "condition kind");
    term.expression_as_written = t.text(3);
    term.location = read_location(t, 4);
    auto& branch = path.at(static_cast<std::size_t>(t.i64(0)));
    if (t.i64(1) < 0) {
      branch.own_condition = std::move(term);
    } else {
      branch.preceding_branches.push_back(std::move(term));
    }
  }
  return path;
}

std::vector<RelationRow> ReadSnapshot::relations_to(std::int64_t symbol_row, RelationKind kind) const {
  Stmt& s = cache_->get(db_, "SELECT id, source_id, target_id, kind, confidence FROM relation"
                             " WHERE target_id=? AND kind=? ORDER BY id");
  s.b(symbol_row).b(to_string(kind));
  return read_relations(s);
}

std::vector<RelationRow> ReadSnapshot::relations_from(std::int64_t symbol_row, RelationKind kind) const {
  Stmt& s = cache_->get(db_, "SELECT id, source_id, target_id, kind, confidence FROM relation"
                             " WHERE source_id=? AND kind=? ORDER BY id");
  s.b(symbol_row).b(to_string(kind));
  return read_relations(s);
}

std::vector<RelationRow> ReadSnapshot::relations_between(std::int64_t profile_row, std::int64_t source_row,
                                                        std::int64_t target_row, RelationKind kind) const {
  // UNIQUE(profile_id, source_id, target_id, kind, confidence) serves this
  // directly (rows in confidence order). An ORDER BY id would make the planner
  // prefer relation_by_target and scan every caller of a hot target.
  Stmt& s = cache_->get(db_, "SELECT id, source_id, target_id, kind, confidence FROM relation"
                             " WHERE profile_id=? AND source_id=? AND target_id=? AND kind=?");
  s.b(profile_row).b(source_row).b(target_row).b(to_string(kind));
  return read_relations(s);
}

std::vector<EvidenceRow> ReadSnapshot::evidence(std::int64_t relation_row) const {
  Stmt& s = cache_->get(db_, "SELECT e.unit_id, e.file_id, u.fact_label, e.kind, " + loc_select("e", "f") +
                                 ", e.provider_key, e.evaluation, e.dispatch, e.template_use, e.immediately_invoked_lambda,"
                                 " e.template_arguments, e.subobject_role, e.subobject, e.lifetime, e.potentially_elided,"
                                 " e.base_access, e.base_access_written, e.base_virtual, e.base_ordinal"
                                 " FROM evidence e JOIN unit u ON u.id=e.unit_id LEFT JOIN file f ON f.id=e.file_id" +
                                 loc_join("e") + " WHERE e.relation_id=? ORDER BY e.id");
  s.b(relation_row);
  std::vector<EvidenceRow> out;
  while (s.step()) {
    EvidenceRow r;
    r.unit_row = s.i64(0);
    if (!s.is_null(1)) r.file_row = s.i64(1);
    Evidence& e = r.evidence;
    e.analysis_unit = s.text(2);
    e.kind = parse_enum<EvidenceKind>(s.text(3), 3, "evidence kind");
    e.location = read_location(s, 4);
    e.provider_key = s.opt_text(15);
    e.evaluation = parse_enum<EvaluationContext>(s.text(16), 7, "evaluation context");
    e.dispatch = parse_enum<DispatchKind>(s.text(17), 4, "dispatch");
    e.template_use = parse_enum<TemplateUse>(s.text(18), 6, "template use");
    e.immediately_invoked_lambda = s.flag(19);
    e.template_arguments = s.text(20);
    e.subobject_role = parse_enum<SubobjectRole>(s.text(21), 4, "subobject role");
    e.subobject = s.text(22);
    e.lifetime = parse_enum<LifetimeKind>(s.text(23), 5, "lifetime");
    e.potentially_elided = s.flag(24);
    if (!s.is_null(25)) {
      e.base = BaseEdgeDetails{parse_enum<BaseAccess>(s.text(25), 3, "base access"), s.flag(26), s.flag(27), s.u32(28)};
    }
    out.push_back(std::move(r));
  }
  return out;
}

std::vector<SiteRow> ReadSnapshot::evidence_sites(std::int64_t relation_row) const {
  Stmt& s = cache_->get(db_, "SELECT unit_id, file_id, version_id, b_off, e_off, b_line, b_col, e_line, e_col, span_hash,"
                             " dispatch FROM evidence WHERE relation_id=? ORDER BY id");
  s.b(relation_row);
  std::vector<SiteRow> out;
  while (s.step()) {
    SiteRow r;
    r.unit_row = s.i64(0);
    if (!s.is_null(1)) r.file_row = s.i64(1);
    r.version_row = s.i64(2);
    r.span = SourceSpan{s.u32(3), s.u32(4), s.u32(5), s.u32(6), s.u32(7), s.u32(8)};
    r.span_hash = s.opt_digest(9);
    r.dispatch = parse_enum<DispatchKind>(s.text(10), 4, "dispatch");
    out.push_back(std::move(r));
  }
  return out;
}

VersionRow ReadSnapshot::file_version(std::int64_t version_row) const {
  Stmt& s = cache_->get(db_, "SELECT f.path, v.path_as_written, v.digest, v.size FROM file_version v"
                             " LEFT JOIN file f ON f.id=v.file_id WHERE v.id=?");
  s.b(version_row);
  if (!s.step()) throw StoreError("corrupt store: unknown file version row");
  VersionRow r{s.text(0), s.text(1), FileContentHash{s.digest(2), static_cast<std::uint64_t>(s.i64(3))}};
  s.reset();
  return r;
}

std::vector<UnresolvedRow> ReadSnapshot::unresolved_in(std::int64_t symbol_row) const {
  Stmt& s = cache_->get(db_, "SELECT x.unit_id, x.file_id, u.fact_label, x.expression, " + loc_select("x", "f") +
                                 ", x.reason, y.stable_id FROM unresolved x JOIN unit u ON u.id=x.unit_id"
                                 " JOIN symbol y ON y.id=x.enclosing_id LEFT JOIN file f ON f.id=x.file_id" +
                                 loc_join("x") + " WHERE x.enclosing_id=? ORDER BY x.id");
  s.b(symbol_row);
  std::vector<UnresolvedRow> out;
  while (s.step()) {
    UnresolvedRow r;
    r.unit_row = s.i64(0);
    if (!s.is_null(1)) r.file_row = s.i64(1);
    r.site.analysis_unit = s.text(2);
    r.site.expression = s.text(3);
    r.site.location = read_location(s, 4);
    r.site.reason = parse_enum<UnresolvedReason>(s.text(15), 8, "unresolved reason");
    r.site.enclosing = StableId{s.text(16)};
    out.push_back(std::move(r));
  }
  return out;
}

std::vector<UnitSummaryRow> ReadSnapshot::unit_summaries(std::string_view profile) const {
  const std::int64_t id = profile_row(db_, profile);
  Stmt& s = cache_->get(db_, "SELECT u.id, u.slot, u.fact_label, f.semantic FROM unit u"
                             " LEFT JOIN unit_freshness f ON f.unit_id=u.id WHERE u.profile_id=? ORDER BY u.slot");
  s.b(id);
  std::vector<UnitSummaryRow> out;
  while (s.step()) {
    UnitSummaryRow r{s.i64(0), s.text(1), s.text(2), Freshness::unknown};
    if (!s.is_null(3)) r.semantic = parse_enum<Freshness>(s.text(3), 3, "freshness");
    out.push_back(std::move(r));
  }
  return out;
}

// ---------------------------------------------------------------------------
// Public view: the rules of load_facts(Projection::public_), per symbol.

struct PublicView::Impl {
  sqlite3* db = nullptr;
  std::int64_t profile = 0;
  std::vector<RepoRelativePath> withheld;
  std::set<std::string> paths;
  std::set<std::int64_t> rows;
  Sha256Digest digest;
  std::unique_ptr<Stmt> locations;
  std::unique_ptr<Stmt> placeholder;
  std::unique_ptr<Stmt> linkage;
  std::unique_ptr<Stmt> by_stable_id;
  std::unordered_map<std::int64_t, std::optional<SymbolPresence>> memo;

  std::optional<SymbolPresence> evaluate(std::int64_t row) {
    if (const auto it = memo.find(row); it != memo.end()) return it->second;
    // Locations: any visible real location makes the symbol indexed; only
    // implicit visible ones make it implicit_hidden.
    bool visible_real = false;
    bool visible_implicit = false;
    std::int64_t committed = 0;
    locations->reset().b(row);
    while (locations->step()) {
      ++committed;
      if (!locations->is_null(0) && rows.count(locations->i64(0))) continue;
      if (locations->text(1) == "implicit_declaration") {
        visible_implicit = true;
      } else {
        visible_real = true;
        break;
      }
    }
    locations->reset();
    std::optional<SymbolPresence> result;
    if (visible_real) {
      result = SymbolPresence::indexed;
    } else if (visible_implicit) {
      result = SymbolPresence::implicit_hidden;
    } else if (committed == 0) {
      placeholder->reset().b(row);
      if (placeholder->step()) result = SymbolPresence::external_placeholder;  // else endpoint_only: withheld
      placeholder->reset();
    }
    if (result) {
      linkage->reset().b(row);
      if (!linkage->step()) throw StoreError("corrupt store: unknown symbol row");
      const std::string kind = linkage->text(0);
      const std::string file_scope = linkage->text(1);
      const std::string enclosing = linkage->text(2);
      linkage->reset();
      if (kind == "internal" && paths.count(file_scope)) {
        result.reset();
      } else if (kind == "local") {
        by_stable_id->reset().b(profile).b(enclosing);
        std::optional<std::int64_t> enclosing_row;
        if (by_stable_id->step()) enclosing_row = by_stable_id->i64(0);
        by_stable_id->reset();
        if (enclosing_row && !evaluate(*enclosing_row)) result.reset();
      }
    }
    memo[row] = result;
    return result;
  }
};

PublicView::PublicView(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
PublicView::PublicView(PublicView&&) noexcept = default;
PublicView& PublicView::operator=(PublicView&&) noexcept = default;
PublicView::~PublicView() = default;

const std::vector<RepoRelativePath>& PublicView::withheld() const { return impl_->withheld; }
const Sha256Digest& PublicView::digest() const { return impl_->digest; }
bool PublicView::path_withheld(std::string_view generic) const {
  return !generic.empty() && impl_->paths.count(std::string(generic)) != 0;
}
bool PublicView::file_withheld(const std::optional<std::int64_t>& file_row) const {
  return file_row && impl_->rows.count(*file_row) != 0;
}
std::optional<SymbolPresence> PublicView::presence(std::int64_t symbol_row) const { return impl_->evaluate(symbol_row); }

PublicView ReadSnapshot::public_view(std::string_view profile) const {
  auto impl = std::make_unique<PublicView::Impl>();
  impl->db = db_;
  impl->profile = profile_row(db_, profile);
  impl->withheld = withheld_files(profile);  // sorted: built from a std::set
  Sha256 digest;
  for (const auto& p : impl->withheld) {
    impl->paths.insert(p.generic);
    digest.update(p.generic);
    digest.update("\n");
  }
  impl->digest = digest.finish();
  {
    Stmt& s = cache_->get(db_, "SELECT id, path FROM file WHERE profile_id=?");
    s.b(impl->profile);
    while (s.step()) {
      if (impl->paths.count(s.text(1))) impl->rows.insert(s.i64(0));
    }
  }
  impl->locations = std::make_unique<Stmt>(db_, "SELECT file_id, role FROM symbol_location WHERE symbol_id=?");
  impl->placeholder = std::make_unique<Stmt>(db_, "SELECT 1 FROM unit_symbol WHERE symbol_id=? AND placeholder=1 LIMIT 1");
  impl->linkage = std::make_unique<Stmt>(db_, "SELECT linkage, linkage_file, linkage_enclosing FROM symbol WHERE id=?");
  impl->by_stable_id = std::make_unique<Stmt>(db_, "SELECT id FROM symbol WHERE profile_id=? AND stable_id=?");
  return PublicView(std::move(impl));
}

// ---------------------------------------------------------------------------
// Read-only store.

std::string_view to_string(ReadOnlyFailure value) {
  switch (value) {
    case ReadOnlyFailure::missing:
      return "missing";
    case ReadOnlyFailure::foreign:
      return "foreign";
    case ReadOnlyFailure::schema_version:
      return "schema_version";
    case ReadOnlyFailure::root_mismatch:
      return "root_mismatch";
  }
  return "unknown";
}

namespace {

// `file:` URI of an absolute path with every byte outside the unreserved set
// percent-encoded, so '?', '#', '%' and non-ASCII names survive.
std::string file_uri(const fs::path& absolute, bool immutable) {
  const std::string generic = path_to_utf8_generic(absolute);
  std::string uri = generic.rfind('/', 0) == 0 ? "file://" : "file:///";
  static const char* hex = "0123456789ABCDEF";
  for (const unsigned char c : generic) {
    if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~' || c == '/' || c == ':') {
      uri += static_cast<char>(c);
    } else {
      uri += '%';
      uri += hex[c >> 4];
      uri += hex[c & 15];
    }
  }
  uri += immutable ? "?mode=ro&immutable=1" : "?mode=ro";
  return uri;
}

sqlite3* open_read_only(const fs::path& absolute, bool immutable) {
  sqlite3* db = nullptr;
  const int rc = sqlite3_open_v2(file_uri(absolute, immutable).c_str(), &db,
                                 SQLITE_OPEN_READONLY | SQLITE_OPEN_URI | SQLITE_OPEN_NOMUTEX, nullptr);
  if (rc != SQLITE_OK) {
    std::string message = db ? sqlite3_errmsg(db) : "out of memory";
    sqlite3_close_v2(db);
    throw StoreError("cannot open store " + path_to_utf8_generic(absolute) + " read-only: " + message);
  }
  sqlite3_extended_result_codes(db, 1);
  return db;
}

std::uint32_t big_endian32(const unsigned char* p) {
  return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
         (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

// Schema identity and root of an open connection; returns the stored root.
std::string verify_identity(sqlite3* db, const std::string& shown, const std::optional<fs::path>& expected_root) {
  std::int64_t app = 0;
  std::int64_t version = 0;
  std::optional<std::string> root;
  try {
    app = pragma_int(db, "PRAGMA application_id");
    version = pragma_int(db, "PRAGMA user_version");
  } catch (const StoreError& e) {
    throw ReadOnlyStoreError(ReadOnlyFailure::foreign, shown + " is not a LiveCodeMap store: " + e.what());
  }
  if (app != kApplicationId) {
    throw ReadOnlyStoreError(ReadOnlyFailure::foreign, shown + " is not a LiveCodeMap store (application id mismatch)");
  }
  if (version != kSchemaVersion) {
    throw ReadOnlyStoreError(ReadOnlyFailure::schema_version, shown + " has store schema version " +
                                                                  std::to_string(version) + " (this build reads " +
                                                                  std::to_string(kSchemaVersion) + ")");
  }
  try {
    Stmt s(db, "SELECT value FROM meta WHERE key='workspace_root'");
    if (s.step()) root = s.text(0);
  } catch (const StoreError&) {
  }
  if (!root) throw ReadOnlyStoreError(ReadOnlyFailure::foreign, shown + " has no workspace root");
  if (expected_root &&
      path_key(path_from_utf8(*root)) != path_key(fs::absolute(*expected_root).lexically_normal())) {
    throw ReadOnlyStoreError(ReadOnlyFailure::root_mismatch, shown + " belongs to workspace root " + *root);
  }
  return *root;
}

}  // namespace

struct ReadOnlyStore::Impl {
  fs::path db_path;
  std::string workspace_root;
  sqlite3* db = nullptr;
  ~Impl() {
    if (db) sqlite3_close_v2(db);
  }
};

ReadOnlyStore::ReadOnlyStore(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
ReadOnlyStore::ReadOnlyStore(ReadOnlyStore&&) noexcept = default;
ReadOnlyStore& ReadOnlyStore::operator=(ReadOnlyStore&&) noexcept = default;
ReadOnlyStore::~ReadOnlyStore() = default;

const fs::path& ReadOnlyStore::path() const { return impl_->db_path; }
fs::path ReadOnlyStore::workspace_root() const { return path_from_utf8(impl_->workspace_root); }

ReadOnlyStore ReadOnlyStore::open(const fs::path& db_path, const std::optional<fs::path>& expected_root) {
  auto impl = std::make_unique<Impl>();
  impl->db_path = fs::absolute(db_path).lexically_normal();
  const fs::path& path = impl->db_path;
  const std::string shown = path_to_utf8_generic(path);
  std::error_code ec;
  if (!fs::is_regular_file(path, ec)) throw ReadOnlyStoreError(ReadOnlyFailure::missing, "no store file at " + shown);
  fs::path wal = path;
  wal += "-wal";
  fs::path shm = path;
  shm += "-shm";
  const bool wal_has_frames = fs::is_regular_file(wal, ec) && fs::file_size(wal, ec) > 0;
  if (!wal_has_frames) {
    // Without WAL frames every committed page is in the main file: check its
    // header with plain file I/O before SQLite sees the file at all.
    std::array<unsigned char, 100> header{};
    std::ifstream in(path, std::ios::binary);
    in.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    if (in.gcount() != static_cast<std::streamsize>(header.size()) ||
        std::string_view(reinterpret_cast<const char*>(header.data()), 16) != std::string_view("SQLite format 3\0", 16)) {
      throw ReadOnlyStoreError(ReadOnlyFailure::foreign, shown + " is not a LiveCodeMap store");
    }
    if (big_endian32(header.data() + 68) != static_cast<std::uint32_t>(kApplicationId)) {
      throw ReadOnlyStoreError(ReadOnlyFailure::foreign, shown + " is not a LiveCodeMap store (application id mismatch)");
    }
    if (big_endian32(header.data() + 60) != static_cast<std::uint32_t>(kSchemaVersion)) {
      throw ReadOnlyStoreError(ReadOnlyFailure::schema_version,
                               shown + " has store schema version " + std::to_string(big_endian32(header.data() + 60)) +
                                   " (this build reads " + std::to_string(kSchemaVersion) + ")");
    }
    const bool wal_mode = header[18] == 2 && header[19] == 2;
    if (wal_mode && (!fs::exists(wal, ec) || !fs::exists(shm, ec))) {
      // A read-only open would create the sidecars: verify the root first
      // through an immutable connection, which creates nothing.
      sqlite3* probe = open_read_only(path, true);
      try {
        verify_identity(probe, shown, expected_root);
      } catch (...) {
        sqlite3_close_v2(probe);
        throw;
      }
      sqlite3_close_v2(probe);
    }
  }
  impl->db = open_read_only(path, false);
  sqlite3* db = impl->db;
  sqlite3_busy_timeout(db, 5000);
  impl->workspace_root = verify_identity(db, shown, expected_root);
  // Connection-local settings only; nothing here writes to the file.
  exec(db, "PRAGMA query_only=1; PRAGMA mmap_size=2147418112; PRAGMA cache_size=-524288;");
  return ReadOnlyStore(std::move(impl));
}

ReadSnapshot ReadOnlyStore::read() const {
  sqlite3* db = impl_->db;
  exec(db, "BEGIN");
  try {
    pragma_int(db, "SELECT count(*) FROM profile");  // binds the snapshot
  } catch (...) {
    exec_noexcept(db, "ROLLBACK");
    throw;
  }
  return ReadSnapshot(db, path_from_utf8(impl_->workspace_root), false);
}

FreshnessReport ReadOnlyStore::inspect_freshness(const InspectRequest& request) const {
  const auto start = std::chrono::steady_clock::now();
  const auto deadline = start + request.budget;
  FreshnessReport report;
  InspectionPlan plan;
  {
    ReadSnapshot snap = read();
    load_inspection_plan(snap.db_, request.profile, plan);
  }
  report.fact_generation = plan.generation;
  report.inputs_total = plan.distinct.size();
  const auto finish = [&](InspectionOutcome outcome, const Probe& first, const Probe* second) {
    report.outcome = outcome;
    conclude_inspection(plan, request.profile, first, second, report);
    report.elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    return report;
  };
  Probe first;
  if (std::chrono::steady_clock::now() >= deadline) return finish(InspectionOutcome::budget_exhausted, first, nullptr);
  const bool first_complete = probe_inputs(plan, request, 1, deadline, first);
  report.inputs_checked = first.size();
  if (!first_complete) return finish(InspectionOutcome::budget_exhausted, first, nullptr);
  if (request.after_first_pass) request.after_first_pass();
  Probe second;
  const bool second_complete = probe_inputs(plan, request, 2, deadline, second);
  report.inputs_checked = second.size();
  if (!second_complete) return finish(InspectionOutcome::budget_exhausted, first, nullptr);
  if (request.before_publish) request.before_publish();
  std::int64_t generation = 0;
  {
    ReadSnapshot snap = read();
    const auto info = snap.profile_info(request.profile);
    if (!info) throw std::invalid_argument("unknown profile '" + request.profile + "'");
    generation = info->fact_generation;
  }
  if (generation != plan.generation) return finish(InspectionOutcome::generation_changed, first, nullptr);
  if (std::chrono::steady_clock::now() >= deadline) return finish(InspectionOutcome::budget_exhausted, first, nullptr);
  return finish(InspectionOutcome::complete, first, &second);
}

}  // namespace lcm::store
