#include "zalie/value-generators.hpp"

#include <algorithm>
#include <amc/vector.hpp>
#include <array>
#include <bit>
#include <cassert>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>

#include "zalie/data-file.hpp"
#include "zalie/load-settings.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/string-utils.hpp"
#include "zalie/variables.hpp"

namespace zalie {

namespace {

constexpr std::uint64_t kMaxCount = std::numeric_limits<std::uint64_t>::max();
constexpr std::string_view kLowerCaseLetters = "abcdefghijklmnopqrstuvwxyz";
constexpr std::string_view kDefaultDateFormat = "%Y-%m-%d";
constexpr std::string_view kDateConversions = "YmdHMSjs%";
constexpr std::uint32_t kMaxRandomLength = 1U << 20;
constexpr std::uint32_t kMaxDecimals = 30;
constexpr int kFeistelRounds = 4;
// The rounding of a decimal range: 0.7 - 0.1 divided by 0.1 is just under 6.
constexpr double kDecimalCountTolerance = 1e-9;
// Largest number of values of a decimal range: beyond, the double precision cannot tell two neighbours apart.
constexpr double kMaxDecimalCount = 9e15;

constexpr std::size_t kIntegerBytes = 24;
// A double written without exponent, its sign and its point included, whatever the number of decimals.
constexpr std::size_t kDecimalBytes = 350 + kMaxDecimals;
constexpr std::size_t kUuidBytes = 36;
constexpr std::size_t kClockBytes = 64;
constexpr std::size_t kDateConversionBytes = 24;

[[nodiscard]] constexpr std::uint64_t Mix64(std::uint64_t value) noexcept {  // the finalizer of SplitMix64
  value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
  return value ^ (value >> 31U);
}

[[nodiscard]] constexpr std::uint64_t Hash(std::uint64_t key, std::uint64_t value) noexcept {
  return Mix64(Mix64(key + 0x9E3779B97F4A7C15ULL) + value);
}

// The position of 'index' in a pseudo-random shuffle of [0, count) chosen by 'key', without storing the shuffle: a
// Feistel network on the smallest even number of bits holding 'count', walking back into the range when it lands above.
[[nodiscard]] constexpr std::uint64_t Permute(std::uint64_t key, std::uint64_t index, std::uint64_t count) noexcept {
  if (count <= 1) {
    return 0;
  }
  auto bits = static_cast<unsigned>(std::bit_width(count - 1U));
  bits = std::max(bits + (bits & 1U), 2U);
  const unsigned halfBits = bits / 2U;
  const std::uint64_t mask = (std::uint64_t{1} << halfBits) - 1U;
  do {
    std::uint64_t left = index >> halfBits;
    std::uint64_t right = index & mask;
    for (int round = 0; round < kFeistelRounds; ++round) {
      const std::uint64_t next = left ^ (Hash(key + static_cast<std::uint64_t>(round), right) & mask);
      left = right;
      right = next;
    }
    index = (left << halfBits) | right;
  } while (index >= count);
  return index;
}

// A value of [0, count) for a request: in 'number' order, at random, or through a shuffle redone at each pass.
[[nodiscard]] constexpr std::uint64_t PickPosition(ValueOrder order, std::uint64_t number, std::uint64_t count,
                                                   std::uint64_t key) noexcept {
  switch (order) {
    case ValueOrder::random:
      return Hash(key, number) % count;
    case ValueOrder::shuffled:
      return Permute(Hash(key, number / count), number % count, count);
    case ValueOrder::inOrder:
      break;
  }
  return number % count;
}

[[nodiscard]] constexpr std::uint64_t SaturatingMultiply(std::uint64_t lhs, std::uint64_t rhs) noexcept {
  return lhs != 0 && rhs > kMaxCount / lhs ? kMaxCount : lhs * rhs;
}

// Number of values from 'first' to 'last' by 'step', 0 if 'last' cannot be reached. Saturates.
[[nodiscard]] constexpr std::uint64_t RangeCount(std::int64_t first, std::int64_t last, std::int64_t step) noexcept {
  const bool ascending = step > 0;
  if (step == 0 || (ascending ? first > last : first < last)) {
    return 0;
  }
  const std::uint64_t span = ascending ? static_cast<std::uint64_t>(last) - static_cast<std::uint64_t>(first)
                                       : static_cast<std::uint64_t>(first) - static_cast<std::uint64_t>(last);
  const std::uint64_t magnitude =
      ascending ? static_cast<std::uint64_t>(step) : std::uint64_t{0} - static_cast<std::uint64_t>(step);
  const std::uint64_t quotient = span / magnitude;
  return quotient == kMaxCount ? kMaxCount : quotient + 1U;
}

// The value at position 'row' of a range, without signed overflow.
[[nodiscard]] constexpr std::int64_t RangeValue(std::int64_t first, std::int64_t step, std::uint64_t row) noexcept {
  return static_cast<std::int64_t>(static_cast<std::uint64_t>(first) + (row * static_cast<std::uint64_t>(step)));
}

// Parses "2026-01-31", optionally followed by "T12:30" or "T12:30:45" (a space also separates the time), UTC.
[[nodiscard]] bool ParseDate(std::string_view text, std::int64_t& seconds) noexcept {
  using namespace std::chrono;
  const auto readNumber = [](std::string_view& rest, std::size_t nbDigits, int& out) {
    if (rest.size() < nbDigits) {
      return false;
    }
    const char* pEnd = rest.data() + nbDigits;
    const auto res = std::from_chars(rest.data(), pEnd, out);
    if (res.ec != std::errc{} || res.ptr != pEnd) {
      return false;
    }
    rest.remove_prefix(nbDigits);
    return true;
  };
  const auto skip = [](std::string_view& rest, std::string_view separators) {
    if (rest.empty() || !separators.contains(rest.front())) {
      return false;
    }
    rest.remove_prefix(1);
    return true;
  };
  int yearValue = 0;
  int monthValue = 0;
  int dayValue = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  if (!readNumber(text, 4, yearValue) || !skip(text, "-") || !readNumber(text, 2, monthValue) || !skip(text, "-") ||
      !readNumber(text, 2, dayValue)) {
    return false;
  }
  if (!text.empty()) {
    if (!skip(text, "T ") || !readNumber(text, 2, hour) || !skip(text, ":") || !readNumber(text, 2, minute)) {
      return false;
    }
    if (!text.empty() && (!skip(text, ":") || !readNumber(text, 2, second) || !text.empty())) {
      return false;
    }
  }
  const year_month_day date{year{yearValue}, month{static_cast<unsigned>(monthValue)},
                            day{static_cast<unsigned>(dayValue)}};
  if (!date.ok() || hour > 23 || minute > 59 || second > 59) {
    return false;
  }
  seconds = (sys_days{date}.time_since_epoch().count() * 86400LL) + (hour * 3600LL) + (minute * 60LL) + second;
  return true;
}

// Whether every conversion of a date format is known.
[[nodiscard]] bool IsValidDateFormat(std::string_view format) noexcept {
  for (std::size_t pos = format.find('%'); pos != std::string_view::npos; pos = format.find('%', pos + 2)) {
    if (pos + 1 == format.size() || !kDateConversions.contains(format[pos + 1])) {
      return false;
    }
  }
  return true;
}

// Appends 'value' with at least 'width' digits, its sign first.
void AppendPadded(RawChars32& out, std::int64_t value, std::uint32_t width) {
  std::array<char, kIntegerBytes> digits{};
  const auto res = std::to_chars(digits.data(), digits.data() + digits.size(), value);
  std::string_view text(digits.data(), static_cast<std::size_t>(res.ptr - digits.data()));
  if (value < 0) {
    out.push_back('-');
    text.remove_prefix(1);
  }
  for (std::size_t idx = text.size(); idx < width; ++idx) {
    out.push_back('0');
  }
  // Not append(): GCC 15 sees its branch for copies over 32 bytes and reports a read past 'digits' (-Warray-bounds).
  out.ensureAvailableCapacity(text.size());
  char* pEnd = out.data() + out.size();
  std::memcpy(pEnd, text.data(), text.size());
  out.setEnd(pEnd + text.size());
}

void AppendDate(RawChars32& out, std::int64_t seconds, std::string_view format) {
  using namespace std::chrono;
  const sys_seconds instant{std::chrono::seconds{seconds}};
  const sys_days dayStart = floor<days>(instant);
  const year_month_day date{dayStart};
  const hh_mm_ss<std::chrono::seconds> time{instant - dayStart};
  for (std::size_t pos = 0; pos < format.size(); ++pos) {
    if (format[pos] != '%') {
      out.push_back(format[pos]);
      continue;
    }
    switch (format[++pos]) {
      case 'Y':
        AppendPadded(out, static_cast<int>(date.year()), 4);
        break;
      case 'm':
        AppendPadded(out, static_cast<unsigned>(date.month()), 2);
        break;
      case 'd':
        AppendPadded(out, static_cast<unsigned>(date.day()), 2);
        break;
      case 'H':
        AppendPadded(out, time.hours().count(), 2);
        break;
      case 'M':
        AppendPadded(out, time.minutes().count(), 2);
        break;
      case 'S':
        AppendPadded(out, time.seconds().count(), 2);
        break;
      case 'j':
        AppendPadded(out, (dayStart - sys_days{date.year() / January / 1}).count() + 1, 3);
        break;
      case 's':
        AppendPadded(out, seconds, 0);
        break;
      default:
        out.push_back('%');
        break;
    }
  }
}

// One value per line (the line breaks of Windows included), blank lines skipped.
void SplitLines(std::string_view text, StringPool& strings, amc::vector<std::string_view>& lines) {
  std::string_view stored = strings.store(text);
  while (!stored.empty()) {
    const std::size_t lineEnd = stored.find('\n');
    std::string_view line = stored.substr(0, lineEnd);
    stored = lineEnd == std::string_view::npos ? std::string_view{} : stored.substr(lineEnd + 1);
    if (line.ends_with('\r')) {
      line.remove_suffix(1);
    }
    if (!line.empty()) {
      lines.push_back(line);
    }
  }
}

}  // namespace

void ValueGenerators::addSource(Variable& variable, std::uint64_t count, ValueOrder order, std::uint32_t dataFile) {
  variable.source = _sources.size();
  _sources.push_back({.count = count, .dataFile = dataFile, .order = order});
}

std::uint32_t ValueGenerators::add(const LoadParameter& parameter, const std::filesystem::path& baseDirectory,
                                   DataFileIndexCache& dataFiles, RawChars32& error) {
  Variable variable;
  std::size_t maxBytes = 0;
  switch (parameter.source) {
    case ValueSource::list: {
      const std::uint32_t begin = _listValues.size();
      SplitLines(parameter.listValues, _strings, _listValues);
      if (_listValues.size() == begin) {
        error.assign(std::string_view{"The list is empty"});
        return kNoVariable;
      }
      variable.kind = Kind::list;
      variable.position = begin;
      addSource(variable, _listValues.size() - begin, parameter.order);
      break;
    }
    case ValueSource::integerRange: {
      if (parameter.integerStep == 0) {
        error.assign(std::string_view{"The step is zero"});
        return kNoVariable;
      }
      const std::uint64_t count = RangeCount(parameter.integerFrom, parameter.integerTo, parameter.integerStep);
      if (count == 0) {
        error.assign(std::string_view{"The range is empty"});
        return kNoVariable;
      }
      variable.kind = Kind::integer;
      variable.first = parameter.integerFrom;
      variable.step = parameter.integerStep;
      variable.width = parameter.integerPadding;
      maxBytes = kIntegerBytes + parameter.integerPadding;
      addSource(variable, count, parameter.order);
      break;
    }
    case ValueSource::decimalRange: {
      if (parameter.decimalStep == 0.0 || !std::isfinite(parameter.decimalStep)) {
        error.assign(std::string_view{"The step is zero"});
        return kNoVariable;
      }
      if (parameter.decimals > kMaxDecimals) {
        error.assign(std::string_view{"Too many decimals"});
        return kNoVariable;
      }
      const double steps = (parameter.decimalTo - parameter.decimalFrom) / parameter.decimalStep;
      if (!std::isfinite(steps) || steps < -kDecimalCountTolerance) {
        error.assign(std::string_view{"The range is empty"});
        return kNoVariable;
      }
      if (steps > kMaxDecimalCount) {
        error.assign(std::string_view{"The range has too many values"});
        return kNoVariable;
      }
      variable.kind = Kind::decimal;
      variable.decimalFirst = parameter.decimalFrom;
      variable.decimalStep = parameter.decimalStep;
      variable.width = parameter.decimals;
      maxBytes = kDecimalBytes;
      addSource(variable, static_cast<std::uint64_t>(std::floor(steps + kDecimalCountTolerance)) + 1U, parameter.order);
      break;
    }
    case ValueSource::dateRange: {
      std::int64_t first = 0;
      std::int64_t last = 0;
      if (!ParseDate(Trim(parameter.dateFrom), first) || !ParseDate(Trim(parameter.dateTo), last)) {
        error.assign(std::string_view{"Invalid date: expected 2026-01-31, optionally followed by T12:30:45"});
        return kNoVariable;
      }
      const std::string_view format = parameter.dateFormat.empty() ? kDefaultDateFormat : View(parameter.dateFormat);
      if (!IsValidDateFormat(format)) {
        error.assign(std::string_view{"Unknown conversion in the date format"});
        return kNoVariable;
      }
      if (parameter.dateStepSeconds == 0) {
        error.assign(std::string_view{"The step is zero"});
        return kNoVariable;
      }
      const std::uint64_t count = RangeCount(first, last, parameter.dateStepSeconds);
      if (count == 0) {
        error.assign(std::string_view{"The range is empty"});
        return kNoVariable;
      }
      variable.kind = Kind::date;
      variable.first = first;
      variable.step = parameter.dateStepSeconds;
      variable.text = _strings.store(format);
      maxBytes = format.size() * kDateConversionBytes;
      addSource(variable, count, parameter.order);
      break;
    }
    case ValueSource::randomString: {
      if (parameter.randomLength == 0 || parameter.randomLength > kMaxRandomLength) {
        error.assign(std::string_view{"Invalid length of the random string"});
        return kNoVariable;
      }
      variable.kind = Kind::randomString;
      variable.width = parameter.randomLength;
      variable.text =
          _strings.store(parameter.randomCharacters.empty() ? kLowerCaseLetters : View(parameter.randomCharacters));
      maxBytes = parameter.randomLength;
      break;
    }
    case ValueSource::uuid:
      variable.kind = Kind::uuid;
      maxBytes = kUuidBytes;
      break;
    case ValueSource::timestamp: {
      static constexpr std::array<std::string_view, 3> kClockNames = {"$timestamp", "$timestampMs", "$isoTimestamp"};
      variable.kind = Kind::clock;
      variable.text = kClockNames[static_cast<std::size_t>(parameter.timestampFormat)];
      maxBytes = kClockBytes;
      break;
    }
    case ValueSource::dataFile: {
      if (Trim(parameter.filePath).empty()) {
        error.assign(std::string_view{"No file"});
        return kNoVariable;
      }
      std::shared_ptr<const DataFileIndex> pIndex =
          dataFiles.get(ResolveDataFilePath(Trim(parameter.filePath), baseDirectory), parameter.fileFormat);
      if (!pIndex->valid()) {
        error.assign(pIndex->error());
        return kNoVariable;
      }
      variable.kind = Kind::dataFile;
      if (!parameter.fileColumn.empty()) {
        variable.position = pIndex->findColumn(parameter.fileColumn);
        if (variable.position == pIndex->columns().size()) {
          error = Concat({"No column '", parameter.fileColumn, "' in the file"});
          return kNoVariable;
        }
      }
      // The columns of one file stay together: they share its row.
      const auto known = std::ranges::find_if(
          _dataFiles, [&pIndex](const std::shared_ptr<const DataFileIndex>& pOther) { return pOther == pIndex; });
      if (known != _dataFiles.end()) {
        const auto dataFile = static_cast<std::uint32_t>(known - _dataFiles.begin());
        const auto source = std::ranges::find(_sources, dataFile, &Source::dataFile);
        variable.source = static_cast<std::uint32_t>(source - _sources.begin());
      } else {
        addSource(variable, pIndex->nbRows(), parameter.order, _dataFiles.size());
        _dataFiles.push_back(std::move(pIndex));
      }
      break;
    }
    case ValueSource::fixed:
      error.assign(std::string_view{"A fixed value is not generated"});
      return kNoVariable;
  }
  _maxGeneratedBytes += maxBytes;
  _variables.push_back(variable);
  return _variables.size() - 1U;
}

std::uint32_t ValueGenerators::addDynamic(std::string_view name) {
  Variable variable;
  std::size_t maxBytes = 0;
  if (name == "$randomUUID" || name == "$guid") {
    variable.kind = Kind::uuid;
    maxBytes = kUuidBytes;
  } else if (name == "$randomInt") {
    variable.kind = Kind::randomInt;
    maxBytes = kIntegerBytes;
  } else {
    variable.kind = Kind::clock;
    variable.text = _strings.store(name);
    maxBytes = kClockBytes;
  }
  _maxGeneratedBytes += maxBytes;
  _variables.push_back(variable);
  return _variables.size() - 1U;
}

void ValueGenerators::finalize() noexcept {
  _passLength = 0;
  _sharedCount = 0;
  if (_sources.empty()) {
    return;
  }
  _sharedOrder = _sources.front().order;
  std::uint64_t smallest = kMaxCount;
  std::uint64_t largest = 0;
  std::uint64_t product = 1;
  for (const Source& source : _sources) {
    smallest = std::min(smallest, source.count);
    largest = std::max(largest, source.count);
    product = SaturatingMultiply(product, source.count);
  }
  switch (_combine) {
    case CombineMode::independent:
      _passLength = largest;
      break;
    case CombineMode::together:
      _passLength = _sharedCount = smallest;
      break;
    case CombineMode::allCombinations:
      _passLength = _sharedCount = product;
      break;
  }
}

std::uint64_t ValueGenerators::valueCount(std::uint32_t variable) const noexcept {
  const std::uint32_t source = _variables[variable].source;
  return source == kNoSource ? 0 : _sources[source].count;
}

void ValueGenerators::computeRows(std::uint64_t number, std::span<std::uint64_t> rows) const noexcept {
  if (_combine == CombineMode::independent) {
    for (std::uint32_t idx = 0; idx < _sources.size(); ++idx) {
      rows[idx] = PickPosition(_sources[idx].order, number, _sources[idx].count, Hash(_seed, idx));
    }
    return;
  }
  if (_sharedCount == 0) {
    return;
  }
  std::uint64_t position = PickPosition(_sharedOrder, number, _sharedCount, _seed);
  if (_combine == CombineMode::together) {
    std::ranges::fill(rows, position);
    return;
  }
  for (std::uint32_t idx = _sources.size(); idx-- > 0;) {  // the last parameter advances first, like nested loops
    rows[idx] = position % _sources[idx].count;
    position /= _sources[idx].count;
  }
}

void ValueGenerators::generate(const Variable& variable, std::uint32_t index, std::uint64_t number, std::uint64_t row,
                               RawChars32& out) const {
  switch (variable.kind) {
    case Kind::integer:
      AppendPadded(out, RangeValue(variable.first, variable.step, row), variable.width);
      break;
    case Kind::decimal: {
      out.ensureAvailableCapacity(kDecimalBytes);
      const double value = variable.decimalFirst + (static_cast<double>(row) * variable.decimalStep);
      const auto res = std::to_chars(out.data() + out.size(), out.data() + out.size() + kDecimalBytes, value,
                                     std::chars_format::fixed, static_cast<int>(variable.width));
      out.setEnd(res.ptr);
      break;
    }
    case Kind::date:
      AppendDate(out, RangeValue(variable.first, variable.step, row), variable.text);
      break;
    case Kind::randomString: {
      std::uint64_t state = Hash(_seed + index, number);
      for (std::uint32_t idx = 0; idx < variable.width; ++idx) {
        state = Mix64(state + 0x9E3779B97F4A7C15ULL);
        out.push_back(variable.text[static_cast<std::size_t>(state % variable.text.size())]);
      }
      break;
    }
    case Kind::uuid: {
      const std::uint64_t high = Hash(_seed + index, number);
      AppendUuid(high, Mix64(high + 0x9E3779B97F4A7C15ULL), out);
      break;
    }
    case Kind::randomInt:
      AppendInt(out, Hash(_seed + index, number) % 1001U);
      break;
    case Kind::clock: {
      [[maybe_unused]] const bool isKnown = AppendDynamicVariable(variable.text, out);
      assert(isKnown);
      break;
    }
    case Kind::list:
    case Kind::dataFile:
      break;  // views of the list or of the row: nothing to generate
  }
}

ValueGenerators::Cursor::Cursor(const ValueGenerators& generators) : _pGenerators(&generators) {
  _values.resize(generators._variables.size());
  _rows.resize(generators._sources.size());
  _generated.reserve(static_cast<RawChars32::size_type>(generators._maxGeneratedBytes));
  _readers.resize(generators._sources.size());
  for (std::uint32_t idx = 0; idx < generators._sources.size(); ++idx) {
    const std::uint32_t dataFile = generators._sources[idx].dataFile;
    if (dataFile == kNoSource) {
      continue;
    }
    _readers[idx] = std::make_unique<DataFileReader>(*generators._dataFiles[dataFile]);
    if (_error.empty() && !_readers[idx]->error().empty()) {
      _error.assign(_readers[idx]->error());
    }
  }
}

bool ValueGenerators::Cursor::fill(std::uint64_t number) {
  const ValueGenerators& generators = *_pGenerators;
  generators.computeRows(number, _rows);
  for (std::uint32_t idx = 0; idx < _readers.size(); ++idx) {
    if (_readers[idx] != nullptr && !_readers[idx]->read(_rows[idx])) {
      return false;
    }
  }
  _generated.clear();
  for (std::uint32_t idx = 0; idx < generators._variables.size(); ++idx) {
    const Variable& variable = generators._variables[idx];
    const std::uint64_t row = variable.source == kNoSource ? 0 : _rows[variable.source];
    if (variable.kind == Kind::list) {
      _values[idx] = generators._listValues[variable.position + static_cast<std::uint32_t>(row)];
    } else if (variable.kind == Kind::dataFile) {
      _values[idx] = _readers[variable.source]->value(variable.position);
    } else {
      const auto begin = _generated.size();
      generators.generate(variable, idx, number, row, _generated);
      _values[idx] = std::string_view(_generated).substr(begin);
    }
  }
  return true;
}

}  // namespace zalie
