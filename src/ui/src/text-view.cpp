#include "zalie/ui/text-view.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cfloat>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

#include "zalie/raw-chars.hpp"
#include "zalie/string-utils.hpp"
#include "zalie/syntax.hpp"
#include "zalie/text-document.hpp"
#include "zalie/ui/theme.hpp"
#include "zalie/ui/widgets.hpp"

namespace zalie::ui {

namespace {

constexpr std::size_t kMaxMatches = 100000;

std::size_t NbDigits(std::size_t value) noexcept {
  std::size_t digits = 1;
  while (value >= 10) {
    value /= 10;
    ++digits;
  }
  return digits;
}

float TextWidth(ImFont* pFont, float fontSize, std::string_view text) {
  return pFont->CalcTextSizeA(fontSize, FLT_MAX, 0.0F, text.data(), text.data() + text.size()).x;
}

// Copies the selected lines to the clipboard (a selection needs a line).
void CopyLines(std::string_view text, const LineIndex& lines, std::size_t first, std::size_t last) {
  assert(!lines.empty());
  last = std::min(last, lines.size() - 1);
  const std::size_t begin = lines.lineBegin(first);
  const std::string_view lastLine = lines.line(text, last);
  const std::size_t end = static_cast<std::size_t>(lastLine.data() - text.data()) + lastLine.size();
  CopyToClipboard(text.substr(begin, end - begin));
}

}  // namespace

void TextViewState::reset() noexcept {
  matches.clear();
  matchedQuery.clear();
  pMatchedText = nullptr;
  matchedTextSize = 0;
  currentMatch = 0;
  selectionAnchor = kNoLine;
  selectionCursor = kNoLine;
  dragging = false;
}

std::size_t UpdateMatches(std::string_view text, TextViewState& state) {
  if (View(state.query) == View(state.matchedQuery) && state.pMatchedText == text.data() &&
      state.matchedTextSize == text.size()) {
    return state.matches.size();
  }
  state.matchedQuery.assign(state.query);
  state.pMatchedText = text.data();
  state.matchedTextSize = text.size();
  state.matches.clear();
  state.currentMatch = 0;
  const std::string_view query = state.query;
  if (query.empty()) {
    return 0;
  }
  for (std::size_t pos = FindIgnoreCase(text, query);
       pos != std::string_view::npos && state.matches.size() < kMaxMatches;
       pos = FindIgnoreCase(text, query, pos + query.size())) {
    state.matches.push_back(static_cast<std::uint32_t>(pos));
  }
  state.scrollToCurrentMatch = !state.matches.empty();
  return state.matches.size();
}

void GoToMatch(TextViewState& state, bool forward) noexcept {
  if (state.matches.empty()) {
    return;
  }
  const std::size_t count = state.matches.size();
  state.currentMatch = forward ? (state.currentMatch + 1) % count : (state.currentMatch + count - 1) % count;
  state.scrollToCurrentMatch = true;
}

void TextView(const char* strId, std::string_view text, const LineIndex& lines, TextViewState& state,
              const TextViewOptions& options) {
  const Palette& pal = CurrentPalette();
  ScopedFont mono(GetFonts().pMono, Fonts::kMonoSize);
  ImFont* pFont = ImGui::GetFont();
  const float fontSize = ImGui::GetFontSize();
  const float lineHeight = std::floor(fontSize * 1.42F);
  const float charWidth = TextWidth(pFont, fontSize, "M");
  const float padding = std::floor(charWidth * 0.8F);
  const float gutterWidth =
      options.lineNumbers
          ? (static_cast<float>(NbDigits(std::max<std::size_t>(lines.size(), 1))) * charWidth) + (padding * 2.0F)
          : padding;
  const float contentWidth = gutterWidth + (static_cast<float>(lines.longestLine()) * charWidth) + (padding * 2.0F);

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, pal.surface);
  const bool visible =
      ImGui::BeginChild(strId, options.size, options.border ? ImGuiChildFlags_Borders : ImGuiChildFlags_None,
                        ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoMove);
  ImGui::PopStyleColor();
  ImGui::PopStyleVar();
  if (!visible) {
    ImGui::EndChild();
    return;
  }

  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImDrawList* pDrawList = ImGui::GetWindowDrawList();
  const ImVec2 windowPos = ImGui::GetWindowPos();
  const ImVec2 windowSize = ImGui::GetWindowSize();
  const std::size_t nbLines = lines.size();

