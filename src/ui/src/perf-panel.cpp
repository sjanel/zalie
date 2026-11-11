// Performance tab: the run settings of a request (limits and parameter table), its plan compiled off the UI thread, the
// run and its dashboard (key numbers, charts and tables).

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include "zalie/collection.hpp"
#include "zalie/exchange.hpp"
#include "zalie/file-io.hpp"
#include "zalie/latency-histogram.hpp"
#include "zalie/load-plan.hpp"
#include "zalie/load-preview.hpp"
#include "zalie/load-report.hpp"
#include "zalie/load-runner.hpp"
#include "zalie/load-settings.hpp"
#include "zalie/method.hpp"
#include "zalie/rate-controller.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/request.hpp"
#include "zalie/resolved-request.hpp"
#include "zalie/settings.hpp"
#include "zalie/string-utils.hpp"
#include "zalie/ui/app.hpp"
#include "zalie/ui/assets.hpp"
#include "zalie/ui/chart.hpp"
#include "zalie/ui/icons.hpp"
#include "zalie/ui/perf-tab.hpp"
#include "zalie/ui/theme.hpp"
#include "zalie/ui/widgets.hpp"
#include "zalie/url.hpp"
#include "zalie/value-generators.hpp"
#include "zalie/variables.hpp"
#include "zalie/workspace.hpp"

namespace zalie::ui {

namespace {

constexpr std::size_t kErrorClass = static_cast<std::size_t>(LoadClass::error);
constexpr std::array<std::string_view, kNbLoadClasses> kClassNames{"2xx", "3xx", "4xx", "5xx", "Errors"};
constexpr std::array<std::string_view, kNbLoadErrorKinds> kErrorKindNames{
    "Connection", "TLS", "Timeout", "Closed", "Malformed", "Too large", "Other", "Invalid value"};
// Indexed by ValueSource.
constexpr std::array<const char*, 10> kSourceNames{"Fixed value", "List",          "Integer range", "Decimal range",
                                                   "Date range",  "Random string", "UUID",          "Timestamp",
                                                   "Data file",   "Not sent"};
constexpr std::array<const char*, 3> kOrderNames{"In order", "Random", "Shuffled"};
constexpr std::array<const char*, 3> kCombineNames{"Independent", "Together", "All combinations"};
constexpr std::array<const char*, 3> kTimestampNames{"Unix seconds", "Unix milliseconds", "ISO 8601"};
constexpr std::array<const char*, 3> kFileFormatNames{"Text, one value per line", "CSV with a header", "JSON Lines"};
constexpr std::array<const char*, PerfCharts::kNbLatencySeries> kLatencyNames{"p50", "p90", "p99", "max"};
// Indexed by RateState.
constexpr std::array<std::string_view, 6> kRateStates{"ramping up",
                                                      "probing by small steps",
                                                      "holding, the response times rise",
                                                      "backing off, the server is overloaded",
                                                      "limited by the connections, more of them would go faster",
                                                      "at its maximum"};

struct TimeUnit {
  const char* name;
  std::int64_t seconds;
};
constexpr std::array kDateSteps{TimeUnit{"seconds", 1}, TimeUnit{"minutes", 60}, TimeUnit{"hours", 3600},
                                TimeUnit{"days", 86400}};

// Bars of the latency histogram per power of ten.
constexpr double kBinsPerDecade = 20.0;

// Percentiles of the curve: 0 to 99.9, evenly spaced by their nines.
constexpr int kCurvePoints = 61;
constexpr double kCurvePointsPerNine = 20.0;

[[nodiscard]] bool HasOrder(ValueSource source) noexcept {
  return source == ValueSource::list || source == ValueSource::integerRange || source == ValueSource::decimalRange ||
         source == ValueSource::dateRange || source == ValueSource::dataFile;
}

[[nodiscard]] ImU32 Mix(ImU32 lhs, ImU32 rhs, float ratio) noexcept {
  const ImVec4 left = ImGui::ColorConvertU32ToFloat4(lhs);
  const ImVec4 right = ImGui::ColorConvertU32ToFloat4(rhs);
  return ImGui::ColorConvertFloat4ToU32(ImVec4(left.x + ((right.x - left.x) * ratio),
                                               left.y + ((right.y - left.y) * ratio),
                                               left.z + ((right.z - left.z) * ratio), left.w));
}

// The colors of the status pill of a response; errors darker than 5xx.
[[nodiscard]] ImU32 ClassColor(std::size_t outcome) noexcept {
  const Palette& pal = CurrentPalette();
  switch (static_cast<LoadClass>(outcome)) {
    case LoadClass::status2xx:
      return pal.success;
    case LoadClass::status3xx:
      return pal.info;
    case LoadClass::status4xx:
      return pal.warning;
    case LoadClass::status5xx:
      return pal.danger;
    case LoadClass::error:
      break;
  }
  return Mix(pal.danger, pal.text, 0.45F);
}

[[nodiscard]] ImU32 LatencyColor(std::size_t series) noexcept {
  const Palette& pal = CurrentPalette();
  static constexpr std::array<ImU32 Palette::*, PerfCharts::kNbLatencySeries> kColors{
      &Palette::info, &Palette::success, &Palette::warning, &Palette::danger};
  return pal.*kColors[series];
}

LoadSettings& EnsureLoad(Request& request) {
  if (!request.load) {
    request.load.emplace();
  }
  return *request.load;
}

const LoadSettings& LoadOf(const Request& request) noexcept {
  static const LoadSettings kDefaults;
  return request.load ? *request.load : kDefaults;
}

// The format of a data file from its extension.
[[nodiscard]] DataFileFormat FormatOfFile(const std::filesystem::path& path) {
  const std::filesystem::path extension = path.extension();
  if (extension == ".csv" || extension == ".CSV") {
    return DataFileFormat::csv;
  }
  if (extension == ".jsonl" || extension == ".ndjson" || extension == ".JSONL") {
    return DataFileFormat::jsonLines;
  }
  return DataFileFormat::text;
}

// What the values of a parameter are, in a few words.
std::string_view Summary(const LoadParameter& parameter, std::span<char> buffer) {
  switch (parameter.source) {
    case ValueSource::fixed:
      return parameter.fixedValue.empty() ? std::string_view{"(empty)"} : std::string_view(parameter.fixedValue);
    case ValueSource::list: {
      const std::string_view values = parameter.listValues;
      const std::string_view first = values.substr(0, values.find('\n'));
      return FormatTo(buffer, "{}{}", first, first.size() < values.size() ? ", ..." : "");
    }
    case ValueSource::integerRange:
      if (parameter.integerStep == 1) {
        return FormatTo(buffer, "{} to {}", parameter.integerFrom, parameter.integerTo);
      }
      return FormatTo(buffer, "{} to {} by {}", parameter.integerFrom, parameter.integerTo, parameter.integerStep);
    case ValueSource::decimalRange:
      return FormatTo(buffer, "{} to {} by {}", parameter.decimalFrom, parameter.decimalTo, parameter.decimalStep);
    case ValueSource::dateRange:
      return FormatTo(buffer, "{} to {}", std::string_view(parameter.dateFrom), std::string_view(parameter.dateTo));
    case ValueSource::randomString:
      return FormatTo(buffer, "{} random characters", parameter.randomLength);
    case ValueSource::uuid:
      return "a new UUID for each request";
    case ValueSource::timestamp:
      return kTimestampNames[static_cast<std::size_t>(parameter.timestampFormat)];
    case ValueSource::dataFile: {
      if (Trim(parameter.filePath).empty()) {
        return "no file chosen";
      }
      const std::string_view path = parameter.filePath;
      const std::string_view fileName = path.substr(path.find_last_of("/\\") + 1);
      if (parameter.fileColumn.empty()) {
        return fileName;
      }
      return FormatTo(buffer, "{}, column {}", fileName, std::string_view(parameter.fileColumn));
    }
    case ValueSource::omitted:
      return "not sent during the run";
  }
  return {};
}

// The saved parameter of 'row', created if it has none: a query parameter starts from the value of the request
// (already in the collection file), a variable from nothing (its value may be a secret of the environment).
LoadParameter& MaterializeParameter(Request& request, LoadRow& row) {
  if (row.parameter == LoadRow::kNoParameter) {
    LoadParameter parameter;
    parameter.name = row.name;
    parameter.queryParameter = row.queryParameter;
    if (row.queryParameter) {
      parameter.fixedValue = row.defaultValue;
      parameter.listValues = row.defaultValue;
    }
    EnsureLoad(request).parameters.push_back(std::move(parameter));
    row.parameter = request.load->parameters.size() - 1;
  }
  return request.load->parameters[row.parameter];
}

[[nodiscard]] std::int64_t NowUnixSeconds() noexcept {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// A framed key number of the dashboard.
void Tile(std::string_view label, std::string_view value, std::string_view detail, ImU32 valueColor, ImVec2 size) {
  const Palette& pal = CurrentPalette();
  const float scale = ImGui::GetStyle().FontScaleDpi;
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  ImDrawList* pDrawList = ImGui::GetWindowDrawList();
  pDrawList->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), pal.surface, 6.0F * scale);
  pDrawList->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), pal.border, 6.0F * scale);
  const float pad = 10.0F * scale;
  pDrawList->PushClipRect(pos, ImVec2(pos.x + size.x - (4.0F * scale), pos.y + size.y), true);
  float y = pos.y + (8.0F * scale);
  {
    ScopedFont small(GetFonts().pUi, Fonts::kUiSize * 0.8F);
    pDrawList->AddText(ImVec2(pos.x + pad, y), pal.textDim, label.data(), label.data() + label.size());
    y += ImGui::GetFontSize() + (2.0F * scale);
  }
  {
    ScopedFont title(GetFonts().pUi, Fonts::kTitleSize * 1.15F);
    pDrawList->AddText(ImVec2(pos.x + pad, y), valueColor, value.data(), value.data() + value.size());
    y += ImGui::GetFontSize() + (2.0F * scale);
  }
  {
    ScopedFont small(GetFonts().pUi, Fonts::kUiSize * 0.8F);
    pDrawList->AddText(ImVec2(pos.x + pad, y), pal.textDim, detail.data(), detail.data() + detail.size());
  }
  pDrawList->PopClipRect();
  ImGui::Dummy(size);
}

