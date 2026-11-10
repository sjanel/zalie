#include "zalie/load-report.hpp"

#include <amc/vector.hpp>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <glaze/glaze.hpp>
#include <string>
#include <string_view>

#include "model-glaze.hpp"
#include "zalie/file-io.hpp"
#include "zalie/latency-histogram.hpp"
#include "zalie/load-runner.hpp"
#include "zalie/load-settings.hpp"
#include "zalie/method.hpp"
#include "zalie/raw-chars.hpp"

// The report as written, in a named namespace: glaze reflects the names of the members of types with external linkage.
namespace zalie::report {

// NOLINTNEXTLINE(misc-use-internal-linkage) - glaze requires external linkage for reflection
struct Request {
  std::string_view name;
  Method method{Method::get};
  std::string_view url;
};

// NOLINTNEXTLINE(misc-use-internal-linkage)
struct DataFile {
  std::string_view path;
  DataFileFormat format{DataFileFormat::csv};
  std::uint64_t rows{0};
  std::uint64_t sizeBytes{0};
};

// NOLINTNEXTLINE(misc-use-internal-linkage)
struct Totals {
  std::uint64_t requests{0};
  std::uint64_t bodyBytes{0};
  std::uint64_t minBodyBytes{0};
  std::uint64_t maxBodyBytes{0};
  double elapsedSeconds{0.0};
  double meanRequestsPerSecond{0.0};
  double peakRequestsPerSecond{0.0};
  // Adaptive rate: the highest rate measured while the server was healthy. 0 for a fixed rate.
  double sustainedRequestsPerSecond{0.0};
  double bodyBytesPerSecond{0.0};
  double cpuSeconds{0.0};
  double clientLoad{0.0};
  bool clientSaturated{false};
};

// Requests of a class of outcome, and their times to the end of the body.
// NOLINTNEXTLINE(misc-use-internal-linkage)
struct OutcomeClass {
  std::string_view name;
  std::uint64_t count{0};
  LatencySummary latency;
};

// NOLINTNEXTLINE(misc-use-internal-linkage)
struct Error {
  std::string_view kind;
  std::uint64_t count{0};
  std::string_view firstMessage;
};

// Non-empty buckets: [lower bound in ns, count]. The width of a bucket follows from its lower bound (see
// LatencyHistogram).
using Buckets = amc::vector<std::array<std::uint64_t, 2>>;

// NOLINTNEXTLINE(misc-use-internal-linkage)
struct Histograms {
  Buckets total;
  Buckets head;
};

// NOLINTNEXTLINE(misc-use-internal-linkage)
struct Series {
  // Order of the counts of each point.
  std::array<std::string_view, kNbLoadClasses> classes;
  // Index since the end of the warm-up of the first point (a long run keeps its last hour).
  std::uint32_t firstSecond{0};
  amc::vector<LoadSecond> points;
};

// NOLINTNEXTLINE(misc-use-internal-linkage)
struct Report {
  std::string_view format{"zalie-run"};
  std::uint32_t formatVersion{1};
  std::string_view zalieVersion{ZALIE_VERSION_STR};
  std::string startedAt;
  Request request;
  LoadSettings settings;
  std::uint64_t seed{0};
  std::uint32_t connections{0};
  std::string_view setupError;
  amc::vector<std::string_view> warnings;
  amc::vector<DataFile> dataFiles;
  Totals totals;
  LatencySummary latency;
  LatencySummary headLatency;
  amc::vector<OutcomeClass> classes;
  amc::vector<LoadStatusCount> statuses;
  amc::vector<Error> errors;
  Histograms histograms;
  Series series;
};

}  // namespace zalie::report

