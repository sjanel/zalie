// A small chart widget drawn with ImDrawList: no plotting dependency, colors of the theme, testable headless.

#include "zalie/ui/chart.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <span>
#include <string_view>

#include "zalie/ui/theme.hpp"
#include "zalie/ui/widgets.hpp"

namespace zalie::ui {

namespace {

constexpr float kNan = std::numeric_limits<float>::quiet_NaN();
constexpr int kTargetYTicks = 4;
constexpr float kMinZoomDragPixels = 4.0F;
constexpr float kAreaAlpha = 0.45F;

// Three significant digits at most, without exponent nor trailing zeros: "1", "1.5", "12.5", "125".
std::string_view FormatNumber(double value, std::string_view suffix, std::span<char> buffer) {
  const double magnitude = std::abs(value);
  std::string_view number;
  if (magnitude >= 100.0 || magnitude == 0.0) {
    number = FormatTo(buffer, "{:.0f}", value);
  } else if (magnitude >= 10.0) {
    number = FormatTo(buffer, "{:.1f}", value);
  } else if (magnitude >= 1.0) {
    number = FormatTo(buffer, "{:.2f}", value);
  } else {
    number = FormatTo(buffer, "{:.3f}", value);
  }
  if (number.contains('.')) {
    while (number.ends_with('0')) {
      number.remove_suffix(1);
    }
    if (number.ends_with('.')) {
      number.remove_suffix(1);
    }
  }
  const std::size_t room = std::min(suffix.size(), buffer.size() - number.size());
  // TODO: use aeronet tech primitives (memory-utils.hpp)
  // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
  std::copy_n(suffix.data(), room, buffer.data() + number.size());
  return {buffer.data(), number.size() + room};
}

// Maps a value of an axis to the space where the axis is linear.
double Transform(ChartScale scale, double value) noexcept {
  switch (scale) {
    case ChartScale::log:
      return value > 0.0 ? std::log10(value) : std::numeric_limits<double>::quiet_NaN();
    case ChartScale::nines:
      return -std::log10(std::max(1e-9, 1.0 - (value / 100.0)));
    case ChartScale::linear:
      break;
  }
  return value;
}

double Inverse(ChartScale scale, double transformed) noexcept {
  switch (scale) {
    case ChartScale::log:
      return std::pow(10.0, transformed);
    case ChartScale::nines:
      return 100.0 * (1.0 - std::pow(10.0, -transformed));
    case ChartScale::linear:
      break;
  }
  return transformed;
}

// A round step close to range / target: 1, 2 or 5 times a power of ten, or a round duration for elapsed seconds.
double NiceStep(double range, int target, ChartUnit unit) noexcept {
  const double raw = std::max(range / target, 1e-12);
  if (unit == ChartUnit::seconds && raw >= 1.0) {
    static constexpr double kSteps[]{1.0,   2.0,   5.0,    10.0,   15.0,   30.0,    60.0,    120.0,  300.0,
                                     600.0, 900.0, 1800.0, 3600.0, 7200.0, 14400.0, 28800.0, 86400.0};
    static constexpr std::size_t kStepsSize = sizeof(kSteps) / sizeof(kSteps[0]);
    const auto* it = std::ranges::lower_bound(kSteps, raw);
    return it == kSteps + kStepsSize ? kSteps[kStepsSize - 1U] * std::ceil(raw / kSteps[kStepsSize - 1U]) : *it;
  }
  const double magnitude = std::pow(10.0, std::floor(std::log10(raw)));
  const double normalized = raw / magnitude;
  double nice = 10.0;
  if (normalized < 1.5) {
    nice = 1.0;
  } else if (normalized < 3.0) {
    nice = 2.0;
  } else if (normalized < 7.0) {
    nice = 5.0;
  }
  return nice * magnitude;
}

// The range of an axis, in transformed space.
struct AxisRange {
  [[nodiscard]] float toScreen(double transformed, float from, float to) const noexcept {
    return from + (static_cast<float>((transformed - min) / (max - min)) * (to - from));
  }

