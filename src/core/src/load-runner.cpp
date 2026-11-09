#include "zalie/load-runner.hpp"

#include <aeronet/aeronet-client.hpp>
#include <algorithm>
#include <amc/vector.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>

#include "http-client-setup.hpp"
#include "zalie/latency-histogram.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/resolved-request.hpp"

// After the aeronet headers: windows.h defines DELETE, a member of http::Method (http-method.hpp undefines it).
#ifdef _WIN32
#include <windows.h>
#elif defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#include <sys/time.h>
#endif

namespace zalie {

namespace {

using std::chrono::steady_clock;
using TimePoint = steady_clock::time_point;

constexpr std::chrono::milliseconds kCollectPeriod{100};
// A point of the time series is closed this long after its end, to wait for the statistics of all the workers.
constexpr std::chrono::milliseconds kSecondGrace{250};
constexpr std::size_t kMaxSeriesSeconds = 3600;

// How far behind its schedule the rate cap may fall and still catch up: it absorbs the imprecision of the sleeps
// without letting a stall turn into a burst.
constexpr std::int64_t kRateToleranceNs = 10'000'000;
// Longest sleep of a worker waiting for its turn: bounds the time to react to a stop.
constexpr std::int64_t kMaxSlotSleepNs = 10'000'000;

constexpr int kMinStatus = 100;
constexpr int kMaxStatus = 999;
constexpr std::size_t kNbStatuses = kMaxStatus - kMinStatus + 1;
constexpr std::size_t kErrorClass = static_cast<std::size_t>(LoadClass::error);
constexpr std::uint32_t kNoLastSecond = std::numeric_limits<std::uint32_t>::max();

// Above this load the client is considered as the bottleneck (the threshold of the aeronet benchmarks).
constexpr double kSaturatedClientLoad = 0.9;

constexpr std::string_view kUnexpectedExceptionMessage = "unexpected exception while sending the request";

[[nodiscard]] std::uint32_t NbVCpus() noexcept { return std::max(std::thread::hardware_concurrency(), 1U); }

[[nodiscard]] std::int64_t ToNs(steady_clock::duration duration) noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
}

[[nodiscard]] double ToSeconds(steady_clock::duration duration) noexcept {
  return std::chrono::duration<double>(duration).count();
}

// User + system CPU time consumed so far by all the threads of this process (0 where unsupported).
[[nodiscard]] double ProcessCpuSeconds() noexcept {
#ifdef _WIN32
  FILETIME creationTime{};
  FILETIME exitTime{};
  FILETIME kernelTime{};
  FILETIME userTime{};
  if (GetProcessTimes(GetCurrentProcess(), &creationTime, &exitTime, &kernelTime, &userTime) != 0) {
    const auto seconds = [](const FILETIME& time) {  // in units of 100 ns
      return 1e-7 * static_cast<double>((static_cast<std::uint64_t>(time.dwHighDateTime) << 32U) | time.dwLowDateTime);
    };
    return seconds(kernelTime) + seconds(userTime);
  }
#elif defined(__unix__) || defined(__APPLE__)
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) == 0) {
    const auto seconds = [](const timeval& time) {
      return static_cast<double>(time.tv_sec) + (1e-6 * static_cast<double>(time.tv_usec));
    };
    return seconds(usage.ru_utime) + seconds(usage.ru_stime);
  }
#endif
  return 0.0;
}

[[nodiscard]] LoadClass ClassOf(int status) noexcept {
  if (status >= 500) {
    return LoadClass::status5xx;
  }
  if (status >= 400) {
    return LoadClass::status4xx;
  }
  if (status >= 300) {
    return LoadClass::status3xx;
  }
  return LoadClass::status2xx;
}

