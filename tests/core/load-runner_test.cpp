// Integration tests of the load runner: real HTTP exchanges with a local aeronet server.

#include "zalie/load-runner.hpp"

#include <gtest/gtest.h>

#include <aeronet/http-method.hpp>
#include <aeronet/http-request-view.hpp>
#include <aeronet/http-response.hpp>
#include <aeronet/http-server-config.hpp>
#include <aeronet/router.hpp>
#include <aeronet/single-http-server.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "zalie/method.hpp"
#include "zalie/resolved-request.hpp"

namespace zalie {
namespace {

using namespace std::chrono_literals;

constexpr std::string_view kBody = "hello";
constexpr std::size_t kClass2xx = static_cast<std::size_t>(LoadClass::status2xx);
constexpr std::size_t kClass3xx = static_cast<std::size_t>(LoadClass::status3xx);
constexpr std::size_t kClass4xx = static_cast<std::size_t>(LoadClass::status4xx);
constexpr std::size_t kClass5xx = static_cast<std::size_t>(LoadClass::status5xx);
constexpr std::size_t kClassError = static_cast<std::size_t>(LoadClass::error);

class LoadRunnerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    aeronet::Router router;
    router.setPath(aeronet::http::Method::GET, "/ok", [this](const aeronet::HttpRequestView& req) {
      _hits.fetch_add(1, std::memory_order_relaxed);
      return req.makeResponse(200, kBody, "text/plain");
    });
    // Each code in turn: with one connection, the counts are exact.
    router.setPath(aeronet::http::Method::GET, "/mixed", [this](const aeronet::HttpRequestView& req) {
      static constexpr std::array<aeronet::http::StatusCode, 5> kCodes = {200, 204, 302, 404, 500};
      const std::uint64_t hit = _hits.fetch_add(1, std::memory_order_relaxed);
      return req.makeResponse(kCodes[hit % kCodes.size()]);
    });
    router.setPath(aeronet::http::Method::POST, "/capture", [this](const aeronet::HttpRequestView& req) {
      {
        std::scoped_lock lock(_capturedMutex);
        _capturedBody = std::string(req.body());
        _capturedHeader = std::string(req.headerValueOrEmpty("x-custom"));
        _capturedContentType = std::string(req.headerValueOrEmpty("content-type"));
      }
      _hits.fetch_add(1, std::memory_order_relaxed);
      return req.makeResponse(200, kBody, "text/plain");
    });
    router.setPath(aeronet::http::Method::GET, "/slow", [](const aeronet::HttpRequestView& req) {
      std::this_thread::sleep_for(200ms);
      return req.makeResponse(200, kBody, "text/plain");
    });
    router.setPath(aeronet::http::Method::GET, "/stall", [](const aeronet::HttpRequestView& req) {
      std::this_thread::sleep_for(1050ms);
      return req.makeResponse(200, kBody, "text/plain");
    });
    _server = std::make_unique<aeronet::SingleHttpServer>(
        aeronet::HttpServerConfig{}.withPort(0).withKeepAliveTimeout(5s).withPollInterval(20ms), std::move(router));
    _port = _server->port();
    _server->start();
  }

  void TearDown() override { _server.reset(); }

  [[nodiscard]] std::string url(std::string_view path) const {
    return "http://127.0.0.1:" + std::to_string(_port) + std::string(path);
  }

  [[nodiscard]] ResolvedRequest makeRequest(std::string_view path, Method method = Method::get) const {
    ResolvedRequest req;
    req.method = method;
    req.url = req.strings.store(url(path));
    return req;
  }

  // Runs until the end and returns the final state.
  static std::shared_ptr<const LoadSnapshot> runToEnd(ResolvedRequest request, const LoadOptions& options) {
    LoadRunner runner(std::move(request), options);
    EXPECT_TRUE(runner.waitForCompletion(30s));
    std::shared_ptr<const LoadSnapshot> snapshot = runner.snapshot();
    EXPECT_EQ(snapshot->phase, LoadPhase::finished);
    return snapshot;
  }