void PrepareCharts(PerfCharts& charts, const LoadSnapshot& snapshot) {
  const auto nbPoints = static_cast<std::uint32_t>(snapshot.series.size());
  charts.times.resize(nbPoints);
  charts.rates.resize(static_cast<std::uint32_t>(kNbLoadClasses + 1) * nbPoints);
  charts.latencies.resize(PerfCharts::kNbLatencySeries * nbPoints);
  for (std::uint32_t idx = 0; idx < nbPoints; ++idx) {
    const LoadSecond& point = snapshot.series[idx];
    const float seconds = point.seconds > 0.0F ? point.seconds : 1.0F;
    charts.times[idx] = static_cast<float>(snapshot.firstSecond + idx) + seconds;
    for (std::uint32_t outcome = 0; outcome < kNbLoadClasses; ++outcome) {
      charts.rates[(outcome * nbPoints) + idx] = static_cast<float>(point.counts[outcome]) / seconds;
    }
    charts.rates[(kNbLoadClasses * nbPoints) + idx] =
        point.targetRate != 0 ? static_cast<float>(point.targetRate) : std::numeric_limits<float>::quiet_NaN();
    // Errors have no response time.
    const bool responses = point.requests() != point.counts[kErrorClass];
    const std::array<std::uint64_t, PerfCharts::kNbLatencySeries> values{point.p50Ns, point.p90Ns, point.p99Ns,
                                                                         point.maxNs};
    for (std::uint32_t series = 0; series < PerfCharts::kNbLatencySeries; ++series) {
      charts.latencies[(series * nbPoints) + idx] =
          responses ? static_cast<float>(values[series]) : std::numeric_limits<float>::quiet_NaN();
    }
  }

  // Histogram: the buckets of the latency histogram are log-linear (their width doubles at each power of two), the
  // axis is logarithmic. Their counts are spread over bins of equal width on the axis, so that the bars compare.
  const LatencyHistogram& histogram = snapshot.totalHistogram;
  charts.bucketEdges.clear();
  charts.bucketCounts.clear();
  int first = 0;
  int last = LatencyHistogram::kNbBuckets - 1;
  while (first <= last && histogram.bucketCount(first) == 0) {
    ++first;
  }
  while (last >= first && histogram.bucketCount(last) == 0) {
    --last;
  }
  if (first <= last) {
    const auto lowerOf = [](int idx) {
      return std::max(1.0, static_cast<double>(LatencyHistogram::BucketLowerNs(idx)));
    };
    const auto upperOf = [](int idx) {
      return static_cast<double>(LatencyHistogram::BucketLowerNs(idx) + LatencyHistogram::BucketWidthNs(idx));
    };
    const double logFirst = std::log10(lowerOf(first));
    const double logLast = std::log10(upperOf(last));
    const auto nbBins = static_cast<std::uint32_t>(std::max(1.0, std::ceil((logLast - logFirst) * kBinsPerDecade)));
    const double binWidth = (logLast - logFirst) / nbBins;
    for (std::uint32_t bin = 0; bin <= nbBins; ++bin) {
      charts.bucketEdges.push_back(static_cast<float>(std::pow(10.0, logFirst + (bin * binWidth))));
    }
    charts.bucketCounts.resize(nbBins, 0.0F);
    for (int idx = first; idx <= last; ++idx) {
      const std::uint64_t count = histogram.bucketCount(idx);
      if (count == 0) {
        continue;
      }
      // In proportion of the overlap of the bucket and each bin, in log space.
      const double low = std::log10(lowerOf(idx));
      const double high = std::max(low + 1e-12, std::log10(upperOf(idx)));
      auto bin = static_cast<std::uint32_t>(std::clamp((low - logFirst) / binWidth, 0.0, nbBins - 1.0));
      for (; bin < nbBins; ++bin) {
        const double binLow = logFirst + (bin * binWidth);
        if (binLow >= high) {
          break;
        }
        const double overlap = std::min(high, binLow + binWidth) - std::max(low, binLow);
        if (overlap > 0.0) {
          charts.bucketCounts[bin] += static_cast<float>(static_cast<double>(count) * overlap / (high - low));
        }
      }
    }
  }

  charts.percentiles.clear();
  charts.percentileNs.clear();
  if (histogram.count() != 0) {
    for (int point = 0; point < kCurvePoints; ++point) {
      const double percentile = 100.0 * (1.0 - std::pow(10.0, -point / kCurvePointsPerNine));
      charts.percentiles.push_back(static_cast<float>(percentile));
      charts.percentileNs.push_back(static_cast<float>(histogram.percentileNs(percentile)));
    }
  }
  for (ChartState* pState :
       {&charts.rateChart, &charts.latencyChart, &charts.histogramChart, &charts.percentileChart}) {
    pState->invalidate();
  }
  charts.pSnapshot = &snapshot;
}

// A combo over names indexed by an enum. Returns true when changed.
template <class Enum, std::size_t N>
bool EnumCombo(const char* label, Enum& value, const std::array<const char*, N>& names, float width) {
  ImGui::SetNextItemWidth(width);
  bool changed = false;
  if (ImGui::BeginCombo(label, names[static_cast<std::size_t>(value)])) {
    for (std::size_t idx = 0; idx < N; ++idx) {
      if (ImGui::Selectable(names[idx], idx == static_cast<std::size_t>(value))) {
        value = static_cast<Enum>(idx);
        changed = true;
      }
    }
    ImGui::EndCombo();
  }
  return changed;
}

// The label of an input on its left, the input after it.
void InlineLabel(std::string_view label) {
  ImGui::AlignTextToFramePadding();
  TextDim(label);
  ImGui::SameLine();
}

// Starts the next item of a flow layout on the same line when 'width' fits, on the next line otherwise.
void Flow(float width, bool first) {
  if (!first) {
    ImGui::SameLine(0.0F, ImGui::GetStyle().ItemSpacing.x * 3.0F);
    if (ImGui::GetContentRegionAvail().x < width) {
      ImGui::NewLine();
    }
  }
}

}  // namespace

// --------------------------------------------------------------------------------------------------------------- //
// Tabs and runs

void App::stopRunner(std::unique_ptr<LoadRunner> runner) {
  runner->stop();
  if (runner->snapshot()->phase != LoadPhase::finished) {
    // Its requests in flight end at their next bytes or their timeout: released later, never waited for here.
    _stoppingRunners.push_back(std::move(runner));
  }
}

void App::pollRuns() {
  for (auto& pTab : _tabs) {
    if (!pTab->perf || !pTab->perf->runner) {
      continue;
    }
    PerfTab& perf = *pTab->perf;
    perf.snapshot = perf.runner->snapshot();
    if (perf.snapshot->phase == LoadPhase::finished) {
      perf.runner.reset();  // its threads are over
      if (!perf.snapshot->setupError.empty()) {
        toast(Concat({"The run could not start: ", perf.snapshot->setupError}), true);
      }
    }
  }
  if (!_stoppingRunners.empty()) {
    const auto finished = std::ranges::remove_if(
        _stoppingRunners, [](const auto& pRunner) { return pRunner->snapshot()->phase == LoadPhase::finished; });
    _stoppingRunners.erase(finished.begin(), finished.end());
  }
}

void App::showLoadSnapshot(RequestTab& tab, std::shared_ptr<const LoadSnapshot> snapshot) {
  assert(tab.perf);
  PerfTab& perf = *tab.perf;
  if (perf.runner) {
    stopRunner(std::move(perf.runner));
  }
  if (const Request* pRequest = requestOf(tab); pRequest != nullptr) {
    perf.runSettings = LoadOf(*pRequest);
    perf.runName = pRequest->name;
    perf.runUrl = pRequest->url;
    perf.runMethod = pRequest->method;
  }
  perf.runDurationMs = perf.runSettings.durationMs;
  perf.runMaxRequests = perf.runSettings.maxRequests;
  perf.startedAt = std::chrono::system_clock::now();
  perf.snapshot = std::move(snapshot);
  ++_revision;
}

bool App::exportRun(RequestTab& tab, const std::filesystem::path& path) {
  if (!tab.perf || !tab.perf->snapshot) {
    toast("No run to export", true);
    return false;
  }
  const PerfTab& perf = *tab.perf;
  const auto exported = ExportLoadReport(
      path, *perf.snapshot, perf.runSettings,
      {.requestName = perf.runName, .url = perf.runUrl, .startedAt = perf.startedAt, .method = perf.runMethod});
  if (!exported) {
    toast(exported.error(), true);
    return false;
  }
  toast(Concat({"Run exported to ", PathToUtf8(path)}));
  return true;
}

std::filesystem::path App::dataBaseDirectory(const RequestTab& tab) {
  const OpenCollection* pCollection = collectionOf(tab);
  if (pCollection != nullptr && !pCollection->path.empty()) {
    return pCollection->path.parent_path();
  }
  std::error_code errc;
  std::filesystem::path current = std::filesystem::current_path(errc);
  return errc ? std::filesystem::path{} : current;
}

void App::listPerfRows(RequestTab& tab, const Request& request) {
  PerfTab& perf = *tab.perf;
  if (perf.rowsRevision == _revision) {
    return;
  }
  perf.rowsRevision = _revision;
  const OpenCollection* pCollection = collectionOf(tab);
  static const Collection kNoCollection;
  const Collection& collection = pCollection == nullptr ? kNoCollection : pCollection->collection;
  const VariableScope scope = MakeScope(collection, pCollection == nullptr ? -1 : pCollection->activeEnvironment);
  ListLoadRows(request, collection, scope, perf.rows);
  if (perf.selectedRow >= perf.rows.size()) {
    perf.selectedRow = perf.rows.empty() ? 0 : perf.rows.size() - 1;
  }
}

void App::submitPlan(RequestTab& tab, const Request& request) {
  PerfTab& perf = *tab.perf;
  PlanCompiler::Job job;
  job.request = request;
  LoadSettings& settings = EnsureLoad(job.request);
  if (settings.seed == 0) {
    settings.seed = perf.previewSeed;
  }
  if (const OpenCollection* pCollection = collectionOf(tab); pCollection != nullptr) {
    // Only what the request uses: not the whole tree of the collection.
    const Collection& collection = pCollection->collection;
    job.context.variables = collection.variables;
    job.context.headers = collection.headers;
    job.context.auth = collection.auth;
    const std::int32_t environment = pCollection->activeEnvironment;
    if (environment >= 0 && std::cmp_less(environment, collection.environments.size())) {
      job.context.environments.push_back(collection.environments[static_cast<std::uint32_t>(environment)]);
    }
  }
  job.baseDirectory = dataBaseDirectory(tab);
  job.slot = perf.slot;
  job.revision = _revision;
  _planCompiler.submit(std::move(job));
  perf.submittedRevision = _revision;
}

void App::updatePerformanceTab(RequestTab& tab, Request& request) {
  PerfTab& perf = *tab.perf;
  listPerfRows(tab, request);
  if (perf.submittedRevision != _revision) {
    submitPlan(tab, request);
  }
  {
    std::scoped_lock lock(perf.slot->mutex);
    if (perf.slot->revision != perf.previewRevision) {
      perf.preview = perf.slot->preview;
      perf.previewRevision = perf.slot->revision;
    }
  }
  if (!perf.runRequested || perf.running() || !perf.planCurrent()) {
    return;
  }
  perf.runRequested = false;
  const LoadPreview& preview = *perf.preview;
  if (!preview.error.empty()) {
    toast(preview.error, true);
    return;
  }
  if (!preview.plan->valid()) {
    toast(Concat({"The run cannot start: ", preview.plan->issues().front().message}), true);
    return;
  }
  if (!MethodIsSafe(preview.plan->method()) || !IsLocalHost(preview.plan->host())) {
    perf.confirmRequested = true;
    return;
  }
  startRun(tab, request);
}