[[nodiscard]] LoadErrorKind KindOf(aeronet::HttpClientErrc errc) noexcept {
  switch (errc) {
    case aeronet::HttpClientErrc::connectFailed:
      return LoadErrorKind::connection;
    case aeronet::HttpClientErrc::tlsError:
      return LoadErrorKind::tls;
    case aeronet::HttpClientErrc::timeout:
      return LoadErrorKind::timeout;
    case aeronet::HttpClientErrc::writeError:
    case aeronet::HttpClientErrc::connectionClosed:
      return LoadErrorKind::closed;
    case aeronet::HttpClientErrc::malformedResponse:
    case aeronet::HttpClientErrc::protocolUnsupported:
      return LoadErrorKind::malformed;
    case aeronet::HttpClientErrc::responseTooLarge:
      return LoadErrorKind::tooLarge;
    default:
      return LoadErrorKind::other;
  }
}

[[nodiscard]] LatencySummary Summarize(const LatencyHistogram& histogram) noexcept {
  return {.count = histogram.count(),
          .minNs = histogram.minNs(),
          .p50Ns = histogram.percentileNs(50.0),
          .p90Ns = histogram.percentileNs(90.0),
          .p95Ns = histogram.percentileNs(95.0),
          .p99Ns = histogram.percentileNs(99.0),
          .p999Ns = histogram.percentileNs(99.9),
          .maxNs = histogram.maxNs(),
          .meanNs = histogram.meanNs(),
          .stddevNs = histogram.stddevNs()};
}

// Only follows the response: no copy and no analysis, so that the run measures the server rather than Zalie.
class CountingSink final : public aeronet::ResponseBodySink {
 public:
  explicit CountingSink(const std::atomic<bool>& stop) noexcept : _stop(stop) {}

  void reset() noexcept {
    _bodyBytes = 0;
    _status = 0;
  }

  bool onHead(const aeronet::HttpResponse& head, const aeronet::ResponseBodyInfo& /*body*/) override {
    _headAt = steady_clock::now();
    _status = head.status();
    return !_stop.load(std::memory_order_relaxed);
  }

  bool onBody(std::string_view chunk) override {
    _bodyBytes += chunk.size();
    return !_stop.load(std::memory_order_relaxed);
  }

  [[nodiscard]] bool headSeen() const noexcept { return _status != 0; }
  [[nodiscard]] int status() const noexcept { return _status; }
  [[nodiscard]] TimePoint headAt() const noexcept { return _headAt; }
  [[nodiscard]] std::uint64_t bodyBytes() const noexcept { return _bodyBytes; }

 private:
  const std::atomic<bool>& _stop;
  TimePoint _headAt;
  std::uint64_t _bodyBytes{0};
  int _status{0};
};

// What became of one request.
struct SendResult {
  std::string_view errorMessage;
  // 0 when no response was received.
  int status{0};
  LoadErrorKind errorKind{LoadErrorKind::other};
  // Interrupted by the end of the run: not to be counted.
  bool stopped{false};
};

[[nodiscard]] SendResult Send(aeronet::HttpClient& client, const aeronet::HttpRequest& request, CountingSink& sink) {
  SendResult ret;
  try {
    const aeronet::HttpClientResult result = client.request(request, sink);
    if (result) {
      ret.status = sink.headSeen() ? sink.status() : static_cast<int>(result->status());
    } else if (result.error() == aeronet::HttpClientErrc::aborted) {
      ret.stopped = true;
    } else {
      ret.errorKind = KindOf(result.error());
      ret.errorMessage = aeronet::ErrcToStr(result.error());
    }
  } catch (const std::exception&) {
    ret.errorMessage = kUnexpectedExceptionMessage;
  }
  return ret;
}

