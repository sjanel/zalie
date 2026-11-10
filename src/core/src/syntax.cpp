#include "zalie/syntax.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace zalie {

namespace {

constexpr std::size_t kNpos = std::string_view::npos;

// Classes of the bytes of a line, as bits of a single table: a rule made of several classes is tested with the union
// of their bits, in a single lookup.
constexpr unsigned kBlank = 1U << 0U;            // ' ' / '\t' / '\r'
constexpr unsigned kWordStart = 1U << 1U;        // ALPHA / '_'
constexpr unsigned kDigit = 1U << 2U;            // DIGIT
constexpr unsigned kNumberSymbol = 1U << 3U;     // '.' / '-' / '+', inside a number (1.5e-3)
constexpr unsigned kQuote = 1U << 4U;            // '"' / '\''
constexpr unsigned kTagEnd = 1U << 5U;           // '>' / '/': end of an XML tag or attribute name
constexpr unsigned kTagMark = 1U << 6U;          // '/' / '!' / '?' just after the '<' of an XML tag
constexpr unsigned kEqual = 1U << 7U;            // '='
constexpr unsigned kCodePunctuation = 1U << 8U;  // the punctuation colored in code: { } [ ] ( ) ; , . : = < >
constexpr unsigned kWord = kWordStart | kDigit;
constexpr unsigned kNumber = kWord | kNumberSymbol;

// Returns true if 'ch' is in one of the classes of 'classMask'.
constexpr bool IsOfClass(char ch, unsigned classMask) noexcept {
  static constexpr auto kCharClasses = [] {
    std::array<std::uint16_t, 256> table{};
    const auto add = [&table](std::string_view chars, unsigned charClass) {
      for (const char byte : chars) {
        std::uint16_t& bits = table[static_cast<unsigned char>(byte)];
        bits = static_cast<std::uint16_t>(bits | charClass);
      }
    };
    add(" \t\r", kBlank);
    add("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_", kWordStart);
    add("0123456789", kDigit);
    add(".-+", kNumberSymbol);
    add("\"'", kQuote);
    add(">/", kTagEnd);
    add("/!?", kTagMark);
    add("=", kEqual);
    add("{}[]();,.:=<>", kCodePunctuation);
    return table;
  }();
  return (kCharClasses[static_cast<unsigned char>(ch)] & classMask) != 0;
}

// Position of the first byte from 'pos' that is not in 'classMask', or the line size.
constexpr std::size_t SkipClass(std::string_view line, std::size_t pos, unsigned classMask) noexcept {
  while (pos < line.size() && IsOfClass(line[pos], classMask)) {
    ++pos;
  }
  return pos;
}

// Position of the first byte from 'pos' that is in 'classMask', or the line size.
constexpr std::size_t FindClass(std::string_view line, std::size_t pos, unsigned classMask) noexcept {
  while (pos < line.size() && !IsOfClass(line[pos], classMask)) {
    ++pos;
  }
  return pos;
}

// End of a construct spanning lines (string, comment), scanned on one line.
struct Scan {
  std::size_t end;  // after the closing delimiter, or the line size
  bool closed;      // false: the construct goes on on the next line
};

// Scans up to and including 'terminator' from 'pos'.
Scan ScanTo(std::string_view line, std::size_t pos, std::string_view terminator) noexcept {
  const std::size_t found = line.find(terminator, pos);
  return found == kNpos ? Scan{line.size(), false} : Scan{found + terminator.size(), true};
}

// Scans the rest of a string from 'pos' (just after its opening quote, or the start of a line continuing it) up to
// and including the closing 'quote'. A backslash escapes the next byte if 'escapes'.
Scan ScanQuoted(std::string_view line, std::size_t pos, char quote, bool escapes) noexcept {
  if (!escapes) {
    return ScanTo(line, pos, std::string_view(&quote, 1));
  }
  for (; pos < line.size(); ++pos) {
    if (line[pos] == quote) {
      return {pos + 1, true};
    }
    if (line[pos] == '\\') {
      ++pos;
    }
  }
  return {line.size(), false};
}

// Scans the rest of a C++ raw string literal from 'pos': it ends at ')' + delimiter + '"'.
Scan ScanRawString(std::string_view line, std::size_t pos) noexcept {
  for (pos = line.find(')', pos); pos != kNpos; pos = line.find(')', pos + 1)) {
    const std::size_t delimiterEnd = SkipClass(line, pos + 1, kWord);
    if (delimiterEnd < line.size() && line[delimiterEnd] == '"') {
      return {delimiterEnd + 1, true};
    }
  }
  return {line.size(), false};
}

void Push(LineTokens& out, std::size_t begin, std::size_t end, TokenKind kind) {
  if (end > begin) {
    out.push_back({static_cast<std::uint32_t>(begin), static_cast<std::uint32_t>(end - begin), kind});
  }
}

void TokenizeJson(std::string_view line, SyntaxState& /*state*/, LineTokens& out) {
  std::size_t pos = 0;
  while (pos < line.size()) {
    const char ch = line[pos];
    std::size_t end = pos + 1;
    TokenKind kind = TokenKind::punctuation;
    if (IsOfClass(ch, kBlank)) {
      pos = end;
      continue;
    }
    if (ch == '"') {
      end = ScanQuoted(line, end, '"', true).end;
      const std::size_t next = SkipClass(line, end, kBlank);
      kind = next < line.size() && line[next] == ':' ? TokenKind::key : TokenKind::string;
    } else if (ch == '-' || IsOfClass(ch, kDigit)) {
      end = SkipClass(line, end, kNumber);
      kind = TokenKind::number;
    } else if (IsOfClass(ch, kWordStart)) {
      end = SkipClass(line, end, kWord);
      kind = TokenKind::keyword;
    }
    Push(out, pos, end, kind);
    pos = end;
  }
}

// XML / HTML states.
enum : SyntaxState { kXmlText = 0, kXmlTag = 1, kXmlComment = 2, kXmlDoubleQuoted = 3, kXmlSingleQuoted = 4 };

// Tokenizes one item of a tag from 'pos' (attribute, '=', value or end of the tag), returns the position after it.
std::size_t TokenizeXmlTagItem(std::string_view line, std::size_t pos, SyntaxState& state, LineTokens& out) {
  const char ch = line[pos];
  std::size_t end = pos + 1;
  TokenKind kind = TokenKind::punctuation;  // '='
  if (IsOfClass(ch, kBlank)) {
    return end;
  }
  if (ch == '>' || ((ch == '/' || ch == '?') && end < line.size() && line[end] == '>')) {
    end += ch == '>' ? 0 : 1;
    kind = TokenKind::tag;
    state = kXmlText;
  } else if (IsOfClass(ch, kQuote)) {
    // Attribute value, possibly continued on the next lines.
    const Scan scan = ScanQuoted(line, end, ch, false);
    end = scan.end;
    kind = TokenKind::string;
    if (!scan.closed) {
      state = ch == '"' ? kXmlDoubleQuoted : kXmlSingleQuoted;
    }
  } else if (ch != '=') {
    end = FindClass(line, end, kBlank | kEqual | kTagEnd | kQuote);
    kind = TokenKind::attribute;
  }
  Push(out, pos, end, kind);
  return end;
}

void TokenizeXml(std::string_view line, SyntaxState& state, LineTokens& out) {
  std::size_t pos = 0;
  while (pos < line.size()) {
    if (state == kXmlTag) {
      pos = TokenizeXmlTagItem(line, pos, state, out);
    } else if (state != kXmlText) {
      // Continuation of a comment or of an attribute value.
      const bool comment = state == kXmlComment;
      const Scan scan =
          comment ? ScanTo(line, pos, "-->") : ScanQuoted(line, pos, state == kXmlDoubleQuoted ? '"' : '\'', false);
      Push(out, pos, scan.end, comment ? TokenKind::comment : TokenKind::string);
      pos = scan.end;
      if (scan.closed) {
        state = comment ? kXmlText : kXmlTag;
      }
    } else {
      pos = line.find('<', pos);
      if (pos == kNpos) {
        return;
      }
      if (line.substr(pos).starts_with("<!--")) {
        state = kXmlComment;
        continue;
      }
      const std::size_t end = FindClass(line, SkipClass(line, pos + 1, kTagMark), kBlank | kTagEnd);
      Push(out, pos, end, TokenKind::tag);
      pos = end;
      state = kXmlTag;
    }
  }
}

// Shell states: inside a multi-line single / double quoted argument.
enum : SyntaxState { kShellNormal = 0, kShellSingle = 1, kShellDouble = 2 };

void TokenizeShell(std::string_view line, SyntaxState& state, LineTokens& out) {
  std::size_t pos = 0;
  bool wordStart = true;
  bool firstWord = state == kShellNormal;
  while (pos < line.size()) {
    if (state != kShellNormal) {
      // Continuation of a quoted argument opened on a previous line.
      const Scan scan = ScanQuoted(line, pos, state == kShellSingle ? '\'' : '"', state == kShellDouble);
      Push(out, pos, scan.end, TokenKind::string);
      pos = scan.end;
      if (scan.closed) {
        state = kShellNormal;
      }
      wordStart = false;
      continue;
    }
    const char ch = line[pos];
    if (IsOfClass(ch, kBlank)) {
      ++pos;
      wordStart = true;
      continue;
    }
    const bool ansiC = ch == '$' && pos + 1 < line.size() && line[pos + 1] == '\'';  // $'...' escapes like C
    if (ansiC || IsOfClass(ch, kQuote)) {
      const char quote = ansiC ? '\'' : ch;
      const Scan scan = ScanQuoted(line, pos + (ansiC ? 2 : 1), quote, ansiC || quote == '"');
      Push(out, pos, scan.end, TokenKind::string);
      pos = scan.end;
      if (!scan.closed) {
        state = quote == '\'' ? kShellSingle : kShellDouble;
      }
      wordStart = false;
      firstWord = false;
      continue;
    }
    if (ch == '#' && wordStart) {
      Push(out, pos, line.size(), TokenKind::comment);
      return;
    }
    if (ch == '\\' && pos + 1 == line.size()) {
      Push(out, pos, pos + 1, TokenKind::punctuation);
      return;
    }
    const std::size_t end = FindClass(line, pos + 1, kBlank | kQuote);
    if (wordStart && (ch == '-' || firstWord)) {
      Push(out, pos, end, ch == '-' ? TokenKind::attribute : TokenKind::keyword);
    }
    firstWord = false;
    wordStart = false;
    pos = end;
  }
}

constexpr std::array kCodeKeywords{
    std::string_view{"auto"},     std::string_view{"await"},   std::string_view{"break"},   std::string_view{"const"},
    std::string_view{"continue"}, std::string_view{"def"},     std::string_view{"defer"},   std::string_view{"else"},
    std::string_view{"err"},      std::string_view{"error"},   std::string_view{"false"},   std::string_view{"False"},
    std::string_view{"for"},      std::string_view{"from"},    std::string_view{"func"},    std::string_view{"if"},
    std::string_view{"import"},   std::string_view{"int"},     std::string_view{"let"},     std::string_view{"nil"},
    std::string_view{"None"},     std::string_view{"null"},    std::string_view{"package"}, std::string_view{"return"},
    std::string_view{"std"},      std::string_view{"true"},    std::string_view{"True"},    std::string_view{"var"},
    std::string_view{"while"},    std::string_view{"aeronet"},
};

// Code states: inside a C++ raw string literal or a Go raw (backquoted) string.
enum : SyntaxState { kCodeNormal = 0, kCodeRawString = 1, kCodeBackquote = 2 };

void TokenizeCode(std::string_view line, SyntaxState& state, LineTokens& out) {
  std::size_t pos = 0;
  while (pos < line.size()) {
    if (state != kCodeNormal) {
      const Scan scan = state == kCodeRawString ? ScanRawString(line, pos) : ScanTo(line, pos, "`");
      Push(out, pos, scan.end, TokenKind::string);
      pos = scan.end;
      if (scan.closed) {
        state = kCodeNormal;
      }
      continue;
    }
    const char ch = line[pos];
    std::size_t end = pos + 1;
    TokenKind kind = TokenKind::plain;  // not pushed
    if (ch == 'R' && end < line.size() && line[end] == '"') {
      state = kCodeRawString;
      end = pos + 2;
      kind = TokenKind::string;
    } else if (ch == '`') {
      state = kCodeBackquote;
      kind = TokenKind::string;
    } else if (IsOfClass(ch, kQuote)) {
      end = ScanQuoted(line, end, ch, true).end;
      kind = TokenKind::string;
    } else if ((ch == '/' && end < line.size() && line[end] == '/') ||
               (ch == '#' && !line.substr(pos).starts_with("#include"))) {
      Push(out, pos, line.size(), TokenKind::comment);
      return;
    } else if (ch == '#' || ch == '$') {
      end = SkipClass(line, end, kWord);
      kind = ch == '#' ? TokenKind::keyword : TokenKind::attribute;
    } else if (IsOfClass(ch, kDigit)) {
      end = SkipClass(line, end, kNumber);
      kind = TokenKind::number;
    } else if (IsOfClass(ch, kWordStart)) {
      end = SkipClass(line, end, kWord);
      if (std::ranges::contains(kCodeKeywords, line.substr(pos, end - pos))) {
        kind = TokenKind::keyword;
      } else if (end < line.size() && line[end] == '(') {
        kind = TokenKind::tag;  // function call
      }
    } else if (IsOfClass(ch, kCodePunctuation)) {
      kind = TokenKind::punctuation;
    }
    if (kind != TokenKind::plain) {
      Push(out, pos, end, kind);
    }
    pos = end;
  }
}

// HTTP states: request / status line, headers, body.
enum : SyntaxState { kHttpStart = 0, kHttpHeaders = 1, kHttpBody = 2 };

void TokenizeHttp(std::string_view line, SyntaxState& state, LineTokens& out) {
  if (line.ends_with('\r')) {
    line.remove_suffix(1);
  }
  if (state == kHttpStart) {
    // First word (method of a request, version of a response) and version of a request.
    const std::size_t spacePos = line.find(' ');
    Push(out, 0, std::min(spacePos, line.size()), TokenKind::keyword);
    const std::size_t versionPos = line.rfind(" HTTP/");
    if (versionPos != kNpos && versionPos != spacePos) {
      Push(out, versionPos + 1, line.size(), TokenKind::comment);
    }
    state = kHttpHeaders;
  } else if (state == kHttpHeaders) {
    const std::size_t colonPos = line.find(':');
    if (line.empty()) {
      state = kHttpBody;  // the body is not colored
    } else if (colonPos != kNpos) {
      Push(out, 0, colonPos, TokenKind::key);
      Push(out, colonPos, colonPos + 1, TokenKind::punctuation);
      Push(out, colonPos + 1, line.size(), TokenKind::string);
    }
  }
}

void TokenizeVariables(std::string_view line, SyntaxState& /*state*/, LineTokens& out) {
  for (std::size_t pos = line.find("{{"); pos != kNpos; pos = line.find("{{", pos)) {
    const std::size_t closePos = line.find("}}", pos + 2);
    if (closePos == kNpos) {
      return;
    }
    Push(out, pos, closePos + 2, TokenKind::variable);
    pos = closePos + 2;
  }
}

// Tokenizers, in the order of SyntaxLanguage.
constexpr std::array kTokenizers{TokenizeVariables, TokenizeJson, TokenizeXml,
                                 TokenizeShell,     TokenizeCode, TokenizeHttp};
static_assert(kTokenizers.size() == static_cast<std::size_t>(SyntaxLanguage::http) + 1);

}  // namespace

void TokenizeLine(SyntaxLanguage language, std::string_view line, SyntaxState& state, LineTokens& out) {
  assert(static_cast<std::size_t>(language) < kTokenizers.size());
  out.clear();
  kTokenizers[static_cast<std::size_t>(language)](line, state, out);
}

}  // namespace zalie