void App::startRun(RequestTab& tab, const Request& request) {
  PerfTab& perf = *tab.perf;
  assert(perf.planCurrent() && !perf.running());
  const std::shared_ptr<const LoadPlan>& plan = perf.preview->plan;
  const LoadSettings& settings = LoadOf(request);
  LoadOptions options = MakeLoadOptions(settings);
  options.wakeFn = _executor.options().wakeFn;
  options.pWakeUserData = _executor.options().pWakeUserData;
  options.maxBodyBytes = static_cast<std::size_t>(_workspace.settings().maxBodyMiB) << 20U;
  options.maxConnections = _workspace.settings().maxLoadConnections;

  perf.runSettings = settings;
  perf.runSettings.seed = plan->seed();
  perf.runName = request.name;
  perf.runUrl = request.url;
  perf.runMethod = request.method;
  perf.runMaxRequests = PlannedRequests(*plan, options);
  perf.runDurationMs = options.durationMs;
  perf.startedAt = std::chrono::system_clock::now();
  perf.runner = std::make_unique<LoadRunner>(plan, options);
  perf.snapshot = perf.runner->snapshot();
  for (ChartState* pState :
       {&perf.charts.rateChart, &perf.charts.latencyChart, &perf.charts.histogramChart, &perf.charts.percentileChart}) {
    pState->resetZoom();
  }
  if (settings.seed == 0) {
    // The next run gets a new seed: its preview is compiled again.
    perf.previewSeed = NewId();
    perf.submittedRevision = 0;
  }
}

// --------------------------------------------------------------------------------------------------------------- //
// Drawing

void App::drawPerformancePanel(RequestTab& tab, Request& request) {
  updatePerformanceTab(tab, request);
  drawPerfHeader(tab, request);

  // The run settings above, the dashboard below, like the request editor and its response.
  Settings& settings = _workspace.settings();
  const float scale = ImGui::GetStyle().FontScaleDpi;
  const float splitter = 8.0F * scale;
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  const float minHeight = 110.0F * scale;
  float settingsHeight =
      std::clamp(avail.y * settings.requestEditorRatio, minHeight, std::max(minHeight, avail.y - minHeight));
  ImGui::BeginChild("##perfSettings", ImVec2(0, settingsHeight));
  drawPerfSettings(tab, request);
  drawPerfParameters(tab, request);
  drawPerfRowEditor(tab, request);
  ImGui::EndChild();
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
  if (Splitter("##perfSplit", false, splitter, settingsHeight, minHeight, avail.y - minHeight,
               ImGui::GetContentRegionAvail().x)) {
    settings.requestEditorRatio = settingsHeight / std::max(1.0F, avail.y);
  }
  ImGui::PopStyleVar();
  ImGui::BeginChild("##perfDashboard", ImVec2(0, 0));
  drawPerfDashboard(*tab.perf);
  ImGui::EndChild();
  drawPerfDialogs(tab, request);
}

void App::drawPerfHeader(RequestTab& tab, const Request& request) {
  PerfTab& perf = *tab.perf;
  const Palette& pal = CurrentPalette();
  const ImGuiStyle& style = ImGui::GetStyle();
  const float scale = style.FontScaleDpi;
  const float buttonWidth = 140.0F * scale;
  const float exportWidth = 110.0F * scale;
  // The Run button stands out: taller than the other items of the row, which are centered on it.
  const float buttonHeight = std::round(ImGui::GetFrameHeight() * 1.35F);
  const float rowY = ImGui::GetCursorPosY();
  const auto buttonLabel = [](Icon icon, std::string_view text, ImU32 color) {
    ScopedFont font(GetFonts().pUi, Fonts::kUiSize * 1.15F);
    DrawItemIconLabel(icon, ImGui::GetFontSize() * 1.5F, text, color);
  };

  ImGui::SetCursorPosY(rowY + std::floor((buttonHeight - ImGui::GetFrameHeight()) * 0.5F));
  ImGui::AlignTextToFramePadding();
  MethodLabel(request.method, false);
  ImGui::SameLine();
  {
    // The URL as written, cut at the buttons.
    ScopedFont mono(GetFonts().pMono, Fonts::kMonoSize);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float width =
        std::max(10.0F, ImGui::GetContentRegionAvail().x - buttonWidth - exportWidth - (style.ItemSpacing.x * 3));
    const std::string_view url = request.url;
    const float textY = pos.y + style.FramePadding.y;
    ImDrawList* pDrawList = ImGui::GetWindowDrawList();
    pDrawList->PushClipRect(pos, ImVec2(pos.x + width, pos.y + ImGui::GetFrameHeight()), true);
    pDrawList->AddText(ImVec2(pos.x, textY), pal.textDim, url.data(), url.data() + url.size());
    pDrawList->PopClipRect();
    ImGui::Dummy(ImVec2(width, ImGui::GetFrameHeight()));
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && !url.empty()) {
      ImGui::SetTooltip("%.*s", static_cast<int>(url.size()), url.data());
    }
  }
  // SameLine() brings the cursor back to the centered line, the buttons take the whole height.
  ImGui::SameLine();
  ImGui::SetCursorPosY(rowY);
  if (perf.running()) {
    ImGui::PushStyleColor(ImGuiCol_Button, WithAlpha(pal.danger, 0.15F));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, WithAlpha(pal.danger, 0.3F));
    if (ImGui::Button("##perfStop", ImVec2(buttonWidth, buttonHeight))) {
      perf.runner->stop();
    }
    ImGui::PopStyleColor(2);
    buttonLabel(Icon::stop, "Stop", pal.danger);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Stop the run (Esc): the requests in flight are not counted");
    }
  } else {
    // "Run", with Zalie's paw prints.
    if (PrimaryButton("##perfRun", ImVec2(buttonWidth, buttonHeight))) {
      perf.runRequested = true;
    }
    buttonLabel(Icon::pawPrints, perf.runRequested ? "Preparing..." : "Run", pal.onAccent);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Run (Ctrl+Enter)");
    }
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(!perf.snapshot || perf.running());
  if (ImGui::Button("Export JSON...", ImVec2(exportWidth, 0))) {
    perf.exportPath =
        PathToUtf8(_workspace.home() / "runs" / PathFromUtf8(LoadReportFileName(perf.runName, perf.startedAt)));
    ImGui::OpenPopup("Export the run###perfExport");
  }
  ImGui::EndDisabled();
}

void App::drawPerfSettings(RequestTab& tab, Request& request) {
  const float scale = ImGui::GetStyle().FontScaleDpi;
  bool changed = false;
  const float numberWidth = 80.0F * scale;
  const std::uint32_t vCpus = std::max(1U, std::thread::hardware_concurrency());
  const std::uint32_t maxConnections =
      _workspace.settings().maxLoadConnections != 0 ? _workspace.settings().maxLoadConnections : vCpus;
  std::array<char, 96> buffer{};
  // Each input edits a copy of its value: the settings are created in the request only when a value changes.
  const auto integer = [&](const char* label, const char* strId, auto LoadSettings::* pMember, ImGuiDataType type,
                           const char* tooltip) {
    InlineLabel(label);
    auto number = LoadOf(request).*pMember;
    ImGui::SetNextItemWidth(numberWidth);
    if (ImGui::InputScalar(strId, type, &number)) {
      EnsureLoad(request).*pMember = number;
      changed = true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("%s", tooltip);
    }
  };
  const auto seconds = [&](const char* label, const char* strId, std::uint32_t LoadSettings::* pMember,
                           const char* tooltip) {
    InlineLabel(label);
    float value = static_cast<float>(LoadOf(request).*pMember) / 1000.0F;
    ImGui::SetNextItemWidth(numberWidth);
    if (ImGui::InputFloat(strId, &value, 0.0F, 0.0F, "%.1f s")) {
      EnsureLoad(request).*pMember =
          static_cast<std::uint32_t>(std::lround(std::clamp(value, 0.0F, 86400.0F) * 1000.0F));
      changed = true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("%s", tooltip);
    }
  };

  SectionTitle("RUN");
  integer("Connections", "##connections", &LoadSettings::connections, ImGuiDataType_U32,
          "Each connection is a thread, sending its next request when the previous one is answered. 0: the number "
          "of vCPUs. The limit is in the settings.");
  if (LoadOf(request).connections > maxConnections) {
    EnsureLoad(request).connections = maxConnections;
    changed = true;
  }
  if (LoadOf(request).connections == 0) {
    ImGui::SameLine();
    TextDim(FormatTo(buffer, "auto: {}", std::min(vCpus, maxConnections)));
  }
  Flow(numberWidth * 2.0F, false);
  seconds("Duration", "##duration", &LoadSettings::durationMs,
          "The run ends after this time, warm-up included. 0: no time limit.");
  Flow(numberWidth * 2.0F, false);
  seconds("Warm-up", "##warmup", &LoadSettings::warmupMs,
          "The requests started during the first seconds are sent, but not counted.");
  Flow(numberWidth * 2.0F, false);
  integer("Max requests", "##maxRequests", &LoadSettings::maxRequests, ImGuiDataType_U64,
          "The run ends after this number of requests, warm-up included. 0: no limit.");
  Flow(numberWidth * 2.0F, false);
  const bool adaptive = LoadOf(request).adaptiveRate;
  integer("Max req/s", "##maxRate", &LoadSettings::maxRequestsPerSecond, ImGuiDataType_U32,
          adaptive ? "The adaptive rate never goes above this. 0: no maximum."
                   : "The requests never go faster, whatever the number of connections. 0: no limit.");
  Flow(numberWidth * 2.0F, false);
  {
    bool adaptiveRate = adaptive;
    if (ImGui::Checkbox("Adaptive rate", &adaptiveRate)) {
      EnsureLoad(request).adaptiveRate = adaptiveRate;
      changed = true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip(
          "The rate starts at Min req/s and adapts to what the server sustains, about every 2 seconds:\n"
          "it doubles while the response times stay close to their baseline, then rises by small steps,\n"
          "holds when they rise, and backs off when requests fail (5xx, 429, no response) or the response\n"
          "times explode. The connections still bound it: give it enough of them.");
    }
  }
  if (adaptive) {
    Flow(numberWidth * 2.0F, false);
    integer("Min req/s", "##minRate", &LoadSettings::minRequestsPerSecond, ImGuiDataType_U32,
            "The adaptive rate starts here, during the warm-up too, and never goes below: a rate the server surely "
            "sustains. 0: 10.");
    Flow(numberWidth * 2.0F, false);
    InlineLabel("Max p99");
    double maxP99Ms = LoadOf(request).maxP99Ms;
    ImGui::SetNextItemWidth(numberWidth);
    if (ImGui::InputDouble("##maxP99", &maxP99Ms, 0.0, 0.0, "%.1f ms")) {
      EnsureLoad(request).maxP99Ms = std::clamp(maxP99Ms, 0.0, 3'600'000.0);
      changed = true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip(
          "The adaptive rate backs off when the p99 of the response times exceeds this, and holds close to it. 0: no "
          "cap, the baseline only.");
    }
  }
  Flow(numberWidth * 1.5F, false);
  {
    bool onePass = LoadOf(request).onePass;
    if (ImGui::Checkbox("One pass", &onePass)) {
      EnsureLoad(request).onePass = onePass;
      changed = true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("The run also ends once every value of the parameters has been sent.");
    }
  }
  Flow(numberWidth * 3.0F, false);
  {
    InlineLabel("Combine");
    CombineMode combine = LoadOf(request).combine;
    if (EnumCombo("##combine", combine, kCombineNames, numberWidth * 2.0F)) {
      EnsureLoad(request).combine = combine;
      changed = true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip(
          "Independent: each parameter advances on its own.\n"
          "Together: paired by position, like the columns of a file.\n"
          "All combinations: every combination, the last parameter advancing first.\n"
          "Together and all combinations follow the order of the first parameter.");
    }
  }
  Flow(numberWidth * 2.5F, false);
  integer("Seed", "##seed", &LoadSettings::seed, ImGuiDataType_U64,
          "Equal seeds send equal requests (timestamps aside). 0: a new seed for each run.");
  if (changed) {
    touch(collectionOf(tab));
  }
  drawPerfPlan(tab, request);
}