  double min{0.0};
  double max{1.0};
};

// Calls 'tick(transformed value)' for each tick of the axis.
template <class Tick>
void ForEachTick(const AxisRange& range, const ChartAxis& axis, int target, Tick&& tick) {
  static constexpr int kMaxTicks = 64;
  assert(std::isfinite(range.min) && std::isfinite(range.max) && range.max > range.min);
  if (axis.scale == ChartScale::linear) {
    const double step = NiceStep(range.max - range.min, target, axis.unit);
    const double first = std::ceil((range.min / step) - 1e-9);
    for (int idx = 0; idx < kMaxTicks; ++idx) {
      const double value = (first + idx) * step;
      if (value > range.max + (step * 1e-9)) {
        break;
      }
      tick(value);
    }
    return;
  }
  // Log and nines axes: each power of ten (each nine), with 2 and 5 between them when they are few.
  const double first = std::floor(range.min);
  const double last = std::ceil(range.max);
  const bool dense = axis.scale == ChartScale::log && last - first <= 2.0;
  for (double decade = first; decade <= last && decade < first + kMaxTicks; decade += 1.0) {
    if (decade >= range.min - 1e-9 && decade <= range.max + 1e-9) {
      tick(decade);
    }
    if (dense) {
      for (double factor : {2.0, 5.0}) {
        const double value = decade + std::log10(factor);
        if (value >= range.min && value <= range.max) {
          tick(value);
        }
      }
    }
  }
}

void PreparePoints(std::span<const float> x, std::span<const ChartSeries> series, ChartState& state, float width) {
  state.pointsRevision = state.revision;
  state.pointsWidth = width;
  std::size_t first = 0;
  std::size_t last = x.size();
  if (state.zoomed()) {
    // One point beyond each side of the window, so that the lines reach its borders.
    first = static_cast<std::size_t>(std::ranges::lower_bound(x, static_cast<float>(state.zoomMin)) - x.begin());
    last = static_cast<std::size_t>(std::ranges::upper_bound(x, static_cast<float>(state.zoomMax)) - x.begin());
    first = first == 0 ? 0 : first - 1;
    last = std::min(last + 1, x.size());
  }
  const std::size_t count = last - first;
  const std::size_t maxPoints = std::max<std::size_t>(2, static_cast<std::size_t>(width));
  const std::size_t stride = std::min(count, maxPoints);
  const std::size_t nbSeries = series.size();
  state.stride = static_cast<std::uint32_t>(stride);
  state.points.resize(static_cast<std::uint32_t>((nbSeries + 1) * stride));
  float* pX = state.points.data();
  if (count <= maxPoints) {
    std::copy_n(x.data() + first, count, pX);
    for (std::size_t idx = 0; idx < nbSeries; ++idx) {
      std::copy_n(series[idx].values.data() + first, count, pX + ((idx + 1) * stride));
    }
    state.nbPoints = static_cast<std::uint32_t>(count);
    return;
  }
  // More points than pixels: one point per pixel column, its values reduced.
  const double x0 = x[first];
  const double columnWidth = (static_cast<double>(x[last - 1]) - x0) / static_cast<double>(maxPoints);
  std::size_t nbPoints = 0;
  std::size_t pos = first;
  for (std::size_t column = 0; column < maxPoints && pos < last; ++column) {
    const double end = column + 1 == maxPoints ? std::numeric_limits<double>::infinity()
                                               : x0 + (columnWidth * static_cast<double>(column + 1));
    const std::size_t begin = pos;
    double sumX = 0.0;
    while (pos < last && x[pos] < end) {
      sumX += x[pos];
      ++pos;
    }
    if (pos == begin) {
      continue;
    }
    pX[nbPoints] = static_cast<float>(sumX / static_cast<double>(pos - begin));
    for (std::size_t idx = 0; idx < nbSeries; ++idx) {
      const std::span<const float> values = series[idx].values.subspan(begin, pos - begin);
      double sum = 0.0;
      float max = kNan;
      std::size_t nbValues = 0;
      for (float value : values) {
        if (!std::isnan(value)) {
          sum += value;
          max = std::isnan(max) ? value : std::max(max, value);
          ++nbValues;
        }
      }
      float reduced = kNan;
      if (nbValues != 0) {
        reduced =
            series[idx].reduce == ChartReduce::max ? max : static_cast<float>(sum / static_cast<double>(nbValues));
      }
      pX[((idx + 1) * stride) + nbPoints] = reduced;
    }
    ++nbPoints;
  }
  state.nbPoints = static_cast<std::uint32_t>(nbPoints);
}

// The value of a point for the stacking: hidden series and missing values count as 0.
float StackValue(float value) noexcept { return std::isnan(value) ? 0.0F : value; }

void DrawSwatch(ImDrawList* pDrawList, ImVec2 pos, float size, ImU32 color) {
  pDrawList->AddRectFilled(pos, ImVec2(pos.x + size, pos.y + size), color, size * 0.25F);
}

}  // namespace

std::string_view FormatChartValue(ChartUnit unit, double value, std::span<char> buffer) {
  const double magnitude = std::abs(value);
  if (magnitude == 0.0 && unit != ChartUnit::percent) {
    return FormatTo(buffer, "0");
  }
  switch (unit) {
    case ChartUnit::count:
      if (magnitude >= 1e9) {
        return FormatNumber(value / 1e9, "G", buffer);
      }
      if (magnitude >= 1e6) {
        return FormatNumber(value / 1e6, "M", buffer);
      }
      if (magnitude >= 1e4) {
        return FormatNumber(value / 1e3, "k", buffer);
      }
      return FormatNumber(value, "", buffer);
    case ChartUnit::nanoseconds:
      if (magnitude >= 1e9) {
        return FormatNumber(value / 1e9, " s", buffer);
      }
      if (magnitude >= 1e6) {
        return FormatNumber(value / 1e6, " ms", buffer);
      }
      if (magnitude >= 1e3) {
        return FormatNumber(value / 1e3, " \xC2\xB5s", buffer);
      }
      return FormatNumber(value, " ns", buffer);
    case ChartUnit::bytes: {
      static constexpr double kKiB = 1024.0;
      if (magnitude >= kKiB * kKiB * kKiB) {
        return FormatNumber(value / (kKiB * kKiB * kKiB), " GB", buffer);
      }
      if (magnitude >= kKiB * kKiB) {
        return FormatNumber(value / (kKiB * kKiB), " MB", buffer);
      }
      if (magnitude >= kKiB) {
        return FormatNumber(value / kKiB, " KB", buffer);
      }
      return FormatNumber(value, " B", buffer);
    }
    case ChartUnit::seconds: {
      if (magnitude < 60.0) {
        return FormatNumber(value, " s", buffer);
      }
      const auto total = static_cast<std::int64_t>(std::llround(value));
      if (total % 60 == 0) {
        return FormatTo(buffer, "{} min", total / 60);
      }
      return FormatTo(buffer, "{}:{:02}", total / 60, total % 60);
    }
    case ChartUnit::percent:
      if (magnitude >= 90.0 && magnitude < 100.0) {
        // 99.95: as many digits as nines.
        return FormatTo(buffer, "{:g} %", std::round(value * 1000.0) / 1000.0);
      }
      return FormatNumber(value, " %", buffer);
  }
  return {};
}

void Chart(const char* strId, std::span<const float> x, std::span<const ChartSeries> series, ChartState& state,
           const ChartOptions& options) {
  assert(series.size() <= ChartState::kMaxSeries);
  const Palette& pal = CurrentPalette();
  const ImGuiStyle& style = ImGui::GetStyle();
  const float scale = style.FontScaleDpi;
  const bool bars = options.style == ChartStyle::bars;
  const std::size_t nbValues = bars ? (x.empty() ? 0 : x.size() - 1) : x.size();
  for ([[maybe_unused]] const ChartSeries& item : series) {
    assert(item.values.size() == nbValues);
  }

  ImGui::PushID(strId);
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const float width = std::max(ImGui::GetContentRegionAvail().x, 120.0F * scale);
  const float height = options.height > 0.0F ? options.height : 200.0F * scale;
  ImGui::InvisibleButton("##chart", ImVec2(width, height));
  const bool hovered = ImGui::IsItemHovered();
  const bool active = ImGui::IsItemActive();
  ImDrawList* pDrawList = ImGui::GetWindowDrawList();
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  std::array<char, 64> buffer{};

  ScopedFont font(GetFonts().pUi, Fonts::kUiSize * 0.8F);
  const float lineHeight = ImGui::GetTextLineHeight();
  const float pad = 6.0F * scale;

  // Title on the left, legend on the right (a click hides or shows a series).
  pDrawList->AddText(origin, pal.text, options.title.data(), options.title.data() + options.title.size());
  float legendX = origin.x + width;
  const float swatch = lineHeight * 0.7F;
  for (std::size_t idx = series.size(); idx-- > 0;) {
    const ChartSeries& item = series[idx];
    const float textWidth = ImGui::CalcTextSize(item.name.data(), item.name.data() + item.name.size()).x;
    const float itemWidth = swatch + (4.0F * scale) + textWidth;
    legendX -= itemWidth;
    const ImVec2 itemMin(legendX, origin.y);
    const ImVec2 itemMax(legendX + itemWidth, origin.y + lineHeight);
    const bool hidden = state.isHidden(idx);
    const bool itemHovered = hovered && ImGui::IsMouseHoveringRect(itemMin, itemMax);
    DrawSwatch(pDrawList, ImVec2(legendX, origin.y + ((lineHeight - swatch) * 0.5F)), swatch,
               hidden ? WithAlpha(item.color, 0.25F) : item.color);
    pDrawList->AddText(ImVec2(legendX + swatch + (4.0F * scale), origin.y),
                       hidden ? pal.textDim : (itemHovered ? pal.accent : pal.text), item.name.data(),
                       item.name.data() + item.name.size());
    if (itemHovered) {
      ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
      if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        state.hidden ^= 1U << idx;
      }
    }
    legendX -= 12.0F * scale;
  }

