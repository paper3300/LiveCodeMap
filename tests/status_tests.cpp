#include <doctest/doctest.h>

#include <vector>

#include "lcm/status.hpp"

using namespace lcm;

namespace {

LayerState layer(Layer which, Availability availability, Freshness freshness) {
  LayerState s;
  s.layer = which;
  s.availability = availability;
  s.freshness = freshness;
  return s;
}

}  // namespace

TEST_CASE("single layer summary priority") {
  CHECK(summarize_layer(layer(Layer::structure, Availability::ready, Freshness::fresh)) == UserStatus::ready);
  CHECK(summarize_layer(layer(Layer::structure, Availability::partial, Freshness::fresh)) == UserStatus::partial);
  CHECK(summarize_layer(layer(Layer::structure, Availability::ready, Freshness::stale)) == UserStatus::stale);
  CHECK(summarize_layer(layer(Layer::structure, Availability::partial, Freshness::stale)) == UserStatus::stale);
  CHECK(summarize_layer(layer(Layer::structure, Availability::missing, Freshness::unknown)) == UserStatus::missing);
  CHECK(summarize_layer(layer(Layer::structure, Availability::failed, Freshness::fresh)) == UserStatus::failed);
}

TEST_CASE("unverified freshness is never presented as ready") {
  CHECK(summarize_layer(layer(Layer::semantic_graph, Availability::ready, Freshness::unknown)) == UserStatus::stale);
  CHECK(summarize_layer(layer(Layer::semantic_graph, Availability::partial, Freshness::unknown)) == UserStatus::stale);
  std::vector<LayerState> layers = {layer(Layer::structure, Availability::ready, Freshness::unknown)};
  CHECK(summarize_overall(layers) == UserStatus::stale);
}

TEST_CASE("non-applicable layers are excluded from summaries") {
  const LayerState uht = not_applicable(Layer::uht_context, "plain C++ project has no Unreal Header Tool context");
  CHECK_FALSE(uht.applicable);
  CHECK_FALSE(summarize_layer(uht).has_value());

  std::vector<LayerState> layers = {
      layer(Layer::structure, Availability::ready, Freshness::fresh),
      layer(Layer::compile_context, Availability::ready, Freshness::fresh),
      uht,
      layer(Layer::semantic_graph, Availability::ready, Freshness::fresh),
      layer(Layer::retrieval, Availability::ready, Freshness::fresh),
  };
  CHECK(summarize_overall(layers) == UserStatus::ready);

  // Marking UHT as missing/failed would wrongly degrade a plain C++ project;
  // the not-applicable form must not.
  layers[2].availability = Availability::failed;  // still applicable == false
  CHECK(summarize_overall(layers) == UserStatus::ready);
}

TEST_CASE("plain C++ without a compilation database: compile context is explicitly missing, UHT not applicable") {
  LayerState compile_context = layer(Layer::compile_context, Availability::missing, Freshness::unknown);
  compile_context.reason =
      "no compile_commands.json selected; generate one (e.g. CMAKE_EXPORT_COMPILE_COMMANDS=ON) and run livecodemap build";
  LayerState semantic = layer(Layer::semantic_graph, Availability::missing, Freshness::unknown);
  semantic.reason = "compile context missing";
  std::vector<LayerState> layers = {
      layer(Layer::structure, Availability::ready, Freshness::fresh),
      compile_context,
      not_applicable(Layer::uht_context, "plain C++ project"),
      semantic,
      layer(Layer::retrieval, Availability::ready, Freshness::fresh),
  };
  // Structure and text search stay usable; the missing context is visible as
  // `partial` with its own reason rather than hidden or escalated to failure.
  CHECK(summarize_overall(layers) == UserStatus::partial);
  CHECK(summarize_layer(layers[1]) == UserStatus::missing);
  CHECK_FALSE(summarize_layer(layers[2]).has_value());
  CHECK(layers[1].reason.find("Unreal") == std::string::npos);
}

TEST_CASE("overall status: structure decides, then stale, then partial") {
  std::vector<LayerState> layers = {
      layer(Layer::structure, Availability::ready, Freshness::fresh),
      layer(Layer::semantic_graph, Availability::ready, Freshness::fresh),
      layer(Layer::deep, Availability::missing, Freshness::unknown),
  };
  // Optional deep missing does not block structure use: partial, not missing.
  CHECK(summarize_overall(layers) == UserStatus::partial);

  layers[2] = layer(Layer::deep, Availability::failed, Freshness::unknown);
  CHECK(summarize_overall(layers) == UserStatus::partial);

  layers[1] = layer(Layer::semantic_graph, Availability::ready, Freshness::stale);
  CHECK(summarize_overall(layers) == UserStatus::stale);

  layers[0] = layer(Layer::structure, Availability::missing, Freshness::unknown);
  CHECK(summarize_overall(layers) == UserStatus::missing);

  layers[0] = layer(Layer::structure, Availability::failed, Freshness::unknown);
  CHECK(summarize_overall(layers) == UserStatus::failed);

  CHECK(summarize_overall(std::vector<LayerState>{}) == UserStatus::missing);
}

TEST_CASE("layer state keeps availability, freshness and attempt times apart") {
  LayerState s = layer(Layer::semantic_graph, Availability::partial, Freshness::fresh);
  s.last_success = Timestamp{std::chrono::seconds{1000}};
  s.last_attempt = Timestamp{std::chrono::seconds{2000}};
  s.units = UnitCounts{10, 8, 2, 0, 3};
  CHECK(s.last_attempt > s.last_success);
  CHECK(s.units.total == 10);
  CHECK(s.units.failed == 2);
  CHECK(summarize_layer(s) == UserStatus::partial);
  CHECK(to_string(Layer::semantic_graph) == "semantic_graph");
  CHECK(to_string(UserStatus::partial) == "partial");
}