void App::drawPerfPlan(RequestTab& tab, const Request& request) {
  const PerfTab& perf = *tab.perf;
  const Palette& pal = CurrentPalette();
  std::array<char, 160> buffer{};
  // What the run will send.
  if (!perf.preview) {
    TextDim("Preparing the run...");
    return;
  }
  const LoadPreview& preview = *perf.preview;
  const LoadPlan& plan = *preview.plan;
  if (plan.valid() && preview.error.empty()) {
    LoadOptions options = MakeLoadOptions(LoadOf(request));
    options.maxConnections = _workspace.settings().maxLoadConnections;
    const std::uint64_t planned = PlannedRequests(plan, options);
    const std::uint32_t connections = LoadConnections(options);
    std::array<char, 64> pass{};
    const std::string_view passText = plan.passLength() == 0
                                          ? std::string_view{}
                                          : FormatTo(pass, "  \xC2\xB7  one pass: {} requests", plan.passLength());
    std::array<char, 64> rate{};
    std::string_view rateText;
    if (options.adaptiveRate) {
      const double start = RateController::StartRate(options.minRequestsPerSecond, options.maxRequestsPerSecond);
      rateText = options.maxRequestsPerSecond != 0
                     ? FormatTo(rate, "  \xC2\xB7  adaptive rate {:g} to {} req/s", start, options.maxRequestsPerSecond)
                     : FormatTo(rate, "  \xC2\xB7  adaptive rate from {:g} req/s", start);
    }
    if (planned != 0) {
      TextDim(FormatTo(buffer, "Planned: {} requests on {} connections{}{}", planned, connections, rateText, passText));
    } else if (options.durationMs != 0) {
      TextDim(FormatTo(buffer, "Planned: {:g} s on {} connections{}{}", options.durationMs / 1000.0, connections,
                       rateText, passText));
    } else {
      TextColored(pal.warning, "No limit: the run goes on until you stop it.");
    }
  }
  for (const LoadPlanIssue& issue : plan.issues()) {
    if (issue.parameter != LoadPlanIssue::kNoParameter) {
      const auto& parameters = LoadOf(request).parameters;
      const std::string_view name =
          issue.parameter < parameters.size() ? std::string_view(parameters[issue.parameter].name) : std::string_view{};
      TextColored(pal.warning, FormatTo(buffer, "{}: {}", name, std::string_view(issue.message)));
    } else {
      TextColored(pal.warning, issue.message);
    }
  }
  if (!preview.error.empty()) {
    TextColored(pal.danger, preview.error);
  }
  for (const RawChars32& warning : plan.warnings()) {
    TextDim(warning);
  }
}

void App::drawPerfParameters(RequestTab& tab, Request& request) {
  PerfTab& perf = *tab.perf;
  listPerfRows(tab, request);
  const Palette& pal = CurrentPalette();
  const float scale = ImGui::GetStyle().FontScaleDpi;
  OpenCollection* pCollection = collectionOf(tab);
  ImGui::Spacing();
  SectionTitle("PARAMETERS");
  if (perf.rows.empty()) {
    TextDim("No {{variable}} nor query parameter to vary: every request is the same.");
    if (!perf.droppedPath.empty()) {
      importPath(PathFromUtf8(perf.droppedPath));
      perf.droppedPath.clear();
    }
    drawPerfAddQuery(tab, request);
    return;
  }
  const LoadPreview* pPreview = perf.preview.get();
  std::array<char, 128> buffer{};
  std::uint32_t dropTarget = LoadRow::kNoParameter;
  bool structural = false;
  static constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                            ImGuiTableFlags_Resizable | ImGuiTableFlags_NoSavedSettings;
  if (ImGui::BeginTable("##parameters", 7, kFlags)) {
    ImGui::TableSetupColumn("Parameter", ImGuiTableColumnFlags_WidthFixed, 150.0F * scale);
    ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed, 130.0F * scale);
    ImGui::TableSetupColumn("Values", ImGuiTableColumnFlags_WidthStretch, 1.0F);
    ImGui::TableSetupColumn("Order", ImGuiTableColumnFlags_WidthFixed, 100.0F * scale);
    ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed, 80.0F * scale);
    ImGui::TableSetupColumn("Samples", ImGuiTableColumnFlags_WidthStretch, 1.2F);
    ImGui::TableSetupColumn("##rowButtons", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
    ImGui::TableHeadersRow();
    for (std::uint32_t idx = 0; idx < perf.rows.size() && !structural; ++idx) {
      LoadRow& row = perf.rows[idx];
      LoadParameter* pParameter =
          row.parameter == LoadRow::kNoParameter || !request.load ? nullptr : &request.load->parameters[row.parameter];
      ImGui::PushID(static_cast<int>(idx));
      ImGui::TableNextRow();

      // Name: selects the row.
      ImGui::TableSetColumnIndex(0);
      if (ImGui::Selectable("##row", idx == perf.selectedRow,
                            ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
                            ImVec2(0, ImGui::GetFrameHeight()))) {
        perf.selectedRow = idx;
      }
      // The row spans every column: not clipped by the first one (the window is, by IsWindowHovered).
      if (!perf.droppedPath.empty() && ImGui::IsWindowHovered() &&
          ImGui::IsMouseHoveringRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), false)) {
        dropTarget = idx;
      }
      bool edited = false;
      bool hasIssue = false;
      if (pPreview != nullptr && row.parameter != LoadRow::kNoParameter) {
        hasIssue = std::ranges::any_of(pPreview->plan->issues(),
                                       [&row](const LoadPlanIssue& issue) { return issue.parameter == row.parameter; });
      }
      ImGui::SameLine();
      const bool omitted = pParameter != nullptr && pParameter->source == ValueSource::omitted;
      if (row.added && pParameter != nullptr) {
        // Added for the run: its key is edited here.
        if (hasIssue) {
          ImGui::PushStyleColor(ImGuiCol_Text, pal.warning);
        }
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("(added)").x - (4.0F * scale));
        if (perf.focusAddedName && idx == perf.selectedRow) {
          ImGui::SetKeyboardFocusHere();
          perf.focusAddedName = false;
        }
        edited |= InputText("##name", pParameter->name, 0, "key");
        if (hasIssue) {
          ImGui::PopStyleColor();
        }
        ImGui::SameLine(0, 4.0F * scale);
        ImGui::AlignTextToFramePadding();
        TextDim("(added)");
      } else {
        ImGui::AlignTextToFramePadding();
        ImU32 nameColor = pal.text;
        if (row.unused || omitted) {
          nameColor = pal.textDim;
        } else if (hasIssue || (row.undefined && pParameter == nullptr)) {
          nameColor = pal.warning;
        }
        TextColored(nameColor, row.name);
        if (row.queryParameter) {
          ImGui::SameLine(0, 4.0F * scale);
          TextDim("(query)");
        }
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        const char* help = "A {{variable}} of the request";
        if (row.unused) {
          help = "Not used by the request any more: choose Default to remove it";
        } else if (row.added) {
          help = "A query parameter added for the run only: the request does not have it";
        } else if (omitted) {
          help = "A query parameter left out of the run: a normal Fetch keeps sending it";
        } else if (row.queryParameter) {
          help = "A query parameter of the request: varied for the run only, a normal Fetch keeps sending its value";
        }
        ImGui::SetTooltip("%s", help);
      }

      // Source.
      ImGui::TableSetColumnIndex(1);
      ImGui::SetNextItemWidth(-FLT_MIN);
      const char* sourceName =
          pParameter == nullptr ? "Default" : kSourceNames[static_cast<std::size_t>(pParameter->source)];
      // Large: the default height scrolls before the last sources.
      if (ImGui::BeginCombo("##source", sourceName, ImGuiComboFlags_HeightLarge)) {
        // An added query parameter has no value in the request to fall back to: removed by its cross.
        if (!row.added && ImGui::Selectable("Default", pParameter == nullptr) && pParameter != nullptr) {
          request.load->parameters.erase(request.load->parameters.begin() + row.parameter);
          pParameter = nullptr;
          structural = true;
        }
        if (!row.added && ImGui::IsItemHovered()) {
          ImGui::SetTooltip("%s", row.queryParameter ? "The value written in the request"
                                                     : "The value of the environment or of the collection");
        }
        for (std::size_t source = 0; source < kSourceNames.size(); ++source) {
          if (static_cast<ValueSource>(source) == ValueSource::omitted && (!row.queryParameter || row.added)) {
            continue;  // only a query parameter of the request can be left out
          }
          const bool selected = pParameter != nullptr && static_cast<std::size_t>(pParameter->source) == source;
          if (!ImGui::Selectable(kSourceNames[source], selected) || selected) {
            continue;
          }
          structural |= row.parameter == LoadRow::kNoParameter;
          pParameter = &MaterializeParameter(request, row);
          pParameter->source = static_cast<ValueSource>(source);
          // A range starts valid, from what the row sends now when it fits.
          std::string_view current;
          if (row.added) {
            current = pParameter->fixedValue;
          } else if (row.queryParameter) {
            current = row.defaultValue;
          }
          SeedRange(*pParameter, current, NowUnixSeconds());
          perf.selectedRow = idx;
          edited = true;
        }
        ImGui::EndCombo();
      }

      // Values, order, count and samples.
      ImGui::TableSetColumnIndex(2);
      ImGui::AlignTextToFramePadding();
      if (pParameter == nullptr) {
        if (row.undefined) {
          TextColored(pal.warning, "undefined");
        } else {
          TextDim(row.defaultValue);
        }
      } else if (pParameter->source == ValueSource::omitted) {
        TextDim(Summary(*pParameter, buffer));
      } else {
        Text(Summary(*pParameter, buffer));
      }
      ImGui::TableSetColumnIndex(3);
      if (pParameter != nullptr && !structural && HasOrder(pParameter->source)) {
        if (EnumCombo("##order", pParameter->order, kOrderNames, -FLT_MIN)) {
          touch(pCollection);
        }
      } else {
        ImGui::AlignTextToFramePadding();
        TextDim("-");
      }
      ImGui::TableSetColumnIndex(4);
      ImGui::AlignTextToFramePadding();
      if (pParameter == nullptr) {
        TextDim("1");
      } else if (pParameter->source == ValueSource::omitted) {
        TextDim("-");
      } else if (pPreview != nullptr && row.parameter < pPreview->samples.size()) {
        const std::uint64_t count = pPreview->plan->valueCount(row.parameter);
        if (count == 0) {
          TextDim("unbounded");
        } else {
          Text(FormatChartValue(ChartUnit::count, static_cast<double>(count), buffer));
        }
      }
      ImGui::TableSetColumnIndex(5);
      ImGui::AlignTextToFramePadding();
      {
        ScopedFont mono(GetFonts().pMono, Fonts::kMonoSize * 0.92F);
        if (pParameter == nullptr) {
          TextDim(row.defaultValue);
        } else if (pPreview != nullptr && row.parameter < pPreview->samples.size()) {
          TextDim(pPreview->samples[row.parameter]);
        }
      }

      // The cross leaves a query parameter out of the run (removes an added one), the plus sends it again.
      ImGui::TableSetColumnIndex(6);
      if (row.queryParameter) {
        if (pParameter != nullptr && pParameter->source == ValueSource::omitted) {
          if (IconButton("##restore", Icon::plus, "Send it again during the run") && !structural) {
            request.load->parameters.erase(request.load->parameters.begin() + row.parameter);
            pParameter = nullptr;
            structural = true;
          }
        } else if (IconButton("##remove", Icon::close,
                              row.added ? "Remove it from the run"
                                        : "Leave it out of the run: a normal Fetch keeps sending it") &&
                   !structural) {
          if (row.added) {
            request.load->parameters.erase(request.load->parameters.begin() + row.parameter);
          } else {
            MaterializeParameter(request, row).source = ValueSource::omitted;
            perf.selectedRow = idx;
          }
          pParameter = nullptr;
          structural = true;
        }
      }
      if (structural || edited) {
        touch(pCollection);
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  drawPerfAddQuery(tab, request);

  // A dropped file: a data file for the row under the mouse, or the selected data file row; a collection otherwise.
  if (!perf.droppedPath.empty() && !structural) {
    if (dropTarget == LoadRow::kNoParameter && perf.selectedRow < perf.rows.size()) {
      const LoadRow& selected = perf.rows[perf.selectedRow];
      if (selected.parameter != LoadRow::kNoParameter && request.load &&
          request.load->parameters[selected.parameter].source == ValueSource::dataFile) {
        dropTarget = perf.selectedRow;
      }
    }
    const std::filesystem::path dropped = PathFromUtf8(perf.droppedPath);
    perf.droppedPath.clear();
    if (dropTarget == LoadRow::kNoParameter) {
      importPath(dropped);
      return;
    }
    LoadRow& row = perf.rows[dropTarget];
    LoadParameter& parameter = MaterializeParameter(request, row);
    parameter.source = ValueSource::dataFile;
    parameter.filePath = DataFilePathFor(dropped, dataBaseDirectory(tab));
    parameter.fileFormat = FormatOfFile(dropped);
    perf.selectedRow = dropTarget;
    touch(pCollection);
    toast(Concat({"Data file of '", row.name, "': ", parameter.filePath}));
  }
}