  // The points of the window shown, prepared again only when the data, the width or the zoom change.
  const float plotTop = origin.y + lineHeight + pad;
  const float plotBottom = origin.y + height - lineHeight - pad;
  float plotLeft = origin.x;
  const float plotRight = origin.x + width - pad;
  const float approxPlotWidth = std::max(1.0F, width - (50.0F * scale));
  if (!bars && (state.pointsRevision != state.revision || std::abs(state.pointsWidth - approxPlotWidth) >= 1.0F)) {
    PreparePoints(x, series, state, approxPlotWidth);
  }
  const std::uint32_t nbPoints = bars ? static_cast<std::uint32_t>(nbValues) : state.nbPoints;
  const float* pX = bars ? x.data() : state.points.data();
  const auto valueAt = [&](std::size_t seriesIdx, std::size_t point) {
    return bars ? series[seriesIdx].values[point]
                : state.points[static_cast<std::uint32_t>(((seriesIdx + 1) * state.stride) + point)];
  };

  // Ranges of the axes, in transformed space.
  AxisRange xRange;
  if (nbPoints != 0) {
    const float xFirst = pX[0];
    const float xLast = bars ? pX[nbPoints] : pX[nbPoints - 1];
    xRange = {.min = Transform(options.x.scale, xFirst), .max = Transform(options.x.scale, xLast)};
    if (state.zoomed() && options.x.scale == ChartScale::linear) {
      xRange = {.min = state.zoomMin, .max = state.zoomMax};
    }
  }
  if (!std::isfinite(xRange.min) || !std::isfinite(xRange.max)) {
    xRange = {};
  }
  if (!(xRange.max > xRange.min)) {
    xRange.max = xRange.min + 1.0;
  }
  double yMin = std::numeric_limits<double>::infinity();
  double yMax = -std::numeric_limits<double>::infinity();
  for (std::size_t point = 0; point < nbPoints; ++point) {
    double stacked = 0.0;
    for (std::size_t idx = 0; idx < series.size(); ++idx) {
      if (state.isHidden(idx)) {
        continue;
      }
      const float value = valueAt(idx, point);
      if (options.style == ChartStyle::stackedAreas) {
        stacked += StackValue(value);
        continue;
      }
      if (std::isnan(value) || (options.y.scale == ChartScale::log && value <= 0.0F)) {
        continue;
      }
      yMin = std::min(yMin, static_cast<double>(value));
      yMax = std::max(yMax, static_cast<double>(value));
    }
    if (options.style == ChartStyle::stackedAreas && stacked > 0.0) {
      yMin = std::min(yMin, stacked);
      yMax = std::max(yMax, stacked);
    }
  }
  AxisRange yRange;
  if (options.y.scale == ChartScale::log) {
    if (yMax > 0.0 && yMin > 0.0) {
      yRange = {.min = std::floor(std::log10(yMin)), .max = std::ceil(std::log10(yMax))};
    }
    if (!(yRange.max > yRange.min)) {
      yRange.max = yRange.min + 1.0;
    }
  } else {
    const double top = yMax > 0.0 ? yMax * 1.05 : 1.0;
    const double step = NiceStep(top, kTargetYTicks, options.y.unit);
    yRange = {.min = 0.0, .max = std::ceil(top / step) * step};
  }

