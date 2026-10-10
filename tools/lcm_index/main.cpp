// Index the explicitly configured actions of one selected build profile.
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "lcm/analyzer/profile_index.hpp"
#include "lcm/compile_db.hpp"
#include "lcm/source.hpp"
#include "lcm/store/store.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace lcm;
using namespace lcm::analyzer;

namespace {

std::string read_bytes(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot read " + path_to_utf8_generic(path));
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void write_if_changed(const fs::path& path, const std::string& bytes) {
  if (fs::exists(path) && read_bytes(path) == bytes) return;
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out || !out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()))) {
    throw std::runtime_error("cannot write " + path_to_utf8_generic(path));
  }
}

fs::path absolute_path(const std::string& text) {
  const fs::path path = path_from_utf8(text);
  if (!path.is_absolute()) throw std::runtime_error("profile path must be absolute: " + text);
  return path.lexically_normal();
}

json command(const fs::path& directory, const fs::path& source, const std::vector<std::string>& arguments) {
  if (arguments.empty()) throw std::runtime_error("compile arguments are empty");
  return {{"directory", path_to_utf8_generic(directory)}, {"file", path_to_utf8_generic(source)},
          {"arguments", arguments}};
}

}  // namespace

int main(int argc, char** argv) {
  fs::path config_path;
  fs::path state_dir;
  fs::path report_path;
  std::string selected;
  std::size_t jobs = 1;
  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    if (i + 1 >= argc) {
      std::cerr << "missing value for " << flag << "\n";
      return 2;
    }
    const std::string value = argv[++i];
    if (flag == "--config") config_path = path_from_utf8(value);
    else if (flag == "--state-dir") state_dir = path_from_utf8(value);
    else if (flag == "--report") report_path = path_from_utf8(value);
    else if (flag == "--profile") selected = value;
    else if (flag == "--jobs") {
      if (value.empty() || value.size() > 3 || !std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return std::isdigit(c) != 0;
          }) || std::stoul(value) == 0) {
        std::cerr << "--jobs must be a positive number (at most 999)\n";
        return 2;
      }
      jobs = std::stoul(value);
    }
    else {
      std::cerr << "unknown option: " << flag << "\n";
      return 2;
    }
  }
  if (config_path.empty() || state_dir.empty() || report_path.empty()) {
    std::cerr << "usage: lcm_index --config <profiles.json> --state-dir <dir> --report <report.json>"
                 " [--profile <name>] [--jobs <n>]\n";
    return 2;
  }

  try {
    const json config = json::parse(read_bytes(config_path));
    if (config.at("schema").get<std::string>() != "lcm.replay-profiles.v1") {
      throw std::runtime_error("unsupported profile config schema");
    }
    if (selected.empty()) selected = config.at("active_profile").get<std::string>();
    if (selected.empty() || !std::all_of(selected.begin(), selected.end(), [](unsigned char c) {
          return std::isalnum(c) || c == '_' || c == '-';
        })) {
      throw std::runtime_error("profile name must use letters, digits, '_' or '-'");
    }
    const json& profiles = config.at("profiles");
    const json& profile = profiles.at(selected);
    std::set<std::string> profile_keys;
    for (auto it = profiles.begin(); it != profiles.end(); ++it) {
      const std::string key = it.value().value("profile_key", it.key());
      if (key.empty() || !profile_keys.insert(key).second) {
        throw std::runtime_error("profile keys must be nonempty and distinct");
      }
    }
    const fs::path root = absolute_path(config.at("repository_root").get<std::string>());
    const fs::path resource_dir = absolute_path(config.at("resource_dir").get<std::string>());
    const fs::path directory = absolute_path(profile.at("directory").get<std::string>());
    const json& units = profile.at("units");
    if (!units.is_object() || units.empty()) throw std::runtime_error("selected profile has no declared units");
    state_dir = fs::absolute(state_dir).lexically_normal();
    fs::create_directories(state_dir);
    const fs::path commands_path = state_dir / (selected + ".compile_commands.json");
    const fs::path database_path = state_dir / "index.sqlite";

    json commands = json::array();
    ProfileIndexRequest request;
    request.profile = profile.value("profile_key", selected);
    request.uht_applicable = profile.value("uht_applicable", false);
    request.syntax = CommandSyntax::windows;
    request.compile_database = commands_path;
    request.resource_dir = resource_dir;
    request.jobs = jobs;
    std::set<std::string> compilers;
    std::vector<std::pair<std::size_t, fs::path>> producers;
    std::set<std::string> written_producers;  // each distinct producer entry is written once
    for (auto it = units.begin(); it != units.end(); ++it) {
      const json& unit = it.value();
      const fs::path source = absolute_path(unit.at("source").get<std::string>());
      const auto arguments = unit.at("arguments").get<std::vector<std::string>>();
      if (arguments.empty()) throw std::runtime_error("unit '" + it.key() + "' has no compiler");
      const fs::path compiler = absolute_path(arguments.front());
      if (compilers.insert(path_to_utf8_generic(compiler)).second) {
        request.context_inputs.push_back({compiler, store::InputRole::compiler});
      }
      commands.push_back(command(directory, source, arguments));
      SelectedAction action;
      action.slot = it.key();
      action.source = source;
      if (unit.contains("unity_members")) {
        for (const auto& member : unit.at("unity_members")) {
          action.unity_members.push_back(absolute_path(member.get<std::string>()));
        }
      }
      if (unit.contains("producer_source") != unit.contains("producer_arguments")) {
        throw std::runtime_error("unit '" + it.key() + "' has an incomplete PCH producer");
      }
      if (unit.contains("producer_source")) {
        const fs::path producer = absolute_path(unit.at("producer_source").get<std::string>());
        json entry = command(directory, producer, unit.at("producer_arguments").get<std::vector<std::string>>());
        if (written_producers.insert(entry.dump()).second) commands.push_back(std::move(entry));
        producers.emplace_back(request.actions.size(), producer);
      }
      request.actions.push_back(std::move(action));
    }
    write_if_changed(commands_path, commands.dump(2) + "\n");
    const auto parsed = load_compilation_database(commands_path, CommandSyntax::windows);
    if (!parsed.database) throw std::runtime_error("generated compilation database is invalid");
    for (const auto& [index, producer] : producers) {
      const auto choice = parsed.database->select_unique(producer);
      if (choice.kind != CommandSelection::Kind::unique) {
        throw std::runtime_error("PCH producer command is missing or ambiguous");
      }
      request.actions[index].pch_producer_command_id = choice.command->command_id;
    }

    auto store = store::Store::open(database_path, root);
    const ProfileIndexReport result = index_profile(store, request);
    json slots = json::array();
    for (const auto& slot : result.slots) {
      slots.push_back({{"slot", slot.slot}, {"plan", std::string(to_string(slot.plan))},
                       {"result", std::string(to_string(slot.result))}, {"reasons", slot.reasons}});
    }
    const json report{{"selected_profile", selected}, {"profile_key", request.profile},
                      {"outcome", std::string(to_string(result.outcome))}, {"message", result.message},
                      {"errors", result.errors}, {"slots", slots},
                      {"planned_analyses", result.planned_analyses}, {"actual_analyses", result.actual_analyses},
                      {"generation_before", result.generation_before}, {"generation_after", result.generation_after},
                      {"failure_recorded", result.failure_recorded},
                      {"build_currency", "unknown"},
                      {"compile_database", path_to_utf8_generic(commands_path)}};
    write_if_changed(report_path, report.dump(2) + "\n");
    std::cout << "profile=" << selected << " outcome=" << to_string(result.outcome)
              << " analyzed=" << result.actual_analyses << " generation=" << result.generation_after << "\n";
    return result.outcome == IndexOutcome::committed || result.outcome == IndexOutcome::unchanged ? 0 : 4;
  } catch (const std::exception& error) {
    std::cerr << "profile index failed: " << error.what() << "\n";
    return 3;
  }
}
