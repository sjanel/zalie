#include "zalie/load-report.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include "test-utils.hpp"
#include "zalie/file-io.hpp"
#include "zalie/load-plan.hpp"
#include "zalie/load-runner.hpp"
#include "zalie/load-settings.hpp"
#include "zalie/method.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/resolved-request.hpp"

namespace zalie {
namespace {

using namespace std::chrono_literals;

constexpr std::size_t k2xx = static_cast<std::size_t>(LoadClass::status2xx);
constexpr std::size_t kError = static_cast<std::size_t>(LoadClass::error);

// 2026-11-09T21:30:05Z
constexpr std::chrono::system_clock::time_point kStartedAt{std::chrono::seconds{1794259805}};

LoadSnapshot MakeSnapshot() {
  LoadSnapshot snapshot;
  snapshot.phase = LoadPhase::finished;
  snapshot.connections = 4;
  snapshot.elapsedSeconds = 2.5;
  snapshot.meanRequestsPerSecond = 1200.0;
  snapshot.counts[k2xx] = 2999;
  snapshot.counts[kError] = 1;
  for (int idx = 0; idx < 2999; ++idx) {
    snapshot.totalHistogram.record(2'000'000);
    snapshot.headHistogram.record(1'000'000);
  }
  snapshot.statuses.push_back({.count = 2999, .status = 200});
  snapshot.errors[static_cast<std::size_t>(LoadErrorKind::timeout)] = {.count = 1, .firstMessage = "timed out"};
  snapshot.series.push_back({.counts = {1200, 0, 0, 0, 0}, .bodyBytes = 6000, .p50Ns = 2'000'000});
  snapshot.series.push_back({.counts = {1799, 0, 0, 0, 1}, .bodyBytes = 8995, .seconds = 0.5F});
  snapshot.plan = std::make_shared<const LoadPlan>(ResolvedRequest{});
  return snapshot;
}

TEST(LoadReport, HoldsTheWholeRun) {
  LoadSettings settings;
  settings.connections = 4;
  settings.combine = CombineMode::together;
  const std::string json = SerializeLoadReport(
      MakeSnapshot(), settings,
      {.requestName = "Search", .url = "{{base}}/search", .startedAt = kStartedAt, .method = Method::post});
  for (const char* expected :
       {R"("format": "zalie-run")", R"("formatVersion": 1)", R"("startedAt": "2026-11-09T21:30:05Z")",
        R"("name": "Search")", R"("method": "POST")", R"("url": "{{base}}/search")", R"("combine": "together")",
        R"("seed": 1)", R"("requests": 3000)", R"("name": "2xx")", R"("name": "error")", R"("status": 200)",
        R"("kind": "timeout")", R"("firstMessage": "timed out")", R"("classes": [)", R"("seconds": 0.5)",
        R"("p50Ns": 2000000)"}) {
    EXPECT_TRUE(json.contains(expected)) << expected << "\n" << json;
  }
  // Histograms keep their non-empty buckets only.
  EXPECT_FALSE(json.contains("[\n        0,"));
  EXPECT_TRUE(json.ends_with("}\n"));
}

TEST(LoadReport, FakeSnapshotsWithoutPlan) {
  LoadSnapshot snapshot;
  const std::string json = SerializeLoadReport(snapshot, LoadSettings{}, {.startedAt = kStartedAt});
  EXPECT_TRUE(json.contains(R"("seed": 0)"));
  EXPECT_TRUE(json.contains(R"("dataFiles": [])"));
}

TEST(LoadReport, ExportWritesTheFile) {
  const test::TempDir dir;
  const std::filesystem::path path = dir.path() / "runs" / "search.json";
  ASSERT_TRUE(ExportLoadReport(path, MakeSnapshot(), LoadSettings{}, {.startedAt = kStartedAt}).has_value());
  auto content = ReadFile(path);
  ASSERT_TRUE(content.has_value());
  EXPECT_TRUE(View(*content).contains(R"("format": "zalie-run")"));
  // A directory cannot be written over.
  EXPECT_FALSE(ExportLoadReport(dir.path(), MakeSnapshot(), LoadSettings{}, {.startedAt = kStartedAt}).has_value());
}

TEST(LoadReport, FileName) {
  EXPECT_EQ(View(LoadReportFileName("Search users", kStartedAt)), "Search-users-20261109-213005.json");
  EXPECT_EQ(View(LoadReportFileName("  ../x//y?  ", kStartedAt)), "x-y-20261109-213005.json");
  EXPECT_EQ(View(LoadReportFileName("", kStartedAt)), "run-20261109-213005.json");
  EXPECT_EQ(View(LoadReportFileName(std::string(100, 'a'), kStartedAt)),
            std::string(64, 'a') + "-20261109-213005.json");
}

}  // namespace
}  // namespace zalie