  // Y labels: the widest one gives the left margin of the plot.
  float labelsWidth = 0.0F;
  ForEachTick(yRange, options.y, kTargetYTicks, [&](double tick) {
    const std::string_view label = FormatChartValue(options.y.unit, Inverse(options.y.scale, tick), buffer);
    labelsWidth = std::max(labelsWidth, ImGui::CalcTextSize(label.data(), label.data() + label.size()).x);
  });
  plotLeft += labelsWidth + pad;
  const auto toX = [&](double value) {
    return xRange.toScreen(Transform(options.x.scale, value), plotLeft, plotRight);
  };
  const auto toY = [&](double value) {
    const double transformed = Transform(options.y.scale, value);
    return std::isnan(transformed) ? plotBottom : yRange.toScreen(transformed, plotBottom, plotTop);
  };

  // Grid and labels.
  const ImU32 gridColor = WithAlpha(pal.border, 0.8F);
  ForEachTick(yRange, options.y, kTargetYTicks, [&](double tick) {
    const float y = std::floor(yRange.toScreen(tick, plotBottom, plotTop)) + 0.5F;
    pDrawList->AddLine(ImVec2(plotLeft, y), ImVec2(plotRight, y), gridColor);
    const std::string_view label = FormatChartValue(options.y.unit, Inverse(options.y.scale, tick), buffer);
    const float labelWidth = ImGui::CalcTextSize(label.data(), label.data() + label.size()).x;
    pDrawList->AddText(ImVec2(plotLeft - pad - labelWidth, y - (lineHeight * 0.5F)), pal.textDim, label.data(),
                       label.data() + label.size());
  });
  const int xTarget = std::max(2, static_cast<int>((plotRight - plotLeft) / (90.0F * scale)));
  float lastLabelEnd = -std::numeric_limits<float>::infinity();
  ForEachTick(xRange, options.x, xTarget, [&](double tick) {
    const float tickX = std::floor(xRange.toScreen(tick, plotLeft, plotRight)) + 0.5F;
    pDrawList->AddLine(ImVec2(tickX, plotBottom), ImVec2(tickX, plotBottom + (3.0F * scale)), pal.border);
    const std::string_view label = FormatChartValue(options.x.unit, Inverse(options.x.scale, tick), buffer);
    const float labelWidth = ImGui::CalcTextSize(label.data(), label.data() + label.size()).x;
    const float labelX = std::clamp(tickX - (labelWidth * 0.5F), plotLeft, plotRight - labelWidth);
    if (labelX > lastLabelEnd + pad) {
      pDrawList->AddText(ImVec2(labelX, plotBottom + (4.0F * scale)), pal.textDim, label.data(),
                         label.data() + label.size());
      lastLabelEnd = labelX + labelWidth;
    }
  });
  pDrawList->AddLine(ImVec2(plotLeft, plotBottom + 0.5F), ImVec2(plotRight, plotBottom + 0.5F), pal.border);