void App::drawPerfAddQuery(RequestTab& tab, Request& request) {
  PerfTab& perf = *tab.perf;
  const Palette& pal = CurrentPalette();
  const float scale = ImGui::GetStyle().FontScaleDpi;
  const ImVec2 size(ImGui::CalcTextSize("Query parameter").x + ImGui::GetFrameHeight() + (12.0F * scale),
                    ImGui::GetFrameHeight());
  ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
  const bool clicked = ImGui::Button("##addQuery", size);
  ImGui::PopStyleColor();
  DrawItemIconLabel(Icon::plus, ImGui::GetFontSize(), "Query parameter", pal.textDim);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("Add a query parameter, sent during the run only: the request does not change");
  }
  if (!clicked) {
    return;
  }
  LoadParameter parameter;
  parameter.queryParameter = true;
  EnsureLoad(request).parameters.push_back(std::move(parameter));
  // Its row comes last: the parameters matching none of the request come after the others, in their order.
  perf.selectedRow = perf.rows.size();
  perf.focusAddedName = true;
  touch(collectionOf(tab));
}

void App::drawPerfRowEditor(RequestTab& tab, Request& request) {
  PerfTab& perf = *tab.perf;
  listPerfRows(tab, request);
  if (perf.rows.empty()) {
    return;
  }
  const Palette& pal = CurrentPalette();
  const float scale = ImGui::GetStyle().FontScaleDpi;
  OpenCollection* pCollection = collectionOf(tab);
  const LoadRow& row = perf.rows[perf.selectedRow];
  std::array<char, 160> buffer{};
  ImGui::Spacing();
  ImGui::PushID("##rowEditor");
  if (row.parameter == LoadRow::kNoParameter || !request.load) {
    TextDim(FormatTo(buffer, "{}:", std::string_view(row.name)));
    ImGui::SameLine();
    if (row.queryParameter) {
      TextDim(FormatTo(buffer, "sends '{}', as written in the request. Choose a source to vary it for the run.",
                       std::string_view(row.defaultValue)));
    } else if (row.undefined) {
      TextColored(pal.warning, "no value. Define it in an environment, or choose a source.");
    } else {
      TextDim("sends the value of the environment or of the collection. Choose a source to vary it.");
    }
    ImGui::PopID();
    return;
  }
  LoadParameter& parameter = request.load->parameters[row.parameter];
  const float fieldWidth = 160.0F * scale;
  bool changed = false;
  SectionTitle(FormatTo(buffer, "{} \xC2\xB7 {}", std::string_view(row.name),
                        kSourceNames[static_cast<std::size_t>(parameter.source)]));
  const auto number = [&](const char* label, ImGuiDataType type, void* pValue, bool sameLine, const char* format) {
    if (sameLine) {
      ImGui::SameLine(0, 16.0F * scale);
    }
    InlineLabel(label);
    ImGui::SetNextItemWidth(fieldWidth * 0.7F);
    ImGui::PushID(label);
    changed |= ImGui::InputScalar("##value", type, pValue, nullptr, nullptr, format);
    ImGui::PopID();
  };
  const auto text = [&](const char* label, RawChars32& value, const char* hint, float width, bool sameLine) {
    if (sameLine) {
      ImGui::SameLine(0, 16.0F * scale);
    }
    InlineLabel(label);
    ImGui::SetNextItemWidth(width);
    ImGui::PushID(label);
    changed |= InputText("##value", value, 0, hint);
    ImGui::PopID();
  };
  switch (parameter.source) {
    case ValueSource::fixed:
      text("Value", parameter.fixedValue, "sent with every request", fieldWidth * 2.5F, false);
      break;
    case ValueSource::list: {
      TextDim("One value per line, typed or pasted:");
      ScopedFont mono(GetFonts().pMono, Fonts::kMonoSize);
      changed |= InputTextMultiline(
          "##list", parameter.listValues,
          ImVec2(std::min(ImGui::GetContentRegionAvail().x, 520.0F * scale), ImGui::GetTextLineHeight() * 6.0F));
      break;
    }
    case ValueSource::integerRange: {
      number("From", ImGuiDataType_S64, &parameter.integerFrom, false, nullptr);
      number("To", ImGuiDataType_S64, &parameter.integerTo, true, nullptr);
      number("Step", ImGuiDataType_S64, &parameter.integerStep, true, nullptr);
      int padding = parameter.integerPadding;
      ImGui::SameLine(0, 16.0F * scale);
      InlineLabel("Digits");
      ImGui::SetNextItemWidth(fieldWidth * 0.5F);
      if (ImGui::InputInt("##padding", &padding, 0)) {
        parameter.integerPadding = static_cast<std::uint8_t>(std::clamp(padding, 0, 20));
        changed = true;
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("Minimum number of digits: 5 sends 00001");
      }
      break;
    }
    case ValueSource::decimalRange: {
      number("From", ImGuiDataType_Double, &parameter.decimalFrom, false, "%g");
      number("To", ImGuiDataType_Double, &parameter.decimalTo, true, "%g");
      number("Step", ImGuiDataType_Double, &parameter.decimalStep, true, "%g");
      int decimals = parameter.decimals;
      ImGui::SameLine(0, 16.0F * scale);
      InlineLabel("Decimals");
      ImGui::SetNextItemWidth(fieldWidth * 0.5F);
      if (ImGui::InputInt("##decimals", &decimals, 0)) {
        parameter.decimals = static_cast<std::uint8_t>(std::clamp(decimals, 0, 30));
        changed = true;
      }
      break;
    }
    case ValueSource::dateRange: {
      text("From", parameter.dateFrom, "2026-01-01", fieldWidth, false);
      text("To", parameter.dateTo, "2026-01-31T23:59", fieldWidth, true);
      // The step in the largest unit dividing it.
      std::size_t unit = kDateSteps.size() - 1;
      while (unit > 0 && parameter.dateStepSeconds % kDateSteps[unit].seconds != 0) {
        --unit;
      }
      std::int64_t step = parameter.dateStepSeconds / kDateSteps[unit].seconds;
      ImGui::SameLine(0, 16.0F * scale);
      InlineLabel("Step");
      ImGui::SetNextItemWidth(fieldWidth * 0.5F);
      if (ImGui::InputScalar("##step", ImGuiDataType_S64, &step)) {
        parameter.dateStepSeconds = step * kDateSteps[unit].seconds;
        changed = true;
      }
      ImGui::SameLine();
      ImGui::SetNextItemWidth(fieldWidth * 0.7F);
      if (ImGui::BeginCombo("##stepUnit", kDateSteps[unit].name)) {
        for (const TimeUnit& choice : kDateSteps) {
          if (ImGui::Selectable(choice.name, choice.seconds == kDateSteps[unit].seconds)) {
            parameter.dateStepSeconds = step * choice.seconds;
            changed = true;
          }
        }
        ImGui::EndCombo();
      }
      text("Format", parameter.dateFormat, "%Y-%m-%d", fieldWidth, false);
      ImGui::SameLine();
      TextDim("%Y %m %d %H %M %S %j (day of the year) %s (unix seconds), UTC");
      break;
    }
    case ValueSource::randomString:
      number("Length", ImGuiDataType_U32, &parameter.randomLength, false, nullptr);
      text("Characters", parameter.randomCharacters, "abcdefghijklmnopqrstuvwxyz", fieldWidth * 2.0F, true);
      break;
    case ValueSource::uuid:
      TextDim("A random version 4 UUID for each request, derived from the seed of the run: reproducible.");
      break;
    case ValueSource::omitted:
      TextDim("Left out of the requests of the run. A normal Fetch keeps sending it.");
      break;
    case ValueSource::timestamp:
      InlineLabel("Format");
      changed |= EnumCombo("##timestampFormat", parameter.timestampFormat, kTimestampNames, fieldWidth * 1.2F);
      ImGui::SameLine();
      TextDim("the time of the request, not reproducible");
      break;
    case ValueSource::dataFile: {
      text("File", parameter.filePath, "users.csv  (relative to the collection, or drop the file on its row)",
           std::min(ImGui::GetContentRegionAvail().x - (80.0F * scale), 520.0F * scale), false);
      InlineLabel("Format");
      changed |= EnumCombo("##fileFormat", parameter.fileFormat, kFileFormatNames, fieldWidth * 1.3F);
      const LoadPreview* pPreview = perf.preview.get();
      const DataFileIndex* pIndex = pPreview != nullptr && row.parameter < pPreview->dataFiles.size()
                                        ? pPreview->dataFiles[row.parameter].get()
                                        : nullptr;
      if (pIndex != nullptr && pIndex->valid() && parameter.fileFormat != DataFileFormat::text) {
        ImGui::SameLine(0, 16.0F * scale);
        InlineLabel("Column");
        ImGui::SetNextItemWidth(fieldWidth);
        const std::string_view current = parameter.fileColumn.empty() && !pIndex->columns().empty()
                                             ? std::string_view(pIndex->columns().front())
                                             : std::string_view(parameter.fileColumn);
        RawChars32 preview(current);
        preview.push_back('\0');
        if (ImGui::BeginCombo("##column", preview.data())) {
          for (const RawChars32& column : pIndex->columns()) {
            RawChars32 name(View(column));
            name.push_back('\0');
            if (ImGui::Selectable(name.data(), View(column) == current)) {
              parameter.fileColumn = column;
              changed = true;
            }
          }
          ImGui::EndCombo();
        }
      }
      if (pIndex == nullptr) {
        break;
      }
      if (!pIndex->valid()) {
        TextColored(pal.warning, pIndex->error());
        break;
      }
      std::array<char, 32> size{};
      TextDim(FormatTo(buffer, "{} rows, {} columns, {}", pIndex->nbRows(), pIndex->columns().size(),
                       FormatBytes(pIndex->fileSize(), size)));
      if (pIndex->firstMalformedLine() != 0) {
        ImGui::SameLine();
        TextColored(pal.warning, FormatTo(buffer, "first malformed line: {}", pIndex->firstMalformedLine()));
      }
      // The first rows of the file.
      const std::size_t nbColumns = std::max<std::size_t>(1, pIndex->columns().size());
      const auto values = pIndex->previewValues();
      if (!values.empty() && nbColumns <= 64 &&
          ImGui::BeginTable("##fileRows", static_cast<int>(nbColumns),
                            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit)) {
        for (const RawChars32& column : pIndex->columns()) {
          RawChars32 name(View(column));
          name.push_back('\0');
          ImGui::TableSetupColumn(name.data());
        }
        ImGui::TableHeadersRow();
        ScopedFont mono(GetFonts().pMono, Fonts::kMonoSize * 0.92F);
        for (std::size_t idx = 0; idx < values.size(); ++idx) {
          if (idx % nbColumns == 0) {
            ImGui::TableNextRow();
          }
          ImGui::TableSetColumnIndex(static_cast<int>(idx % nbColumns));
          TextDim(values[idx]);
        }
        ImGui::EndTable();
      }
      break;
    }
  }
  if (changed) {
    touch(pCollection);
  }

  // The first requests as they will be sent.
  if (perf.preview && !perf.preview->requests.empty()) {
    ImGui::Spacing();
    SectionTitle("FIRST REQUESTS");
    ScopedFont mono(GetFonts().pMono, Fonts::kMonoSize * 0.92F);
    for (std::size_t idx = 0; idx < perf.preview->requests.size(); ++idx) {
      const LoadPreviewRequest& preview = perf.preview->requests[static_cast<std::uint32_t>(idx)];
      TextDim(FormatTo(buffer, "#{}", idx + 1));
      ImGui::SameLine();
      if (preview.refused) {
        TextColored(pal.warning, preview.line);
      } else {
        Text(preview.line);
      }
      if (!preview.body.empty()) {
        ImGui::Indent(24.0F * scale);
        TextDim(preview.body);
        ImGui::Unindent(24.0F * scale);
      }
    }
  }
  ImGui::PopID();
}

