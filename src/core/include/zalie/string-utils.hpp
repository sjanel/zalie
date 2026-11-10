#pragma once

#include <aeronet/cctype.hpp>
#include <aeronet/string-equal-ignore-case.hpp>
#include <aeronet/tchars.hpp>
#include <aeronet/tolower-str.hpp>
#include <algorithm>
#include <cstddef>
#include <string_view>

// String helpers of Zalie, on top of the ASCII primitives of aeronet used as they are in the code: aeronet::isspace,
// aeronet::isdigit, aeronet::tolower and aeronet::CaseInsensitiveEqual (a word at a time).

namespace zalie {

// Removes leading and trailing ASCII white spaces.
[[nodiscard]] constexpr std::string_view Trim(std::string_view sv) noexcept {
  const char* pBegin = sv.data();
  const char* pEnd = pBegin + sv.size();
  while (pBegin != pEnd && aeronet::isspace(*pBegin)) {
    ++pBegin;
  }
  while (pBegin != pEnd && aeronet::isspace(pEnd[-1])) {
    --pEnd;
  }
  return {pBegin, pEnd};
}

// ASCII case-insensitive prefix test.
[[nodiscard]] constexpr bool StartsWithIgnoreCase(std::string_view sv, std::string_view prefix) noexcept {
  return sv.size() >= prefix.size() && aeronet::CaseInsensitiveEqual(sv.substr(0, prefix.size()), prefix);
}

// ASCII case-insensitive search of `needle` in `haystack`, starting at `pos`. Returns npos if absent.
[[nodiscard]] std::size_t FindIgnoreCase(std::string_view haystack, std::string_view needle,
                                         std::size_t pos = 0) noexcept;

// Whether `haystack` contains `needle`, ASCII case-insensitively.
[[nodiscard]] inline bool ContainsIgnoreCase(std::string_view haystack, std::string_view needle) noexcept {
  return FindIgnoreCase(haystack, needle) != std::string_view::npos;
}

// Replaces the content of 'out' with 'sv' in ASCII lower case.
template <class Buffer>
void AssignLower(Buffer& out, std::string_view sv) {
  out.clear();
  out.ensureAvailableCapacity(sv.size());
  aeronet::tolower_n(sv.data(), sv.size(), out.data());
  out.setSize(static_cast<Buffer::size_type>(sv.size()));
}

// Whether `name` is a valid HTTP field name (RFC 9110 token).
[[nodiscard]] constexpr bool IsHttpToken(std::string_view name) noexcept {
  return !name.empty() && std::ranges::all_of(name, [](char ch) { return aeronet::is_tchar(ch); });
}

}  // namespace zalie
