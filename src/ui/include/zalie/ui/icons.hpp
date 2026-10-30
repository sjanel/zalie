#pragma once

#include <imgui.h>

#include <cstdint>

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
};

// Draws 'icon' centered on 'center', fitting a square of 'size' pixels.
void DrawIcon(ImDrawList* pDrawList, Icon icon, ImVec2 center, float size, ImU32 color);

// A square, frameless button showing an icon, with an optional tooltip. 'active' draws it highlighted.
bool IconButton(const char* strId, Icon icon, const char* tooltip = nullptr, bool active = false);

}  // namespace zalie::ui
