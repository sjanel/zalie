#pragma once

// Scanning of JSON text without building a document: used by the JSONPath engine and the JSON Lines data files.

#include <cstddef>
#include <string_view>

namespace zalie {

[[nodiscard]] constexpr bool IsJsonSpace(char ch) noexcept {
  return ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t';
}

[[nodiscard]] inline std::size_t SkipSpaces(std::string_view json, std::size_t pos) noexcept {
  while (pos < json.size() && IsJsonSpace(json[pos])) {
    ++pos;
  }
  return pos;
}

// 'pos' on the opening quote: position after the closing quote, npos if there is none.
[[nodiscard]] inline std::size_t StringEnd(std::string_view json, std::size_t pos) noexcept {
  for (++pos; pos < json.size(); ++pos) {
    if (json[pos] == '\\') {
      ++pos;
    } else if (json[pos] == '"') {
      return pos + 1;
    }
  }
  return std::string_view::npos;
}

// 'pos' on the first character of a value: position after its last character, npos if it is incomplete.
[[nodiscard]] inline std::size_t ValueEnd(std::string_view json, std::size_t pos) noexcept {
  if (pos >= json.size()) {
    return std::string_view::npos;
  }
  const char first = json[pos];
  if (first == '"') {
    return StringEnd(json, pos);
  }
  if (first == '{' || first == '[') {
    std::size_t depth = 0;
    while (pos < json.size()) {
      const char ch = json[pos];
      if (ch == '"') {
        pos = StringEnd(json, pos);
        if (pos == std::string_view::npos) {
          return std::string_view::npos;
        }
        continue;
      }
      if (ch == '{' || ch == '[') {
        ++depth;
      } else if ((ch == '}' || ch == ']') && --depth == 0) {
        return pos + 1;
      }
      ++pos;
    }
    return std::string_view::npos;
  }
  const std::size_t begin = pos;
  while (pos < json.size() && !IsJsonSpace(json[pos]) && json[pos] != ',' && json[pos] != '}' && json[pos] != ']') {
    ++pos;
  }
  return pos == begin ? std::string_view::npos : pos;
}

// Calls 'func(rawKey, value)' for the members of 'object' (a view starting with '{'), in order, until it returns
// false. 'rawKey' is the key without its quotes, escapes untouched. Returns true if the whole object was well-formed
// and all the calls returned true.
template <class Func>
[[nodiscard]] bool ScanJsonObject(std::string_view object, Func&& func) {
  std::size_t pos = SkipSpaces(object, 1);
  if (pos < object.size() && object[pos] == '}') {
    return pos + 1 == object.size();
  }
  while (pos < object.size() && object[pos] == '"') {
    const std::size_t keyEnd = StringEnd(object, pos);
    if (keyEnd == std::string_view::npos) {
      return false;
    }
    const std::string_view rawKey = object.substr(pos + 1, keyEnd - pos - 2);
    pos = SkipSpaces(object, keyEnd);
    if (pos >= object.size() || object[pos] != ':') {
      return false;
    }
    pos = SkipSpaces(object, pos + 1);
    const std::size_t valueEnd = ValueEnd(object, pos);
    if (valueEnd == std::string_view::npos || !func(rawKey, object.substr(pos, valueEnd - pos))) {
      return false;
    }
    pos = SkipSpaces(object, valueEnd);
    if (pos >= object.size()) {
      return false;
    }
    if (object[pos] == '}') {
      return pos + 1 == object.size();
    }
    if (object[pos] != ',') {
      return false;
    }
    pos = SkipSpaces(object, pos + 1);
  }
  return false;
}

}  // namespace zalie
