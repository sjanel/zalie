#pragma once

#include <imgui.h>

#include <amc/vector.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace zalie::ui {

// What the values of an axis measure: it chooses the round ticks and the labels.
enum class ChartUnit : std::uint8_t {
  count,
  // Durations: "500 us", "1.5 ms", "2 s".
  nanoseconds,
  bytes,
  // Elapsed time: "30 s", "2 min", "2:30".
  seconds,
  percent,
};

enum class ChartScale : std::uint8_t {
  linear,
  // Base 10 logarithm: values spanning several orders of magnitude (latencies). Values <= 0 are not drawn.
  log,
  // Percentiles in [0, 100) spaced by their nines: 90, 99 and 99.9 equally apart (latency percentile curves).
  nines,
};

enum class ChartStyle : std::uint8_t {
  // One line per series.
  lines,
  // The series stacked on each other, filled.
  stackedAreas,
  // One bar per value, from x[i] to x[i + 1]: 'x' has one more value than the series.
  bars,
};

// How the values falling into one pixel are summarized, when there are more points than pixels.
enum class ChartReduce : std::uint8_t { mean, max };

struct ChartSeries {
  std::string_view name;
  // One value per x (NaN: no value).
  std::span<const float> values;
  ImU32 color{0};
  ChartReduce reduce{ChartReduce::mean};
  // With stacked areas: a line over the stack (a target), not stacked and not in the total.
  bool overlay{false};
};

// A labelled vertical line (the percentiles of a histogram).
struct ChartMarker {
  std::string_view label;
  float x{0.0F};
  ImU32 color{0};
};

struct ChartAxis {
  ChartUnit unit{ChartUnit::count};
  ChartScale scale{ChartScale::linear};
};

struct ChartOptions {
  std::string_view title;
  // Appended to the values shown on hover ("/s").
  std::string_view valueSuffix;
  std::span<const ChartMarker> markers;
  ChartAxis x;
  ChartAxis y;
  // 0: 200 pixels at scale 1.
  float height{0.0F};
  ChartStyle style{ChartStyle::lines};
  // Dragging over a linear x axis zooms on a window of it, a double click shows everything again.
  bool zoomable{false};
};

// Interaction state of a chart and the points it draws, kept by its owner across frames.
struct ChartState {
  static constexpr std::size_t kMaxSeries = 32;

  // To call when the data changes: the points drawn are prepared again at the next frame.
  void invalidate() noexcept { ++revision; }

  void resetZoom() noexcept {
    zoomMin = 0.0;
    zoomMax = 0.0;
    ++revision;
  }

  [[nodiscard]] bool zoomed() const noexcept { return zoomMax > zoomMin; }

  [[nodiscard]] bool isHidden(std::size_t series) const noexcept {
    return series < kMaxSeries && ((hidden >> series) & 1U) != 0;
  }

  // The points drawn: 'stride' values of x, then 'stride' values per series, 'nbPoints' of them used. At most one per
  // pixel of the plot, prepared when the revision or the width change.
  amc::vector<float> points;
  // Screen positions of a line, reused from a frame to the next.
  amc::vector<ImVec2> scratch;
  // Window of x shown, everything when zoomMax <= zoomMin.
  double zoomMin{0.0};
  double zoomMax{0.0};
  std::uint64_t revision{1};
  std::uint64_t pointsRevision{0};
  float pointsWidth{0.0F};
  // Screen x where a zoom drag started, negative when none.
  float dragStartX{-1.0F};
  // Bit i: series i is hidden (clicked in the legend).
  std::uint32_t hidden{0};
  std::uint32_t nbPoints{0};
  std::uint32_t stride{0};
};

// Draws a chart of 'series' over 'x' (increasing), filling the available width: title, legend (a click hides or shows
// a series), axes with round ticks, the series and the markers. Hovering shows the values of every series at that x.
// A frame costs the same whatever the length of the series: see ChartState::points.
void Chart(const char* strId, std::span<const float> x, std::span<const ChartSeries> series, ChartState& state,
           const ChartOptions& options);

// A value as the axes and the tooltips of the charts show it: "1.2k", "850 us", "1.5 MB", "2:30", "99.9 %".
[[nodiscard]] std::string_view FormatChartValue(ChartUnit unit, double value, std::span<char> buffer);

}  // namespace zalie::ui
