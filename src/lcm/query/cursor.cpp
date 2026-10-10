#include "lcm/query/cursor.hpp"

#include <array>

#include "lcm/hash.hpp"

namespace lcm::query {

namespace {

constexpr std::string_view kPrefix = "lcq1";
constexpr std::string_view kChecksumSalt = "lcm-query-cursor-v1";
constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

std::string base64url(std::string_view bytes) {
  std::string out;
  std::uint32_t buffer = 0;
  int bits = 0;
  for (const unsigned char c : bytes) {
    buffer = (buffer << 8) | c;
    bits += 8;
    while (bits >= 6) {
      bits -= 6;
      out += kAlphabet[(buffer >> bits) & 63];
    }
  }
  if (bits > 0) out += kAlphabet[(buffer << (6 - bits)) & 63];
  return out;
}

std::optional<std::string> unbase64url(std::string_view text) {
  std::array<int, 256> value{};
  value.fill(-1);
  for (int i = 0; i < 64; ++i) value[static_cast<unsigned char>(kAlphabet[i])] = i;
  std::string out;
  std::uint32_t buffer = 0;
  int bits = 0;
  for (const unsigned char c : text) {
    const int v = value[c];
    if (v < 0) return std::nullopt;
    buffer = (buffer << 6) | static_cast<std::uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out += static_cast<char>((buffer >> bits) & 0xFF);
    }
  }
  // Leftover bits must be zero padding of the last character.
  if (bits >= 6 || (buffer & ((1u << bits) - 1)) != 0) return std::nullopt;
  return out;
}

std::string checksum(std::string_view payload) {
  Sha256 h;
  h.update(kChecksumSalt);
  h.update(payload);
  const Sha256Digest d = h.finish();
  return std::string(reinterpret_cast<const char*>(d.bytes.data()), 16);
}

}  // namespace

std::string short_digest(std::string_view text) { return sha256(text).hex().substr(0, 32); }

std::string encode_cursor(const CursorBinding& b, const nlohmann::json& position) {
  nlohmann::json payload;
  payload["v"] = 1;
  payload["tool"] = b.tool;
  payload["db"] = b.db;
  payload["profile"] = b.profile;
  payload["fg"] = b.fact_generation;
  payload["rg"] = b.ranking_generation;
  payload["args"] = b.args;
  payload["wh"] = b.withheld;
  payload["pos"] = position;
  const std::string text = payload.dump();  // nlohmann::json keeps keys sorted
  return std::string(kPrefix) + "." + base64url(text) + "." + base64url(checksum(text));
}

std::optional<DecodedCursor> decode_cursor(std::string_view text) {
  const auto first = text.find('.');
  const auto second = first == std::string_view::npos ? first : text.find('.', first + 1);
  if (second == std::string_view::npos || text.substr(0, first) != kPrefix ||
      text.find('.', second + 1) != std::string_view::npos) {
    return std::nullopt;
  }
  const auto payload = unbase64url(text.substr(first + 1, second - first - 1));
  const auto sum = unbase64url(text.substr(second + 1));
  if (!payload || !sum || *sum != checksum(*payload)) return std::nullopt;
  const nlohmann::json j = nlohmann::json::parse(*payload, nullptr, false);
  if (!j.is_object()) return std::nullopt;
  const auto str = [&](const char* k) { return j.contains(k) && j[k].is_string(); };
  const auto num = [&](const char* k) { return j.contains(k) && j[k].is_number_integer(); };
  if (!num("v") || j["v"].get<int>() != 1 || !str("tool") || !str("db") || !str("profile") || !num("fg") ||
      !num("rg") || !str("args") || !str("wh") || !j.contains("pos") || !j["pos"].is_object()) {
    return std::nullopt;
  }
  DecodedCursor out;
  out.binding = CursorBinding{j["tool"].get<std::string>(), j["db"].get<std::string>(), j["profile"].get<std::string>(),
                              j["fg"].get<std::int64_t>(),  j["rg"].get<std::int64_t>(), j["args"].get<std::string>(),
                              j["wh"].get<std::string>()};
  out.position = j["pos"];
  return out;
}

}  // namespace lcm::query