// The statistics a worker accumulates between two publications.
struct StatsBlock {
  void merge(const StatsBlock& other) noexcept {
    headLatency.merge(other.headLatency);
    for (std::size_t idx = 0; idx < kNbLoadClasses; ++idx) {
      totalLatency[idx].merge(other.totalLatency[idx]);
    }
    for (std::size_t idx = 0; idx < kNbStatuses; ++idx) {
      statuses[idx] += other.statuses[idx];
    }
    for (std::size_t idx = 0; idx < kNbLoadErrorKinds; ++idx) {
      errors[idx] += other.errors[idx];
      if (firstMessages[idx].empty()) {
        firstMessages[idx] = other.firstMessages[idx];
      }
    }
    bodyBytes += other.bodyBytes;
    minBodyBytes = std::min(minBodyBytes, other.minBodyBytes);
    maxBodyBytes = std::max(maxBodyBytes, other.maxBodyBytes);
    nbRecorded += other.nbRecorded;
  }

  LatencyHistogram headLatency;
  std::array<LatencyHistogram, kNbLoadClasses> totalLatency;
  std::array<std::uint64_t, kNbStatuses> statuses{};
  std::array<std::uint64_t, kNbLoadErrorKinds> errors{};
  std::array<std::string_view, kNbLoadErrorKinds> firstMessages{};
  std::uint64_t bodyBytes{0};
  std::uint64_t minBodyBytes{std::numeric_limits<std::uint64_t>::max()};
  std::uint64_t maxBodyBytes{0};
  std::uint64_t nbRecorded{0};
  // The second of the time series of these statistics.
  std::uint32_t second{0};
};

// One connection: a thread and its statistics, handed over to the collector without locking. '*pShared' belongs to the
// collector while 'sharedFull' is true, and to the worker otherwise: the worker never waits for the collector, it just
// goes on accumulating when the previous block was not picked up yet.
struct Worker {
  std::array<StatsBlock, 2> blocks;
  StatsBlock* pActive{blocks.data()};
  StatsBlock* pShared{blocks.data() + 1};
  TimePoint nextPublish;
  TimePoint exitedAt;
  std::atomic<bool> sharedFull{false};
  std::atomic<bool> inFlight{false};
  std::jthread thread;
};

// A second of the time series still receiving statistics.
struct OpenSecond {
  LatencyHistogram latency;
  std::array<std::uint64_t, kNbLoadClasses> counts{};
  std::uint64_t bodyBytes{0};
  std::uint32_t inFlight{0};
};

}  // namespace

std::uint32_t LoadConnections(const LoadOptions& options) noexcept {
  const std::uint32_t vCpus = NbVCpus();
  const std::uint32_t limit = options.maxConnections != 0 ? options.maxConnections : vCpus;
  return std::min(options.connections != 0 ? options.connections : vCpus, limit);
}

