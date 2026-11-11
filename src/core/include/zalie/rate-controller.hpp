#pragma once

#include <cstdint>

#include "zalie/latency-histogram.hpp"

namespace zalie {

// What the adaptive rate of a run did at its last decision.
enum class RateState : std::uint8_t {
  // Doubles the rate after each healthy measure, until the first sign of load.
  rampUp,
  // Raises the rate by small steps while the server stays healthy.
  probe,
  // Keeps the rate: the response times rose clearly above their baseline, or close to the p99 cap.
  hold,
  // Lowered the rate: the server is overloaded (failures, no response, response times far above their baseline or
  // above the p99 cap).
  backOff,
  // The connections do not reach the rate: the client, not the server, limits it.
  clientLimited,
  // Healthy at the highest rate allowed.
  ceiling,
};

// Adapts the rate of a run, between a minimum and a maximum, to what the server sustains, like a congestion control.
// The minimum is assumed safe: the measures made there give the baseline of the response times (the lowest of the
// healthy measures) and of the share of failures (over all the healthy measures). Then the rate doubles while the
// server stays healthy, rises by small steps after the first sign of load, holds while the response times rise, and
// drops at once when the server is overloaded. Fed with the measures of one second at a time; a second mixing the
// previous rate and a new one is not judged.
class RateController {
 public:
  // Rate of a run whose minimum is 0, and highest rate of a run without maximum.
  static constexpr double kDefaultMinRate = 10.0;
  static constexpr double kHighestRate = 10'000'000.0;
  // A measure covers at least this many requests, or this many seconds at low rates.
  static constexpr std::uint64_t kMinWindowRequests = 30;
  static constexpr std::uint32_t kMaxWindowSeconds = 5;
  // Factors of the rate: after a healthy measure (ramping up, then probing), on overloaded response times, on failures.
  static constexpr double kRampUpFactor = 2.0;
  static constexpr double kProbeFactor = 1.05;
  static constexpr double kSlowBackOffFactor = 0.8;
  static constexpr double kFastBackOffFactor = 0.5;
  // Response times are compared to their baseline times a factor, plus this margin: the noise of a fast local server.
  static constexpr std::uint64_t kLatencySlackNs = 500'000;
  // Failures above their baseline share that overload the server. Over half of the requests failing always does.
  static constexpr double kFailureTolerance = 0.01;
  static constexpr double kMaxFailureShare = 0.5;
  static constexpr std::uint64_t kMinFailures = 3;

  // The rate a run starts from, and never goes below: 'minRate' (kDefaultMinRate if 0), at most 'maxRate' (0: none).
  [[nodiscard]] static double StartRate(std::uint32_t minRate, std::uint32_t maxRate) noexcept;

  // 'minRate' and 'maxRate' as for StartRate. 'maxP99Ns' 0: no cap on the p99 of the response times.
  RateController(std::uint32_t minRate, std::uint32_t maxRate, std::uint64_t maxP99Ns) noexcept;

  // Takes the measures of the next second: the times to the end of the body of the received responses, the requests
  // that ended in that second (refused values excluded), and how many of them failed (no response, 5xx, 429). Returns
  // true if the rate changed.
  bool onSecond(const LatencyHistogram& latency, std::uint64_t requests, std::uint64_t failures) noexcept;

  // The requests per second to send now.
  [[nodiscard]] double rate() const noexcept { return _rate; }

  [[nodiscard]] RateState state() const noexcept { return _state; }

  // Highest rate measured while the server was healthy. 0 before the first measure.
  [[nodiscard]] double sustainedRate() const noexcept { return _sustainedRate; }

  // The lowest median response time of a healthy measure. 0 before the first measure.
  [[nodiscard]] std::uint64_t baselineP50Ns() const noexcept { return _baseP50Ns; }

 private:
  enum class Verdict : std::uint8_t { healthy, slower, overloaded, failing };

  [[nodiscard]] Verdict judge() const noexcept;

  bool evaluate() noexcept;

  bool setRate(double rate) noexcept;

  // The measure in progress.
  LatencyHistogram _window;
  double _minRate;
  double _maxRate;
  double _rate;
  double _sustainedRate{0.0};
  std::uint64_t _maxP99Ns;
  std::uint64_t _baseP50Ns{0};
  std::uint64_t _baseP90Ns{0};
  // Sums of the healthy measures: the baseline share of failures.
  std::uint64_t _healthyRequests{0};
  std::uint64_t _healthyFailures{0};
  std::uint64_t _windowRequests{0};
  std::uint64_t _windowFailures{0};
  std::uint32_t _windowSeconds{0};
  RateState _state{RateState::rampUp};
  bool _rampingUp{true};
  // The next second mixes the previous rate and the new one.
  bool _settling{false};
};

}  // namespace zalie