  // Mouse: line selection.
  const ImGuiIO& io = ImGui::GetIO();
  const auto lineAt = [&](float mouseY) {
    const float relative = (mouseY - origin.y) / lineHeight;
    if (relative < 0 || nbLines == 0) {
      return std::size_t{0};
    }
    return std::min(static_cast<std::size_t>(relative), nbLines - 1);
  };
  if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && nbLines != 0) {
    const std::size_t line = lineAt(io.MousePos.y);
    if (io.KeyShift && state.selectionAnchor != TextViewState::kNoLine) {
      state.selectionCursor = line;
    } else {
      state.selectionAnchor = state.selectionCursor = line;
    }
    state.dragging = true;
  }
  if (state.dragging) {
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      state.selectionCursor = lineAt(io.MousePos.y);
      // Auto-scroll while dragging out of the view.
      if (io.MousePos.y < windowPos.y) {
        ImGui::SetScrollY(ImGui::GetScrollY() - lineHeight);
      } else if (io.MousePos.y > windowPos.y + windowSize.y) {
        ImGui::SetScrollY(ImGui::GetScrollY() + lineHeight);
      }
    } else {
      state.dragging = false;
    }
  }
  const bool hasSelection = state.selectionAnchor != TextViewState::kNoLine && nbLines != 0;
  const std::size_t selFirst = hasSelection ? std::min(state.selectionAnchor, state.selectionCursor) : 0;
  const std::size_t selLast = hasSelection ? std::max(state.selectionAnchor, state.selectionCursor) : 0;

  // Keyboard: copy / select all, when the view is focused.
  if (ImGui::IsWindowFocused()) {
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_C) && hasSelection) {
      CopyLines(text, lines, selFirst, selLast);
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_A) && nbLines != 0) {
      state.selectionAnchor = 0;
      state.selectionCursor = nbLines - 1;
    }
  }
  if (ImGui::BeginPopupContextWindow("##context")) {
    if (ImGui::MenuItem("Copy selected lines", "Ctrl+C", false, hasSelection)) {
      CopyLines(text, lines, selFirst, selLast);
    }
    if (ImGui::MenuItem("Copy all")) {
      CopyToClipboard(text);
    }
    if (ImGui::MenuItem("Select all", "Ctrl+A", false, nbLines != 0)) {
      state.selectionAnchor = 0;
      state.selectionCursor = nbLines - 1;
    }
    ImGui::EndPopup();
  }

  // Scroll to the current match.
  if (state.scrollToCurrentMatch && state.currentMatch < state.matches.size()) {
    const std::uint32_t match = state.matches[static_cast<std::uint32_t>(state.currentMatch)];
    const std::size_t line = lines.lineOf(match);
    ImGui::SetScrollY(std::max(0.0F, (static_cast<float>(line) * lineHeight) - (windowSize.y * 0.4F)));
    const std::size_t column = match - lines.lineBegin(line);
    const float matchX = gutterWidth + (static_cast<float>(column) * charWidth);
    if (matchX < ImGui::GetScrollX() || matchX > ImGui::GetScrollX() + windowSize.x - (gutterWidth * 2.0F)) {
      ImGui::SetScrollX(std::max(0.0F, matchX - (windowSize.x * 0.3F)));
    }
    state.scrollToCurrentMatch = false;
  }

  // Visible lines only.
  const std::size_t queryLength = state.matchedQuery.size();
  LineTokens tokens;
  std::array<char, 24> numberBuffer{};
  const float scrollX = ImGui::GetScrollX();
  ImGuiListClipper clipper;
  clipper.Begin(static_cast<int>(nbLines), lineHeight);
  while (clipper.Step()) {
    for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
      const auto idx = static_cast<std::size_t>(row);
      const std::string_view line = lines.line(text, idx);
      const float y = origin.y + (static_cast<float>(idx) * lineHeight);
      const float textY = y + std::floor((lineHeight - fontSize) * 0.5F);
      const float textX = origin.x + gutterWidth;

      if (hasSelection && idx >= selFirst && idx <= selLast) {
        pDrawList->AddRectFilled(ImVec2(windowPos.x, y),
                                 ImVec2(windowPos.x + std::max(windowSize.x, contentWidth), y + lineHeight),
                                 pal.selection);
      }
      // Find highlights.
      if (queryLength != 0) {
        const std::size_t lineBegin = lines.lineBegin(idx);
        const std::size_t lineEnd = lineBegin + line.size();
        auto it = std::ranges::lower_bound(state.matches, static_cast<std::uint32_t>(lineBegin));
        for (; it != state.matches.end() && *it < lineEnd; ++it) {
          const std::size_t column = *it - lineBegin;
          const float x0 = textX + TextWidth(pFont, fontSize, line.substr(0, column));
          const float x1 = x0 + TextWidth(pFont, fontSize, line.substr(column, queryLength));
          const bool current = std::cmp_equal(it - state.matches.begin(), state.currentMatch);
          pDrawList->AddRectFilled(ImVec2(x0, y + 1), ImVec2(x1, y + lineHeight - 1),
                                   current ? pal.findCurrent : pal.findMatch, 3.0F);
        }
      }
      // Tokens, then the plain gaps between them.
      SyntaxState syntaxState = lines.stateAt(idx);
      TokenizeLine(options.language, line, syntaxState, tokens);
      float x = textX;
      std::size_t pos = 0;
      const float clipRight = windowPos.x + windowSize.x;
      const auto drawSegment = [&](std::size_t begin, std::size_t end, ImU32 color) {
        if (end <= begin || x > clipRight) {
          return;
        }
        const std::string_view segment = line.substr(begin, end - begin);
        const float width = TextWidth(pFont, fontSize, segment);
        if (x + width >= windowPos.x) {
          pDrawList->AddText(pFont, fontSize, ImVec2(x, textY), color, segment.data(), segment.data() + segment.size());
        }
        x += width;
      };
      for (const Token& token : tokens) {
        drawSegment(pos, token.begin, pal.syntax[0]);
        if (token.kind == TokenKind::variable) {
          const float width = TextWidth(pFont, fontSize, line.substr(token.begin, token.length));
          pDrawList->AddRectFilled(ImVec2(x - 1, y + 2), ImVec2(x + width + 1, y + lineHeight - 2),
                                   pal.variableBackground, 3.0F);
        }
        drawSegment(token.begin, token.begin + token.length, SyntaxColor(token.kind));
        pos = token.begin + token.length;
      }
      drawSegment(pos, line.size(), pal.syntax[0]);

      // Line numbers, in a gutter that stays visible when scrolled horizontally.
      if (options.lineNumbers) {
        const float gutterX = origin.x + scrollX;
        pDrawList->AddRectFilled(ImVec2(gutterX, y), ImVec2(gutterX + gutterWidth - (padding * 0.5F), y + lineHeight),
                                 pal.surface);
        const auto [pEnd, errc] =
            std::to_chars(numberBuffer.data(), numberBuffer.data() + numberBuffer.size(), idx + 1);
        const std::string_view number(numberBuffer.data(), static_cast<std::size_t>(pEnd - numberBuffer.data()));
        const float numberX = gutterX + gutterWidth - padding - TextWidth(pFont, fontSize, number);
        pDrawList->AddText(pFont, fontSize, ImVec2(numberX, textY), pal.gutter, number.data(),
                           number.data() + number.size());
      }
      ImGui::Dummy(ImVec2(contentWidth, lineHeight));
    }
  }
  clipper.End();
  if (nbLines == 0) {
    ImGui::Dummy(ImVec2(1, lineHeight));
  }
  ImGui::EndChild();
}