class LoadRunner::Impl {
 public:
  Impl(ResolvedRequest request, const LoadOptions& options)
      : _request(std::move(request)),
        _options(options),
        _slotIntervalNs(options.maxRequestsPerSecond != 0 ? 1'000'000'000LL / options.maxRequestsPerSecond : 0),
        _nbConnections(LoadConnections(options)),
        _snapshot(std::make_shared<const LoadSnapshot>()),
        _pTotals(std::make_unique<StatsBlock>()),
        _pOpenSeconds(std::make_unique<std::array<OpenSecond, 2>>()) {
    _collector = std::jthread([this] { collectorMain(); });
    spawnWorkers();
  }

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(Impl&&) = delete;

  ~Impl() {
    stop();
    _collector.join();
    _workers.clear();  // joins
  }

  void stop() {
    std::scoped_lock lock(_mutex);
    if (_stopRequestedAt == TimePoint::max()) {
      _stopRequestedAt = steady_clock::now();
    }
    _stop.store(true, std::memory_order_relaxed);
  }

  [[nodiscard]] std::shared_ptr<const LoadSnapshot> snapshot() const {
    std::scoped_lock lock(_snapshotMutex);
    return _snapshot;
  }

  bool waitForCompletion(std::chrono::milliseconds timeout) {
    std::unique_lock lock(_mutex);
    return _condition.wait_for(lock, timeout, [this] { return _finished; });
  }

 private:
  void spawnWorkers() {
    try {
      _workers.reserve(_nbConnections);
      for (std::uint32_t idx = 0; idx < _nbConnections; ++idx) {
        _workers.push_back(std::make_unique<Worker>());
        Worker& worker = *_workers.back();
        worker.thread = std::jthread([this, &worker, idx] { workerMain(worker, idx == 0); });
      }
    } catch (const std::exception& ex) {
      if (!_workers.empty() && !_workers.back()->thread.joinable()) {
        _workers.pop_back();
      }
      reportSetupError(ex.what());
    }
    {
      std::scoped_lock lock(_mutex);
      _spawnDone = true;
    }
    _condition.notify_all();
  }

  void reportSetupError(std::string_view message) {
    std::scoped_lock lock(_mutex);
    if (_setupError.empty()) {
      _setupError = RawChars32(message);
    }
    if (_stopRequestedAt == TimePoint::max()) {
      _stopRequestedAt = steady_clock::now();
    }
    _stop.store(true, std::memory_order_relaxed);
  }

  // ------------------------------------------- Worker threads ------------------------------------------- //

  void workerMain(Worker& worker, bool reportWarnings) {
    std::optional<aeronet::HttpClient> client;
    std::optional<aeronet::HttpRequest> request;
    try {
      client.emplace(MakeConfig(KeyOf(_request.settings), _options.maxBodyBytes));
      request.emplace(client->makeRequest(ToAeronet(_request.method), _request.url));
      AddHeaders(_request, *request, [this, reportWarnings](std::string_view reason, std::string_view name) {
        if (reportWarnings) {
          std::scoped_lock lock(_mutex);
          _warnings.push_back(Concat({reason, name}));
        }
      });
      if (!_request.body.empty()) {
        const std::string_view contentType = _request.headerValue("content-type");
        // The body stays owned by this object for the whole run: no copy.
        request->bodyStatic(std::string_view(_request.body), contentType.empty() ? "text/plain" : contentType);
      }
    } catch (const std::exception& ex) {
      request.reset();
      reportSetupError(ex.what());
    }
    waitForStart();
    if (request) {
      sendLoop(worker, *client, *request);
    }
    worker.exitedAt = steady_clock::now();
    if (_nbRunning.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      {
        std::scoped_lock lock(_mutex);  // the collector cannot miss the notification between its check and its wait
      }
      _condition.notify_all();
    }
  }

  // Every worker passes here, set up or not, so that the collector knows when all of them are ready.
  void waitForStart() {
    std::unique_lock lock(_mutex);
    ++_nbReady;
    _condition.notify_all();
    _condition.wait(lock, [this] { return _started; });
  }

  void sendLoop(Worker& worker, aeronet::HttpClient& client, const aeronet::HttpRequest& request) {
    CountingSink sink(_stop);
    while (!_stop.load(std::memory_order_relaxed)) {
      const std::uint64_t number = _nextRequest.fetch_add(1, std::memory_order_relaxed);
      if (_options.maxRequests != 0 && number >= _options.maxRequests) {
        return;
      }
      if (_slotIntervalNs != 0 && !waitForSlot()) {
        return;
      }
      const TimePoint begin = steady_clock::now();  // after the wait: a chosen pause is not a latency
      if (begin >= _deadline) {
        return;
      }
      sink.reset();
      worker.inFlight.store(true, std::memory_order_relaxed);
      const SendResult result = Send(client, request, sink);
      worker.inFlight.store(false, std::memory_order_relaxed);
      const TimePoint end = steady_clock::now();
      if (!result.stopped && begin >= _measureStart) {
        record(worker, result, sink, begin, end);
      }
    }
  }

  // Waits for the next free send slot of the rate cap, shared by the workers. Returns false if the run ends meanwhile.
  // The n-th request cannot start before (n - 1) intervals, whatever the tolerance: the cap holds since the start.
  [[nodiscard]] bool waitForSlot() {
    const std::int64_t nowNs = ToNs(steady_clock::now() - _start);
    std::int64_t next = _nextSlotNs.load(std::memory_order_relaxed);
    std::int64_t slot = 0;
    do {
      slot = std::max(next, nowNs - kRateToleranceNs);
    } while (!_nextSlotNs.compare_exchange_weak(next, slot + _slotIntervalNs, std::memory_order_relaxed));
    while (!_stop.load(std::memory_order_relaxed)) {
      const std::int64_t remainingNs = slot - ToNs(steady_clock::now() - _start);
      if (remainingNs <= 0) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::nanoseconds(std::min(remainingNs, kMaxSlotSleepNs)));
    }
    return false;
  }

