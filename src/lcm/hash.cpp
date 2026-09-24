#include "lcm/hash.hpp"

#include <algorithm>
#include <bit>
#include <cstring>

namespace lcm {
namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

constexpr std::array<std::uint32_t, 8> kInitialState = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                                        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

std::uint32_t load_be32(const std::uint8_t* p) {
  return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) |
         std::uint32_t{p[3]};
}

void store_be32(std::uint8_t* p, std::uint32_t v) {
  p[0] = static_cast<std::uint8_t>(v >> 24);
  p[1] = static_cast<std::uint8_t>(v >> 16);
  p[2] = static_cast<std::uint8_t>(v >> 8);
  p[3] = static_cast<std::uint8_t>(v);
}

}  // namespace

std::string Sha256Digest::hex() const {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(64);
  for (std::uint8_t b : bytes) {
    out.push_back(kDigits[b >> 4]);
    out.push_back(kDigits[b & 0x0f]);
  }
  return out;
}

Sha256::Sha256() : state_(kInitialState) {}

void Sha256::update(std::string_view data) {
  update(std::as_bytes(std::span<const char>(data.data(), data.size())));
}

void Sha256::update(std::span<const std::byte> data) {
  total_len_ += data.size();
  absorb(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
}

void Sha256::absorb(const std::uint8_t* p, std::size_t remaining) {
  if (remaining == 0) return;  // explicit no-op: p may be null for an empty span
  if (buffer_len_ > 0) {
    const std::size_t take = std::min(remaining, buffer_.size() - buffer_len_);
    std::memcpy(buffer_.data() + buffer_len_, p, take);
    buffer_len_ += take;
    p += take;
    remaining -= take;
    if (buffer_len_ == buffer_.size()) {
      process_block(buffer_.data());
      buffer_len_ = 0;
    }
  }
  while (remaining >= 64) {
    process_block(p);
    p += 64;
    remaining -= 64;
  }
  if (remaining > 0) {
    std::memcpy(buffer_.data(), p, remaining);
    buffer_len_ = remaining;
  }
}

void Sha256::process_block(const std::uint8_t* block) {
  std::array<std::uint32_t, 64> w{};
  for (std::size_t i = 0; i < 16; ++i) w[i] = load_be32(block + i * 4);
  for (std::size_t i = 16; i < 64; ++i) {
    const std::uint32_t s0 = std::rotr(w[i - 15], 7) ^ std::rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const std::uint32_t s1 = std::rotr(w[i - 2], 17) ^ std::rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
  std::uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
  for (std::size_t i = 0; i < 64; ++i) {
    const std::uint32_t s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
    const std::uint32_t ch = (e & f) ^ (~e & g);
    const std::uint32_t t1 = h + s1 + ch + kRoundConstants[i] + w[i];
    const std::uint32_t s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
    const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t t2 = s0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

Sha256Digest Sha256::finish() {
  const std::uint64_t bit_len = total_len_ * 8;
  std::array<std::uint8_t, 72> pad{};
  pad[0] = 0x80;
  const std::size_t pad_len = (buffer_len_ < 56) ? (56 - buffer_len_) : (120 - buffer_len_);
  std::array<std::uint8_t, 8> len_bytes{};
  for (std::size_t i = 0; i < 8; ++i) len_bytes[7 - i] = static_cast<std::uint8_t>(bit_len >> (8 * i));
  absorb(pad.data(), pad_len);
  absorb(len_bytes.data(), len_bytes.size());
  Sha256Digest digest;
  for (std::size_t i = 0; i < 8; ++i) store_be32(digest.bytes.data() + i * 4, state_[i]);
  return digest;
}

Sha256Digest sha256(std::span<const std::byte> data) {
  Sha256 h;
  h.update(data);
  return h.finish();
}

Sha256Digest sha256(std::string_view data) {
  Sha256 h;
  h.update(data);
  return h.finish();
}

}  // namespace lcm
