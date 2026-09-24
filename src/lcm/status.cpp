#include "lcm/status.hpp"

namespace lcm {

std::string_view to_string(Layer value) {
  switch (value) {
    case Layer::structure:
      return "structure";
    case Layer::compile_context:
      return "compile_context";
    case Layer::uht_context:
      return "uht_context";
    case Layer::semantic_graph:
      return "semantic_graph";
    case Layer::retrieval:
      return "retrieval";
    case Layer::deep:
      return "deep";
  }
  return "unknown";
}

std::string_view to_string(Availability value) {
  switch (value) {
    case Availability::missing:
      return "missing";
    case Availability::partial:
      return "partial";
    case Availability::ready:
      return "ready";
    case Availability::failed:
      return "failed";
  }
  return "unknown";
}

std::string_view to_string(Freshness value) {
  switch (value) {
    case Freshness::unknown:
      return "unknown";
    case Freshness::fresh:
      return "fresh";
    case Freshness::stale:
      return "stale";
  }
  return "unknown";
}

std::string_view to_string(UserStatus value) {
  switch (value) {
    case UserStatus::ready:
      return "ready";
    case UserStatus::partial:
      return "partial";
    case UserStatus::stale:
      return "stale";
    case UserStatus::missing:
      return "missing";
    case UserStatus::failed:
      return "failed";
  }
  return "unknown";
}

LayerState not_applicable(Layer layer, std::string reason) {
  LayerState state;
  state.layer = layer;
  state.applicable = false;
  state.reason = std::move(reason);
  return state;
}

std::optional<UserStatus> summarize_layer(const LayerState& state) {
  if (!state.applicable) return std::nullopt;
  switch (state.availability) {
    case Availability::failed:
      return UserStatus::failed;
    case Availability::missing:
      return UserStatus::missing;
    case Availability::partial:
    case Availability::ready:
      break;
  }
  if (state.freshness != Freshness::fresh) return UserStatus::stale;  // stale or not yet verified
  if (state.availability == Availability::partial) return UserStatus::partial;
  return UserStatus::ready;
}

UserStatus summarize_overall(std::span<const LayerState> layers) {
  bool any_applicable = false;
  bool any_stale = false;
  bool any_not_ready = false;
  for (const auto& layer : layers) {
    const auto status = summarize_layer(layer);
    if (!status) continue;
    any_applicable = true;
    if (layer.layer == Layer::structure && (*status == UserStatus::failed || *status == UserStatus::missing)) {
      return *status;
    }
    if (*status == UserStatus::stale) any_stale = true;
    if (*status != UserStatus::ready) any_not_ready = true;
  }
  if (!any_applicable) return UserStatus::missing;
  if (any_stale) return UserStatus::stale;
  if (any_not_ready) return UserStatus::partial;
  return UserStatus::ready;
}

}  // namespace lcm