  void record(Worker& worker, const SendResult& result, const CountingSink& sink, TimePoint begin, TimePoint end) {
    const auto second =
        static_cast<std::uint32_t>(std::chrono::floor<std::chrono::seconds>(end - _measureStart).count());
    if (second != worker.pActive->second) {
      Publish(worker, end);  // a block holds one second of the time series
    }
    StatsBlock& block = *worker.pActive;
    if (block.nbRecorded == 0) {
      block.second = second;
    }
    ++block.nbRecorded;
    const auto totalNs = static_cast<std::uint64_t>(ToNs(end - begin));
    if (result.status != 0) {
      block.totalLatency[static_cast<std::size_t>(ClassOf(result.status))].record(totalNs);
      block.headLatency.record(sink.headSeen() ? static_cast<std::uint64_t>(ToNs(sink.headAt() - begin)) : totalNs);
      ++block.statuses[static_cast<std::size_t>(std::clamp(result.status, kMinStatus, kMaxStatus) - kMinStatus)];
      block.bodyBytes += sink.bodyBytes();
      block.minBodyBytes = std::min(block.minBodyBytes, sink.bodyBytes());
      block.maxBodyBytes = std::max(block.maxBodyBytes, sink.bodyBytes());
    } else {
      block.totalLatency[kErrorClass].record(totalNs);
      const auto kind = static_cast<std::size_t>(result.errorKind);
      ++block.errors[kind];
      if (block.firstMessages[kind].empty()) {
        block.firstMessages[kind] = result.errorMessage;
      }
    }
    if (end >= worker.nextPublish) {
      Publish(worker, end);
    }
  }

  // Hands the active block to the collector, unless it did not pick up the previous one yet.
  static void Publish(Worker& worker, TimePoint now) noexcept {
    if (worker.pActive->nbRecorded == 0 || worker.sharedFull.load(std::memory_order_acquire)) {
      return;
    }
    std::swap(worker.pActive, worker.pShared);
    worker.sharedFull.store(true, std::memory_order_release);
    worker.nextPublish = now + kCollectPeriod;
  }

  // ------------------------------------------- Collector thread ------------------------------------------- //

  void collectorMain() {
    startRun();
    while (true) {
      TimePoint now = steady_clock::now();
      if (!_cpuBase && now >= _measureStart) {
        _cpuBase = ProcessCpuSeconds();
      }
      TimePoint wake = now + kCollectPeriod;
      if (now < _measureStart) {
        wake = std::min(wake, _measureStart);
      }
      if (now < _deadline) {
        wake = std::min(wake, _deadline);
      }
      TimePoint stopRequestedAt;
      {
        std::unique_lock lock(_mutex);
        _condition.wait_until(lock, wake, [this] { return _nbRunning.load(std::memory_order_relaxed) == 0; });
        stopRequestedAt = _stopRequestedAt;
      }
      now = steady_clock::now();
      if (now >= _deadline) {
        _stop.store(true, std::memory_order_relaxed);
      }
      if (_nbRunning.load(std::memory_order_acquire) == 0) {
        break;
      }
      if (stopRequestedAt != TimePoint::max()) {
        limitLastSecond(stopRequestedAt);
      }
      for (const std::unique_ptr<Worker>& worker : _workers) {
        collectShared(*worker);
      }
      sampleInFlight(now);
      closeElapsedSeconds(now);
      publishSnapshot(makeSnapshot(phaseAt(now), measuredSeconds(now)));
    }
    finishRun();
  }

