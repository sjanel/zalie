#pragma once

#include <imgui.h>

#include <cstdint>
#include <string_view>

namespace zalie::ui {

// Small vector icons, drawn with ImDrawList: crisp at any DPI, no icon font to embed.
enum class Icon : std::uint8_t {
  plus,
  gear,
  search,
  close,
  sun,
  moon,
  sidebar,
  inspector,
  folder,
  copy,
  send,
  history,
  trash,
  edit,
  import,
  warning,
  check,
  stop,
  dots,
  chevronUp,
  chevronDown,
  eye,
  paw,
  columns,
  pawPrints,
};

// Draws 'icon' centered on 'center', fitting a square of 'size' pixels.
void DrawIcon(ImDrawList* pDrawList, Icon icon, ImVec2 center, float size, ImU32 color);

// Draws 'icon' ('iconSize' pixels) and 'text' side by side, centered together in the last item: the label of a button
// submitted without one.
void DrawItemIconLabel(Icon icon, float iconSize, std::string_view text, ImU32 color);

// A square, frameless button showing an icon, with an optional tooltip. 'active' draws it highlighted.
bool IconButton(const char* strId, Icon icon, const char* tooltip = nullptr, bool active = false);

}  // namespace zalie::ui
