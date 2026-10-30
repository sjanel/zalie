#pragma once

#include <imgui.h>

#include <amc/vector.hpp>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "zalie/raw-chars.hpp"
#include "zalie/syntax.hpp"
#include "zalie/text-document.hpp"

namespace zalie::ui {

// Interaction state of a TextView (selection, find), kept by its owner across frames.
struct TextViewState {
  static constexpr std::size_t kNoLine = static_cast<std::size_t>(-1);

  // Forgets the selection and the find results (to call when the text changes).
  void reset() noexcept;

  // Searched text (edited by the owner, e.g. in a find bar). Case-insensitive.
  RawChars32 query;
  // Byte offsets of the matches of 'query' in the text, sorted.
  amc::vector<std::uint32_t> matches;
  // The query and the text the matches were computed for.
  RawChars32 matchedQuery;
  const char* pMatchedText{nullptr};
  std::size_t matchedTextSize{0};
  std::size_t currentMatch{0};
  // Line selection: anchor and cursor lines, kNoLine when nothing is selected.
  std::size_t selectionAnchor{kNoLine};
  std::size_t selectionCursor{kNoLine};
  bool scrollToCurrentMatch{false};
  bool dragging{false};
};

struct TextViewOptions {
  SyntaxLanguage language{SyntaxLanguage::plain};
  ImVec2 size{0, 0};
  bool lineNumbers{true};
  bool border{false};
};

// Read-only text viewer for texts of any size: only the visible lines are laid out and drawn (ImGuiListClipper over a
// LineIndex), straight from the text buffer - no copy, no per-frame allocation. Syntax colored, with find highlights,
// line selection (click, shift-click, drag) and copy (Ctrl+C, context menu).
void TextView(const char* strId, std::string_view text, const LineIndex& lines, TextViewState& state,
              const TextViewOptions& options);

// Recomputes the matches of state.query in 'text' if the query or the text changed. Returns the number of matches.
std::size_t UpdateMatches(std::string_view text, TextViewState& state);

// Moves to the next (or previous) match and scrolls to it.
void GoToMatch(TextViewState& state, bool forward) noexcept;

// Hex dump viewer for binary bodies, virtualized like TextView.
void HexView(const char* strId, std::string_view bytes, ImVec2 size);

}  // namespace zalie::ui
