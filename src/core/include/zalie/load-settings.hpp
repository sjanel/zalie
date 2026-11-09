#pragma once

#include <amc/vector.hpp>
#include <cstdint>

#include "zalie/raw-chars.hpp"

namespace zalie {

inline constexpr std::uint32_t kDefaultLoadDurationMs = 10000;

// Where the values of a parameter of a run come from.
enum class ValueSource : std::uint8_t {
  fixed,
  list,
  integerRange,
  decimalRange,
  dateRange,
  randomString,
  uuid,
  timestamp,
  dataFile
};

// How a list, a range or a data file is read. 'shuffled' sends each value once per pass, in a random order.
enum class ValueOrder : std::uint8_t { inOrder, random, shuffled };

// How the parameters advance relative to each other. With 'together' and 'allCombinations' they share one position, so
// the order of the first of them applies to all.
enum class CombineMode : std::uint8_t { independent, together, allCombinations };

enum class TimestampFormat : std::uint8_t { seconds, milliseconds, iso8601 };

// Plain text (one value per line), CSV with a header (one column per variable), or JSON Lines (one column per top-level
// key of the first line).
enum class DataFileFormat : std::uint8_t { text, csv, jsonLines };

// One row of the parameter table of a run. Every source keeps its own fields, so switching source in the UI loses
// nothing.
struct LoadParameter {
  bool operator==(const LoadParameter&) const = default;

  // The {{variable}} fed by this row or, with 'queryParameter', the key of a query parameter of the request.
  RawChars32 name;
  RawChars32 fixedValue;
  // One value per line.
  RawChars32 listValues;
  // "2026-01-31", optionally followed by "T12:30" or "T12:30:45", UTC.
  RawChars32 dateFrom;
  RawChars32 dateTo;
  // Conversions %Y %m %d %H %M %S %j %s %%. Empty: "%Y-%m-%d".
  RawChars32 dateFormat;
  // Empty: the lower-case letters.
  RawChars32 randomCharacters;
  // Absolute, or relative to the directory of the collection.
  RawChars32 filePath;
  // Empty: the first column.
  RawChars32 fileColumn;
  double decimalFrom{0.0};
  double decimalTo{1.0};
  double decimalStep{0.1};
  std::int64_t integerFrom{1};
  std::int64_t integerTo{100};
  std::int64_t integerStep{1};
  std::int64_t dateStepSeconds{86400};
  std::uint32_t randomLength{8};
  // Minimum number of digits: 5 gives 00001.
  std::uint8_t integerPadding{0};
  std::uint8_t decimals{2};
  ValueSource source{ValueSource::fixed};
  ValueOrder order{ValueOrder::inOrder};
  TimestampFormat timestampFormat{TimestampFormat::seconds};
  DataFileFormat fileFormat{DataFileFormat::csv};
  // 'name' is the key of a query parameter whose value is replaced for the run only: a normal Fetch keeps the original.
  bool queryParameter{false};

  using trivially_relocatable = AllTriviallyRelocatable<RawChars32>;
};

// The run settings of a request, saved next to it. For the limits, 0 means none: the run ends at the first one reached.
struct LoadSettings {
  bool operator==(const LoadSettings&) const = default;

  amc::vector<LoadParameter> parameters;
  // Equal seeds give equal requests, timestamps aside. 0: a new seed for each run.
  std::uint64_t seed{0};
  std::uint64_t maxRequests{0};
  std::uint32_t durationMs{kDefaultLoadDurationMs};
  std::uint32_t maxRequestsPerSecond{0};
  std::uint32_t warmupMs{0};
  // 0: the number of vCPUs.
  std::uint32_t connections{0};
  CombineMode combine{CombineMode::independent};
  // Also ends the run after one pass over the values.
  bool onePass{false};

  using trivially_relocatable = AllTriviallyRelocatable<amc::vector<LoadParameter>>;
};

}  // namespace zalie