void App::drawPerfDialogs(RequestTab& tab, const Request& request) {
  PerfTab& perf = *tab.perf;
  const Palette& pal = CurrentPalette();
  const float scale = ImGui::GetStyle().FontScaleDpi;
  std::array<char, 256> buffer{};
  static constexpr const char* kConfirmPopup = "Start the run?###perfConfirm";
  if (perf.confirmRequested) {
    perf.confirmRequested = false;
    ImGui::OpenPopup(kConfirmPopup);
  }
  const ImGuiViewport* pViewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(pViewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5F, 0.5F));
  ImGui::SetNextWindowSize(ImVec2(520.0F * scale, 0), ImGuiCond_Appearing);
  if (ImGui::BeginPopupModal(kConfirmPopup, nullptr, ImGuiWindowFlags_NoSavedSettings)) {
    if (!perf.planCurrent() || perf.running()) {
      ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
      return;
    }
    const LoadPlan& plan = *perf.preview->plan;
    LoadOptions options = MakeLoadOptions(LoadOf(request));
    options.maxConnections = _workspace.settings().maxLoadConnections;
    const std::uint64_t planned = PlannedRequests(plan, options);
    const std::string_view method = MethodToStr(plan.method());
    const std::string_view host =
        plan.host().empty() ? std::string_view{"hosts generated for each request"} : plan.host();
    ImGui::PushTextWrapPos(0);
    if (planned != 0) {
      Text(FormatTo(buffer, "This run sends up to {} {} requests to {}, on {} connections.", planned, method, host,
                    LoadConnections(options)));
    } else if (options.adaptiveRate) {
      Text(FormatTo(buffer, "This run sends {} requests to {} for {:g} s, on {} connections.", method, host,
                    options.durationMs / 1000.0, LoadConnections(options)));
    } else {
      Text(FormatTo(buffer, "This run sends {} requests to {} for {:g} s, on {} connections, as fast as they go.",
                    method, host, options.durationMs / 1000.0, LoadConnections(options)));
    }
    if (options.adaptiveRate) {
      const double start = RateController::StartRate(options.minRequestsPerSecond, options.maxRequestsPerSecond);
      if (options.maxRequestsPerSecond != 0) {
        Text(FormatTo(buffer, "Its rate starts at {:g} requests per second and adapts to the server, up to {}.", start,
                      options.maxRequestsPerSecond));
      } else {
        Text(FormatTo(buffer, "Its rate starts at {:g} requests per second and adapts to the server, without maximum.",
                      start));
      }
    }
    if (!IsLocalHost(plan.host())) {
      TextColored(pal.warning, "It is not the local machine: load only servers you are allowed to.");
    }
    if (!MethodIsSafe(plan.method())) {
      TextColored(pal.warning, FormatTo(buffer, "{} requests may change data on the server.", method));
    }
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    // Keys pressed from the second frame on: the Ctrl+Enter that asked for the run does not confirm it.
    const bool keys = !ImGui::IsWindowAppearing();
    if (PrimaryButton("Run") ||
        (keys && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)))) {
      startRun(tab, request);
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || (keys && ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }

  ImGui::SetNextWindowPos(pViewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5F, 0.5F));
  ImGui::SetNextWindowSize(ImVec2(640.0F * scale, 0), ImGuiCond_Appearing);
  if (ImGui::BeginPopupModal("Export the run###perfExport", nullptr, ImGuiWindowFlags_NoSavedSettings)) {
    TextDim("The whole run as JSON: request, settings and seed, totals, percentiles, histograms, statuses, errors and");
    TextDim("time series.");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::IsWindowAppearing()) {
      ImGui::SetKeyboardFocusHere();
    }
    const bool enter = InputText("##exportPath", perf.exportPath, ImGuiInputTextFlags_EnterReturnsTrue);
    if ((PrimaryButton("Export") || enter) && !Trim(perf.exportPath).empty() &&
        exportRun(tab, PathFromUtf8(Trim(perf.exportPath)))) {
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || (ImGui::IsKeyPressed(ImGuiKey_Escape) && !ImGui::IsAnyItemActive())) {
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}