  [[nodiscard]] std::uint64_t hits() const noexcept { return _hits.load(std::memory_order_relaxed); }

  std::unique_ptr<aeronet::SingleHttpServer> _server;
  std::atomic<std::uint64_t> _hits{0};
  std::mutex _capturedMutex;
  std::string _capturedBody;
  std::string _capturedHeader;
  std::string _capturedContentType;
  std::uint16_t _port{0};
};

// A run bounded by its number of requests only.
LoadOptions RequestsOnly(std::uint64_t maxRequests, std::uint32_t connections) {
  return {.maxRequests = maxRequests, .durationMs = 0, .connections = connections, .maxConnections = connections};
}

TEST(LoadConnectionsTest, DefaultsToTheNumberOfVCpusWhichIsAlsoTheLimit) {
  const std::uint32_t vCpus = std::max(std::thread::hardware_concurrency(), 1U);
  EXPECT_EQ(LoadConnections({}), vCpus);
  EXPECT_EQ(LoadConnections({.connections = vCpus + 10}), vCpus);
  EXPECT_EQ(LoadConnections({.connections = 1}), 1U);
}

TEST(LoadConnectionsTest, TheLimitCanBeRaised) {
  const std::uint32_t vCpus = std::max(std::thread::hardware_concurrency(), 1U);
  EXPECT_EQ(LoadConnections({.connections = vCpus + 10, .maxConnections = vCpus + 10}), vCpus + 10);
  EXPECT_EQ(LoadConnections({.connections = vCpus + 10, .maxConnections = vCpus + 5}), vCpus + 5);
}

TEST_F(LoadRunnerTest, StopsAfterTheRequestLimit) {
  const auto snapshot = runToEnd(makeRequest("/ok"), RequestsOnly(200, 4));
  EXPECT_EQ(snapshot->connections, 4U);
  EXPECT_EQ(snapshot->requests(), 200U);
  EXPECT_EQ(hits(), 200U);
  EXPECT_EQ(snapshot->counts[kClass2xx], 200U);
  ASSERT_EQ(snapshot->statuses.size(), 1U);
  EXPECT_EQ(snapshot->statuses[0].status, 200);
  EXPECT_EQ(snapshot->statuses[0].count, 200U);
  EXPECT_EQ(snapshot->bodyBytes, 200U * kBody.size());
  EXPECT_EQ(snapshot->minBodyBytes, kBody.size());
  EXPECT_EQ(snapshot->maxBodyBytes, kBody.size());
  EXPECT_GT(snapshot->elapsedSeconds, 0.0);
  EXPECT_LT(snapshot->elapsedSeconds, 30.0);
  EXPECT_GT(snapshot->meanRequestsPerSecond, 0.0);
  EXPECT_GE(snapshot->peakRequestsPerSecond, snapshot->meanRequestsPerSecond);
  EXPECT_EQ(snapshot->inFlight, 0U);
  EXPECT_TRUE(snapshot->setupError.empty());
}

TEST_F(LoadRunnerTest, MeasuresTheHeadAndTheEndOfTheBody) {
  const auto snapshot = runToEnd(makeRequest("/ok"), RequestsOnly(100, 2));
  EXPECT_EQ(snapshot->head.count, 100U);
  EXPECT_EQ(snapshot->total.count, 100U);
  EXPECT_EQ(snapshot->byClass[kClass2xx].count, 100U);
  EXPECT_GT(snapshot->total.minNs, 0U);
  EXPECT_LE(snapshot->head.minNs, snapshot->total.minNs);
  EXPECT_LE(snapshot->head.maxNs, snapshot->total.maxNs);
  EXPECT_LE(snapshot->total.p50Ns, snapshot->total.p99Ns);
  EXPECT_LE(snapshot->total.p99Ns, snapshot->total.maxNs);
  EXPECT_GT(snapshot->total.meanNs, 0.0);
  EXPECT_EQ(snapshot->totalHistogram.count(), 100U);
  EXPECT_EQ(snapshot->headHistogram.count(), 100U);
}