  pDrawList->PushClipRect(ImVec2(plotLeft, plotTop - 1.0F), ImVec2(plotRight + 1.0F, plotBottom + 1.0F), true);
  if (nbPoints == 0) {
    static constexpr std::string_view kNoData = "No data yet";
    const ImVec2 size = ImGui::CalcTextSize(kNoData.data(), kNoData.data() + kNoData.size());
    pDrawList->AddText(ImVec2((plotLeft + plotRight - size.x) * 0.5F, (plotTop + plotBottom - size.y) * 0.5F),
                       pal.textDim, kNoData.data(), kNoData.data() + kNoData.size());
  }
  const float thickness = 1.5F * scale;
  switch (options.style) {
    case ChartStyle::lines:
      for (std::size_t idx = 0; idx < series.size(); ++idx) {
        if (state.isHidden(idx)) {
          continue;
        }
        state.scratch.clear();
        const auto flush = [&] {
          if (state.scratch.size() == 1) {
            pDrawList->AddCircleFilled(state.scratch[0], thickness * 1.5F, series[idx].color);
          } else if (state.scratch.size() > 1) {
            pDrawList->AddPolyline(state.scratch.data(), static_cast<int>(state.scratch.size()), series[idx].color,
                                   thickness);
          }
          state.scratch.clear();
        };
        for (std::size_t point = 0; point < nbPoints; ++point) {
          const float value = valueAt(idx, point);
          if (std::isnan(value) || (options.y.scale == ChartScale::log && value <= 0.0F)) {
            flush();
            continue;
          }
          state.scratch.push_back(ImVec2(toX(pX[point]), toY(value)));
        }
        flush();
      }
      break;
    case ChartStyle::stackedAreas: {
      // Two passes: the fills (without anti-aliasing, so that the quads of an area join without seams), then the top
      // lines over them. 'scratch' holds the screen x and the running sum of each point.
      for (int pass = 0; pass < 2; ++pass) {
        pDrawList->PushDrawFlag(ImDrawFlags_AAFill, pass != 0);
        state.scratch.clear();
        for (std::size_t point = 0; point < nbPoints; ++point) {
          state.scratch.push_back(ImVec2(toX(pX[point]), 0.0F));
        }
        for (std::size_t idx = 0; idx < series.size(); ++idx) {
          if (state.isHidden(idx)) {
            continue;
          }
          const ImU32 fill = WithAlpha(series[idx].color, kAreaAlpha);
          ImVec2 previousBottom;
          ImVec2 previousTop;
          for (std::uint32_t point = 0; point < nbPoints; ++point) {
            ImVec2& slot = state.scratch[point];
            const float base = slot.y;
            slot.y = base + StackValue(valueAt(idx, point));
            const ImVec2 bottomPos(slot.x, toY(base));
            const ImVec2 topPos(slot.x, toY(slot.y));
            if (point != 0) {
              if (pass == 0) {
                pDrawList->AddQuadFilled(previousBottom, previousTop, topPos, bottomPos, fill);
              } else if (previousTop.y != previousBottom.y || topPos.y != bottomPos.y) {
                // The top line of a series where it has values: an empty series does not hide the one below.
                pDrawList->AddLine(previousTop, topPos, series[idx].color, thickness);
              }
            }
            previousBottom = bottomPos;
            previousTop = topPos;
          }
        }
        pDrawList->PopDrawFlag();
      }
      break;
    }
    case ChartStyle::bars:
      for (std::size_t idx = 0; idx < series.size(); ++idx) {
        if (state.isHidden(idx)) {
          continue;
        }
        for (std::size_t point = 0; point < nbPoints; ++point) {
          const float value = series[idx].values[point];
          if (std::isnan(value) || value <= 0.0F) {
            continue;
          }
          const float left = toX(pX[point]);
          const float right = std::max(left + 1.0F, toX(pX[point + 1]) - 1.0F);
          pDrawList->AddRectFilled(ImVec2(left, toY(value)), ImVec2(right, plotBottom), series[idx].color);
        }
      }
      break;
  }
  for (const ChartMarker& marker : options.markers) {
    const double transformed = Transform(options.x.scale, marker.x);
    if (std::isnan(transformed)) {
      continue;  // not on the axis: 0 on a logarithmic one (a percentile not computed)
    }
    const float markerX = std::floor(xRange.toScreen(transformed, plotLeft, plotRight)) + 0.5F;
    pDrawList->AddLine(ImVec2(markerX, plotTop), ImVec2(markerX, plotBottom), marker.color, 1.0F);
    pDrawList->AddText(ImVec2(markerX + (3.0F * scale), plotTop), marker.color, marker.label.data(),
                       marker.label.data() + marker.label.size());
  }

