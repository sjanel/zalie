#pragma once

#include <amc/vector.hpp>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numeric>
#include <string_view>

#include "zalie/latency-histogram.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/resolved-request.hpp"

namespace zalie {

// Outcome of a request: the class of its status (1xx with 2xx and 6xx and above with 5xx, like the status pill of a
// response), or 'error' when no response was received.
enum class LoadClass : std::uint8_t { status2xx, status3xx, status4xx, status5xx, error };
inline constexpr std::size_t kNbLoadClasses = 5;

// Why a request got no response.
enum class LoadErrorKind : std::uint8_t { connection, tls, timeout, closed, malformed, tooLarge, other };
inline constexpr std::size_t kNbLoadErrorKinds = 7;

enum class LoadPhase : std::uint8_t { starting, warmup, running, stopping, finished };

// What a run sends, and when it ends. For the limits, 0 means none: the run ends at the first limit reached.
struct LoadOptions {
  using WakeFn = void (*)(void* pUserData);

  static constexpr std::size_t kDefaultMaxBodyBytes = 512UL * 1024UL * 1024UL;
  static constexpr std::uint32_t kDefaultDurationMs = 10000;

  // Called from the collector thread after each new snapshot (the app wakes its event loop with it).
  WakeFn wakeFn{nullptr};
  void* pWakeUserData{nullptr};
  // Limit of a decoded body: larger responses count as 'tooLarge' errors.
  std::size_t maxBodyBytes{kDefaultMaxBodyBytes};
  // Requests sent, warm-up included.
  std::uint64_t maxRequests{0};
  // Warm-up included.
  std::uint32_t durationMs{kDefaultDurationMs};
  std::uint32_t maxRequestsPerSecond{0};
  // First part of the run: the requests started during it are sent but not counted.
  std::uint32_t warmupMs{0};
  // Number of connections, each one is a thread. 0: the number of vCPUs.
  std::uint32_t connections{0};
  // Upper bound of 'connections'. 0: the number of vCPUs.
  std::uint32_t maxConnections{0};
};

// The number of connections a run with these options uses.
[[nodiscard]] std::uint32_t LoadConnections(const LoadOptions& options) noexcept;

// Key numbers of a latency histogram.
struct LatencySummary {
  std::uint64_t count{0};
  std::uint64_t minNs{0};
  std::uint64_t p50Ns{0};
  std::uint64_t p90Ns{0};
  std::uint64_t p95Ns{0};
  std::uint64_t p99Ns{0};
  std::uint64_t p999Ns{0};
  std::uint64_t maxNs{0};
  double meanNs{0.0};
  double stddevNs{0.0};
};

// One point of the time series. The latencies are the times to the end of the body of the received responses.
struct LoadSecond {
  [[nodiscard]] std::uint64_t requests() const noexcept {
    return std::accumulate(counts.begin(), counts.end(), std::uint64_t{0});
  }

  // Per class of outcome.
  std::array<std::uint64_t, kNbLoadClasses> counts{};
  std::uint64_t bodyBytes{0};
  std::uint64_t p50Ns{0};
  std::uint64_t p90Ns{0};
  std::uint64_t p99Ns{0};
  std::uint64_t maxNs{0};
  // Time covered by this point in seconds: 1, or less for the last point of a run.
  float seconds{1.0F};
  // Highest number of requests seen in flight.
  std::uint32_t inFlight{0};
};

struct LoadStatusCount {
  std::uint64_t count;
  std::int16_t status;
};

struct LoadErrorStats {
  std::uint64_t count{0};
  // Static storage. Empty when there was no error of this kind.
  std::string_view firstMessage;
};

// The state of a run at one instant, ready to draw. Immutable once published, and independent of the runner.
struct LoadSnapshot {
  [[nodiscard]] std::uint64_t requests() const noexcept {
    return std::accumulate(counts.begin(), counts.end(), std::uint64_t{0});
  }

  // One second per point, oldest first. A long run keeps the last hour only: see firstSecond.
  amc::vector<LoadSecond> series;
  // Sorted by status code.
  amc::vector<LoadStatusCount> statuses;
  // The headers of the request that are not sent, and why.
  amc::vector<RawChars32> warnings;
  // Set when the run could not start (invalid URL...).
  RawChars32 setupError;
  // Times to the response head and to the end of the body of the received responses. A failed connection is not a
  // response time: it only appears in classHistograms and byClass, under 'error'.
  LatencyHistogram headHistogram;
  LatencyHistogram totalHistogram;
  std::array<LatencyHistogram, kNbLoadClasses> classHistograms;
  LatencySummary head;
  LatencySummary total;
  std::array<LatencySummary, kNbLoadClasses> byClass;
  std::array<LoadErrorStats, kNbLoadErrorKinds> errors;
  // Requests counted so far, per class of outcome.
  std::array<std::uint64_t, kNbLoadClasses> counts{};
  // Decoded body bytes, and sizes of the bodies of the received responses.
  std::uint64_t bodyBytes{0};
  std::uint64_t minBodyBytes{0};
  std::uint64_t maxBodyBytes{0};
  // Time measured so far, warm-up excluded.
  double elapsedSeconds{0.0};
  double meanRequestsPerSecond{0.0};
  // Over the last complete second.
  double currentRequestsPerSecond{0.0};
  double peakRequestsPerSecond{0.0};
  double bodyBytesPerSecond{0.0};
  // CPU time of the whole process over the measured time. Stays 0 where unsupported.
  double cpuSeconds{0.0};
  // cpuSeconds relative to the CPUs the connections can use. Can exceed 1: the other threads of the process count too.
  double clientLoad{0.0};
  // Index since the end of the warm-up of the first point of 'series'.
  std::uint32_t firstSecond{0};
  std::uint32_t connections{0};
  std::uint32_t inFlight{0};
  LoadPhase phase{LoadPhase::starting};
  // The client, not the server, limits the throughput: the measures say little about the server.
  bool clientSaturated{false};
};

// Sends a request over and over from its own threads (the interactive executor stays available) in the closed model:
// each connection sends its next request as soon as the previous one completed. Bodies are counted, never kept.
// The workers publish their statistics without locking, a collector thread merges them about 10 times per second into
// a snapshot, which is all the UI reads: its cost does not depend on the number of connections.
class LoadRunner {
 public:
  // Starts the run.
  LoadRunner(ResolvedRequest request, const LoadOptions& options);

  LoadRunner(const LoadRunner&) = delete;
  LoadRunner& operator=(const LoadRunner&) = delete;
  LoadRunner(LoadRunner&&) = delete;
  LoadRunner& operator=(LoadRunner&&) = delete;

  // Stops the run and waits for its threads: the requests in flight end at their next received bytes, or their timeout.
  ~LoadRunner();

  // Ends the run: no new request is sent, the ones in flight end at their next received bytes and are not counted.
  void stop();

  // The latest state of the run. Never null, and cheap enough to be called every frame.
  [[nodiscard]] std::shared_ptr<const LoadSnapshot> snapshot() const;

  // Waits for the end of the run, or the timeout. Returns false on timeout.
  bool waitForCompletion(std::chrono::milliseconds timeout);

 private:
  class Impl;

  std::unique_ptr<Impl> _impl;
};

}  // namespace zalie
