// file_api: the public API of one physical file (contract R10, D5).
//
// Entries are the public symbols with a declaration or definition location in
// the file, ordered by their first location in the file (begin line, begin
// column, end line, end column), then stable ID. Each entry lists its
// locations here (repeated per-unit locations merged) and its other visible
// locations ("owner links"). The file's public direct includes are listed in
// directive order. The scan is complete on every call (no partial mode).

#include <algorithm>
#include <map>
#include <set>
#include <tuple>

#include "lcm/query/identifier.hpp"
#include "lcm/query/query_internal.hpp"

namespace lcm::query::detail {

namespace fs = std::filesystem;

namespace {

using SpanKey = std::tuple<std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t>;  // begin/end line, column

SpanKey span_key(const SourceSpan& s) { return {s.begin_line, s.begin_column, s.end_line, s.end_column}; }

struct Place {
  LocationRole role = LocationRole::declaration;
  SourceLocation location;
  std::set<std::string> units;
};

using PlaceKey = std::tuple<std::string, std::uint32_t, std::uint32_t, int, std::optional<Sha256Digest>, Sha256Digest>;

PlaceKey place_key(const store::LocationRow& l) {
  const SourceLocation& s = l.location.location;
  return {s.file.generic, s.span.begin_offset, s.span.end_offset, static_cast<int>(l.location.role), s.span_hash,
          s.content_hash.digest};
}

Json place_json(const Place& p) {
  Json j = location_json(p.location);
  j["role"] = std::string(to_string(p.role));
  j["units"] = p.units.size();
  return j;
}

Json term_json(const ConditionTerm& t) {
  return Json{{"kind", std::string(to_string(t.kind))}, {"expression", t.expression_as_written}};
}

Json condition_json(const std::vector<ConditionalBranch>& path) {
  Json out = Json::array();
  for (const auto& b : path) {
    Json preceding = Json::array();
    for (const auto& t : b.preceding_branches) preceding.push_back(term_json(t));
    out.push_back(Json{{"is_else", b.is_else},
                       {"condition", b.own_condition ? term_json(*b.own_condition) : Json(nullptr)},
                       {"preceding", std::move(preceding)}});
  }
  return out;
}

}  // namespace

Json file_api(Call& call, const FileApiArgs& args) {
  if (args.limit == 0 || args.limit > kMaxLimit) {
    return call.error("invalid_limit", "limit must be between 1 and " + std::to_string(kMaxLimit));
  }
  const std::string input = trim(args.path);
  if (input.empty()) return call.error("empty_path", "the path is empty");
  const fs::path given = path_from_utf8(input);
  for (const auto& part : given) {
    if (part == "..") return call.error("invalid_path", "'" + input + "' contains a '..' segment; pass a file path inside the workspace");
  }
  if (!given.is_absolute() && (given.has_root_name() || given.has_root_directory())) {
    return call.error("invalid_path", "'" + input + "' is neither repository-relative nor absolute");
  }
  const fs::path root = call.engine().store->workspace_root();
  const fs::path absolute = (given.is_absolute() ? given : root / given).lexically_normal();
  const auto rel = make_repo_relative_on_disk(root, absolute);
  if (!rel) return call.error("outside_workspace", "'" + input + "' is not inside the workspace root " + path_to_utf8_generic(root));
  std::error_code ec;
  if (rel->generic.empty() || fs::is_directory(absolute, ec)) {
    return call.error("not_a_file", "'" + input + "' is a directory; pass a file");
  }
  const auto file = call.snap().file_by_path(call.profile().key, rel->generic);
  if (!file || call.view().file_withheld(file->row)) {
    return call.error("file_not_indexed", "'" + rel->generic +
                                              "' is not in the public index (unknown to this profile, excluded or missing)");
  }
  const nlohmann::json binding{{"path", file->path}};
  std::uint64_t returned = 0;
  if (args.cursor) {
    nlohmann::json pos;
    if (auto e = call.accept(*args.cursor, binding, pos)) return *e;
    if (!pos.contains("returned") || !pos["returned"].is_number_unsigned()) {
      return call.error("invalid_cursor", "the cursor position is malformed; re-run the query without a cursor");
    }
    returned = pos["returned"].get<std::uint64_t>();
  }

  // Public direct includes. Per-unit observations are merged only when every
  // rendered field is equal: the key holds the directive location (span, span
  // hash, content hash) and every directive field, so distinct expansions or
  // file versions stay separate entries. The condition path is lexical text of
  // the same file bytes before the same offset, so equal content implies it.
  using IncludeKey = std::tuple<SourceSpan, std::optional<Sha256Digest>, FileContentHash, int, int, std::string,
                                std::optional<std::string>, int, int, std::string, std::string>;
  std::map<IncludeKey, std::pair<store::IncludeRow, std::set<std::string>>> includes;
  for (auto& inc : call.snap().includes_from(file->row)) {
    if (call.view().file_withheld(inc.location_file_row) || call.view().file_withheld(inc.includer_file_row) ||
        (inc.include.resolved_repo_target && call.view().file_withheld(inc.target_file_row))) {
      continue;
    }
    call.note_unit(file->path, inc.unit_row);
    const DirectIncludeFact& d = inc.include;
    IncludeKey key{d.directive_location.span,
                   d.directive_location.span_hash,
                   d.directive_location.content_hash,
                   static_cast<int>(d.directive_kind),
                   static_cast<int>(d.operand_kind),
                   d.operand_as_written,
                   d.expanded_spelling,
                   static_cast<int>(d.activity),
                   static_cast<int>(d.resolution),
                   d.resolved_repo_target ? d.resolved_repo_target->generic : std::string(),
                   d.directive_location.file.generic};
    auto& slot = includes[key];
    if (slot.second.empty()) slot.first = inc;
    slot.second.insert(d.analysis_unit);
  }
  Json include_json = Json::array();
  for (const auto& [key, value] : includes) {
    const DirectIncludeFact& d = value.first.include;
    include_json.push_back(Json{{"location", location_json(d.directive_location)},
                                {"directive", std::string(to_string(d.directive_kind))},
                                {"operand", d.operand_as_written},
                                {"operand_kind", std::string(to_string(d.operand_kind))},
                                {"expanded", d.expanded_spelling ? Json(*d.expanded_spelling) : Json(nullptr)},
                                {"activity", std::string(to_string(d.activity))},
                                {"resolution", std::string(to_string(d.resolution))},
                                {"target", d.resolved_repo_target ? Json(d.resolved_repo_target->generic) : Json(nullptr)},
                                {"condition_path", condition_json(call.snap().include_condition_path(value.first.row))},
                                {"units", value.second.size()}});
  }

  // Symbols declared or defined here (implicit declarations are not API).
  // The scan keeps row handles only; per-unit copies are merged by (offsets,
  // role, file version) here -- equal bytes and offsets imply an equal span
  // hash -- and by content when rendered.
  using SiteKey = std::tuple<std::uint32_t, std::uint32_t, int, std::int64_t>;
  struct Site {
    SourceSpan span;
    std::optional<Sha256Digest> span_hash;
    std::vector<std::int64_t> unit_rows;  // with repeats; deduplicated when rendered
  };
  struct Entry {
    std::int64_t symbol = 0;
    SpanKey first{};
    std::map<SiteKey, Site> here;
  };
  std::unordered_map<std::int64_t, Entry> by_symbol;
  call.snap().for_each_location_site_in_file(file->row, [&](const store::FileSiteRow& l) {
    if (l.role == LocationRole::implicit_declaration) return;
    auto [it, inserted] = by_symbol.try_emplace(l.symbol_row);
    Entry& e = it->second;
    const SpanKey k = span_key(l.span);
    if (inserted || k < e.first) e.first = k;
    e.symbol = l.symbol_row;
    Site& site = e.here[SiteKey{l.span.begin_offset, l.span.end_offset, static_cast<int>(l.role), l.version_row}];
    if (site.unit_rows.empty()) {
      site.span = l.span;
      site.span_hash = l.span_hash;
    }
    site.unit_rows.push_back(l.unit_row);
  });
  std::unordered_map<std::int64_t, store::VersionRow> versions;
  const auto version = [&](std::int64_t row) -> const store::VersionRow& {
    auto v = versions.find(row);
    if (v == versions.end()) v = versions.emplace(row, call.snap().file_version(row)).first;
    return v->second;
  };
  std::vector<Entry*> entries;
  for (auto& [row, e] : by_symbol) {
    if (call.view().presence(row)) entries.push_back(&e);
  }
  std::sort(entries.begin(), entries.end(), [](const Entry* a, const Entry* b) {
    return std::tie(a->first, a->symbol) < std::tie(b->first, b->symbol);
  });
  // Equal first spans are ordered by stable ID (rows are snapshot-local).
  std::unordered_map<std::int64_t, std::string> ids;
  for (std::size_t i = 0; i < entries.size();) {
    std::size_t j = i + 1;
    while (j < entries.size() && entries[j]->first == entries[i]->first) ++j;
    if (j - i > 1) {
      for (std::size_t k = i; k < j; ++k) ids.emplace(entries[k]->symbol, call.snap().symbol(entries[k]->symbol).id.value);
      std::sort(entries.begin() + static_cast<std::ptrdiff_t>(i), entries.begin() + static_cast<std::ptrdiff_t>(j),
                [&](const Entry* a, const Entry* b) { return ids[a->symbol] < ids[b->symbol]; });
    }
    i = j;
  }
  if (returned > entries.size()) {
    return call.error("invalid_cursor", "the cursor position is outside the result; re-run the query without a cursor");
  }

  Json entry_json = Json::array();
  const std::size_t end = std::min<std::size_t>(entries.size(), returned + args.limit);
  for (std::size_t i = returned; i < end; ++i) {
    const Entry& e = *entries[i];
    const store::SymbolRow symbol = call.snap().symbol(e.symbol);
    std::map<PlaceKey, Place> here_places;
    for (const auto& [key, site] : e.here) {
      const store::VersionRow& v = version(std::get<3>(key));
      Place& p = here_places[PlaceKey{v.path, site.span.begin_offset, site.span.end_offset, std::get<2>(key),
                                      site.span_hash, v.content_hash.digest}];
      p.role = static_cast<LocationRole>(std::get<2>(key));
      p.location = SourceLocation{RepoRelativePath{v.path, v.path_as_written}, v.content_hash, site.span, site.span_hash};
      for (const std::int64_t u : site.unit_rows) {
        call.note_unit(symbol.id.value, u);
        p.units.insert(call.unit_label(u));
      }
    }
    Json here = Json::array();
    for (const auto& [key, place] : here_places) here.push_back(place_json(place));
    std::map<PlaceKey, Place> other;
    for (const auto& l : call.snap().symbol_locations(e.symbol)) {
      if (l.location.role == LocationRole::implicit_declaration || l.file_row == file->row ||
          call.view().file_withheld(l.file_row)) {
        continue;
      }
      Place& p = other[place_key(l)];
      p.role = l.location.role;
      p.location = l.location.location;
      p.units.insert(l.location.analysis_unit);
    }
    Json elsewhere = Json::array();
    for (const auto& [key, place] : other) elsewhere.push_back(place_json(place));
    entry_json.push_back(Json{{"symbol", symbol_json(call, symbol, *call.view().presence(e.symbol))},
                              {"here", std::move(here)},
                              {"elsewhere", std::move(elsewhere)}});
  }

  Json file_json{{"path", file->path}};
  const std::string lower = [&] {
    std::string s = file->path;
    for (char& c : s) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    return s;
  }();
  if (lower.size() >= 4 && lower.compare(lower.size() - 4, 4, ".inl") == 0) {
    file_json["inl_context"] = nullptr;
    file_json["inl_context_reason"] = "the including context of an .inl file is not recorded by this index";
  }
  Json result;
  result["file"] = std::move(file_json);
  result["module"] = nullptr;
  result["module_reason"] = "module/target metadata is not recorded by this index";
  result["includes"] = std::move(include_json);
  result["entries"] = std::move(entry_json);

  Page page;
  page.complete = true;
  page.discovered = entries.size();
  page.returned_before = returned;
  page.shown = end - returned;
  page.cursor = [&](std::uint64_t r) { return call.make_cursor(binding, nlohmann::json{{"returned", r}}); };
  return call.finish(std::move(result), page);
}

}  // namespace lcm::query::detail
