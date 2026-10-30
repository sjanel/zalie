#pragma once

#include <amc/smallvector.hpp>
#include <cstdint>
#include <string_view>

namespace zalie {

// Syntax classes used to color text.
enum class TokenKind : std::uint8_t {
  plain,
  key,
  string,
  number,
  keyword,
  punctuation,
  tag,
  attribute,
  comment,
  variable
};

// A colored span of a line: [begin, begin + length) in bytes.
struct Token {
  std::uint32_t begin;
  std::uint32_t length;
  TokenKind kind;
};

// Tokens of one line. Lines are short: the inline capacity avoids any allocation while rendering.
using LineTokens = amc::SmallVector<Token, 32>;

// Language used to color a text.
enum class SyntaxLanguage : std::uint8_t { plain, json, xml, shell, code, http };

// State carried from one line to the next (multi-line comments, tags, strings...). Zero is the initial state.
using SyntaxState = std::uint8_t;

// Tokenizes one line of 'language' starting in 'state' (updated to the state at the end of the line).
// Tokens cover the colored spans only: the gaps are plain text. Adjacent tokens are ordered by position.
void TokenizeLine(SyntaxLanguage language, std::string_view line, SyntaxState& state, LineTokens& out);

}  // namespace zalie
