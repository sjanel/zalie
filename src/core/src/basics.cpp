// Small building blocks: methods, key / value lookups, string helpers.

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
  for (Method method : kMethods) {
    if (EqualsIgnoreCase(name, MethodToStr(method))) {
      return method;
    }
  }
  return std::nullopt;
}

const KeyValue* FindEnabled(const KeyValues& entries, std::string_view key, bool caseInsensitive) noexcept {
  const auto it = std::ranges::find_if(entries, [key, caseInsensitive](const KeyValue& entry) {
    return entry.enabled && (caseInsensitive ? EqualsIgnoreCase(entry.key, key) : View(entry.key) == key);
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
  const char first = ToLowerAscii(needle.front());
  const std::size_t last = haystack.size() - needle.size();
  for (std::size_t i = pos; i <= last; ++i) {
    if (ToLowerAscii(haystack[i]) == first && EqualsIgnoreCase(haystack.substr(i, needle.size()), needle)) {
      return i;
    }
  }
  return std::string_view::npos;
}

RawChars32 ToLower(std::string_view sv) {
  RawChars32 ret(sv.size());
  for (char ch : sv) {
    ret.unchecked_push_back(ToLowerAscii(ch));
  }
  return ret;
}

bool IsHttpToken(std::string_view name) noexcept {
  if (name.empty()) {
    return false;
  }
  return std::ranges::all_of(name, [](char ch) {
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) {
      return true;
    }
    static constexpr std::string_view kTokenSpecials = "!#$%&'*+-.^_`|~";
    return kTokenSpecials.contains(ch);
  });
}

}  // namespace zalie
