#pragma once

#include <amc/vector.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>

#include "zalie/collection.hpp"
#include "zalie/data-file.hpp"
#include "zalie/load-preview.hpp"
#include "zalie/load-runner.hpp"
#include "zalie/load-settings.hpp"
#include "zalie/method.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/request.hpp"
#include "zalie/ui/chart.hpp"

namespace zalie::ui {

// Compiles the plans of the Performance tabs on a thread of its own: data files are scanned there, never on the UI
// thread. A new job of a tab replaces the one it has still queued.
class PlanCompiler {
 public:
  using WakeFn = void (*)(void* pUserData);

  // Where the jobs of a tab publish their outcome, shared by the tab and its jobs.
  struct Slot {
    std::mutex mutex;
    std::shared_ptr<const LoadPreview> preview;
    std::uint64_t revision{0};
  };

  struct Job {
    Request request;
    // The parts of the collection a request uses (variables, headers, authentication), its active environment being
    // the only one.
    Collection context;
    // Relative data file paths start there.
    std::filesystem::path baseDirectory;
    std::shared_ptr<Slot> slot;
    std::uint64_t revision{0};
  };

  // 'wakeFn' (nullable) is called from the compiler thread after each job.
  PlanCompiler(WakeFn wakeFn, void* pWakeUserData);

  PlanCompiler(const PlanCompiler&) = delete;
  PlanCompiler& operator=(const PlanCompiler&) = delete;
  PlanCompiler(PlanCompiler&&) = delete;
  PlanCompiler& operator=(PlanCompiler&&) = delete;

  // Waits for the job in progress.
  ~PlanCompiler();

  void submit(Job job);

  // Waits until no job is queued nor in progress, or the timeout. Returns false on timeout.
  bool waitIdle(std::chrono::milliseconds timeout);

 private:
  void run(const std::stop_token& stopToken);

  std::mutex _mutex;
  std::condition_variable_any _condition;
  std::deque<Job> _jobs;
  DataFileIndexCache _dataFiles;
  WakeFn _wakeFn;
  void* _pWakeUserData;
  bool _busy{false};
  // Last: started once everything else exists.
  std::jthread _thread;
};

// The charts of the dashboard, prepared once per snapshot (never per frame).
struct PerfCharts {
  static constexpr std::uint32_t kNbLatencySeries = 4;

  // End of each point of the time series, in seconds since the end of the warm-up.
  amc::vector<float> times;
  // Requests per second of each class of outcome: kNbLoadClasses series of times.size() values.
  amc::vector<float> rates;
  // p50, p90, p99 and max of each point, in ns: kNbLatencySeries series of times.size() values.
  amc::vector<float> latencies;
  // Latency histogram: the edges of its bars in ns (one more than the counts, evenly spaced on a logarithmic axis),
  // and the responses of each bar.
  amc::vector<float> bucketEdges;
  amc::vector<float> bucketCounts;
  // Percentile curve: percentiles, and their latency in ns.
  amc::vector<float> percentiles;
  amc::vector<float> percentileNs;
  ChartState rateChart;
  ChartState latencyChart;
  ChartState histogramChart;
  ChartState percentileChart;
  // The snapshot these were prepared for.
  const LoadSnapshot* pSnapshot{nullptr};
};

// The state of a Performance tab: run settings of a request, its plan, its run and the dashboard.
struct PerfTab {
  // Whether 'preview' was compiled from the request as it is now.
  [[nodiscard]] bool planCurrent() const noexcept { return preview != nullptr && previewRevision == submittedRevision; }

  [[nodiscard]] bool running() const noexcept { return runner != nullptr; }

  // The rows of the parameter table, listed again when the request or its settings change.
  amc::vector<LoadRow> rows;
  std::shared_ptr<PlanCompiler::Slot> slot{std::make_shared<PlanCompiler::Slot>()};
  // The last compiled plan, shown by the parameter table and sent by the next run.
  std::shared_ptr<const LoadPreview> preview;
  // The run in progress, released as soon as it is over.
  std::unique_ptr<LoadRunner> runner;
  // The state of the last run, never null once a run started.
  std::shared_ptr<const LoadSnapshot> snapshot;
  PerfCharts charts;
  // What the last run sent, for its export: the request may have changed since.
  LoadSettings runSettings;
  RawChars32 runName;
  RawChars32 runUrl;
  RawChars32 exportPath;
  // A file dropped on the tab, given to the parameter row under the mouse at the next frame.
  RawChars32 droppedPath;
  std::chrono::system_clock::time_point startedAt;
  // Limits of the last run, for its progress.
  std::uint64_t runMaxRequests{0};
  // The tab of an unsaved request (its request lives there), 0 for a saved request.
  std::uint64_t sourceTabUid{0};
  // App revisions the rows were listed for, the last job was submitted for, the preview was compiled for.
  std::uint64_t rowsRevision{0};
  std::uint64_t submittedRevision{0};
  std::uint64_t previewRevision{0};
  // The seed of the preview when the settings ask for a new seed per run: the run sends what the preview shows.
  std::uint64_t previewSeed{1};
  std::uint32_t runDurationMs{0};
  std::uint32_t selectedRow{0};
  Method runMethod{Method::get};
  // Run asked for: starts when the plan of the current request is compiled.
  bool runRequested{false};
  // The run needs a confirmation (other host than the local machine, or a method that may change the server).
  bool confirmRequested{false};
  bool logLatency{false};
};

}  // namespace zalie::ui
