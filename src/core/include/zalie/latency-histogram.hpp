#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace zalie {

// Log-linear (HDR-like) histogram of durations in nanoseconds, adapted from the benchmark harness of aeronet: 16 linear
// sub-buckets per power of two (about 6 % of error at most), fixed memory, histograms of several threads merge by
// addition, and recording costs a few instructions so that it does not disturb the measure.
class LatencyHistogram {
 public:
  static constexpr int kSubBits = 4;
  static constexpr int kSub = 1 << kSubBits;
  static constexpr int kGroups = 60;
  static constexpr int kNbBuckets = kGroups * kSub;

  // Lower bound of a bucket.
  [[nodiscard]] static constexpr std::uint64_t BucketLowerNs(int idx) noexcept {
    if (idx < kSub) {
      return static_cast<std::uint64_t>(idx);
    }
    const int group = idx / kSub;
    const int sub = idx % kSub;
    return (static_cast<std::uint64_t>(kSub) + static_cast<std::uint64_t>(sub)) << (group - 1);
  }

  // Number of distinct values of a bucket.
  [[nodiscard]] static constexpr std::uint64_t BucketWidthNs(int idx) noexcept {
    return idx < kSub ? 1U : std::uint64_t{1} << ((idx / kSub) - 1);
  }

  void record(std::uint64_t ns) noexcept {
    ++_counts[static_cast<std::size_t>(IndexOf(ns))];
    _sum += ns;
    _sumSquares += static_cast<double>(ns) * static_cast<double>(ns);
    ++_count;
    _min = std::min(_min, ns);
    _max = std::max(_max, ns);
  }

  void merge(const LatencyHistogram& other) noexcept {
    for (std::size_t idx = 0; idx < _counts.size(); ++idx) {
      _counts[idx] += other._counts[idx];
    }
    _sum += other._sum;
    _sumSquares += other._sumSquares;
    _count += other._count;
    _min = std::min(_min, other._min);
    _max = std::max(_max, other._max);
  }

  [[nodiscard]] std::uint64_t count() const noexcept { return _count; }
  [[nodiscard]] std::uint64_t bucketCount(int idx) const noexcept { return _counts[static_cast<std::size_t>(idx)]; }
  [[nodiscard]] std::uint64_t minNs() const noexcept { return _count != 0 ? _min : 0; }
  [[nodiscard]] std::uint64_t maxNs() const noexcept { return _max; }

  [[nodiscard]] double meanNs() const noexcept {
    return _count != 0 ? static_cast<double>(_sum) / static_cast<double>(_count) : 0.0;
  }

  // Population standard deviation.
  [[nodiscard]] double stddevNs() const noexcept {
    if (_count == 0) {
      return 0.0;
    }
    const double mean = meanNs();
    return std::sqrt(std::max(0.0, (_sumSquares / static_cast<double>(_count)) - (mean * mean)));
  }

  // Value at the given percentile in [0, 100]: the middle of its bucket, within the observed values.
  [[nodiscard]] std::uint64_t percentileNs(double pct) const noexcept {
    if (_count == 0) {
      return 0;
    }
    const auto target =
        std::max(std::uint64_t{1}, static_cast<std::uint64_t>(std::ceil(pct * static_cast<double>(_count) / 100.0)));
    std::uint64_t cumulative = 0;
    for (int idx = 0; idx < kNbBuckets; ++idx) {
      cumulative += _counts[static_cast<std::size_t>(idx)];
      if (cumulative >= target) {
        return std::clamp(BucketLowerNs(idx) + (BucketWidthNs(idx) / 2), _min, _max);
      }
    }
    return _max;
  }

 private:
  [[nodiscard]] static constexpr int IndexOf(std::uint64_t value) noexcept {
    if (value < static_cast<std::uint64_t>(kSub)) {
      return static_cast<int>(value);
    }
    const int exponent = 63 - std::countl_zero(value);
    const std::uint64_t sub = (value >> (exponent - kSubBits)) - static_cast<std::uint64_t>(kSub);
    const int idx = ((exponent - kSubBits + 1) * kSub) + static_cast<int>(sub);
    return std::min(idx, kNbBuckets - 1);
  }

  std::array<std::uint64_t, static_cast<std::size_t>(kNbBuckets)> _counts{};
  double _sumSquares{0.0};
  std::uint64_t _sum{0};
  std::uint64_t _count{0};
  std::uint64_t _min{std::numeric_limits<std::uint64_t>::max()};
  std::uint64_t _max{0};
};

}  // namespace zalie