namespace zalie {

namespace {

constexpr std::array<std::string_view, kNbLoadClasses> kClassNames{"2xx", "3xx", "4xx", "5xx", "error"};
constexpr std::array<std::string_view, kNbLoadErrorKinds> kErrorKindNames{
    "connection", "tls", "timeout", "closed", "malformed", "too-large", "other", "invalid-value"};

[[nodiscard]] report::Buckets BucketsOf(const LatencyHistogram& histogram) {
  report::Buckets ret;
  for (int idx = 0; idx < LatencyHistogram::kNbBuckets; ++idx) {
    if (const std::uint64_t count = histogram.bucketCount(idx); count != 0) {
      ret.push_back({LatencyHistogram::BucketLowerNs(idx), count});
    }
  }
  return ret;
}

}  // namespace

std::string SerializeLoadReport(const LoadSnapshot& snapshot, const LoadSettings& settings,
                                const LoadReportContext& context) {
  report::Report out;
  out.startedAt = std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(context.startedAt));
  out.request = {.name = context.requestName, .method = context.method, .url = context.url};
  out.settings = settings;
  out.connections = snapshot.connections;
  out.setupError = snapshot.setupError;
  for (const RawChars32& warning : snapshot.warnings) {
    out.warnings.push_back(warning);
  }
  if (snapshot.plan) {
    out.seed = snapshot.plan->seed();
    for (const auto& pIndex : snapshot.plan->generators().dataFiles()) {
      out.dataFiles.push_back({.path = pIndex->path(),
                               .format = pIndex->format(),
                               .rows = pIndex->nbRows(),
                               .sizeBytes = static_cast<std::uint64_t>(pIndex->fileSize())});
    }
  }
  out.totals = {.requests = snapshot.requests(),
                .bodyBytes = snapshot.bodyBytes,
                .minBodyBytes = snapshot.minBodyBytes,
                .maxBodyBytes = snapshot.maxBodyBytes,
                .elapsedSeconds = snapshot.elapsedSeconds,
                .meanRequestsPerSecond = snapshot.meanRequestsPerSecond,
                .peakRequestsPerSecond = snapshot.peakRequestsPerSecond,
                .sustainedRequestsPerSecond = snapshot.sustainedRequestsPerSecond,
                .bodyBytesPerSecond = snapshot.bodyBytesPerSecond,
                .cpuSeconds = snapshot.cpuSeconds,
                .clientLoad = snapshot.clientLoad,
                .clientSaturated = snapshot.clientSaturated};
  out.latency = snapshot.total;
  out.headLatency = snapshot.head;
  for (std::size_t idx = 0; idx < kNbLoadClasses; ++idx) {
    if (snapshot.counts[idx] != 0) {
      out.classes.push_back(
          {.name = kClassNames[idx], .count = snapshot.counts[idx], .latency = snapshot.byClass[idx]});
    }
  }
  out.statuses = snapshot.statuses;
  for (std::size_t idx = 0; idx < kNbLoadErrorKinds; ++idx) {
    const LoadErrorStats& error = snapshot.errors[idx];
    if (error.count != 0) {
      out.errors.push_back({.kind = kErrorKindNames[idx], .count = error.count, .firstMessage = error.firstMessage});
    }
  }
  out.histograms = {.total = BucketsOf(snapshot.totalHistogram), .head = BucketsOf(snapshot.headHistogram)};
  out.series = {.classes = kClassNames, .firstSecond = snapshot.firstSecond, .points = snapshot.series};

  std::string buffer;
  if (const auto errc = glz::write<kWriteOpts>(out, buffer); errc) {
    buffer.clear();  // cannot happen: no variant nor custom writer that may fail
  }
  buffer.push_back('\n');
  return buffer;
}

std::expected<void, RawChars32> ExportLoadReport(const std::filesystem::path& path, const LoadSnapshot& snapshot,
                                                 const LoadSettings& settings, const LoadReportContext& context) {
  return WriteFileAtomically(path, SerializeLoadReport(snapshot, settings, context));
}

RawChars32 LoadReportFileName(std::string_view requestName, std::chrono::system_clock::time_point startedAt) {
  static constexpr std::size_t kMaxNameBytes = 64;
  static constexpr std::size_t kDateBytes = 32;
  RawChars32 ret(kMaxNameBytes + kDateBytes);
  bool pendingDash = false;
  for (char ch : requestName) {
    if (ret.size() >= kMaxNameBytes) {
      break;
    }
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_') {
      if (pendingDash) {
        ret.unchecked_push_back('-');
      }
      ret.unchecked_push_back(ch);
      pendingDash = false;
    } else {
      pendingDash = !ret.empty();
    }
  }
  if (ret.empty()) {
    ret.unchecked_append(std::string_view{"run"});
  }
  // Formatted in place: no copy from a stack buffer.
  const auto res = std::format_to_n(ret.data() + ret.size(), static_cast<std::ptrdiff_t>(kDateBytes),
                                    "-{:%Y%m%d-%H%M%S}.json", std::chrono::floor<std::chrono::seconds>(startedAt));
  ret.setEnd(res.out);
  return ret;
}

}  // namespace zalie