  // Waits for all the workers to be set up, so that the time of the run does not include their creation.
  void startRun() {
    {
      std::unique_lock lock(_mutex);
      _condition.wait(lock, [this] { return _spawnDone && _nbReady == _workers.size(); });
      _start = steady_clock::now();
      _measureStart = _start + std::chrono::milliseconds(_options.warmupMs);
      if (_options.durationMs != 0) {
        _deadline = _start + std::chrono::milliseconds(_options.durationMs);
        const std::uint32_t measuredMs = _options.durationMs - std::min(_options.durationMs, _options.warmupMs);
        _lastSecond = measuredMs == 0 ? 0 : (measuredMs - 1) / 1000;
      }
      _nbRunning.store(static_cast<std::uint32_t>(_workers.size()), std::memory_order_relaxed);
      _started = true;
    }
    _condition.notify_all();
  }

  void collectShared(Worker& worker) {
    if (worker.sharedFull.load(std::memory_order_acquire)) {
      absorb(*worker.pShared);
      *worker.pShared = StatsBlock{};
      worker.sharedFull.store(false, std::memory_order_release);
    }
  }

  void absorb(const StatsBlock& block) {
    if (block.nbRecorded == 0) {
      return;
    }
    _pTotals->merge(block);
    OpenSecond& open = openSecond(std::min(block.second, _lastSecond));
    for (std::size_t idx = 0; idx < kNbLoadClasses; ++idx) {
      open.counts[idx] += block.totalLatency[idx].count();
      if (idx != kErrorClass) {
        open.latency.merge(block.totalLatency[idx]);
      }
    }
    open.bodyBytes += block.bodyBytes;
  }

  // The open point of 'second', closing the ones it leaves behind. A statistic arriving too late for its point (a
  // worker stuck in a slow response) goes to the oldest point still open.
  [[nodiscard]] OpenSecond& openSecond(std::uint32_t second) {
    while (second > _oldestOpen + 1) {
      closeOldest(1.0F);
    }
    return (*_pOpenSeconds)[second <= _oldestOpen ? 0 : 1];
  }

  void closeOldest(float seconds) {
    std::array<OpenSecond, 2>& open = *_pOpenSeconds;
    LoadSecond point{.counts = open[0].counts,
                     .bodyBytes = open[0].bodyBytes,
                     .p50Ns = open[0].latency.percentileNs(50.0),
                     .p90Ns = open[0].latency.percentileNs(90.0),
                     .p99Ns = open[0].latency.percentileNs(99.0),
                     .maxNs = open[0].latency.maxNs(),
                     .seconds = seconds,
                     .inFlight = open[0].inFlight};
    if (_series.size() == kMaxSeriesSeconds) {
      _series.erase(_series.begin());
    }
    _series.push_back(point);
    if (seconds >= 1.0F) {
      _lastFullSecondRate = static_cast<double>(point.requests());
      _peakRate = std::max(_peakRate, *_lastFullSecondRate);
    }
    open[0] = open[1];
    open[1] = OpenSecond{};
    ++_oldestOpen;
  }

  void closeElapsedSeconds(TimePoint now) {
    while (_oldestOpen < _lastSecond && now >= _measureStart + std::chrono::seconds(_oldestOpen + 1) + kSecondGrace) {
      closeOldest(1.0F);
    }
  }

  void sampleInFlight(TimePoint now) {
    if (now < _measureStart) {
      return;
    }
    std::uint32_t inFlight = 0;
    for (const std::unique_ptr<Worker>& worker : _workers) {
      inFlight += worker->inFlight.load(std::memory_order_relaxed) ? 1U : 0U;
    }
    _inFlight = inFlight;
    OpenSecond& open = openSecond(std::min(secondAt(now), _lastSecond));
    open.inFlight = std::max(open.inFlight, inFlight);
  }