void HexView(const char* strId, std::string_view bytes, ImVec2 size) {
  static constexpr std::size_t kBytesPerRow = 16;
  static constexpr char kHex[] = "0123456789abcdef";
  const Palette& pal = CurrentPalette();
  ScopedFont mono(GetFonts().pMono, Fonts::kMonoSize);
  ImGui::PushStyleColor(ImGuiCol_ChildBg, pal.surface);
  const bool visible = ImGui::BeginChild(strId, size, ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
  ImGui::PopStyleColor();
  if (visible) {
    const std::size_t nbRows = (bytes.size() + kBytesPerRow - 1) / kBytesPerRow;
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(nbRows));
    // "00000000  00 01 02 03 04 05 06 07  08 09 0a 0b 0c 0d 0e 0f  |................|"
    std::array<char, 96> row{};
    while (clipper.Step()) {
      for (int rowIdx = clipper.DisplayStart; rowIdx < clipper.DisplayEnd; ++rowIdx) {
        const std::size_t offset = static_cast<std::size_t>(rowIdx) * kBytesPerRow;
        char* pOut = row.data();
        for (int shift = 28; shift >= 0; shift -= 4) {
          *pOut++ = kHex[(offset >> static_cast<unsigned>(shift)) & 0xFU];
        }
        *pOut++ = ' ';
        const std::size_t count = std::min(kBytesPerRow, bytes.size() - offset);
        for (std::size_t i = 0; i < kBytesPerRow; ++i) {
          *pOut++ = ' ';
          if (i == 8) {
            *pOut++ = ' ';  // gap between the two halves of the row
          }
          if (i < count) {
            const auto byte = static_cast<unsigned char>(bytes[offset + i]);
            *pOut++ = kHex[byte >> 4U];
            *pOut++ = kHex[byte & 0xFU];
          } else {
            *pOut++ = ' ';
            *pOut++ = ' ';
          }
        }
        *pOut++ = ' ';
        *pOut++ = ' ';
        *pOut++ = '|';
        for (std::size_t i = 0; i < count; ++i) {
          const char ch = bytes[offset + i];
          *pOut++ = ch >= 0x20 && ch < 0x7F ? ch : '.';
        }
        *pOut++ = '|';
        const std::string_view line(row.data(), static_cast<std::size_t>(pOut - row.data()));
        TextColored(pal.gutter, line.substr(0, 8));
        ImGui::SameLine(0, 0);
        Text(line.substr(8));
      }
    }
  }
  ImGui::EndChild();
}

}  // namespace zalie::ui
