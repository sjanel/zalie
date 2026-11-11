#include "zalie/rate-controller.hpp"

#include <gtest/gtest.h>

#include <cstdint>

#include "zalie/latency-histogram.hpp"

namespace zalie {
namespace {

constexpr std::uint64_t kOneMs = 1'000'000;

// 'count' response times of 'latencyNs'.
LatencyHistogram Latencies(std::uint64_t latencyNs, std::uint64_t count) {
  LatencyHistogram histogram;
  for (std::uint64_t idx = 0; idx < count; ++idx) {
    histogram.record(latencyNs);
  }
  return histogram;
}

// One second at 'reached' requests per second, answered in 'latencyNs', 'failures' of them failing.
bool Second(RateController& controller, std::uint64_t reached, std::uint64_t latencyNs, std::uint64_t failures = 0) {
  return controller.onSecond(Latencies(latencyNs, reached), reached, failures);
}

// One second at the current rate: the server keeps up.
bool AtRate(RateController& controller, std::uint64_t latencyNs = kOneMs, std::uint64_t failures = 0) {
  return Second(controller, static_cast<std::uint64_t>(controller.rate()), latencyNs, failures);
}

// The second following a change mixes two rates: it is not judged, whatever it holds.
void Settle(RateController& controller) { EXPECT_FALSE(Second(controller, 1, 100 * kOneMs, 1)); }

TEST(RateController, StartsAtItsMinimum) {
  EXPECT_DOUBLE_EQ(RateController(0, 0, 0).rate(), RateController::kDefaultMinRate);
  EXPECT_DOUBLE_EQ(RateController(5, 100, 0).rate(), 5.0);
  EXPECT_DOUBLE_EQ(RateController(50, 20, 0).rate(), 20.0);  // a minimum above the maximum is the maximum
  EXPECT_DOUBLE_EQ(RateController::StartRate(0, 5), 5.0);
  EXPECT_DOUBLE_EQ(RateController::StartRate(0, 0), RateController::kDefaultMinRate);
  const RateController controller(100, 0, 0);
  EXPECT_EQ(controller.state(), RateState::rampUp);
  EXPECT_DOUBLE_EQ(controller.sustainedRate(), 0.0);
  EXPECT_EQ(controller.baselineP50Ns(), 0U);
}

TEST(RateController, DoublesWhileHealthyUpToItsMaximum) {
  RateController controller(100, 1000, 0);
  for (const double expected : {200.0, 400.0, 800.0, 1000.0}) {
    EXPECT_TRUE(AtRate(controller));
    EXPECT_DOUBLE_EQ(controller.rate(), expected);
    EXPECT_EQ(controller.state(), RateState::rampUp);
    Settle(controller);
  }
  EXPECT_FALSE(AtRate(controller));
  EXPECT_EQ(controller.state(), RateState::ceiling);
  EXPECT_DOUBLE_EQ(controller.sustainedRate(), 1000.0);
  EXPECT_NEAR(static_cast<double>(controller.baselineP50Ns()), 1e6, 0.07e6);  // the precision of the histogram
}

TEST(RateController, HoldsWhenTheResponseTimesRiseThenProbesBySmallSteps) {
  RateController controller(100, 0, 0);
  ASSERT_TRUE(AtRate(controller));
  Settle(controller);
  // p50 at 2.5 ms against 1 ms: slower (above 1.5 x + 0.5 ms), not overloaded (under 3 x + 0.5 ms).
  EXPECT_FALSE(AtRate(controller, 2'500'000));
  EXPECT_EQ(controller.state(), RateState::hold);
  EXPECT_DOUBLE_EQ(controller.rate(), 200.0);
  // Healthy again: no more doubling after a sign of load.
  EXPECT_TRUE(AtRate(controller));
  EXPECT_EQ(controller.state(), RateState::probe);
  EXPECT_DOUBLE_EQ(controller.rate(), 210.0);
  EXPECT_DOUBLE_EQ(controller.sustainedRate(), 200.0);
}

TEST(RateController, ProbesByOneRequestAtLeast) {
  RateController controller(10, 0, 2 * kOneMs);
  // A measure needs 30 requests: 3 seconds at 10 per second. Close to the cap, the rate holds.
  EXPECT_FALSE(Second(controller, 10, 1'900'000));
  EXPECT_FALSE(Second(controller, 10, 1'900'000));
  EXPECT_FALSE(Second(controller, 10, 1'900'000));
  EXPECT_EQ(controller.state(), RateState::hold);
  EXPECT_FALSE(Second(controller, 10, kOneMs));
  EXPECT_FALSE(Second(controller, 10, kOneMs));
  EXPECT_TRUE(Second(controller, 10, kOneMs));
  EXPECT_EQ(controller.state(), RateState::probe);
  EXPECT_DOUBLE_EQ(controller.rate(), 11.0);  // 10 x 1.05 is less than 10 + 1
}

TEST(RateController, BacksOffAtOnceWhenRequestsFail) {
  RateController controller(100, 0, 0);
  ASSERT_TRUE(AtRate(controller));
  Settle(controller);
  ASSERT_TRUE(AtRate(controller));
  Settle(controller);
  ASSERT_DOUBLE_EQ(controller.rate(), 400.0);
  EXPECT_TRUE(AtRate(controller, kOneMs, 40));  // 10 % of failures
  EXPECT_EQ(controller.state(), RateState::backOff);
  EXPECT_DOUBLE_EQ(controller.rate(), 200.0);
  Settle(controller);
  // Never below the minimum: overloaded there too, it stays there.
  EXPECT_TRUE(AtRate(controller, kOneMs, 100));
  EXPECT_DOUBLE_EQ(controller.rate(), 100.0);
  Settle(controller);
  EXPECT_FALSE(AtRate(controller, kOneMs, 100));
  EXPECT_DOUBLE_EQ(controller.rate(), 100.0);
  EXPECT_EQ(controller.state(), RateState::backOff);
  EXPECT_DOUBLE_EQ(controller.sustainedRate(), 200.0);
}

TEST(RateController, FailuresAsFrequentAsAtTheStartAreNormal) {
  // An endpoint failing 5 % of the time at any rate: only more failures than that overload it.
  RateController controller(100, 0, 0);
  ASSERT_TRUE(AtRate(controller, kOneMs, 5));
  Settle(controller);
  ASSERT_TRUE(AtRate(controller, kOneMs, 10));
  EXPECT_EQ(controller.state(), RateState::rampUp);
  Settle(controller);
  EXPECT_TRUE(AtRate(controller, kOneMs, 40));  // 10 %
  EXPECT_EQ(controller.state(), RateState::backOff);
  // Over half of the requests failing overloads the server, even from the start.
  RateController failing(100, 0, 0);
  EXPECT_FALSE(AtRate(failing, kOneMs, 60));
  EXPECT_EQ(failing.state(), RateState::backOff);
  // A couple of failures in a small measure is noise.
  RateController noisy(10, 0, 0);
  ASSERT_TRUE(Second(noisy, 30, kOneMs));
  Settle(noisy);
  EXPECT_TRUE(Second(noisy, 30, kOneMs, 2));
  EXPECT_EQ(noisy.state(), RateState::rampUp);
}

TEST(RateController, BacksOffSlowlyWhenTheResponseTimesExplode) {
  RateController controller(100, 0, 0);
  ASSERT_TRUE(AtRate(controller));
  Settle(controller);
  EXPECT_TRUE(AtRate(controller, 4 * kOneMs));  // above 3 x 1 ms + 0.5 ms
  EXPECT_EQ(controller.state(), RateState::backOff);
  EXPECT_DOUBLE_EQ(controller.rate(), 160.0);
  // The p90 counts too: a tail growing while the median stays.
  RateController tail(100, 0, 0);
  ASSERT_TRUE(AtRate(tail));
  Settle(tail);
  LatencyHistogram latencies = Latencies(kOneMs, 170);
  latencies.merge(Latencies(10 * kOneMs, 30));
  EXPECT_TRUE(tail.onSecond(latencies, 200, 0));
  EXPECT_EQ(tail.state(), RateState::backOff);
}

TEST(RateController, ThePercentileCapHoldsThenBacksOff) {
  RateController controller(100, 0, 2 * kOneMs);
  EXPECT_FALSE(AtRate(controller, 2'500'000));  // above the cap, at the minimum already
  EXPECT_EQ(controller.state(), RateState::backOff);
  EXPECT_FALSE(AtRate(controller, 1'900'000));  // within 10 % of the cap
  EXPECT_EQ(controller.state(), RateState::hold);
  EXPECT_TRUE(AtRate(controller, 1'500'000));
  EXPECT_DOUBLE_EQ(controller.rate(), 105.0);
  Settle(controller);
  EXPECT_TRUE(AtRate(controller, 2'500'000));
  EXPECT_DOUBLE_EQ(controller.rate(), 100.0);  // 84, raised to the minimum
}

TEST(RateController, KeepsTheRateTheConnectionsReach) {
  RateController controller(100, 0, 0);
  ASSERT_TRUE(AtRate(controller));
  Settle(controller);
  ASSERT_TRUE(AtRate(controller));
  Settle(controller);
  ASSERT_DOUBLE_EQ(controller.rate(), 400.0);
  // 300 out of 400: the client limits the rate, which comes down to what it reaches, plus a margin.
  EXPECT_TRUE(Second(controller, 300, kOneMs));
  EXPECT_EQ(controller.state(), RateState::clientLimited);
  EXPECT_DOUBLE_EQ(controller.rate(), 324.0);
  Settle(controller);
  // Within the margin: kept as it is.
  EXPECT_FALSE(Second(controller, 300, kOneMs));
  EXPECT_DOUBLE_EQ(controller.rate(), 324.0);
  EXPECT_DOUBLE_EQ(controller.sustainedRate(), 300.0);
}

TEST(RateController, LowRatesMeasureOverSeveralSeconds) {
  RateController controller(5, 0, 0);
  for (int second = 0; second < 4; ++second) {
    EXPECT_FALSE(Second(controller, 5, kOneMs));
  }
  EXPECT_DOUBLE_EQ(controller.sustainedRate(), 0.0);  // no measure yet: 20 requests
  EXPECT_TRUE(Second(controller, 5, kOneMs));         // 5 seconds: measured anyway
  EXPECT_DOUBLE_EQ(controller.rate(), 10.0);
  EXPECT_DOUBLE_EQ(controller.sustainedRate(), 5.0);
}

TEST(RateController, NoResponseAtAllBacksOff) {
  RateController controller(100, 0, 0);
  ASSERT_TRUE(AtRate(controller));
  Settle(controller);
  // The server stalls: nothing ends for the longest measure.
  for (std::uint32_t second = 1; second < RateController::kMaxWindowSeconds; ++second) {
    EXPECT_FALSE(controller.onSecond(LatencyHistogram{}, 0, 0));
  }
  EXPECT_TRUE(controller.onSecond(LatencyHistogram{}, 0, 0));
  EXPECT_EQ(controller.state(), RateState::backOff);
  EXPECT_DOUBLE_EQ(controller.rate(), 100.0);
}

}  // namespace
}  // namespace zalie