  // Requests still arriving after the end of the run belong to its last point.
  void limitLastSecond(TimePoint end) { _lastSecond = std::min(_lastSecond, secondAt(end)); }

  [[nodiscard]] std::uint32_t secondAt(TimePoint when) const noexcept {
    return when <= _measureStart
               ? 0U
               : static_cast<std::uint32_t>(std::chrono::floor<std::chrono::seconds>(when - _measureStart).count());
  }

  [[nodiscard]] double measuredSeconds(TimePoint when) const noexcept {
    const TimePoint end = std::min(when, _deadline);
    return end <= _measureStart ? 0.0 : ToSeconds(end - _measureStart);
  }

  [[nodiscard]] LoadPhase phaseAt(TimePoint now) const noexcept {
    if (_stop.load(std::memory_order_relaxed)) {
      return LoadPhase::stopping;
    }
    return now < _measureStart ? LoadPhase::warmup : LoadPhase::running;
  }

  // All the workers have ended: nothing races with this function any more.
  void finishRun() {
    TimePoint stopRequestedAt;
    {
      std::scoped_lock lock(_mutex);
      stopRequestedAt = _stopRequestedAt;
    }
    TimePoint lastExit = _start;
    for (const std::unique_ptr<Worker>& worker : _workers) {
      lastExit = std::max(lastExit, worker->exitedAt);
    }
    const TimePoint end = std::min(stopRequestedAt, lastExit);
    limitLastSecond(end);
    for (const std::unique_ptr<Worker>& worker : _workers) {
      collectShared(*worker);
      absorb(*worker->pActive);
    }
    _inFlight = 0;
    const double elapsed = measuredSeconds(end);
    if (elapsed > 0.0) {
      while (_oldestOpen <= _lastSecond) {
        closeOldest(std::clamp(static_cast<float>(elapsed - _oldestOpen), 0.001F, 1.0F));
      }
    }
    publishSnapshot(makeSnapshot(LoadPhase::finished, elapsed));
    {
      std::scoped_lock lock(_mutex);
      _finished = true;
    }
    _condition.notify_all();
  }

  [[nodiscard]] std::shared_ptr<const LoadSnapshot> makeSnapshot(LoadPhase phase, double elapsed) const {
    const StatsBlock& totals = *_pTotals;
    auto snapshot = std::make_shared<LoadSnapshot>();
    snapshot->phase = phase;
    snapshot->elapsedSeconds = elapsed;
    snapshot->connections = static_cast<std::uint32_t>(_workers.size());
    snapshot->inFlight = _inFlight;
    snapshot->series = _series;
    snapshot->firstSecond = _oldestOpen - static_cast<std::uint32_t>(_series.size());
    {
      std::scoped_lock lock(_mutex);
      snapshot->warnings = _warnings;
      snapshot->setupError = _setupError;
    }
    snapshot->headHistogram = totals.headLatency;
    snapshot->head = Summarize(totals.headLatency);
    for (std::size_t idx = 0; idx < kNbLoadClasses; ++idx) {
      const LatencyHistogram& histogram = totals.totalLatency[idx];
      snapshot->classHistograms[idx] = histogram;
      snapshot->byClass[idx] = Summarize(histogram);
      snapshot->counts[idx] = histogram.count();
      if (idx != kErrorClass) {
        snapshot->totalHistogram.merge(histogram);
      }
    }
    snapshot->total = Summarize(snapshot->totalHistogram);
    for (std::size_t idx = 0; idx < kNbStatuses; ++idx) {
      if (totals.statuses[idx] != 0) {
        snapshot->statuses.push_back(
            {.count = totals.statuses[idx], .status = static_cast<std::int16_t>(static_cast<int>(idx) + kMinStatus)});
      }
    }
    for (std::size_t idx = 0; idx < kNbLoadErrorKinds; ++idx) {
      snapshot->errors[idx] = {.count = totals.errors[idx], .firstMessage = totals.firstMessages[idx]};
    }
    snapshot->bodyBytes = totals.bodyBytes;
    snapshot->minBodyBytes = totals.minBodyBytes == std::numeric_limits<std::uint64_t>::max() ? 0 : totals.minBodyBytes;
    snapshot->maxBodyBytes = totals.maxBodyBytes;
    if (elapsed > 0.0) {
      snapshot->meanRequestsPerSecond = static_cast<double>(totals.nbRecorded) / elapsed;
      snapshot->bodyBytesPerSecond = static_cast<double>(totals.bodyBytes) / elapsed;
      if (_cpuBase) {
        snapshot->cpuSeconds = ProcessCpuSeconds() - *_cpuBase;
        const double usableCpus = std::min<double>(NbVCpus(), std::max<double>(snapshot->connections, 1.0));
        snapshot->clientLoad = snapshot->cpuSeconds / (elapsed * usableCpus);
        snapshot->clientSaturated = snapshot->clientLoad >= kSaturatedClientLoad;
      }
    }
    snapshot->currentRequestsPerSecond = _lastFullSecondRate.value_or(snapshot->meanRequestsPerSecond);
    snapshot->peakRequestsPerSecond = std::max(_peakRate, snapshot->meanRequestsPerSecond);
    return snapshot;
  }

