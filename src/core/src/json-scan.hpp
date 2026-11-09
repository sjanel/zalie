#pragma once

// Scanning of JSON text without building a document: used by the JSONPath engine and the JSON Lines data files.

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "zalie/raw-chars.hpp"

namespace zalie {

[[nodiscard]] constexpr bool IsJsonSpace(char ch) noexcept {
  return ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t';
}

[[nodiscard]] constexpr int HexDigitValue(char ch) noexcept {
  if (ch >= '0' && ch <= '9') {
    return ch - '0';
  }
  if (ch >= 'a' && ch <= 'f') {
    return ch - 'a' + 10;
  }
  if (ch >= 'A' && ch <= 'F') {
    return ch - 'A' + 10;
  }
  return -1;
}

// Reads 4 hex digits at 'pos'; -1 if invalid.
[[nodiscard]] inline std::int32_t ReadHex4(std::string_view text, std::size_t pos) noexcept {
  if (pos + 4 > text.size()) {
    return -1;
  }
  std::int32_t value = 0;
  for (std::size_t i = 0; i < 4; ++i) {
    const int digit = HexDigitValue(text[pos + i]);
    if (digit < 0) {
      return -1;
    }
    value = (value * 16) + digit;
  }
  return value;
}

inline void AppendUtf8(RawChars32& out, std::uint32_t codePoint) {
  if (codePoint < 0x80U) {
    out.push_back(static_cast<char>(codePoint));
  } else if (codePoint < 0x800U) {
    out.push_back(static_cast<char>(0xC0U | (codePoint >> 6U)));
    out.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
  } else if (codePoint < 0x10000U) {
    out.push_back(static_cast<char>(0xE0U | (codePoint >> 12U)));
    out.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
  } else {
    out.push_back(static_cast<char>(0xF0U | (codePoint >> 18U)));
    out.push_back(static_cast<char>(0x80U | ((codePoint >> 12U) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
  }
}

// Appends the decoded escapes of a string body (between the quotes) to 'out': the common JSON escapes, \uXXXX
// (surrogate pairs included) and \' (JSONPath single-quoted names). Never writes more bytes than 'raw' has. Returns
// false on an invalid escape.
[[nodiscard]] inline bool AppendUnescaped(std::string_view raw, RawChars32& out) {
  out.ensureAvailableCapacity(raw.size());
  for (std::size_t pos = 0; pos < raw.size(); ++pos) {
    const char ch = raw[pos];
    if (ch != '\\') {
      out.push_back(ch);
      continue;
    }
    if (++pos == raw.size()) {
      return false;
    }
    switch (raw[pos]) {
      case '"':
        out.push_back('"');
        break;
      case '\'':
        out.push_back('\'');
        break;
      case '\\':
        out.push_back('\\');
        break;
      case '/':
        out.push_back('/');
        break;
      case 'b':
        out.push_back('\b');
        break;
      case 'f':
        out.push_back('\f');
        break;
      case 'n':
        out.push_back('\n');
        break;
      case 'r':
        out.push_back('\r');
        break;
      case 't':
        out.push_back('\t');
        break;
      case 'u': {
        std::int32_t unit = ReadHex4(raw, pos + 1);
        if (unit < 0) {
          return false;
        }
        pos += 4;
        auto codePoint = static_cast<std::uint32_t>(unit);
        if (codePoint >= 0xD800U && codePoint <= 0xDBFFU) {
          // High surrogate: a low one must follow.
          if (pos + 2 >= raw.size() || raw[pos + 1] != '\\' || raw[pos + 2] != 'u') {
            return false;
          }
          unit = ReadHex4(raw, pos + 3);
          if (unit < 0xDC00 || unit > 0xDFFF) {
            return false;
          }
          pos += 6;
          codePoint = 0x10000U + ((codePoint - 0xD800U) << 10U) + (static_cast<std::uint32_t>(unit) - 0xDC00U);
        } else if (codePoint >= 0xDC00U && codePoint <= 0xDFFFU) {
          return false;
        }
        AppendUtf8(out, codePoint);
        break;
      }
      default:
        return false;
    }
  }
  return true;
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
