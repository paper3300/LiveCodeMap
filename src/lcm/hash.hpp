// SHA-256 content hashing used for file/body/span hashes and stable IDs.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace lcm {

struct Sha256Digest {
  std::array<std::uint8_t, 32> bytes{};

  // Lowercase hexadecimal, 64 characters.
  [[nodiscard]] std::string hex() const;

  friend bool operator==(const Sha256Digest&, const Sha256Digest&) = default;
  friend auto operator<=>(const Sha256Digest&, const Sha256Digest&) = default;
};

class Sha256 {
 public:
  Sha256();
  void update(std::span<const std::byte> data);
  void update(std::string_view data);
  [[nodiscard]] Sha256Digest finish();

 private:
  void process_block(const std::uint8_t* block);
  void absorb(const std::uint8_t* data, std::size_t size);

  std::array<std::uint32_t, 8> state_{};
  std::array<std::uint8_t, 64> buffer_{};
  std::size_t buffer_len_ = 0;
  std::uint64_t total_len_ = 0;
};

[[nodiscard]] Sha256Digest sha256(std::span<const std::byte> data);
[[nodiscard]] Sha256Digest sha256(std::string_view data);

}  // namespace lcm
