#include "zalie/ui/icons.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string_view>

#include "zalie/ui/theme.hpp"

namespace zalie::ui {

namespace {

ImVec2 At(ImVec2 center, float size, float x, float y) {
  // (x, y) in [-0.5, 0.5] relative coordinates.
  return {center.x + (x * size), center.y + (y * size)};
}

// Zalie's paw: the pad and four toes, turned by 'angle' radians. 'toeScale' enlarges the toes, for small prints.
void DrawPawPrint(ImDrawList* pDrawList, ImVec2 center, float size, float angle, float toeScale, ImU32 color) {
  const float cosA = std::cos(angle);
  const float sinA = std::sin(angle);
  const auto at = [&](float x, float y) { return At(center, size, (x * cosA) - (y * sinA), (x * sinA) + (y * cosA)); };
  pDrawList->AddEllipseFilled(at(0, 0.17F), ImVec2(size * 0.23F, size * 0.19F), color, angle);
  pDrawList->AddCircleFilled(at(-0.31F, -0.05F), size * 0.095F * toeScale, color);
  pDrawList->AddCircleFilled(at(-0.12F, -0.27F), size * 0.10F * toeScale, color);
  pDrawList->AddCircleFilled(at(0.12F, -0.27F), size * 0.10F * toeScale, color);
  pDrawList->AddCircleFilled(at(0.31F, -0.05F), size * 0.095F * toeScale, color);
}

}  // namespace

void DrawIcon(ImDrawList* pDrawList, Icon icon, ImVec2 center, float size, ImU32 color) {
  const float thick = std::max(1.2F, size / 11.0F);
  const auto line = [&](float x0, float y0, float x1, float y1) {
    pDrawList->AddLine(At(center, size, x0, y0), At(center, size, x1, y1), color, thick);
  };
  switch (icon) {
    case Icon::plus:
      line(-0.36F, 0, 0.36F, 0);
      line(0, -0.36F, 0, 0.36F);
      break;
    case Icon::close:
      line(-0.28F, -0.28F, 0.28F, 0.28F);
      line(0.28F, -0.28F, -0.28F, 0.28F);
      break;
    case Icon::gear: {
      const float outer = size * 0.42F;
      const float inner = size * 0.30F;
      static constexpr int kTeeth = 8;
      pDrawList->PathClear();
      for (int i = 0; i < kTeeth * 4; ++i) {
        const float angle = (static_cast<float>(i) / (kTeeth * 4.0F)) * 2.0F * std::numbers::pi_v<float>;
        const float radius = (i % 4 == 0 || i % 4 == 1) ? outer : inner;
        pDrawList->PathLineTo({center.x + (radius * std::cos(angle)), center.y + (radius * std::sin(angle))});
      }
      pDrawList->PathStroke(color, thick, ImDrawFlags_Closed);
      pDrawList->AddCircle(center, size * 0.12F, color, 0, thick);
      break;
    }
    case Icon::search:
      pDrawList->AddCircle(At(center, size, -0.08F, -0.08F), size * 0.25F, color, 0, thick);
      line(0.11F, 0.11F, 0.36F, 0.36F);
      break;
    case Icon::sun: {
      pDrawList->AddCircle(center, size * 0.18F, color, 0, thick);
      for (int i = 0; i < 8; ++i) {
        const float angle = static_cast<float>(i) * std::numbers::pi_v<float> / 4.0F;
        line(0.29F * std::cos(angle), 0.29F * std::sin(angle), 0.42F * std::cos(angle), 0.42F * std::sin(angle));
      }
      break;
    }
    case Icon::moon: {
      // A crescent: a disc minus another one, drawn as two arcs.
      const float radius = size * 0.36F;
      pDrawList->PathArcTo(center, radius, 0.25F * std::numbers::pi_v<float>, 1.75F * std::numbers::pi_v<float>, 24);
      pDrawList->PathArcTo(At(center, size, 0.22F, -0.12F), radius * 0.78F, 1.45F * std::numbers::pi_v<float>,
                           0.55F * std::numbers::pi_v<float>, 24);
      pDrawList->PathStroke(color, thick, ImDrawFlags_Closed);
      break;
    }
    case Icon::sidebar:
    case Icon::inspector: {
      const ImVec2 min = At(center, size, -0.40F, -0.34F);
      const ImVec2 max = At(center, size, 0.40F, 0.34F);
      pDrawList->AddRect(min, max, color, size * 0.08F, thick);
      const float x = icon == Icon::sidebar ? -0.12F : 0.12F;
      line(x, -0.34F, x, 0.34F);
      break;
    }
    case Icon::folder: {
      pDrawList->PathClear();
      pDrawList->PathLineTo(At(center, size, -0.42F, -0.30F));
      pDrawList->PathLineTo(At(center, size, -0.10F, -0.30F));
      pDrawList->PathLineTo(At(center, size, 0.02F, -0.18F));
      pDrawList->PathLineTo(At(center, size, 0.42F, -0.18F));
      pDrawList->PathLineTo(At(center, size, 0.42F, 0.32F));
      pDrawList->PathLineTo(At(center, size, -0.42F, 0.32F));
      pDrawList->PathFillConvex(WithAlpha(color, 0.25F));
      pDrawList->PathLineTo(At(center, size, -0.42F, -0.30F));
      pDrawList->PathLineTo(At(center, size, -0.10F, -0.30F));
      pDrawList->PathLineTo(At(center, size, 0.02F, -0.18F));
      pDrawList->PathLineTo(At(center, size, 0.42F, -0.18F));
      pDrawList->PathLineTo(At(center, size, 0.42F, 0.32F));
      pDrawList->PathLineTo(At(center, size, -0.42F, 0.32F));
      pDrawList->PathStroke(color, thick, ImDrawFlags_Closed);
      break;
    }
    case Icon::copy:
      pDrawList->AddRect(At(center, size, -0.36F, -0.40F), At(center, size, 0.16F, 0.16F), color, size * 0.06F, thick);
      pDrawList->AddRect(At(center, size, -0.14F, -0.16F), At(center, size, 0.38F, 0.40F), color, size * 0.06F, thick);
      break;
    case Icon::send:
      pDrawList->AddTriangleFilled(At(center, size, -0.34F, -0.36F), At(center, size, 0.40F, 0.0F),
                                   At(center, size, -0.34F, 0.36F), color);
      break;
    case Icon::history: {
      pDrawList->AddCircle(center, size * 0.38F, color, 0, thick);
      line(0, -0.22F, 0, 0);
      line(0, 0, 0.16F, 0.10F);
      break;
    }
    case Icon::trash:
      line(-0.36F, -0.26F, 0.36F, -0.26F);
      line(-0.12F, -0.38F, 0.12F, -0.38F);
      pDrawList->AddRect(At(center, size, -0.26F, -0.18F), At(center, size, 0.26F, 0.40F), color, size * 0.05F, thick);
      break;
    case Icon::edit:
      line(-0.30F, 0.30F, 0.22F, -0.22F);
      line(0.16F, -0.32F, 0.32F, -0.16F);
      line(-0.36F, 0.38F, -0.06F, 0.38F);
      break;
    case Icon::import:
      line(0, -0.40F, 0, 0.14F);
      line(-0.18F, -0.04F, 0, 0.14F);
      line(0.18F, -0.04F, 0, 0.14F);
      line(-0.38F, 0.18F, -0.38F, 0.38F);
      line(-0.38F, 0.38F, 0.38F, 0.38F);
      line(0.38F, 0.38F, 0.38F, 0.18F);
      break;
    case Icon::warning: {
      pDrawList->AddTriangle(At(center, size, 0, -0.40F), At(center, size, 0.42F, 0.36F),
                             At(center, size, -0.42F, 0.36F), color, thick);
      line(0, -0.12F, 0, 0.12F);
      pDrawList->AddCircleFilled(At(center, size, 0, 0.24F), thick * 0.7F, color);
      break;
    }
    case Icon::check:
      line(-0.32F, 0.02F, -0.08F, 0.26F);
      line(-0.08F, 0.26F, 0.34F, -0.24F);
      break;
    case Icon::stop:
      pDrawList->AddRectFilled(At(center, size, -0.28F, -0.28F), At(center, size, 0.28F, 0.28F), color, size * 0.08F);
      break;
    case Icon::dots:
      for (float x : {-0.28F, 0.0F, 0.28F}) {
        pDrawList->AddCircleFilled(At(center, size, x, 0), size * 0.07F, color);
      }
      break;
    case Icon::chevronUp:
      line(-0.26F, 0.12F, 0, -0.14F);
      line(0, -0.14F, 0.26F, 0.12F);
      break;
    case Icon::chevronDown:
      line(-0.26F, -0.12F, 0, 0.14F);
      line(0, 0.14F, 0.26F, -0.12F);
      break;
    case Icon::eye: {
      pDrawList->PathArcTo(At(center, size, 0, 0.42F), size * 0.62F, -0.78F * std::numbers::pi_v<float>,
                           -0.22F * std::numbers::pi_v<float>, 16);
      pDrawList->PathArcTo(At(center, size, 0, -0.42F), size * 0.62F, 0.22F * std::numbers::pi_v<float>,
                           0.78F * std::numbers::pi_v<float>, 16);
      pDrawList->PathStroke(color, thick, ImDrawFlags_Closed);
      pDrawList->AddCircleFilled(center, size * 0.11F, color);
      break;
    }
    case Icon::paw:
      DrawPawPrint(pDrawList, center, size, 0, 1.0F, color);
      break;
    case Icon::pawPrints:
      // Three prints in a triangle, turned like the steps of a trail.
      DrawPawPrint(pDrawList, At(center, size, 0, -0.23F), size * 0.52F, 0.12F, 1.2F, color);
      DrawPawPrint(pDrawList, At(center, size, -0.25F, 0.21F), size * 0.52F, -0.30F, 1.2F, color);
      DrawPawPrint(pDrawList, At(center, size, 0.25F, 0.21F), size * 0.52F, 0.30F, 1.2F, color);
      break;
    case Icon::columns:
      pDrawList->AddRect(At(center, size, -0.40F, -0.34F), At(center, size, 0.40F, 0.34F), color, size * 0.08F, thick);
      line(0, -0.34F, 0, 0.34F);
      break;
  }
}

void DrawItemIconLabel(Icon icon, float iconSize, std::string_view text, ImU32 color) {
  const ImVec2 min = ImGui::GetItemRectMin();
  const ImVec2 max = ImGui::GetItemRectMax();
  const float gap = 6.0F * ImGui::GetStyle().FontScaleDpi;
  const float textWidth = ImGui::CalcTextSize(text.data(), text.data() + text.size()).x;
  const float startX = std::floor(((min.x + max.x) * 0.5F) - ((iconSize + gap + textWidth) * 0.5F));
  const float midY = (min.y + max.y) * 0.5F;
  ImDrawList* pDrawList = ImGui::GetWindowDrawList();
  DrawIcon(pDrawList, icon, ImVec2(startX + (iconSize * 0.5F), midY), iconSize, color);
  pDrawList->AddText(ImVec2(startX + iconSize + gap, std::floor(midY - (ImGui::GetFontSize() * 0.5F))), color,
                     text.data(), text.data() + text.size());
}

bool IconButton(const char* strId, Icon icon, const char* tooltip, bool active) {
  const float size = ImGui::GetFrameHeight();
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  const bool pressed = ImGui::InvisibleButton(strId, ImVec2(size, size));
  const bool hovered = ImGui::IsItemHovered();
  const Palette& pal = CurrentPalette();
  ImDrawList* pDrawList = ImGui::GetWindowDrawList();
  if (hovered || active) {
    pDrawList->AddRectFilled(pos, ImVec2(pos.x + size, pos.y + size),
                             active ? pal.accentSoft : WithAlpha(pal.accent, 0.12F), ImGui::GetStyle().FrameRounding);
  }
  DrawIcon(pDrawList, icon, ImVec2(pos.x + (size * 0.5F), pos.y + (size * 0.5F)), size * 0.52F,
           active || hovered ? pal.accent : pal.textDim);
  if (tooltip != nullptr && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("%s", tooltip);
  }
  return pressed;
}

}  // namespace zalie::ui