TEST_F(LoadRunnerTest, StopsAtTheDuration) {
  const auto snapshot = runToEnd(makeRequest("/ok"), {.durationMs = 300, .connections = 2, .maxConnections = 2});
  EXPECT_GT(snapshot->requests(), 0U);
  EXPECT_NEAR(snapshot->elapsedSeconds, 0.3, 1e-6);
}

TEST_F(LoadRunnerTest, TheFirstLimitReachedEndsTheRun) {
  const auto snapshot =
      runToEnd(makeRequest("/ok"), {.maxRequests = 20, .durationMs = 25000, .connections = 1, .maxConnections = 1});
  EXPECT_EQ(snapshot->requests(), 20U);
  EXPECT_LT(snapshot->elapsedSeconds, 20.0);
}

TEST_F(LoadRunnerTest, TheRateCapHoldsAcrossTheConnections) {
  const auto snapshot = runToEnd(
      makeRequest("/ok"), {.durationMs = 500, .maxRequestsPerSecond = 100, .connections = 4, .maxConnections = 4});
  EXPECT_LE(snapshot->requests(), 51U);
  EXPECT_GE(snapshot->requests(), 25U);
}

TEST_F(LoadRunnerTest, TheWaitOfTheRateCapIsNotALatency) {
  const auto snapshot =
      runToEnd(makeRequest("/ok"),
               {.maxRequests = 8, .durationMs = 0, .maxRequestsPerSecond = 20, .connections = 2, .maxConnections = 2});
  ASSERT_EQ(snapshot->requests(), 8U);
  EXPECT_GE(snapshot->elapsedSeconds, 0.3);       // 7 intervals of 50 ms between the 8 requests
  EXPECT_LT(snapshot->total.p50Ns, 25'000'000U);  // far from the 50 ms between two sends
}

TEST_F(LoadRunnerTest, TheWarmupIsSentButNotCounted) {
  const auto snapshot = runToEnd(
      makeRequest("/ok"),
      {.durationMs = 500, .maxRequestsPerSecond = 100, .warmupMs = 200, .connections = 1, .maxConnections = 1});
  EXPECT_GT(snapshot->requests(), 0U);
  EXPECT_GT(hits(), snapshot->requests() + 5);
  EXPECT_NEAR(snapshot->elapsedSeconds, 0.3, 1e-6);
}

TEST_F(LoadRunnerTest, CountsStatusesByCodeAndByClass) {
  ResolvedRequest request = makeRequest("/mixed");
  request.settings.followRedirects = false;
  const auto snapshot = runToEnd(std::move(request), RequestsOnly(100, 1));
  ASSERT_EQ(snapshot->statuses.size(), 5U);
  std::int16_t previous = 0;
  for (const LoadStatusCount& entry : snapshot->statuses) {
    EXPECT_GT(entry.status, previous);
    EXPECT_EQ(entry.count, 20U);
    previous = entry.status;
  }
  EXPECT_EQ(snapshot->counts[kClass2xx], 40U);
  EXPECT_EQ(snapshot->counts[kClass3xx], 20U);
  EXPECT_EQ(snapshot->counts[kClass4xx], 20U);
  EXPECT_EQ(snapshot->counts[kClass5xx], 20U);
  EXPECT_EQ(snapshot->counts[kClassError], 0U);
  EXPECT_EQ(snapshot->byClass[kClass4xx].count, 20U);
  EXPECT_EQ(snapshot->total.count, 100U);
}

TEST_F(LoadRunnerTest, AConnectionErrorHasNoResponseTime) {
  ResolvedRequest request;
  request.url = request.strings.store("http://127.0.0.1:1/");  // nothing listens on this port
  request.settings.timeoutMs = 2000;
  const auto snapshot = runToEnd(std::move(request), RequestsOnly(5, 1));
  EXPECT_EQ(snapshot->requests(), 5U);
  EXPECT_EQ(snapshot->counts[kClassError], 5U);
  const LoadErrorStats& errors = snapshot->errors[static_cast<std::size_t>(LoadErrorKind::connection)];
  EXPECT_EQ(errors.count, 5U);
  EXPECT_FALSE(errors.firstMessage.empty());
  EXPECT_EQ(snapshot->total.count, 0U);
  EXPECT_EQ(snapshot->head.count, 0U);
  EXPECT_EQ(snapshot->byClass[kClassError].count, 5U);
  EXPECT_TRUE(snapshot->statuses.empty());
}

