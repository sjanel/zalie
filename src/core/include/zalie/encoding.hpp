#pragma once

// Text encodings shared by the importers, the code snippets and the requests: escaped strings, UTF-8, hexadecimal
// digits and base64. One implementation per encoding, on the primitives of aeronet.

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <stdexcept>
#include <string_view>

#include "zalie/raw-chars.hpp"

namespace zalie {

// How a dialect writes the control characters (below 0x20) it has no dedicated escape for.
enum class ControlEscapes : std::uint8_t { none, unicode, octal };

// The replacement text of each character a dialect (JSON, a C++ literal, a shell...) escapes, built at compile time.
class EscapeTable {
 public:
  struct Rule {
    char ch;
    std::string_view text;
  };

  // A replacement has 2 to kMaxTextSize characters ("\u00XX").
  static constexpr std::size_t kMaxTextSize = 6;
  // write() stores kSlotSize bytes whatever the replacement: up to kWriteSlack bytes after its end.
  static constexpr std::size_t kSlotSize = 8;
  static constexpr std::size_t kWriteSlack = kSlotSize - 2;

  // 'rules' are the escaped characters (ASCII) and their replacement. 'controls' adds the escape of every control
  // character without a rule: "\u00XX" (unicode) or "\0oo" (octal).
  consteval EscapeTable(std::initializer_list<Rule> rules, ControlEscapes controls = ControlEscapes::none) {
    for (char ch = 0; controls != ControlEscapes::none && ch < 0x20; ++ch) {
      if (controls == ControlEscapes::unicode) {
        const char text[]{'\\', 'u', '0', '0', "0123456789abcdef"[ch >> 4], "0123456789abcdef"[ch & 0xF]};
        set(ch, std::string_view(text, sizeof(text)));
      } else {
        const char text[]{'\\', '0', static_cast<char>('0' + (ch >> 3)), static_cast<char>('0' + (ch & 7))};
        set(ch, std::string_view(text, sizeof(text)));
      }
    }
    for (const Rule& rule : rules) {
      set(rule.ch, rule.text);
    }
  }

  // Number of bytes the escaping of 'ch' adds to it, 0 when it is written as it is.
  [[nodiscard]] constexpr std::size_t extraSize(char ch) const noexcept {
    return _extraSizes[static_cast<unsigned char>(ch)];
  }

  // Writes the replacement of 'ch', an escaped character (extraSize(ch) != 0), at 'pOut' and returns its end.
  // The store has a constant size, kSlotSize: 'pOut' must have room for it.
  [[nodiscard]] char* write(char ch, char* pOut) const noexcept {
    const auto idx = static_cast<unsigned char>(ch);
    assert(_extraSizes[idx] != 0);
    std::memcpy(pOut, _texts[idx].data(), kSlotSize);
    return pOut + _extraSizes[idx] + 1U;
  }

 private:
  static constexpr std::size_t kNbEscapable = 128;

  consteval void set(char ch, std::string_view text) {
    const auto idx = static_cast<unsigned char>(ch);
    if (idx >= kNbEscapable || text.size() < 2 || text.size() > kMaxTextSize) {
      throw std::invalid_argument("an escape rule is for an ASCII character, replaced by 2 to 6 characters");
    }
    _texts[idx] = {};
    std::ranges::copy(text, _texts[idx].begin());
    _extraSizes[idx] = static_cast<std::uint8_t>(text.size() - 1);
  }

