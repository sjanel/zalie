#include "zalie/rate-controller.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "zalie/latency-histogram.hpp"

namespace zalie {

namespace {

// Response times above their baseline times these factors (plus the slack): the server slows down, or is overloaded.
constexpr double kSlowerP50Factor = 1.5;
constexpr double kSlowerP90Factor = 2.0;
constexpr double kOverloadedP50Factor = 3.0;
constexpr double kOverloadedP90Factor = 4.0;
// A p99 above this share of its cap holds the rate.
constexpr double kNearCapShare = 0.9;
// Below this share of the rate, the client limits it. Between it and kReachedShare, the rate is kept.
constexpr double kClientLimitedShare = 0.9;
constexpr double kReachedShare = 0.95;
// The rate kept when the client limits it, relative to what was reached: between the two shares above, so that it
// stays there while nothing changes.
constexpr double kClientLimitedMargin = 1.08;

[[nodiscard]] bool Above(std::uint64_t valueNs, std::uint64_t baseNs, double factor) noexcept {
  return static_cast<double>(valueNs) >
         (static_cast<double>(baseNs) * factor) + static_cast<double>(RateController::kLatencySlackNs);
}

}  // namespace

double RateController::StartRate(std::uint32_t minRate, std::uint32_t maxRate) noexcept {
  const double rate = minRate != 0 ? static_cast<double>(minRate) : kDefaultMinRate;
  return maxRate != 0 ? std::min(rate, static_cast<double>(maxRate)) : rate;
}

RateController::RateController(std::uint32_t minRate, std::uint32_t maxRate, std::uint64_t maxP99Ns) noexcept
    : _minRate(StartRate(minRate, maxRate)),
      _maxRate(maxRate != 0 ? std::min(static_cast<double>(maxRate), kHighestRate) : kHighestRate),
      _rate(_minRate),
      _maxP99Ns(maxP99Ns) {
  if (maxRate != 0) {
    // A known range: bisected rather than climbed, unless it is too narrow for that.
    _rampingUp = false;
    if (_maxRate > _minRate * kSearchPrecision) {
      _searchLow = _minRate;
      _searchHigh = _maxRate;
      _state = RateState::search;
    } else {
      _state = RateState::probe;
    }
  }
}

bool RateController::onSecond(const LatencyHistogram& latency, std::uint64_t requests, std::uint64_t failures,
                              double lateSeconds) noexcept {
  const double planned = _nextPlanned > 0.0 ? _nextPlanned : _rate;
  _nextPlanned = 0.0;
  if (_settling) {
    _settling = false;
    return false;
  }
  _window.merge(latency);
  _windowRequests += requests;
  _windowFailures += failures;
  _windowPlanned += planned;
  ++_windowSeconds;
  if (_windowRequests < kMinWindowRequests && _windowSeconds < kMaxWindowSeconds) {
    return false;
  }
  const bool changed = evaluate(std::clamp(lateSeconds, 0.0, 1.0));
  _window = LatencyHistogram{};
  _windowRequests = 0;
  _windowFailures = 0;
  _windowPlanned = 0.0;
  _windowSeconds = 0;
  return changed;
}

RateController::Verdict RateController::judge() const noexcept {
  if (_window.count() == 0) {
    return Verdict::failing;  // no response for the whole measure
  }
  const double failureShare = static_cast<double>(_windowFailures) / static_cast<double>(_windowRequests);
  const double failureLimit =
      _healthyRequests == 0
          ? kMaxFailureShare
          : std::min(kMaxFailureShare, (static_cast<double>(_healthyFailures) / static_cast<double>(_healthyRequests)) +
                                           kFailureTolerance);
  if (_windowFailures >= kMinFailures && failureShare > failureLimit) {
    return Verdict::failing;
  }
  const std::uint64_t p99Ns = _window.percentileNs(99.0);
  if (_maxP99Ns != 0 && p99Ns > _maxP99Ns) {
    return Verdict::overloaded;
  }
  if (_healthyRequests != 0) {
    const std::uint64_t p50Ns = _window.percentileNs(50.0);
    const std::uint64_t p90Ns = _window.percentileNs(90.0);
    if (Above(p50Ns, _baseP50Ns, kOverloadedP50Factor) || Above(p90Ns, _baseP90Ns, kOverloadedP90Factor)) {
      return Verdict::overloaded;
    }
    if (Above(p50Ns, _baseP50Ns, kSlowerP50Factor) || Above(p90Ns, _baseP90Ns, kSlowerP90Factor)) {
      return Verdict::slower;
    }
  }
  if (_maxP99Ns != 0 && static_cast<double>(p99Ns) > kNearCapShare * static_cast<double>(_maxP99Ns)) {
    return Verdict::slower;
  }
  return Verdict::healthy;
}

bool RateController::evaluate(double lateSeconds) noexcept {
  const Verdict verdict = judge();
  if (verdict != Verdict::healthy) {
    // Indexed by Verdict.
    static constexpr std::array kStates{RateState::probe, RateState::hold, RateState::backOff, RateState::failing};
    static constexpr std::array kFactors{1.0, 1.0, kSlowBackOffFactor, kFastBackOffFactor};
    _rampingUp = false;
    _state = kStates[static_cast<std::size_t>(verdict)];
    if (_searchHigh > 0.0) {
      // Too high for the server: the new upper bound of the bisection.
      const double low = _searchLow;
      _searchHigh = _rate;
      if (_searchHigh > low * kSearchPrecision) {
        _state = RateState::search;
        return setRate(std::sqrt(low * _searchHigh), lateSeconds);
      }
      endSearch();
      return setRate(low, lateSeconds);
    }
    return setRate(_rate * kFactors[static_cast<std::size_t>(verdict)], lateSeconds);
  }
  const std::uint64_t p50Ns = _window.percentileNs(50.0);
  const std::uint64_t p90Ns = _window.percentileNs(90.0);
  _baseP50Ns = _healthyRequests == 0 ? p50Ns : std::min(_baseP50Ns, p50Ns);
  _baseP90Ns = _healthyRequests == 0 ? p90Ns : std::min(_baseP90Ns, p90Ns);
  _healthyRequests += _windowRequests;
  _healthyFailures += _windowFailures;
  const double reached = static_cast<double>(_windowRequests) / static_cast<double>(_windowSeconds);
  _sustainedRate = std::max(_sustainedRate, reached);
  const double reachedShare = static_cast<double>(_windowRequests) / _windowPlanned;
  if (reachedShare < kClientLimitedShare) {
    endSearch();  // the connections cannot go higher
    _state = RateState::clientLimited;
    return setRate(reached * kClientLimitedMargin, lateSeconds);
  }
  if (reachedShare < kReachedShare) {
    return false;
  }
  if (_searchHigh > 0.0) {
    // Healthy: the new lower bound of the bisection.
    _searchLow = _rate;
    if (_searchHigh > _searchLow * kSearchPrecision) {
      _state = RateState::search;
      return setRate(std::sqrt(_searchLow * _searchHigh), lateSeconds);
    }
    endSearch();  // probing goes on from here
  }
  if (_rate >= _maxRate) {
    _state = RateState::ceiling;
    return false;
  }
  _state = _rampingUp ? RateState::rampUp : RateState::probe;
  return setRate(_rampingUp ? _rate * kRampUpFactor : std::max(_rate * kProbeFactor, _rate + 1.0), lateSeconds);
}

bool RateController::setRate(double rate, double lateSeconds) noexcept {
  const double clamped = std::clamp(rate, _minRate, _maxRate);
  if (clamped == _rate) {
    return false;
  }
  if (clamped < _rate) {
    _settling = true;  // the next second would still show the load of the higher rate
  } else {
    // The next second starts at the previous rate: judged against both.
    _nextPlanned = (_rate * lateSeconds) + (clamped * (1.0 - lateSeconds));
  }
  _rate = clamped;
  return true;
}

}  // namespace zalie
