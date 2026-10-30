#include "zalie/ui/theme.hpp"

#include <imgui.h>

#include <cstdint>
#include <string_view>

#include "zalie/exchange.hpp"
#include "zalie/method.hpp"

namespace zalie::ui {

namespace {

constexpr ImU32 Rgb(std::uint32_t rgb, std::uint32_t alpha = 0xFF) noexcept {
  return IM_COL32((rgb >> 16U) & 0xFFU, (rgb >> 8U) & 0xFFU, rgb & 0xFFU, alpha);
}

ImVec4 Vec(ImU32 color) { return ImGui::ColorConvertU32ToFloat4(color); }

constexpr Palette kCoton{
    .background = Rgb(0xFBF7F1),
    .panel = Rgb(0xF3EDE4),
    .surface = Rgb(0xFFFFFF),
    .border = Rgb(0xE4DACD),
    .text = Rgb(0x3B302B),
    .textDim = Rgb(0x988A80),
    .accent = Rgb(0xE0714F),
    .accentHover = Rgb(0xE8845F),
    .accentActive = Rgb(0xC95E3E),
    .accentSoft = Rgb(0xF8DED2),
    .onAccent = Rgb(0xFFFFFF),
    .success = Rgb(0x2F9E6A),
    .info = Rgb(0x3C7FCC),
    .warning = Rgb(0xD48A1E),
    .danger = Rgb(0xD24B4B),
    .selection = Rgb(0xE0714F, 0x38),
    .findMatch = Rgb(0xF2C14E, 0x70),
    .findCurrent = Rgb(0xF29A2E, 0xC0),
    .variableBackground = Rgb(0xE0714F, 0x22),
    .gutter = Rgb(0xC2B5AA),
    .syntax = {Rgb(0x3B302B), Rgb(0xA3485B), Rgb(0x2E7D5B), Rgb(0xC05A2B), Rgb(0x7A4FB5), Rgb(0x9C8F86), Rgb(0x2F6FB5),
               Rgb(0xA86A1E), Rgb(0xA59a8f), Rgb(0xD0602F)},
    .kind = ThemeKind::coton,
};

constexpr Palette kNuit{
    .background = Rgb(0x1C1917),
    .panel = Rgb(0x23201D),
    .surface = Rgb(0x2C2724),
    .border = Rgb(0x3A3430),
    .text = Rgb(0xEDE6DF),
    .textDim = Rgb(0x958A82),
    .accent = Rgb(0xEC7F5C),
    .accentHover = Rgb(0xF29373),
    .accentActive = Rgb(0xD96A47),
    .accentSoft = Rgb(0x4A3027),
    .onAccent = Rgb(0x1C1917),
    .success = Rgb(0x5CC28E),
    .info = Rgb(0x72A9EA),
    .warning = Rgb(0xEBB04F),
    .danger = Rgb(0xF07070),
    .selection = Rgb(0xEC7F5C, 0x40),
    .findMatch = Rgb(0xF2C14E, 0x50),
    .findCurrent = Rgb(0xF29A2E, 0xA0),
    .variableBackground = Rgb(0xEC7F5C, 0x2C),
    .gutter = Rgb(0x5E5550),
    .syntax = {Rgb(0xEDE6DF), Rgb(0xF0A2AF), Rgb(0x8FD1A6), Rgb(0xF5A97F), Rgb(0xC9A7F5), Rgb(0x8F857E), Rgb(0x8CBDF2),
               Rgb(0xE8C07D), Rgb(0x7E746D), Rgb(0xF29373)},
    .kind = ThemeKind::nuit,
};

const Palette* gpPalette = &kCoton;

}  // namespace

ThemeKind ParseTheme(std::string_view id) noexcept { return id == "nuit" ? ThemeKind::nuit : ThemeKind::coton; }

std::string_view ThemeId(ThemeKind kind) noexcept { return kind == ThemeKind::nuit ? "nuit" : "coton"; }

const Palette& CurrentPalette() noexcept { return *gpPalette; }

ImU32 WithAlpha(ImU32 color, float alpha) noexcept {
  const auto currentAlpha = static_cast<float>((color >> IM_COL32_A_SHIFT) & 0xFFU);
  const auto newAlpha = static_cast<ImU32>(currentAlpha * alpha);
  return (color & ~IM_COL32_A_MASK) | (newAlpha << IM_COL32_A_SHIFT);
}

void ApplyTheme(ThemeKind kind, float scale) {
  gpPalette = kind == ThemeKind::nuit ? &kNuit : &kCoton;
  const Palette& pal = *gpPalette;

  ImGuiStyle style;  // start from the defaults (ScaleAllSizes is not idempotent)
  style.WindowPadding = ImVec2(12, 10);
  style.FramePadding = ImVec2(9, 6);
  style.CellPadding = ImVec2(6, 4);
  style.ItemSpacing = ImVec2(8, 7);
  style.ItemInnerSpacing = ImVec2(6, 5);
  style.IndentSpacing = 16;
  style.ScrollbarSize = 11;
  style.GrabMinSize = 10;
  style.WindowRounding = 10;
  style.ChildRounding = 8;
  style.FrameRounding = 7;
  style.PopupRounding = 8;
  style.ScrollbarRounding = 8;
  style.GrabRounding = 6;
  style.TabRounding = 7;
  style.SelectableRounding = 6;
  style.MenuItemRounding = 6;
  style.WindowBorderSize = 1;
  style.ChildBorderSize = 1;
  style.PopupBorderSize = 1;
  style.FrameBorderSize = 1;
  style.TabBorderSize = 0;
  style.SeparatorSize = 1;
  style.WindowTitleAlign = ImVec2(0.5F, 0.5F);
  style.SelectableTextAlign = ImVec2(0.0F, 0.5F);
  style.DisplaySafeAreaPadding = ImVec2(0, 0);
  style.ScaleAllSizes(scale);
  style.FontScaleDpi = scale;

  ImVec4* colors = style.Colors;
  colors[ImGuiCol_Text] = Vec(pal.text);
  colors[ImGuiCol_TextDisabled] = Vec(pal.textDim);
  colors[ImGuiCol_WindowBg] = Vec(pal.background);
  colors[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
  colors[ImGuiCol_PopupBg] = Vec(pal.surface);
  colors[ImGuiCol_Border] = Vec(pal.border);
  colors[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
  colors[ImGuiCol_FrameBg] = Vec(pal.surface);
  colors[ImGuiCol_FrameBgHovered] =
      Vec(pal.kind == ThemeKind::nuit ? IM_COL32(0x35, 0x2F, 0x2B, 0xFF) : IM_COL32(0xFF, 0xFA, 0xF5, 0xFF));
  colors[ImGuiCol_FrameBgActive] = colors[ImGuiCol_FrameBgHovered];
  colors[ImGuiCol_TitleBg] = Vec(pal.panel);
  colors[ImGuiCol_TitleBgActive] = Vec(pal.panel);
  colors[ImGuiCol_TitleBgCollapsed] = Vec(pal.panel);
  colors[ImGuiCol_MenuBarBg] = Vec(pal.panel);
  colors[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
  colors[ImGuiCol_ScrollbarGrab] = Vec(WithAlpha(pal.textDim, 0.35F));
  colors[ImGuiCol_ScrollbarGrabHovered] = Vec(WithAlpha(pal.textDim, 0.55F));
  colors[ImGuiCol_ScrollbarGrabActive] = Vec(WithAlpha(pal.accent, 0.8F));
  colors[ImGuiCol_CheckMark] = Vec(pal.accent);
  colors[ImGuiCol_CheckboxSelectedBg] = Vec(pal.accentSoft);
  colors[ImGuiCol_TreeLines] = Vec(pal.border);
  colors[ImGuiCol_DragDropTargetBg] = Vec(WithAlpha(pal.accent, 0.12F));
  colors[ImGuiCol_UnsavedMarker] = Vec(pal.accent);
  colors[ImGuiCol_SliderGrab] = Vec(pal.accent);
  colors[ImGuiCol_SliderGrabActive] = Vec(pal.accentActive);
  colors[ImGuiCol_Button] = Vec(pal.surface);
  colors[ImGuiCol_ButtonHovered] = Vec(pal.accentSoft);
  colors[ImGuiCol_ButtonActive] = Vec(WithAlpha(pal.accent, 0.45F));
  colors[ImGuiCol_Header] = Vec(pal.accentSoft);
  colors[ImGuiCol_HeaderHovered] = Vec(WithAlpha(pal.accent, 0.16F));
  colors[ImGuiCol_HeaderActive] = Vec(WithAlpha(pal.accent, 0.28F));
  colors[ImGuiCol_Separator] = Vec(pal.border);
  colors[ImGuiCol_SeparatorHovered] = Vec(pal.accent);
  colors[ImGuiCol_SeparatorActive] = Vec(pal.accentActive);
  colors[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
  colors[ImGuiCol_ResizeGripHovered] = Vec(pal.accent);
  colors[ImGuiCol_ResizeGripActive] = Vec(pal.accentActive);
  colors[ImGuiCol_InputTextCursor] = Vec(pal.accent);
  colors[ImGuiCol_Tab] = ImVec4(0, 0, 0, 0);
  colors[ImGuiCol_TabHovered] = Vec(WithAlpha(pal.accent, 0.14F));
  colors[ImGuiCol_TabSelected] = Vec(pal.surface);
  colors[ImGuiCol_TabSelectedOverline] = Vec(pal.accent);
  colors[ImGuiCol_TabDimmed] = ImVec4(0, 0, 0, 0);
  colors[ImGuiCol_TabDimmedSelected] = Vec(pal.surface);
  colors[ImGuiCol_TabDimmedSelectedOverline] = Vec(WithAlpha(pal.accent, 0.5F));
  colors[ImGuiCol_PlotLines] = Vec(pal.accent);
  colors[ImGuiCol_PlotLinesHovered] = Vec(pal.accentHover);
  colors[ImGuiCol_PlotHistogram] = Vec(pal.accent);
  colors[ImGuiCol_PlotHistogramHovered] = Vec(pal.accentHover);
  colors[ImGuiCol_TableHeaderBg] = Vec(pal.panel);
  colors[ImGuiCol_TableBorderStrong] = Vec(pal.border);
  colors[ImGuiCol_TableBorderLight] = Vec(WithAlpha(pal.border, 0.7F));
  colors[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
  colors[ImGuiCol_TableRowBgAlt] = Vec(WithAlpha(pal.panel, 0.5F));
  colors[ImGuiCol_TextLink] = Vec(pal.accent);
  colors[ImGuiCol_TextSelectedBg] = Vec(pal.selection);
  colors[ImGuiCol_DragDropTarget] = Vec(pal.accent);
  colors[ImGuiCol_NavCursor] = Vec(pal.accent);
  colors[ImGuiCol_NavWindowingHighlight] = Vec(pal.accent);
  colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0, 0, 0, 0.2F);
  colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.1F, 0.07F, 0.05F, kind == ThemeKind::nuit ? 0.55F : 0.28F);

  ImGui::GetStyle() = style;
}

ImU32 MethodColor(Method method) noexcept {
  const bool dark = gpPalette->kind == ThemeKind::nuit;
  switch (method) {
    case Method::get:
      return dark ? IM_COL32(0x5C, 0xC2, 0x8E, 0xFF) : IM_COL32(0x2F, 0x9E, 0x6A, 0xFF);
    case Method::post:
      return dark ? IM_COL32(0xEB, 0xB0, 0x4F, 0xFF) : IM_COL32(0xD4, 0x8A, 0x1E, 0xFF);
    case Method::put:
      return dark ? IM_COL32(0x72, 0xA9, 0xEA, 0xFF) : IM_COL32(0x3C, 0x7F, 0xCC, 0xFF);
    case Method::patch:
      return dark ? IM_COL32(0xB8, 0x95, 0xF0, 0xFF) : IM_COL32(0x8A, 0x5C, 0xD0, 0xFF);
    case Method::del:
      return dark ? IM_COL32(0xF0, 0x70, 0x70, 0xFF) : IM_COL32(0xD2, 0x4B, 0x4B, 0xFF);
    case Method::head:
      return dark ? IM_COL32(0x6F, 0xC4, 0xC4, 0xFF) : IM_COL32(0x3E, 0x92, 0x92, 0xFF);
    case Method::options:
      return dark ? IM_COL32(0xE0, 0x9A, 0xC8, 0xFF) : IM_COL32(0xB0, 0x5C, 0x94, 0xFF);
    case Method::trace:
    case Method::connect:
      break;
  }
  return gpPalette->textDim;
}

ImU32 StatusColor(std::int16_t status, ExchangeState state) noexcept {
  const Palette& pal = *gpPalette;
  if (state == ExchangeState::failed || state == ExchangeState::cancelled) {
    return state == ExchangeState::failed ? pal.danger : pal.textDim;
  }
  if (status >= 500) {
    return pal.danger;
  }
  if (status >= 400) {
    return pal.warning;
  }
  if (status >= 300) {
    return pal.info;
  }
  return pal.success;
}

}  // namespace zalie::ui
