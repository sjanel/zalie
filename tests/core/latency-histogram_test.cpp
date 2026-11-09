#include "zalie/latency-histogram.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

namespace zalie {
namespace {

TEST(LatencyHistogramTest, EmptyHistogramReportsZeros) {
  const LatencyHistogram histogram;
  EXPECT_EQ(histogram.count(), 0U);
  EXPECT_EQ(histogram.minNs(), 0U);
  EXPECT_EQ(histogram.maxNs(), 0U);
  EXPECT_EQ(histogram.percentileNs(50.0), 0U);
  EXPECT_DOUBLE_EQ(histogram.meanNs(), 0.0);
  EXPECT_DOUBLE_EQ(histogram.stddevNs(), 0.0);
}

TEST(LatencyHistogramTest, SmallValuesAreExact) {
  LatencyHistogram histogram;
  for (std::uint64_t value = 1; value <= 10; ++value) {
    histogram.record(value);
  }
  EXPECT_EQ(histogram.percentileNs(0.0), 1U);
  EXPECT_EQ(histogram.percentileNs(50.0), 5U);
  EXPECT_EQ(histogram.percentileNs(90.0), 9U);
  EXPECT_EQ(histogram.percentileNs(100.0), 10U);
}

TEST(LatencyHistogramTest, OneSampleIsEveryPercentile) {
  LatencyHistogram histogram;
  histogram.record(123'456'789);
  EXPECT_EQ(histogram.percentileNs(1.0), 123'456'789U);
  EXPECT_EQ(histogram.percentileNs(50.0), 123'456'789U);
  EXPECT_EQ(histogram.percentileNs(99.9), 123'456'789U);
}

TEST(LatencyHistogramTest, PercentilesStayWithinTheBucketError) {
  LatencyHistogram histogram;
  for (std::uint64_t micros = 1; micros <= 100'000; ++micros) {
    histogram.record(micros * 1000);
  }
  for (const double pct : {50.0, 90.0, 95.0, 99.0, 99.9}) {
    const double expected = pct * 1000.0 * 1000.0;
    EXPECT_NEAR(static_cast<double>(histogram.percentileNs(pct)), expected, expected * 0.04) << pct;
  }
}

TEST(LatencyHistogramTest, ComputesMinMaxMeanAndStandardDeviation) {
  LatencyHistogram histogram;
  for (const std::uint64_t value : {2U, 4U, 4U, 4U, 5U, 5U, 7U, 9U}) {
    histogram.record(value);
  }
  EXPECT_EQ(histogram.count(), 8U);
  EXPECT_EQ(histogram.minNs(), 2U);
  EXPECT_EQ(histogram.maxNs(), 9U);
  EXPECT_DOUBLE_EQ(histogram.meanNs(), 5.0);
  EXPECT_DOUBLE_EQ(histogram.stddevNs(), 2.0);
}

TEST(LatencyHistogramTest, MergingEqualsRecordingEverything) {
  LatencyHistogram left;
  LatencyHistogram right;
  LatencyHistogram both;
  for (std::uint64_t value = 1; value <= 5000; ++value) {
    (value % 2 == 0 ? left : right).record(value * 37);
    both.record(value * 37);
  }
  left.merge(right);
  EXPECT_EQ(left.count(), both.count());
  EXPECT_EQ(left.minNs(), both.minNs());
  EXPECT_EQ(left.maxNs(), both.maxNs());
  EXPECT_DOUBLE_EQ(left.meanNs(), both.meanNs());
  EXPECT_DOUBLE_EQ(left.stddevNs(), both.stddevNs());
  for (const double pct : {0.0, 50.0, 99.0, 100.0}) {
    EXPECT_EQ(left.percentileNs(pct), both.percentileNs(pct)) << pct;
  }
}

TEST(LatencyHistogramTest, MergingAnEmptyHistogramKeepsTheMinimum) {
  LatencyHistogram histogram;
  histogram.record(42);
  histogram.merge(LatencyHistogram{});
  EXPECT_EQ(histogram.minNs(), 42U);
}

TEST(LatencyHistogramTest, HugeValuesFallInTheLastBucket) {
  LatencyHistogram histogram;
  histogram.record(std::numeric_limits<std::uint64_t>::max());
  EXPECT_EQ(histogram.maxNs(), std::numeric_limits<std::uint64_t>::max());
  EXPECT_EQ(histogram.bucketCount(LatencyHistogram::kNbBuckets - 1), 1U);
  EXPECT_EQ(histogram.percentileNs(50.0), std::numeric_limits<std::uint64_t>::max());
}

TEST(LatencyHistogramTest, BucketsPartitionTheValues) {
  for (int idx = 0; idx + 1 < LatencyHistogram::kNbBuckets; ++idx) {
    EXPECT_EQ(LatencyHistogram::BucketLowerNs(idx) + LatencyHistogram::BucketWidthNs(idx),
              LatencyHistogram::BucketLowerNs(idx + 1))
        << idx;
  }
}

TEST(LatencyHistogramTest, ValuesLandInTheBucketContainingThem) {
  for (const std::uint64_t value : {0ULL, 15ULL, 16ULL, 31ULL, 32ULL, 1000ULL, 1'000'000'007ULL, 1ULL << 40U}) {
    LatencyHistogram histogram;
    histogram.record(value);
    int found = -1;
    for (int idx = 0; idx < LatencyHistogram::kNbBuckets; ++idx) {
      if (histogram.bucketCount(idx) != 0) {
        found = idx;
      }
    }
    ASSERT_GE(found, 0) << value;
    EXPECT_GE(value, LatencyHistogram::BucketLowerNs(found));
    EXPECT_LT(value, LatencyHistogram::BucketLowerNs(found) + LatencyHistogram::BucketWidthNs(found));
  }
}

}  // namespace
}  // namespace zalie
