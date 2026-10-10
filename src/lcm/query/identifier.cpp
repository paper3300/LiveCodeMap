#include "lcm/query/identifier.hpp"

namespace lcm::query {

namespace {

enum class CharClass { separator, lower, upper, digit };

CharClass classify(unsigned char c) {
  if (c >= 'a' && c <= 'z') return CharClass::lower;
  if (c >= 'A' && c <= 'Z') return CharClass::upper;
  if (c >= '0' && c <= '9') return CharClass::digit;
  if (c >= 0x80) return CharClass::lower;  // UTF-8 bytes: word characters, never a case boundary
  return CharClass::separator;
}

bool is_space(unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }

}  // namespace

std::string trim(std::string_view text) {
  std::size_t begin = 0;
  std::size_t end = text.size();
  while (begin < end && is_space(static_cast<unsigned char>(text[begin]))) ++begin;
  while (end > begin && is_space(static_cast<unsigned char>(text[end - 1]))) --end;
  return std::string(text.substr(begin, end - begin));
}

std::vector<std::string> identifier_components(std::string_view text) {
  std::vector<std::string> out;
  std::string current;
  const auto flush = [&] {
    if (current.empty()) return;
    for (char& c : current) {
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    out.push_back(std::move(current));
    current.clear();
  };
  for (std::size_t i = 0; i < text.size(); ++i) {
    const auto c = static_cast<unsigned char>(text[i]);
    const CharClass now = classify(c);
    if (now == CharClass::separator) {
      flush();
      continue;
    }
    if (!current.empty()) {
      const CharClass before = classify(static_cast<unsigned char>(current.back()));
      const bool next_lower = i + 1 < text.size() && classify(static_cast<unsigned char>(text[i + 1])) == CharClass::lower;
      const bool boundary = (before == CharClass::lower && now == CharClass::upper) ||
                            ((before == CharClass::digit) != (now == CharClass::digit)) ||
                            (before == CharClass::upper && now == CharClass::upper && next_lower);
      if (boundary) flush();
    }
    current.push_back(static_cast<char>(c));
  }
  flush();
  return out;
}

bool contains_run(const std::vector<std::string>& haystack, const std::vector<std::string>& needle) {
  if (needle.empty() || needle.size() > haystack.size()) return false;
  for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
    bool match = true;
    for (std::size_t j = 0; j < needle.size() && match; ++j) match = haystack[i + j] == needle[j];
    if (match) return true;
  }
  return false;
}

}  // namespace lcm::query
