#pragma once

#include <cstddef>
#include <string_view>

#include "zalie/raw-chars.hpp"

namespace zalie {

[[nodiscard]] constexpr bool IsSpace(char ch) noexcept {
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
}

[[nodiscard]] constexpr char ToLowerAscii(char ch) noexcept {
  return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch;
}

// Removes leading and trailing ASCII white spaces.
[[nodiscard]] constexpr std::string_view Trim(std::string_view sv) noexcept {
  while (!sv.empty() && IsSpace(sv.front())) {
    sv.remove_prefix(1);
  }
  while (!sv.empty() && IsSpace(sv.back())) {
    sv.remove_suffix(1);
  }
  return sv;
}

// ASCII case-insensitive equality.
[[nodiscard]] constexpr bool EqualsIgnoreCase(std::string_view lhs, std::string_view rhs) noexcept {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (std::size_t i = 0; i < lhs.size(); ++i) {
    if (ToLowerAscii(lhs[i]) != ToLowerAscii(rhs[i])) {
      return false;
    }
  }
  return true;
}

// ASCII case-insensitive prefix test.
[[nodiscard]] constexpr bool StartsWithIgnoreCase(std::string_view sv, std::string_view prefix) noexcept {
  return sv.size() >= prefix.size() && EqualsIgnoreCase(sv.substr(0, prefix.size()), prefix);
}

// ASCII case-insensitive search of `needle` in `haystack`, starting at `pos`. Returns npos if absent.
[[nodiscard]] std::size_t FindIgnoreCase(std::string_view haystack, std::string_view needle,
                                         std::size_t pos = 0) noexcept;

// Returns an ASCII lower-cased copy.
[[nodiscard]] RawChars32 ToLower(std::string_view sv);

// Whether `name` is a valid HTTP field name (RFC 9110 token).
[[nodiscard]] bool IsHttpToken(std::string_view name) noexcept;

}  // namespace zalie
