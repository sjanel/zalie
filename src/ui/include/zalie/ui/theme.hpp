#pragma once

#include <imgui.h>

#include <array>
#include <cstdint>
#include <string_view>

#include "zalie/exchange.hpp"
#include "zalie/method.hpp"
#include "zalie/syntax.hpp"

namespace zalie::ui {

// The two themes: "Coton" (light, the cream white of Zalie's fur) and "Nuit" (dark, warm charcoal). Both use a coral
// accent, the color of the terracotta tiles she likes to sleep on.
enum class ThemeKind : std::uint8_t { coton, nuit };

[[nodiscard]] ThemeKind ParseTheme(std::string_view id) noexcept;
[[nodiscard]] std::string_view ThemeId(ThemeKind kind) noexcept;

// Colors not covered by ImGuiStyle, for custom drawing.
struct Palette {
  ImU32 background;
  ImU32 panel;
  ImU32 surface;
  ImU32 border;
  ImU32 text;
  ImU32 textDim;
  ImU32 accent;
  ImU32 accentHover;
  ImU32 accentActive;
  ImU32 accentSoft;
  ImU32 onAccent;
  ImU32 success;
  ImU32 info;
  ImU32 warning;
  ImU32 danger;
  ImU32 selection;
  ImU32 findMatch;
  ImU32 findCurrent;
  ImU32 variableBackground;
  ImU32 gutter;
  // Indexed by TokenKind.
  std::array<ImU32, 10> syntax;
  ThemeKind kind;
};

// The palette of the current theme.
[[nodiscard]] const Palette& CurrentPalette() noexcept;

// Applies a theme to the current ImGui style, with sizes scaled by 'scale' (DPI x user zoom).
void ApplyTheme(ThemeKind kind, float scale);

// Color of a method badge.
[[nodiscard]] ImU32 MethodColor(Method method) noexcept;

// Color of a status code (or of a failed / cancelled exchange).
[[nodiscard]] ImU32 StatusColor(std::int16_t status, ExchangeState state) noexcept;

// Color of a syntax token.
[[nodiscard]] inline ImU32 SyntaxColor(TokenKind kind) noexcept {
  return CurrentPalette().syntax[static_cast<std::size_t>(kind)];
}

// 'color' with its alpha multiplied by 'alpha'.
[[nodiscard]] ImU32 WithAlpha(ImU32 color, float alpha) noexcept;

}  // namespace zalie::ui