TEST_F(LoadRunnerTest, ATimeoutIsAnErrorOfItsOwnKind) {
  ResolvedRequest request = makeRequest("/slow");
  request.settings.timeoutMs = 50;
  const auto snapshot = runToEnd(std::move(request), RequestsOnly(2, 1));
  EXPECT_EQ(snapshot->errors[static_cast<std::size_t>(LoadErrorKind::timeout)].count, 2U);
  EXPECT_EQ(snapshot->counts[kClassError], 2U);
}

TEST_F(LoadRunnerTest, StoppingByHandEndsTheRunEarly) {
  LoadRunner runner(makeRequest("/ok"), {.durationMs = 25000, .connections = 2, .maxConnections = 2});
  std::this_thread::sleep_for(150ms);
  runner.stop();
  ASSERT_TRUE(runner.waitForCompletion(10s));
  const auto snapshot = runner.snapshot();
  EXPECT_EQ(snapshot->phase, LoadPhase::finished);
  EXPECT_GT(snapshot->requests(), 0U);
  EXPECT_GE(hits(), snapshot->requests());
  EXPECT_LT(snapshot->elapsedSeconds, 10.0);
}

TEST_F(LoadRunnerTest, DestroyingARunningRunnerStopsIt) {
  const auto start = std::chrono::steady_clock::now();
  {
    LoadRunner runner(makeRequest("/ok"), {.durationMs = 25000, .connections = 2, .maxConnections = 2});
    std::this_thread::sleep_for(50ms);
  }
  EXPECT_LT(std::chrono::steady_clock::now() - start, 10s);
}

TEST_F(LoadRunnerTest, StopInterruptsTheWaitOfALowRateCap) {
  LoadRunner runner(makeRequest("/ok"),
                    {.durationMs = 25000, .maxRequestsPerSecond = 1, .connections = 4, .maxConnections = 4});
  std::this_thread::sleep_for(50ms);
  runner.stop();
  EXPECT_TRUE(runner.waitForCompletion(5s));
}

TEST_F(LoadRunnerTest, TheTimeSeriesCoversTheRun) {
  const auto snapshot = runToEnd(
      makeRequest("/ok"), {.durationMs = 1300, .maxRequestsPerSecond = 200, .connections = 1, .maxConnections = 1});
  ASSERT_EQ(snapshot->series.size(), 2U);
  EXPECT_EQ(snapshot->firstSecond, 0U);
  EXPECT_FLOAT_EQ(snapshot->series[0].seconds, 1.0F);
  EXPECT_NEAR(snapshot->series[1].seconds, 0.3F, 0.01F);
  EXPECT_GE(snapshot->series[0].requests(), 50U);
  EXPECT_LE(snapshot->series[0].requests(), 215U);
  EXPECT_GT(snapshot->series[0].p50Ns, 0U);
  EXPECT_LE(snapshot->series[0].p50Ns, snapshot->series[0].p99Ns);
  EXPECT_EQ(snapshot->series[0].requests() + snapshot->series[1].requests(), snapshot->requests());
  EXPECT_EQ(snapshot->series[0].bodyBytes + snapshot->series[1].bodyBytes, snapshot->bodyBytes);
}

