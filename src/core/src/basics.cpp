// Small building blocks: methods, key / value lookups, string helpers.

#include <aeronet/string-equal-ignore-case.hpp>
#include <aeronet/toupperlower.hpp>
#include <algorithm>
#include <cstddef>
#include <optional>
#include <string_view>

#include "zalie/key-value.hpp"
#include "zalie/method.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/string-utils.hpp"

namespace zalie {

std::optional<Method> ParseMethod(std::string_view name) noexcept {
  name = Trim(name);
  const auto it = std::ranges::find_if(
      kMethodNames, [name](std::string_view method) { return aeronet::CaseInsensitiveEqual(name, method); });
  if (it == kMethodNames.end()) {
    return std::nullopt;
  }
  return kMethods[static_cast<std::size_t>(it - kMethodNames.begin())];
}

const KeyValue* FindEnabled(const KeyValues& entries, std::string_view key, bool caseInsensitive) noexcept {
  const auto it = std::ranges::find_if(entries, [key, caseInsensitive](const KeyValue& entry) {
    return entry.enabled && (caseInsensitive ? aeronet::CaseInsensitiveEqual(entry.key, key) : View(entry.key) == key);
  });
  return it == entries.end() ? nullptr : &*it;
}

std::size_t FindIgnoreCase(std::string_view haystack, std::string_view needle, std::size_t pos) noexcept {
  if (needle.empty()) {
    return pos <= haystack.size() ? pos : std::string_view::npos;
  }
  if (needle.size() > haystack.size()) {
    return std::string_view::npos;
  }
  const char first = aeronet::tolower(needle.front());
  const std::size_t last = haystack.size() - needle.size();
  for (; pos <= last; ++pos) {
    if (aeronet::tolower(haystack[pos]) == first &&
        aeronet::CaseInsensitiveEqual(haystack.substr(pos, needle.size()), needle)) {
      return pos;
    }
  }
  return std::string_view::npos;
}

}  // namespace zalie