  // Indexed by any byte, so that the scan of a string has no bound to check. Only ASCII characters have a text.
  std::array<std::uint8_t, 256> _extraSizes{};
  std::array<std::array<char, kSlotSize>, kNbEscapable> _texts{};
};

// JSON strings, which are also the double-quoted literals of Python, JavaScript and Go.
inline constexpr EscapeTable kJsonEscapes(
    {{'"', "\\\""}, {'\\', "\\\\"}, {'\n', "\\n"}, {'\r', "\\r"}, {'\t', "\\t"}, {'\b', "\\b"}, {'\f', "\\f"}},
    ControlEscapes::unicode);

// C++ string literals. Octal escapes for the control characters: a hexadecimal one would swallow the hex digits
// that follow it.
inline constexpr EscapeTable kCppEscapes({{'"', "\\\""}, {'\\', "\\\\"}, {'\n', "\\n"}, {'\r', "\\r"}, {'\t', "\\t"}},
                                         ControlEscapes::octal);

// Single-quoted words of a POSIX shell: the quote is closed, escaped and opened again.
inline constexpr EscapeTable kShellEscapes({{'\'', "'\\''"}});

// Single-quoted strings of PowerShell: quotes are doubled.
inline constexpr EscapeTable kPowerShellEscapes({{'\'', "''"}});

// Values of type string of a desktop entry (desktop entry specification, "Possible value types").
inline constexpr EscapeTable kDesktopStringEscapes({{'\\', "\\\\"}, {'\n', "\\n"}, {'\t', "\\t"}, {'\r', "\\r"}});

// Quoted arguments of the Exec key of a desktop entry ("The Exec key"): '%' is doubled.
inline constexpr EscapeTable kDesktopExecEscapes(
    {{'"', "\\\""}, {'`', "\\`"}, {'$', "\\$"}, {'\\', "\\\\"}, {'%', "%%"}});

// A string escaped with a table, between two 'quote' characters unless 'quote' is '\0': a part of AppendAll.
// The runs of characters written as they are, are copied at once.
class Escaped {
 public:
  constexpr Escaped(std::string_view str, const EscapeTable& table, char quote = '\0') noexcept
      : _str(str), _pTable(&table), _quote(quote) {}

  [[nodiscard]] std::size_t maxSize() const noexcept;
  [[nodiscard]] char* writeTo(char* pData) const noexcept;

 private:
  std::string_view _str;
  const EscapeTable* _pTable;
  char _quote;
};

// The base64 encoding (RFC 4648, padded) of a string: a part of AppendAll.
class Base64 {
 public:
  explicit constexpr Base64(std::string_view data) noexcept : _data(data) {}

  [[nodiscard]] std::size_t maxSize() const noexcept;
  [[nodiscard]] char* writeTo(char* pData) const noexcept;

 private:
  std::string_view _data;
};

// The character "\<esc>" stands for in C, JSON, the ANSI-C quotes of the shells and the desktop entries: n, t, r, b,
// f, a, v and e (escape). '\0' when 'esc' is none of them.
[[nodiscard]] constexpr char ControlEscape(char esc) noexcept {
  constexpr std::string_view escapeNames = "ntrbfave";
  constexpr char escapeValues[]{'\n', '\t', '\r', '\b', '\f', '\a', '\v', '\x1B', '\0'};
  return escapeValues[std::min(escapeNames.find(esc), escapeNames.size())];
}

// The UTF-8 encoding of a code point takes at most this number of bytes.
inline constexpr std::size_t kMaxUtf8Size = 4;

// Writes the UTF-8 encoding of 'codePoint' at 'pOut' (at most kMaxUtf8Size bytes) and returns its end.
[[nodiscard]] char* AppendUtf8(char32_t codePoint, char* pOut) noexcept;

// Reads the hexadecimal digits at the start of 'str', at most 'maxDigits' of them, into 'value'. Returns their number.
[[nodiscard]] std::size_t ReadHex(std::string_view str, std::size_t maxDigits, char32_t& value) noexcept;

// Appends the decoded escapes of the body of a JSON string (between its quotes) to 'out': the common JSON escapes,
// \uXXXX (surrogate pairs included) and \' (JSONPath single-quoted names). The decoded text is never longer than
// 'raw': no reallocation when 'out' already has room for it. Returns false on an invalid escape, 'out' is then left
// unchanged.
[[nodiscard]] bool AppendJsonUnescaped(std::string_view raw, RawChars32& out);

}  // namespace zalie
