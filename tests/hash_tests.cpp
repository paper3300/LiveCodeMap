#include <doctest/doctest.h>

#include <string>

#include "lcm/hash.hpp"

namespace {

// Deterministic non-trivial byte pattern shared with the Python script that
// produced the expected digests: byte i = (i*7+3) & 0xff.
std::string pattern(std::size_t n) {
  std::string s(n, '\0');
  for (std::size_t i = 0; i < n; ++i) s[i] = static_cast<char>((i * 7 + 3) & 0xff);
  return s;
}

}  // namespace

TEST_CASE("sha256 matches published test vectors") {
  CHECK(lcm::sha256("").hex() == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(lcm::sha256("abc").hex() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(lcm::sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq").hex() ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  CHECK(lcm::sha256("The quick brown fox jumps over the lazy dog").hex() ==
        "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592");
  CHECK(lcm::sha256(std::string(1000000, 'a')).hex() ==
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("sha256 block boundary lengths match independent answers (hashlib)") {
  struct Case {
    std::size_t length;
    const char* hex;
  };
  const Case cases[] = {
      {0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
      {1, "084fed08b978af4d7d196a7446a86b58009e636b611db16211b65a9aadff29c5"},
      {3, "6ab0dba1f4f1dfbb37b4f9eeb092c09fca4900ad32bdcd147d8dde35d6c87c35"},
      {55, "e7313d333c272e639f790978283f9eb392e843d0f29b7016828bb1daa4aac70b"},
      {56, "4324d65f3c103567f5589c710bc08f8523f929a9272e3af36fc968e52abc6c27"},
      {57, "35df609437dcfea3279283ab79fd554e2bf78f8f7ae2de532d8ee300b09e8f73"},
      {63, "81c80242132f230c3bd41b3e63bbcff16107339549214a99614ff26664625055"},
      {64, "39e3d7b6b5d075d37d053ad89b24b41bef4f3c29760c84447cab3f3be1882241"},
      {65, "aacca6ff74fdbb296d165a45cecfa04e5127bc008770fbbdd48006f2d2fae95e"},
      {119, "9ce7368e4daf32341631b492e80359dc9f594b48453cd0dd5bf0b19279cc177e"},
      {120, "7836b787757e95e58b3ca5aec90b1b004e8deba1e50e9675af9cabf1a13a04b5"},
      {128, "d2742f1f4ac6bb7ca2b239ee18402ba8b3f9f8e652d2a72973c2b9ba11c08cf6"},
      {129, "307f8fc2c1622b92762e818d39a185d4d667ad49a4b07ceae1f4afa008a93ec4"},
  };
  for (const auto& c : cases) {
    CAPTURE(c.length);
    CHECK(lcm::sha256(pattern(c.length)).hex() == c.hex);
  }
}

TEST_CASE("sha256 incremental updates equal one-shot, including empty interleaving") {
  const std::string data = pattern(200);
  const auto expected = lcm::sha256(data).hex();

  SUBCASE("chunked at every split point") {
    for (std::size_t split = 0; split <= data.size(); ++split) {
      lcm::Sha256 h;
      h.update(std::string_view(data).substr(0, split));
      h.update(std::string_view(data).substr(split));
      CAPTURE(split);
      CHECK(h.finish().hex() == expected);
    }
  }

  SUBCASE("empty updates before, between and after buffered data are no-ops") {
    lcm::Sha256 h;
    h.update(std::string_view{});
    h.update(std::string_view(data).substr(0, 10));  // leaves 10 bytes buffered
    h.update(std::span<const std::byte>{});           // null data pointer, zero length
    h.update(std::string_view(data).substr(10, 60));  // crosses a block boundary
    h.update(std::string_view{});
    h.update(std::string_view(data).substr(70));
    h.update(std::span<const std::byte>{});
    CHECK(h.finish().hex() == expected);
  }

  SUBCASE("byte-at-a-time") {
    lcm::Sha256 h;
    for (char c : data) h.update(std::string_view(&c, 1));
    CHECK(h.finish().hex() == expected);
  }
}