  // Zoom: drag over a time axis, double click to show everything again.
  const bool canZoom = options.zoomable && options.x.scale == ChartScale::linear && nbPoints > 1;
  const bool inPlot = mouse.x >= plotLeft && mouse.x <= plotRight && mouse.y >= plotTop && mouse.y <= plotBottom;
  if (canZoom) {
    if (hovered && inPlot && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
      state.resetZoom();
      state.dragStartX = -1.0F;
    } else if (active && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && inPlot) {
      state.dragStartX = mouse.x;
    }
    if (state.dragStartX >= 0.0F) {
      const float from = std::clamp(std::min(state.dragStartX, mouse.x), plotLeft, plotRight);
      const float to = std::clamp(std::max(state.dragStartX, mouse.x), plotLeft, plotRight);
      if (active) {
        pDrawList->AddRectFilled(ImVec2(from, plotTop), ImVec2(to, plotBottom), WithAlpha(pal.accent, 0.15F));
      } else {
        if (to - from >= kMinZoomDragPixels) {
          const auto toValue = [&](float screenX) {
            return xRange.min +
                   ((static_cast<double>(screenX - plotLeft) / (plotRight - plotLeft)) * (xRange.max - xRange.min));
          };
          state.zoomMin = toValue(from);
          state.zoomMax = toValue(to);
          state.invalidate();
        }
        state.dragStartX = -1.0F;
      }
    }
  }