TEST_F(LoadRunnerTest, AStallLeavesEmptySecondsInTheTimeSeries) {
  // Responses after 1.05 s and 2.1 s at the earliest: nothing completes during the first second, and the deadline
  // leaves room for a slow scheduler (the second response must arrive before it to be counted).
  const auto snapshot = runToEnd(makeRequest("/stall"), {.durationMs = 2900, .connections = 1, .maxConnections = 1});
  ASSERT_EQ(snapshot->series.size(), 3U);
  EXPECT_EQ(snapshot->series[0].requests(), 0U);
  EXPECT_EQ(snapshot->series[0].inFlight, 1U);
  EXPECT_EQ(snapshot->series[1].requests(), 1U);
  EXPECT_EQ(snapshot->series[2].requests(), 1U);
  EXPECT_NEAR(snapshot->series[2].seconds, 0.9F, 0.01F);
}

TEST_F(LoadRunnerTest, SendsTheHeadersAndTheBody) {
  ResolvedRequest request = makeRequest("/capture", Method::post);
  request.headers.push_back({"X-Custom", "yes"});
  request.headers.push_back({"Content-Type", "application/json"});
  request.body.append(std::string_view{R"({"a":1})"});
  const auto snapshot = runToEnd(std::move(request), RequestsOnly(3, 1));
  EXPECT_EQ(snapshot->counts[kClass2xx], 3U);
  std::scoped_lock lock(_capturedMutex);
  EXPECT_EQ(_capturedBody, R"({"a":1})");
  EXPECT_EQ(_capturedHeader, "yes");
  EXPECT_EQ(_capturedContentType, "application/json");
}

TEST_F(LoadRunnerTest, ReportsTheHeadersThatAreNotSent) {
  ResolvedRequest request = makeRequest("/ok");
  request.headers.push_back({"Connection", "close"});
  const auto snapshot = runToEnd(std::move(request), RequestsOnly(1, 2));
  ASSERT_EQ(snapshot->warnings.size(), 1U);  // once, not once per connection
  EXPECT_NE(std::string_view(snapshot->warnings[0]).find("Connection"), std::string_view::npos);
}

TEST_F(LoadRunnerTest, ReportsAnInvalidUrl) {
  ResolvedRequest request;
  request.url = request.strings.store("not a url");
  const auto snapshot = runToEnd(std::move(request), RequestsOnly(10, 2));
  EXPECT_FALSE(snapshot->setupError.empty());
  EXPECT_EQ(snapshot->requests(), 0U);
}

TEST_F(LoadRunnerTest, SnapshotsAreImmutableAndIndependentOfTheRunner) {
  std::shared_ptr<const LoadSnapshot> snapshot;
  {
    LoadRunner runner(makeRequest("/ok"), RequestsOnly(10, 1));
    ASSERT_TRUE(runner.waitForCompletion(10s));
    snapshot = runner.snapshot();
  }
  EXPECT_EQ(snapshot->requests(), 10U);
}

TEST_F(LoadRunnerTest, WakesTheEventLoopAtEachSnapshot) {
  std::atomic<int> nbWakes{0};
  LoadOptions options{.durationMs = 350, .connections = 1, .maxConnections = 1};
  options.wakeFn = [](void* pUserData) { static_cast<std::atomic<int>*>(pUserData)->fetch_add(1); };
  options.pWakeUserData = &nbWakes;
  const auto snapshot = runToEnd(makeRequest("/ok"), options);
  EXPECT_GE(snapshot->requests(), 1U);
  EXPECT_GE(nbWakes.load(), 2);  // at least one snapshot during the run, and the final one
  EXPECT_LE(nbWakes.load(), 8);  // about 10 per second: not one per request
}

TEST_F(LoadRunnerTest, ReportsLiveStatisticsDuringTheRun) {
  LoadRunner runner(makeRequest("/ok"), {.durationMs = 800, .connections = 1, .maxConnections = 1});
  std::this_thread::sleep_for(400ms);
  const auto snapshot = runner.snapshot();
  EXPECT_EQ(snapshot->phase, LoadPhase::running);
  EXPECT_GT(snapshot->requests(), 0U);
  EXPECT_GT(snapshot->elapsedSeconds, 0.0);
  ASSERT_TRUE(runner.waitForCompletion(10s));
  EXPECT_EQ(runner.snapshot()->phase, LoadPhase::finished);
}

}  // namespace
}  // namespace zalie
