#pragma once

#include <chrono>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

#include "zalie/load-runner.hpp"
#include "zalie/load-settings.hpp"
#include "zalie/method.hpp"
#include "zalie/raw-chars.hpp"

namespace zalie {

// What a report tells about the run besides its snapshot.
struct LoadReportContext {
  std::string_view requestName;
  // The URL as written in the request, {{variables}} included.
  std::string_view url;
  std::chrono::system_clock::time_point startedAt;
  Method method{Method::get};
};

// The whole run as JSON, the format of the saved runs: request, settings, seed, data files, totals, percentiles,
// histograms (non-empty buckets), statuses, errors and time series.
[[nodiscard]] std::string SerializeLoadReport(const LoadSnapshot& snapshot, const LoadSettings& settings,
                                              const LoadReportContext& context);

// Writes the report of a run to 'path' (atomically, parent directories created).
[[nodiscard]] std::expected<void, RawChars32> ExportLoadReport(const std::filesystem::path& path,
                                                               const LoadSnapshot& snapshot,
                                                               const LoadSettings& settings,
                                                               const LoadReportContext& context);

// A file name for the report of a run: "<request name>-<yyyymmdd-hhmmss>.json" in UTC, the name reduced to letters,
// digits, '-' and '_'.
[[nodiscard]] RawChars32 LoadReportFileName(std::string_view requestName,
                                            std::chrono::system_clock::time_point startedAt);

}  // namespace zalie