void App::drawPerfDashboard(PerfTab& perf) {
  const Palette& pal = CurrentPalette();
  const ImGuiStyle& style = ImGui::GetStyle();
  const float scale = style.FontScaleDpi;
  if (!perf.snapshot) {
    const float height = ImGui::GetContentRegionAvail().y;
    const float spriteHeight = std::min(100.0F * scale, height * 0.45F);
    const float spriteWidth = spriteHeight * 4.0F / 3.0F;
    ImGui::Dummy(ImVec2(0, std::max(0.0F, (height - spriteHeight) * 0.3F)));
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float availWidth = ImGui::GetContentRegionAvail().x;
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - _startTime).count();
    _assets.drawSprite(ImGui::GetWindowDrawList(), Animation::sleep, seconds,
                       ImVec2(origin.x + ((availWidth - spriteWidth) * 0.5F), origin.y), spriteHeight);
    ImGui::Dummy(ImVec2(availWidth, spriteHeight));
    static constexpr std::string_view kHint = "Choose how the parameters vary, then Run (Ctrl+Enter).";
    const float width = ImGui::CalcTextSize(kHint.data(), kHint.data() + kHint.size()).x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0F, (availWidth - width) * 0.5F));
    TextDim(kHint);
    return;
  }
  const LoadSnapshot& snapshot = *perf.snapshot;
  PerfCharts& charts = perf.charts;
  if (charts.pSnapshot != &snapshot) {
    PrepareCharts(charts, snapshot);
  }
  std::array<char, 160> buffer{};
  std::array<char, 64> value{};
  std::array<char, 64> other{};

  // Phase and progress, beside Zalie: she eats her meat as fast as she can while the run goes (the meat follows its
  // progress), then naps. The banner keeps its height when the run ends, so the dashboard does not move.
  const bool finished = snapshot.phase == LoadPhase::finished;
  const double duration = perf.runDurationMs / 1000.0;
  const double warmup = perf.runSettings.warmupMs / 1000.0;
  const bool bounded = duration > warmup || perf.runMaxRequests != 0;
  double progress = 0.0;
  if (duration > warmup) {
    progress = snapshot.elapsedSeconds / (duration - warmup);
  }
  if (perf.runMaxRequests != 0) {
    progress = std::max(progress, static_cast<double>(snapshot.requests()) / static_cast<double>(perf.runMaxRequests));
  }
  progress = std::clamp(progress, 0.0, 1.0);
  const float bannerHeight = 88.0F * scale;
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  {
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - _startTime).count();
    ImDrawList* pDrawList = ImGui::GetWindowDrawList();
    if (finished) {
      _assets.drawSprite(pDrawList, Animation::sleep, seconds, origin, bannerHeight);
    } else {
      // Without a limit, a new piece of meat comes every few seconds.
      static constexpr double kSecondsPerMeal = 8.0;
      const double eaten = bounded ? progress : std::fmod(snapshot.elapsedSeconds / kSecondsPerMeal, 1.0);
      _assets.drawMeal(pDrawList, seconds, static_cast<float>(eaten), origin, bannerHeight);
    }
  }
  // The text beside her, centered on her height.
  const float barHeight = 6.0F * scale;
  const float textHeight = ImGui::GetFrameHeightWithSpacing() + ImGui::GetTextLineHeight() +
                           (finished ? 0.0F : style.ItemSpacing.y + barHeight);
  ImGui::SetCursorScreenPos(ImVec2(origin.x + (bannerHeight * 4.0F / 3.0F) + style.ItemSpacing.x,
                                   origin.y + std::max(0.0F, std::floor((bannerHeight - textHeight) * 0.5F))));
  ImGui::BeginGroup();
  static constexpr std::array<std::string_view, 5> kPhases{"Starting", "Warming up", "Running", "Stopping", "Finished"};
  ImGui::AlignTextToFramePadding();
  TextColored(finished ? pal.success : pal.accent, kPhases[static_cast<std::size_t>(snapshot.phase)]);
  ImGui::SameLine();
  if (duration > 0.0 && !finished) {
    TextDim(FormatTo(buffer, "{:.1f} s of {:g} s", snapshot.elapsedSeconds, std::max(0.0, duration - warmup)));
  } else {
    TextDim(FormatTo(buffer, "{:.1f} s", snapshot.elapsedSeconds));
  }
  ImGui::SameLine();
  TextDim(FormatTo(buffer, "\xC2\xB7  {} connections  \xC2\xB7  {} in flight  \xC2\xB7  seed {}", snapshot.connections,
                   snapshot.inFlight, perf.runSettings.seed));
  static constexpr std::array<std::string_view, 4> kMeals{
      "Zalie sits down to eat...", "Zalie is warming up: these first bites do not count",
      "Zalie is eating as fast as she can!", "Zalie is finishing her last bites..."};
  if (!finished) {
    Text(kMeals[static_cast<std::size_t>(snapshot.phase)]);
    ImGui::ProgressBar(static_cast<float>(bounded ? progress : 0.0),
                       ImVec2(std::min(ImGui::GetContentRegionAvail().x, 320.0F * scale), barHeight), "");
  } else if (snapshot.requests() != 0) {
    Text(FormatTo(buffer, "Zalie ate {} requests. Nap time!",
                  FormatChartValue(ChartUnit::count, static_cast<double>(snapshot.requests()), value)));
  } else {
    Text("Nothing to eat this time.");
  }
  ImGui::EndGroup();
  ImGui::SetCursorScreenPos(
      ImVec2(origin.x, std::max(origin.y + bannerHeight, ImGui::GetItemRectMax().y) + style.ItemSpacing.y));
  if (!snapshot.setupError.empty()) {
    TextColored(pal.danger, FormatTo(buffer, "The run could not start: {}", std::string_view(snapshot.setupError)));
  }
  if (snapshot.clientSaturated) {
    TextColored(pal.warning, FormatTo(buffer,
                                      "The client is saturated (CPU {:.0f} %): these numbers measure Zalie more than "
                                      "the server. Use fewer connections, or load from another machine.",
                                      snapshot.clientLoad * 100.0));
  }

  const bool adaptive = snapshot.targetRequestsPerSecond > 0.0;
  if (adaptive) {
    const std::string_view state = kRateStates[static_cast<std::size_t>(snapshot.rateState)];
    const std::string_view target = FormatChartValue(ChartUnit::count, snapshot.targetRequestsPerSecond, value);
    const std::string_view baseline =
        snapshot.baselineP50Ns == 0
            ? std::string_view{"none yet"}
            : FormatChartValue(ChartUnit::nanoseconds, static_cast<double>(snapshot.baselineP50Ns), other);
    const std::string_view line = FormatTo(
        buffer, "Adaptive rate: {}  \xC2\xB7  target {} req/s  \xC2\xB7  baseline p50 {}", state, target, baseline);
    if (snapshot.rateState == RateState::backOff || snapshot.rateState == RateState::clientLimited) {
      TextColored(pal.warning, line);
    } else {
      TextDim(line);
    }
  }

  // Key numbers.
  const std::uint64_t requests = snapshot.requests();
  const double share = requests == 0 ? 0.0 : 100.0 / static_cast<double>(requests);
  const double rate = finished ? snapshot.meanRequestsPerSecond : snapshot.currentRequestsPerSecond;
  struct TileData {
    std::string_view label;
    std::string_view value;
    std::string_view detail;
    ImU32 color;
  };
  std::array<char, 512> texts{};
  std::size_t used = 0;
  const auto keep = [&](std::string_view text) {
    const std::size_t size = std::min(text.size(), texts.size() - used);
    text.copy(texts.data() + used, size);
    const std::string_view kept(texts.data() + used, size);
    used += size;
    return kept;
  };
  const auto ns = [&](std::uint64_t nanoseconds) {
    return keep(FormatChartValue(ChartUnit::nanoseconds, static_cast<double>(nanoseconds), value));
  };
  const auto count = [&](double number) { return keep(FormatChartValue(ChartUnit::count, number, value)); };
  const std::uint64_t errors = snapshot.counts[kErrorClass];
  // A few errors among many requests still show.
  const double errorPercent = static_cast<double>(errors) * share;
  std::array<char, 16> errorText{};
  const std::string_view errorShare = errors != 0 && errorPercent < 0.1 ? FormatTo(errorText, "{:.2f} %", errorPercent)
                                                                        : FormatTo(errorText, "{:.1f} %", errorPercent);
  const std::array tiles{
      TileData{"Requests / s", count(rate),
               adaptive ? keep(FormatTo(buffer, "sustained {} \xC2\xB7 peak {}",
                                        FormatChartValue(ChartUnit::count, snapshot.sustainedRequestsPerSecond, value),
                                        FormatChartValue(ChartUnit::count, snapshot.peakRequestsPerSecond, other)))
                        : keep(FormatTo(buffer, "mean {} \xC2\xB7 peak {}",
                                        FormatChartValue(ChartUnit::count, snapshot.meanRequestsPerSecond, value),
                                        FormatChartValue(ChartUnit::count, snapshot.peakRequestsPerSecond, other))),
               pal.text},
      TileData{
          "Median (p50)", ns(snapshot.total.p50Ns),
          keep(FormatTo(buffer, "mean {}", FormatChartValue(ChartUnit::nanoseconds, snapshot.total.meanNs, value))),
          pal.text},
      TileData{
          "p99", ns(snapshot.total.p99Ns),
          keep(FormatTo(buffer, "p99.9 {}",
                        FormatChartValue(ChartUnit::nanoseconds, static_cast<double>(snapshot.total.p999Ns), value))),
          pal.text},
      TileData{
          "Max", ns(snapshot.total.maxNs),
          keep(FormatTo(buffer, "min {}",
                        FormatChartValue(ChartUnit::nanoseconds, static_cast<double>(snapshot.total.minNs), value))),
          pal.text},
      TileData{"Success (2xx)", keep(FormatTo(value, "{:.1f} %", static_cast<double>(snapshot.counts[0]) * share)),
               keep(FormatTo(buffer, "{} responses", requests - errors)), pal.success},
      TileData{"Errors", keep(errorShare), keep(FormatTo(buffer, "{} without response", errors)),
               errors == 0 ? pal.text : pal.danger},
      TileData{"Throughput",
               keep(FormatTo(buffer, "{}/s", FormatChartValue(ChartUnit::bytes, snapshot.bodyBytesPerSecond, value))),
               keep(FormatTo(buffer, "{} received",
                             FormatChartValue(ChartUnit::bytes, static_cast<double>(snapshot.bodyBytes), value))),
               pal.text},
      TileData{"Requests", count(static_cast<double>(requests)),
               perf.runMaxRequests == 0 ? std::string_view{"warm-up excluded"}
                                        : keep(FormatTo(buffer, "of {} planned", perf.runMaxRequests)),
               pal.text},
  };
  ImGui::Spacing();
  const float spacing = style.ItemSpacing.x;
  const float availWidth = ImGui::GetContentRegionAvail().x;
  const float minTileWidth = 130.0F * scale;
  const auto columns = static_cast<std::size_t>(
      std::clamp((availWidth + spacing) / (minTileWidth + spacing), 1.0F, static_cast<float>(tiles.size())));
  const float tileWidth = (availWidth - (spacing * static_cast<float>(columns - 1))) / static_cast<float>(columns);
  const float tileHeight = 74.0F * scale;
  for (std::size_t idx = 0; idx < tiles.size(); ++idx) {
    if (idx % columns != 0) {
      ImGui::SameLine();
    }
    Tile(tiles[idx].label, tiles[idx].value, tiles[idx].detail, tiles[idx].color, ImVec2(tileWidth, tileHeight));
  }

  // Charts: two per row when there is room.
  ImGui::Spacing();
  const bool twoColumns = availWidth >= 2.0F * 360.0F * scale;
  const float chartHeight = 210.0F * scale;
  if (ImGui::BeginTable("##charts", twoColumns ? 2 : 1, ImGuiTableFlags_None)) {
    const auto nbPoints = static_cast<std::uint32_t>(charts.times.size());
    const std::span<const float> times(charts.times.data(), charts.times.size());
    std::array<ChartSeries, kNbLoadClasses + 1> rateSeries;
    for (std::uint32_t outcome = 0; outcome < kNbLoadClasses; ++outcome) {
      rateSeries[outcome] = {.name = kClassNames[outcome],
                             .values = {charts.rates.data() + (std::size_t{outcome} * nbPoints), nbPoints},
                             .color = ClassColor(outcome)};
    }
    // The target of the adaptive rate, over the stack.
    rateSeries[kNbLoadClasses] = {.name = "Target",
                                  .values = {charts.rates.data() + (kNbLoadClasses * nbPoints), nbPoints},
                                  .color = pal.text,
                                  .overlay = true};
    std::array<ChartSeries, PerfCharts::kNbLatencySeries> latencySeries;
    for (std::uint32_t series = 0; series < PerfCharts::kNbLatencySeries; ++series) {
      latencySeries[series] = {.name = kLatencyNames[series],
                               .values = {charts.latencies.data() + (std::size_t{series} * nbPoints), nbPoints},
                               .color = LatencyColor(series),
                               .reduce = ChartReduce::max};
    }
    ImGui::TableNextColumn();
    Chart("##rates", times, std::span<const ChartSeries>(rateSeries).first(kNbLoadClasses + (adaptive ? 1 : 0)),
          charts.rateChart,
          {.title = "Requests per second, by status",
           .valueSuffix = " req/s",
           .x = {.unit = ChartUnit::seconds},
           .y = {.unit = ChartUnit::count},
           .height = chartHeight,
           .style = ChartStyle::stackedAreas,
           .zoomable = finished});
    ImGui::TableNextColumn();
    Chart("##latency", times, latencySeries, charts.latencyChart,
          {.title = "Response time",
           .x = {.unit = ChartUnit::seconds},
           .y = {.unit = ChartUnit::nanoseconds, .scale = perf.logLatency ? ChartScale::log : ChartScale::linear},
           .height = chartHeight,
           .zoomable = finished});
    if (ImGui::Checkbox("Logarithmic scale##logLatency", &perf.logLatency)) {
      charts.latencyChart.invalidate();
    }

    const std::array<ChartMarker, 4> markers{
        ChartMarker{.label = "p50", .x = static_cast<float>(snapshot.total.p50Ns), .color = LatencyColor(0)},
        ChartMarker{.label = "p90", .x = static_cast<float>(snapshot.total.p90Ns), .color = LatencyColor(1)},
        ChartMarker{.label = "p99", .x = static_cast<float>(snapshot.total.p99Ns), .color = LatencyColor(2)},
        ChartMarker{.label = "p99.9", .x = static_cast<float>(snapshot.total.p999Ns), .color = LatencyColor(3)}};
    const std::array histogramSeries{ChartSeries{.name = "Responses",
                                                 .values = {charts.bucketCounts.data(), charts.bucketCounts.size()},
                                                 .color = WithAlpha(pal.accent, 0.8F)}};
    ImGui::TableNextColumn();
    Chart(
        "##histogram", {charts.bucketEdges.data(), charts.bucketEdges.size()}, histogramSeries, charts.histogramChart,
        {.title = "Latency distribution",
         .valueSuffix = " responses",
         .markers = snapshot.total.count == 0 ? std::span<const ChartMarker>{} : std::span<const ChartMarker>(markers),
         .x = {.unit = ChartUnit::nanoseconds, .scale = ChartScale::log},
         .y = {.unit = ChartUnit::count},
         .height = chartHeight,
         .style = ChartStyle::bars});
    const std::array curveSeries{ChartSeries{.name = "Response time",
                                             .values = {charts.percentileNs.data(), charts.percentileNs.size()},
                                             .color = pal.accent}};
    ImGui::TableNextColumn();
    Chart("##percentiles", {charts.percentiles.data(), charts.percentiles.size()}, curveSeries, charts.percentileChart,
          {.title = "Latency by percentile",
           .x = {.unit = ChartUnit::percent, .scale = ChartScale::nines},
           .y = {.unit = ChartUnit::nanoseconds, .scale = ChartScale::log},
           .height = chartHeight});
    ImGui::EndTable();
  }
  DrawPerfTables(perf);
}