  // Hover: the values of every series at the x of the mouse.
  if (hovered && inPlot && nbPoints != 0 && (state.dragStartX < 0.0F || !active)) {
    std::size_t point = 0;
    if (bars) {
      while (point + 1 < nbPoints && toX(pX[point + 1]) <= mouse.x) {
        ++point;
      }
    } else {
      // Nearest point: the x are increasing.
      const auto* it = std::lower_bound(pX, pX + nbPoints, mouse.x,
                                        [&](float value, float screenX) { return toX(value) < screenX; });
      point = static_cast<std::size_t>(it - pX);
      if (point == nbPoints || (point > 0 && mouse.x - toX(pX[point - 1]) < toX(pX[point]) - mouse.x)) {
        --point;
      }
      const float lineX = std::floor(toX(pX[point])) + 0.5F;
      pDrawList->AddLine(ImVec2(lineX, plotTop), ImVec2(lineX, plotBottom), WithAlpha(pal.text, 0.35F));
    }
    pDrawList->PopClipRect();
    ImGui::BeginTooltip();
    {
      std::array<char, 64> second{};
      if (bars) {
        std::array<char, 160> range{};
        const std::string_view from = FormatChartValue(options.x.unit, pX[point], buffer);
        const std::string_view to = FormatChartValue(options.x.unit, pX[point + 1], second);
        TextDim(FormatTo(std::span<char>(range), "{} to {}", from, to));
      } else {
        TextDim(FormatChartValue(options.x.unit, pX[point], buffer));
      }
      double total = 0.0;
      std::size_t nbShown = 0;
      for (std::size_t idx = 0; idx < series.size(); ++idx) {
        if (state.isHidden(idx)) {
          continue;
        }
        const float value = valueAt(idx, point);
        total += StackValue(value);
        ++nbShown;
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        DrawSwatch(ImGui::GetWindowDrawList(), ImVec2(pos.x, pos.y + (lineHeight * 0.15F)), swatch, series[idx].color);
        ImGui::Dummy(ImVec2(swatch, lineHeight));
        ImGui::SameLine();
        Text(series[idx].name);
        ImGui::SameLine();
        if (std::isnan(value)) {
          TextDim("-");
        } else {
          const std::string_view formatted = FormatChartValue(options.y.unit, value, buffer);
          Text(FormatTo(std::span<char>(second), "{}{}", formatted, options.valueSuffix));
        }
      }
      if (options.style == ChartStyle::stackedAreas && nbShown > 1) {
        const std::string_view formatted = FormatChartValue(options.y.unit, total, buffer);
        TextDim(FormatTo(std::span<char>(second), "Total {}{}", formatted, options.valueSuffix));
      }
    }
    ImGui::EndTooltip();
  } else {
    pDrawList->PopClipRect();
  }
  ImGui::PopID();
}

}  // namespace zalie::ui
