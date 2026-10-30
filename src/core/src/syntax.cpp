#include "zalie/syntax.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace zalie {

namespace {

constexpr bool IsDigit(char ch) noexcept { return ch >= '0' && ch <= '9'; }
constexpr bool IsAlpha(char ch) noexcept { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_'; }
constexpr bool IsAlnum(char ch) noexcept { return IsAlpha(ch) || IsDigit(ch); }
constexpr bool IsBlank(char ch) noexcept { return ch == ' ' || ch == '\t' || ch == '\r'; }

void Push(LineTokens& out, std::size_t begin, std::size_t end, TokenKind kind) {
  if (end > begin) {
    out.push_back({static_cast<std::uint32_t>(begin), static_cast<std::uint32_t>(end - begin), kind});
  }
}

// End of a string starting at 'pos' (on the opening quote): position after the closing quote, or the line size if
// the string does not end on this line. Backslash escapes are honored if 'escapes'.
std::size_t StringEnd(std::string_view line, std::size_t pos, char quote, bool escapes, bool& closed) noexcept {
  for (++pos; pos < line.size(); ++pos) {
    if (escapes && line[pos] == '\\') {
      ++pos;
    } else if (line[pos] == quote) {
      closed = true;
      return pos + 1;
    }
  }
  closed = false;
  return line.size();
}

std::size_t NumberEnd(std::string_view line, std::size_t pos) noexcept {
  while (pos < line.size() && (IsAlnum(line[pos]) || line[pos] == '.' || line[pos] == '-' || line[pos] == '+')) {
    ++pos;
  }
  return pos;
}

void TokenizeJson(std::string_view line, LineTokens& out) {
  std::size_t pos = 0;
  while (pos < line.size()) {
    const char ch = line[pos];
    if (IsBlank(ch)) {
      ++pos;
    } else if (ch == '"') {
      bool closed = false;
      const std::size_t end = StringEnd(line, pos, '"', true, closed);
      std::size_t next = end;
      while (next < line.size() && IsBlank(line[next])) {
        ++next;
      }
      Push(out, pos, end, next < line.size() && line[next] == ':' ? TokenKind::key : TokenKind::string);
      pos = end;
    } else if (IsDigit(ch) || ch == '-') {
      const std::size_t end = NumberEnd(line, pos + 1);
      Push(out, pos, end, TokenKind::number);
      pos = end;
    } else if (IsAlpha(ch)) {
      std::size_t end = pos + 1;
      while (end < line.size() && IsAlnum(line[end])) {
        ++end;
      }
      Push(out, pos, end, TokenKind::keyword);
      pos = end;
    } else {
      Push(out, pos, pos + 1, TokenKind::punctuation);
      ++pos;
    }
  }
}

// XML / HTML states.
enum : SyntaxState { kXmlText = 0, kXmlTag = 1, kXmlComment = 2, kXmlDoubleQuoted = 3, kXmlSingleQuoted = 4 };

void TokenizeXml(std::string_view line, SyntaxState& state, LineTokens& out) {
  std::size_t pos = 0;
  while (pos < line.size()) {
    switch (state) {
      case kXmlComment: {
        const std::size_t endPos = line.find("-->", pos);
        const std::size_t end = endPos == std::string_view::npos ? line.size() : endPos + 3;
        Push(out, pos, end, TokenKind::comment);
        pos = end;
        if (endPos != std::string_view::npos) {
          state = kXmlText;
        }
        break;
      }
      case kXmlDoubleQuoted:
      case kXmlSingleQuoted: {
        const char quote = state == kXmlDoubleQuoted ? '"' : '\'';
        const std::size_t quotePos = line.find(quote, pos);
        const std::size_t end = quotePos == std::string_view::npos ? line.size() : quotePos + 1;
        Push(out, pos, end, TokenKind::string);
        pos = end;
        if (quotePos != std::string_view::npos) {
          state = kXmlTag;
        }
        break;
      }
      case kXmlTag: {
        const char ch = line[pos];
        if (IsBlank(ch)) {
          ++pos;
        } else if (ch == '>' || ((ch == '/' || ch == '?') && pos + 1 < line.size() && line[pos + 1] == '>')) {
          const std::size_t end = ch == '>' ? pos + 1 : pos + 2;
          Push(out, pos, end, TokenKind::tag);
          pos = end;
          state = kXmlText;
        } else if (ch == '"' || ch == '\'') {
          // Attribute value, possibly continued on the next lines.
          const std::size_t quotePos = line.find(ch, pos + 1);
          const std::size_t end = quotePos == std::string_view::npos ? line.size() : quotePos + 1;
          Push(out, pos, end, TokenKind::string);
          pos = end;
          if (quotePos == std::string_view::npos) {
            state = ch == '"' ? kXmlDoubleQuoted : kXmlSingleQuoted;
          }
        } else if (ch == '=') {
          Push(out, pos, pos + 1, TokenKind::punctuation);
          ++pos;
        } else {
          std::size_t end = pos + 1;
          while (end < line.size() && !IsBlank(line[end]) && line[end] != '=' && line[end] != '>' && line[end] != '/' &&
                 line[end] != '"' && line[end] != '\'') {
            ++end;
          }
          Push(out, pos, end, TokenKind::attribute);
          pos = end;
        }
        break;
      }
      default: {  // text
        const std::size_t ltPos = line.find('<', pos);
        if (ltPos == std::string_view::npos) {
          return;
        }
        pos = ltPos;
        if (line.substr(pos).starts_with("<!--")) {
          state = kXmlComment;
          continue;
        }
        std::size_t end = pos + 1;
        while (end < line.size() && (line[end] == '/' || line[end] == '!' || line[end] == '?')) {
          ++end;
        }
        while (end < line.size() && !IsBlank(line[end]) && line[end] != '>' && line[end] != '/') {
          ++end;
        }
        Push(out, pos, end, TokenKind::tag);
        pos = end;
        state = kXmlTag;
        break;
      }
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
    if (state == kShellSingle || state == kShellDouble) {
      // Continuation of a quoted argument opened on a previous line.
      const char quote = state == kShellSingle ? '\'' : '"';
      const std::size_t begin = pos;
      bool closed = false;
      for (; pos < line.size(); ++pos) {
        if (quote == '"' && line[pos] == '\\') {
          ++pos;
        } else if (line[pos] == quote) {
          closed = true;
          ++pos;
          break;
        }
      }
      pos = std::min(pos, line.size());
      Push(out, begin, pos, TokenKind::string);
      if (closed) {
        state = kShellNormal;
      }
      wordStart = false;
      continue;
    }
    const char ch = line[pos];
    if (IsBlank(ch)) {
      ++pos;
      wordStart = true;
      continue;
    }
    if (ch == '\'' || ch == '"' || (ch == '$' && pos + 1 < line.size() && line[pos + 1] == '\'')) {
      const std::size_t begin = pos;
      if (ch == '$') {
        ++pos;
      }
      const char quote = line[pos];
      bool closed = false;
      const std::size_t end = StringEnd(line, pos, quote, quote == '"' || ch == '$', closed);
      Push(out, begin, end, TokenKind::string);
      pos = end;
      if (!closed) {
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
    std::size_t end = pos;
    while (end < line.size() && !IsBlank(line[end]) && line[end] != '\'' && line[end] != '"') {
      ++end;
    }
    if (wordStart && ch == '-') {
      Push(out, pos, end, TokenKind::attribute);
    } else if (wordStart && firstWord) {
      Push(out, pos, end, TokenKind::keyword);
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
    if (state == kCodeRawString) {
      // Ends at ')' + delimiter + '"'.
      std::size_t end = line.size();
      for (std::size_t scan = pos; scan < line.size(); ++scan) {
        if (line[scan] != ')') {
          continue;
        }
        std::size_t delimEnd = scan + 1;
        while (delimEnd < line.size() && IsAlnum(line[delimEnd])) {
          ++delimEnd;
        }
        if (delimEnd < line.size() && line[delimEnd] == '"') {
          end = delimEnd + 1;
          state = kCodeNormal;
          break;
        }
      }
      Push(out, pos, end, TokenKind::string);
      pos = end;
      continue;
    }
    if (state == kCodeBackquote) {
      const std::size_t quotePos = line.find('`', pos);
      const std::size_t end = quotePos == std::string_view::npos ? line.size() : quotePos + 1;
      Push(out, pos, end, TokenKind::string);
      pos = end;
      if (quotePos != std::string_view::npos) {
        state = kCodeNormal;
      }
      continue;
    }
    const char ch = line[pos];
    if (IsBlank(ch)) {
      ++pos;
    } else if (ch == 'R' && pos + 1 < line.size() && line[pos + 1] == '"') {
      state = kCodeRawString;
      Push(out, pos, pos + 2, TokenKind::string);
      pos += 2;
    } else if (ch == '`') {
      state = kCodeBackquote;
      Push(out, pos, pos + 1, TokenKind::string);
      ++pos;
    } else if (ch == '"' || ch == '\'') {
      bool closed = false;
      const std::size_t end = StringEnd(line, pos, ch, true, closed);
      Push(out, pos, end, TokenKind::string);
      pos = end;
    } else if ((ch == '/' && pos + 1 < line.size() && line[pos + 1] == '/') ||
               (ch == '#' && !line.substr(pos).starts_with("#include"))) {
      Push(out, pos, line.size(), TokenKind::comment);
      return;
    } else if (ch == '#' || ch == '$') {
      std::size_t end = pos + 1;
      while (end < line.size() && IsAlnum(line[end])) {
        ++end;
      }
      Push(out, pos, end, ch == '#' ? TokenKind::keyword : TokenKind::attribute);
      pos = end;
    } else if (IsDigit(ch)) {
      const std::size_t end = NumberEnd(line, pos + 1);
      Push(out, pos, end, TokenKind::number);
      pos = end;
    } else if (IsAlpha(ch)) {
      std::size_t end = pos + 1;
      while (end < line.size() && IsAlnum(line[end])) {
        ++end;
      }
      const std::string_view word = line.substr(pos, end - pos);
      if (std::ranges::contains(kCodeKeywords, word)) {
        Push(out, pos, end, TokenKind::keyword);
      } else if (end < line.size() && line[end] == '(') {
        Push(out, pos, end, TokenKind::tag);  // function call
      }
      pos = end;
    } else {
      if (std::string_view{"{}[]();,.:=<>"}.contains(ch)) {
        Push(out, pos, pos + 1, TokenKind::punctuation);
      }
      ++pos;
    }
  }
}

// HTTP states: request / status line, headers, body.
enum : SyntaxState { kHttpStart = 0, kHttpHeaders = 1, kHttpBody = 2 };

void TokenizeHttp(std::string_view line, SyntaxState& state, LineTokens& out) {
  if (line.ends_with('\r')) {
    line.remove_suffix(1);
  }
  switch (state) {
    case kHttpStart: {
      const std::size_t spacePos = line.find(' ');
      Push(out, 0, spacePos == std::string_view::npos ? line.size() : spacePos, TokenKind::keyword);
      const std::size_t versionPos = line.rfind(" HTTP/");
      if (versionPos != std::string_view::npos && versionPos != spacePos) {
        Push(out, versionPos + 1, line.size(), TokenKind::comment);
      }
      state = kHttpHeaders;
      break;
    }
    case kHttpHeaders: {
      if (line.empty()) {
        state = kHttpBody;
        break;
      }
      const std::size_t colonPos = line.find(':');
      if (colonPos != std::string_view::npos) {
        Push(out, 0, colonPos, TokenKind::key);
        Push(out, colonPos, colonPos + 1, TokenKind::punctuation);
        Push(out, colonPos + 1, line.size(), TokenKind::string);
      }
      break;
    }
    default:
      break;
  }
}

void TokenizeVariables(std::string_view line, LineTokens& out) {
  std::size_t pos = 0;
  while (true) {
    const std::size_t openPos = line.find("{{", pos);
    if (openPos == std::string_view::npos) {
      return;
    }
    const std::size_t closePos = line.find("}}", openPos + 2);
    if (closePos == std::string_view::npos) {
      return;
    }
    Push(out, openPos, closePos + 2, TokenKind::variable);
    pos = closePos + 2;
  }
}

}  // namespace

void TokenizeLine(SyntaxLanguage language, std::string_view line, SyntaxState& state, LineTokens& out) {
  out.clear();
  switch (language) {
    case SyntaxLanguage::json:
      TokenizeJson(line, out);
      break;
    case SyntaxLanguage::xml:
      TokenizeXml(line, state, out);
      break;
    case SyntaxLanguage::shell:
      TokenizeShell(line, state, out);
      break;
    case SyntaxLanguage::code:
      TokenizeCode(line, state, out);
      break;
    case SyntaxLanguage::http:
      TokenizeHttp(line, state, out);
      break;
    case SyntaxLanguage::plain:
      TokenizeVariables(line, out);
      break;
  }
}

}  // namespace zalie