void App::DrawPerfTables(PerfTab& perf) {
  const LoadSnapshot& snapshot = *perf.snapshot;
  const Palette& pal = CurrentPalette();
  const float scale = ImGui::GetStyle().FontScaleDpi;
  std::array<char, 160> buffer{};
  std::array<char, 64> value{};
  const std::uint64_t requests = snapshot.requests();
  const double share = requests == 0 ? 0.0 : 100.0 / static_cast<double>(requests);
  const bool twoColumns = ImGui::GetContentRegionAvail().x >= 2.0F * 360.0F * scale;
  if (!ImGui::BeginTable("##tables", twoColumns ? 2 : 1, ImGuiTableFlags_None)) {
    return;
  }

  // Status distribution: one bar split by class, above the codes and the errors.
  ImGui::TableNextColumn();
  ImGui::Spacing();
  SectionTitle("STATUS DISTRIBUTION");
  {
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float height = 14.0F * scale;
    ImDrawList* pDrawList = ImGui::GetWindowDrawList();
    pDrawList->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), pal.surface, 3.0F * scale);
    float x = pos.x;
    for (std::size_t outcome = 0; outcome < kNbLoadClasses; ++outcome) {
      if (snapshot.counts[outcome] == 0) {
        continue;
      }
      const float segment = width * static_cast<float>(static_cast<double>(snapshot.counts[outcome]) * share / 100.0);
      pDrawList->AddRectFilled(ImVec2(x, pos.y), ImVec2(x + segment, pos.y + height), ClassColor(outcome));
      if (ImGui::IsMouseHoveringRect(ImVec2(x, pos.y), ImVec2(x + segment, pos.y + height))) {
        ImGui::SetTooltip("%.*s: %llu (%.1f %%)", static_cast<int>(kClassNames[outcome].size()),
                          kClassNames[outcome].data(), static_cast<unsigned long long>(snapshot.counts[outcome]),
                          static_cast<double>(snapshot.counts[outcome]) * share);
      }
      x += segment;
    }
    ImGui::Dummy(ImVec2(width, height));
  }
  static constexpr ImGuiTableFlags kTableFlags =
      ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp;
  if (ImGui::BeginTable("##statuses", 3, kTableFlags)) {
    ImGui::TableSetupColumn("Status");
    ImGui::TableSetupColumn("Count");
    ImGui::TableSetupColumn("Share");
    ImGui::TableHeadersRow();
    for (const LoadStatusCount& status : snapshot.statuses) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      TextColored(StatusColor(status.status, ExchangeState::completed), FormatTo(buffer, "{}", status.status));
      ImGui::TableNextColumn();
      Text(FormatTo(buffer, "{}", status.count));
      ImGui::TableNextColumn();
      TextDim(FormatTo(buffer, "{:.2f} %", static_cast<double>(status.count) * share));
    }
    for (std::size_t kind = 0; kind < kNbLoadErrorKinds; ++kind) {
      const LoadErrorStats& error = snapshot.errors[kind];
      if (error.count == 0) {
        continue;
      }
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      TextColored(ClassColor(kErrorClass), kErrorKindNames[kind]);
      if (ImGui::IsItemHovered() && !error.firstMessage.empty()) {
        ImGui::SetTooltip("%.*s", static_cast<int>(error.firstMessage.size()), error.firstMessage.data());
      }
      ImGui::TableNextColumn();
      Text(FormatTo(buffer, "{}", error.count));
      ImGui::TableNextColumn();
      TextDim(FormatTo(buffer, "{:.2f} %", static_cast<double>(error.count) * share));
    }
    ImGui::EndTable();
  }
  if (snapshot.maxBodyBytes != 0) {
    const std::uint64_t responses = requests - snapshot.counts[kErrorClass];
    std::array<char, 32> minText{};
    std::array<char, 32> meanText{};
    std::array<char, 32> maxText{};
    TextDim(FormatTo(
        buffer, "Body sizes: min {} \xC2\xB7 mean {} \xC2\xB7 max {}",
        FormatChartValue(ChartUnit::bytes, static_cast<double>(snapshot.minBodyBytes), minText),
        FormatChartValue(
            ChartUnit::bytes,
            responses == 0 ? 0.0 : static_cast<double>(snapshot.bodyBytes) / static_cast<double>(responses), meanText),
        FormatChartValue(ChartUnit::bytes, static_cast<double>(snapshot.maxBodyBytes), maxText)));
  }
  if (snapshot.cpuSeconds > 0.0) {
    TextDim(FormatTo(buffer, "Client CPU: {:.1f} s, {:.0f} % of the CPUs of the connections", snapshot.cpuSeconds,
                     snapshot.clientLoad * 100.0));
  }

  // Response times: totals, to the head, and per class of outcome.
  ImGui::TableNextColumn();
  ImGui::Spacing();
  SectionTitle("RESPONSE TIMES");
  std::array<std::size_t, kNbLoadClasses> classes{};
  std::size_t nbClasses = 0;
  for (std::size_t outcome = 0; outcome < kNbLoadClasses; ++outcome) {
    if (snapshot.counts[outcome] != 0) {
      classes[nbClasses++] = outcome;
    }
  }
  if (ImGui::BeginTable("##latencies", static_cast<int>(3 + nbClasses), kTableFlags)) {
    ImGui::TableSetupColumn("");
    ImGui::TableSetupColumn("Total");
    ImGui::TableSetupColumn("To head");
    for (std::size_t idx = 0; idx < nbClasses; ++idx) {
      // kClassNames are views of string literals: null-terminated.
      // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
      ImGui::TableSetupColumn(kClassNames[classes[idx]].data());
    }
    ImGui::TableHeadersRow();
    struct Statistic {
      const char* label;
      std::uint64_t LatencySummary::* pValue;
      double LatencySummary::* pDouble;
    };
    static constexpr std::array kStatistics{
        Statistic{"Count", &LatencySummary::count, nullptr},  Statistic{"Min", &LatencySummary::minNs, nullptr},
        Statistic{"Mean", nullptr, &LatencySummary::meanNs},  Statistic{"Std dev", nullptr, &LatencySummary::stddevNs},
        Statistic{"p50", &LatencySummary::p50Ns, nullptr},    Statistic{"p90", &LatencySummary::p90Ns, nullptr},
        Statistic{"p95", &LatencySummary::p95Ns, nullptr},    Statistic{"p99", &LatencySummary::p99Ns, nullptr},
        Statistic{"p99.9", &LatencySummary::p999Ns, nullptr}, Statistic{"Max", &LatencySummary::maxNs, nullptr}};
    const auto cell = [&](const LatencySummary& summary, const Statistic& statistic, bool isCount) {
      ImGui::TableNextColumn();
      const double number =
          statistic.pValue != nullptr ? static_cast<double>(summary.*statistic.pValue) : summary.*statistic.pDouble;
      if (summary.count == 0 && !isCount) {
        TextDim("-");
      } else {
        Text(FormatChartValue(isCount ? ChartUnit::count : ChartUnit::nanoseconds, number, value));
      }
    };
    for (const Statistic& statistic : kStatistics) {
      const bool isCount = statistic.pValue == &LatencySummary::count;
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      TextDim(statistic.label);
      cell(snapshot.total, statistic, isCount);
      cell(snapshot.head, statistic, isCount);
      for (std::size_t idx = 0; idx < nbClasses; ++idx) {
        cell(snapshot.byClass[classes[idx]], statistic, isCount);
      }
    }
    ImGui::EndTable();
  }
  TextDim("Total: to the end of the body. Errors have no response: their time is when they failed.");
  ImGui::EndTable();
}

}  // namespace zalie::ui
