#pragma once

#include <amc/vector.hpp>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "zalie/syntax.hpp"

namespace zalie {

// Nature of a response body, deciding how it is displayed.
enum class ContentKind : std::uint8_t { empty, json, xml, html, javascript, text, image, binary };

// Detects the kind of a body from its Content-Type, falling back to sniffing its first bytes.
[[nodiscard]] ContentKind DetectContentKind(std::string_view contentType, std::string_view body) noexcept;

// Language used to color a body of this kind.
[[nodiscard]] SyntaxLanguage SyntaxFor(ContentKind kind) noexcept;

// Short display name of a kind ("JSON", "HTML"...).
[[nodiscard]] std::string_view ContentKindName(ContentKind kind) noexcept;

// Index of the lines of a text, for virtualized rendering: O(1) access to any line, whatever the size of the text.
// It stores offsets only (the text is not copied). Lines longer than 'maxLineBytes' are split in several visual lines
// (on a UTF-8 character boundary), so that a 50 MB minified document stays smooth to scroll.
class LineIndex {
 public:
  static constexpr std::uint32_t kDefaultMaxLineBytes = 2048;

  // Indexes 'text'. For stateful languages (XML, shell, code) the syntax state at the start of each line is recorded.
  void build(std::string_view text, SyntaxLanguage language = SyntaxLanguage::plain,
             std::uint32_t maxLineBytes = kDefaultMaxLineBytes);

  void clear() noexcept;

  // Number of visual lines (0 for an empty text).
  [[nodiscard]] std::size_t size() const noexcept { return _lines.size(); }
  [[nodiscard]] bool empty() const noexcept { return _lines.empty(); }

  // The line 'idx' of 'text' (the text given to build()), without its end of line.
  [[nodiscard]] std::string_view line(std::string_view text, std::size_t idx) const noexcept {
    const Line& line = _lines[static_cast<std::uint32_t>(idx)];
    return text.substr(line.begin, line.length);
  }

  // Byte offset of the start of line 'idx'.
  [[nodiscard]] std::size_t lineBegin(std::size_t idx) const noexcept {
    return _lines[static_cast<std::uint32_t>(idx)].begin;
  }

  // Syntax state at the start of line 'idx'.
  [[nodiscard]] SyntaxState stateAt(std::size_t idx) const noexcept {
    return _states.empty() ? 0 : _states[static_cast<std::uint32_t>(idx)];
  }

  // Index of the line containing the byte at 'offset' (the last line if beyond).
  [[nodiscard]] std::size_t lineOf(std::size_t offset) const noexcept;

  // Length in bytes of the longest visual line.
  [[nodiscard]] std::uint32_t longestLine() const noexcept { return _longest; }

 private:
  struct Line {
    std::uint32_t begin;
    std::uint32_t length;
  };

  amc::vector<Line> _lines;
  amc::vector<SyntaxState> _states;
  std::uint32_t _longest{0};
};

}  // namespace zalie