  void publishSnapshot(std::shared_ptr<const LoadSnapshot> snapshot) {
    {
      std::scoped_lock lock(_snapshotMutex);
      _snapshot = std::move(snapshot);
    }
    if (_options.wakeFn != nullptr) {
      _options.wakeFn(_options.pWakeUserData);
    }
  }

  // Immutable during the run.
  ResolvedRequest _request;
  LoadOptions _options;
  std::int64_t _slotIntervalNs;
  std::uint32_t _nbConnections;

  // Written by the collector before the workers are released, constant afterwards.
  TimePoint _start;
  TimePoint _measureStart;
  TimePoint _deadline{TimePoint::max()};

  // Contended by nature: kept apart from each other and from the read-mostly fields.
  alignas(64) std::atomic<std::uint64_t> _nextRequest{0};
  alignas(64) std::atomic<std::int64_t> _nextSlotNs{0};
  alignas(64) std::atomic<bool> _stop{false};
  std::atomic<std::uint32_t> _nbRunning{0};

  // Guards what follows, and the start, the stop and the end of the run.
  mutable std::mutex _mutex;
  std::condition_variable _condition;
  amc::vector<RawChars32> _warnings;
  RawChars32 _setupError;
  TimePoint _stopRequestedAt{TimePoint::max()};
  std::size_t _nbReady{0};
  bool _spawnDone{false};
  bool _started{false};
  bool _finished{false};

  mutable std::mutex _snapshotMutex;
  std::shared_ptr<const LoadSnapshot> _snapshot;

  // Only used by the collector thread.
  std::unique_ptr<StatsBlock> _pTotals;
  std::unique_ptr<std::array<OpenSecond, 2>> _pOpenSeconds;
  amc::vector<LoadSecond> _series;
  std::optional<double> _cpuBase;
  std::optional<double> _lastFullSecondRate;
  double _peakRate{0.0};
  std::uint32_t _oldestOpen{0};
  std::uint32_t _lastSecond{kNoLastSecond};
  std::uint32_t _inFlight{0};

  amc::vector<std::unique_ptr<Worker>> _workers;
  std::jthread _collector;
};

LoadRunner::LoadRunner(ResolvedRequest request, const LoadOptions& options)
    : _impl(std::make_unique<Impl>(std::move(request), options)) {}

LoadRunner::~LoadRunner() = default;

void LoadRunner::stop() { _impl->stop(); }

std::shared_ptr<const LoadSnapshot> LoadRunner::snapshot() const { return _impl->snapshot(); }

bool LoadRunner::waitForCompletion(std::chrono::milliseconds timeout) { return _impl->waitForCompletion(timeout); }

}  // namespace zalie
