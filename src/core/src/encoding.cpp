#include "zalie/encoding.hpp"

#include <aeronet/base64.hpp>
#include <aeronet/char-hexadecimal-converter.hpp>
#include <aeronet/find-char.hpp>
#include <aeronet/memory-utils.hpp>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "zalie/raw-chars.hpp"

namespace zalie {

namespace {

constexpr std::string_view kUnicodeEscape = "\\u";
constexpr std::size_t kNbUnicodeDigits = 4;

constexpr bool IsHighSurrogate(char32_t unit) noexcept { return unit >= 0xD800U && unit <= 0xDBFFU; }
constexpr bool IsLowSurrogate(char32_t unit) noexcept { return unit >= 0xDC00U && unit <= 0xDFFFU; }

}  // namespace

std::size_t Escaped::maxSize() const noexcept {
  std::size_t size = _str.size() + EscapeTable::kWriteSlack + (_quote == '\0' ? 0U : 2U);
  for (char ch : _str) {
    size += _pTable->extraSize(ch);
  }
  return size;
}

char* Escaped::writeTo(char* pData) const noexcept {
  const EscapeTable& table = *_pTable;
  if (_quote != '\0') {
    *pData++ = _quote;
  }
  const char* pIt = _str.data();
  const char* const pEnd = pIt + _str.size();
  while (pIt != pEnd) {
    const char* const pRun = pIt;
    while (pIt != pEnd && table.extraSize(*pIt) == 0) {
      ++pIt;
    }
    pData = aeronet::Append(pRun, static_cast<std::size_t>(pIt - pRun), pData);
    if (pIt != pEnd) {
      pData = table.write(*pIt, pData);
      ++pIt;
    }
  }
  if (_quote != '\0') {
    *pData++ = _quote;
  }
  return pData;
}

std::size_t Base64::maxSize() const noexcept { return aeronet::B64EncodedLen(_data.size()); }

char* Base64::writeTo(char* pData) const noexcept {
  char* pEnd = pData + aeronet::B64EncodedLen(_data.size());
  aeronet::B64Encode(_data, pData, pEnd);
  return pEnd;
}

char* AppendUtf8(char32_t codePoint, char* pOut) noexcept {
  const auto continuation = [codePoint](unsigned shift) {
    return static_cast<char>(0x80U | ((codePoint >> shift) & 0x3FU));
  };
  if (codePoint < 0x80U) {
    *pOut++ = static_cast<char>(codePoint);
  } else if (codePoint < 0x800U) {
    *pOut++ = static_cast<char>(0xC0U | (codePoint >> 6U));
    *pOut++ = continuation(0);
  } else if (codePoint < 0x10000U) {
    *pOut++ = static_cast<char>(0xE0U | (codePoint >> 12U));
    *pOut++ = continuation(6);
    *pOut++ = continuation(0);
  } else {
    *pOut++ = static_cast<char>(0xF0U | (codePoint >> 18U));
    *pOut++ = continuation(12);
    *pOut++ = continuation(6);
    *pOut++ = continuation(0);
  }
  return pOut;
}

std::size_t ReadHex(std::string_view str, std::size_t maxDigits, char32_t& value) noexcept {
  const std::size_t limit = std::min(maxDigits, str.size());
  std::size_t nbDigits = 0;
  value = 0;
  for (; nbDigits < limit; ++nbDigits) {
    const std::int8_t digit = aeronet::FromHexDigit(str[nbDigits]);
    if (digit < 0) {
      break;
    }
    value = (value << 4U) | static_cast<char32_t>(digit);
  }
  return nbDigits;
}

bool AppendJsonUnescaped(std::string_view raw, RawChars32& out) {
  static constexpr std::string_view kSameCharEscapes = "\"'\\/";
  static constexpr std::string_view kControlEscapes = "bfnrt";

  out.ensureAvailableCapacity(raw.size());
  char* pOut = out.end();
  const char* pIt = raw.data();
  const char* const pEnd = pIt + raw.size();
  while (pIt != pEnd) {
    const char* const pEscape = aeronet::FindChar(pIt, pEnd, '\\');
    pOut = aeronet::Append(pIt, static_cast<std::size_t>(pEscape - pIt), pOut);
    if (pEscape == pEnd) {
      break;
    }
    pIt = pEscape + 1;
    if (pIt == pEnd) {
      return false;
    }
    const char esc = *pIt++;
    if (kSameCharEscapes.contains(esc)) {
      *pOut++ = esc;
    } else if (kControlEscapes.contains(esc)) {
      *pOut++ = ControlEscape(esc);
    } else if (esc == 'u') {
      char32_t codePoint = 0;
      if (ReadHex({pIt, pEnd}, kNbUnicodeDigits, codePoint) != kNbUnicodeDigits) {
        return false;
      }
      pIt += kNbUnicodeDigits;
      if (IsHighSurrogate(codePoint)) {
        // A low surrogate must follow.
        const std::string_view rest(pIt, pEnd);
        char32_t low = 0;
        if (!rest.starts_with(kUnicodeEscape) ||
            ReadHex(rest.substr(kUnicodeEscape.size()), kNbUnicodeDigits, low) != kNbUnicodeDigits ||
            !IsLowSurrogate(low)) {
          return false;
        }
        pIt += kUnicodeEscape.size() + kNbUnicodeDigits;
        codePoint = 0x10000U + ((codePoint - 0xD800U) << 10U) + (low - 0xDC00U);
      } else if (IsLowSurrogate(codePoint)) {
        return false;
      }
      pOut = AppendUtf8(codePoint, pOut);
    } else {
      return false;
    }
  }
  out.setEnd(pOut);
  return true;
}

}  // namespace zalie
