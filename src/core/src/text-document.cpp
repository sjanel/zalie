#include "zalie/text-document.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "zalie/string-utils.hpp"
#include "zalie/syntax.hpp"

namespace zalie {

namespace {

bool IsBinary(std::string_view body) noexcept {
  static constexpr std::size_t kSniffSize = 8192;
  const std::string_view head = body.substr(0, kSniffSize);
  return head.contains('\0');
}

bool IsImageMagic(std::string_view body) noexcept {
  return body.starts_with("\x89PNG") || body.starts_with("\xFF\xD8\xFF") || body.starts_with("GIF8") ||
         body.starts_with("BM");
}

// Steps back from 'pos' to the start of a UTF-8 sequence (never before 'min').
std::size_t Utf8Boundary(std::string_view text, std::size_t pos, std::size_t min) noexcept {
  while (pos > min && pos < text.size() && (static_cast<unsigned char>(text[pos]) & 0xC0U) == 0x80U) {
    --pos;
  }
  return pos;
}

}  // namespace

ContentKind DetectContentKind(std::string_view contentType, std::string_view body) noexcept {
  if (body.empty()) {
    return ContentKind::empty;
  }
  contentType = contentType.substr(0, contentType.find(';'));
  if (ContainsIgnoreCase(contentType, "json")) {
    return ContentKind::json;
  }
  if (ContainsIgnoreCase(contentType, "html")) {
    return ContentKind::html;
  }
  if (ContainsIgnoreCase(contentType, "xml")) {
    return ContentKind::xml;
  }
  if (ContainsIgnoreCase(contentType, "javascript")) {
    return ContentKind::javascript;
  }
  if (StartsWithIgnoreCase(contentType, "image/")) {
    return IsImageMagic(body) ? ContentKind::image : ContentKind::binary;
  }
  if (IsBinary(body)) {
    return IsImageMagic(body) ? ContentKind::image : ContentKind::binary;
  }
  // No (or a generic) Content-Type: sniff.
  const std::string_view trimmed = Trim(body.substr(0, 256));
  if (trimmed.starts_with('{') || trimmed.starts_with('[')) {
    return ContentKind::json;
  }
  if (StartsWithIgnoreCase(trimmed, "<!doctype html") || StartsWithIgnoreCase(trimmed, "<html")) {
    return ContentKind::html;
  }
  if (trimmed.starts_with("<?xml") || (trimmed.starts_with('<') && contentType.empty())) {
    return ContentKind::xml;
  }
  return ContentKind::text;
}

namespace {

// What Zalie knows of a kind of content, in the order of ContentKind.
struct ContentKindInfo {
  std::string_view name;
  SyntaxLanguage syntax;
};

constexpr std::array<ContentKindInfo, 8> kContentKinds{{
    {.name = "Empty", .syntax = SyntaxLanguage::plain},
    {.name = "JSON", .syntax = SyntaxLanguage::json},
    {.name = "XML", .syntax = SyntaxLanguage::xml},
    {.name = "HTML", .syntax = SyntaxLanguage::xml},
    {.name = "JavaScript", .syntax = SyntaxLanguage::code},
    {.name = "Text", .syntax = SyntaxLanguage::plain},
    {.name = "Image", .syntax = SyntaxLanguage::plain},
    {.name = "Binary", .syntax = SyntaxLanguage::plain},
}};

const ContentKindInfo& InfoOf(ContentKind kind) noexcept {
  assert(static_cast<std::size_t>(kind) < kContentKinds.size());
  return kContentKinds[static_cast<std::size_t>(kind)];
}

}  // namespace

SyntaxLanguage SyntaxFor(ContentKind kind) noexcept { return InfoOf(kind).syntax; }

std::string_view ContentKindName(ContentKind kind) noexcept { return InfoOf(kind).name; }

void LineIndex::clear() noexcept {
  _lines.clear();
  _states.clear();
  _longest = 0;
}

void LineIndex::build(std::string_view text, SyntaxLanguage language, std::uint32_t maxLineBytes) {
  clear();
  if (text.empty()) {
    return;
  }
  // One counting pass (memchr-fast) to size the index exactly: no reallocation while building.
  const auto nbNewLines = static_cast<std::size_t>(std::ranges::count(text, '\n'));
  _lines.reserve(static_cast<std::uint32_t>(nbNewLines + 1 + (text.size() / (std::size_t{16} * maxLineBytes))));
  const bool stateful = language != SyntaxLanguage::plain && language != SyntaxLanguage::json;

  std::size_t pos = 0;
  while (pos < text.size()) {
    std::size_t eolPos = text.find('\n', pos);
    const bool hasEol = eolPos != std::string_view::npos;
    if (!hasEol) {
      eolPos = text.size();
    }
    std::size_t lineEnd = eolPos;
    if (lineEnd > pos && text[lineEnd - 1] == '\r') {
      --lineEnd;
    }
    // Long physical lines become several visual lines.
    std::size_t begin = pos;
    do {
      std::size_t end = lineEnd;
      if (end - begin > maxLineBytes) {
        end = Utf8Boundary(text, begin + maxLineBytes, begin + 1);
      }
      _lines.push_back({static_cast<std::uint32_t>(begin), static_cast<std::uint32_t>(end - begin)});
      _longest = std::max(_longest, static_cast<std::uint32_t>(end - begin));
      begin = end;
    } while (begin < lineEnd);
    pos = eolPos + 1;
    if (hasEol && pos == text.size()) {
      _lines.push_back({static_cast<std::uint32_t>(pos), 0});  // a final newline ends with an empty line
    }
  }

  if (stateful) {
    _states.reserve(_lines.size());
    SyntaxState state = 0;
    LineTokens tokens;
    for (const Line& line : _lines) {
      _states.push_back(state);
      TokenizeLine(language, text.substr(line.begin, line.length), state, tokens);
    }
  }
}

std::size_t LineIndex::lineOf(std::size_t offset) const noexcept {
  if (_lines.empty()) {
    return 0;
  }
  const auto it =
      std::ranges::upper_bound(_lines, offset, {}, [](const Line& line) { return std::size_t{line.begin}; });
  return it == _lines.begin() ? 0 : static_cast<std::size_t>(it - _lines.begin()) - 1;
}

}  // namespace zalie
