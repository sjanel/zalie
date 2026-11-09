#include "zalie/value-generators.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "zalie/data-file.hpp"
#include "zalie/load-settings.hpp"
#include "zalie/raw-chars.hpp"

namespace zalie {
namespace {

constexpr std::uint64_t kSeed = 42;

LoadParameter ListOf(std::string_view values, ValueOrder order = ValueOrder::inOrder) {
  LoadParameter parameter;
  parameter.source = ValueSource::list;
  parameter.listValues.assign(values);
  parameter.order = order;
  return parameter;
}

LoadParameter IntegerRange(std::int64_t from, std::int64_t to, std::int64_t step = 1,
                           ValueOrder order = ValueOrder::inOrder) {
  LoadParameter parameter;
  parameter.source = ValueSource::integerRange;
  parameter.integerFrom = from;
  parameter.integerTo = to;
  parameter.integerStep = step;
  parameter.order = order;
  return parameter;
}

class ValueGeneratorsTest : public ::testing::Test {
 protected:
  void TearDown() override {
    if (!_directory.empty()) {
      std::filesystem::remove_all(_directory);
    }
  }

  std::uint32_t add(ValueGenerators& generators, const LoadParameter& parameter) {
    RawChars32 error;
    const std::uint32_t variable = generators.add(parameter, _directory, _cache, error);
    EXPECT_TRUE(error.empty()) << View(error);
    return variable;
  }

  std::string errorOf(const LoadParameter& parameter) {
    ValueGenerators generators(CombineMode::independent, kSeed);
    RawChars32 error;
    EXPECT_EQ(generators.add(parameter, _directory, _cache, error), ValueGenerators::kNoVariable);
    return std::string(View(error));
  }

  // The value of the single variable for the requests 0 to count - 1.
  std::vector<std::string> valuesOf(const LoadParameter& parameter, std::uint64_t count, std::uint64_t seed = kSeed) {
    ValueGenerators generators(CombineMode::independent, seed);
    add(generators, parameter);
    generators.finalize();
    ValueGenerators::Cursor cursor(generators);
    std::vector<std::string> values;
    for (std::uint64_t number = 0; number < count; ++number) {
      EXPECT_TRUE(cursor.fill(number));
      values.emplace_back(cursor.values()[0]);
    }
    return values;
  }

  std::filesystem::path writeFile(std::string_view name, std::string_view content) {
    if (_directory.empty()) {
      _directory = std::filesystem::temp_directory_path() /
                   ("zalie-generators-" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
      std::filesystem::create_directories(_directory);
    }
    const std::filesystem::path path = _directory / name;
    std::ofstream(path, std::ios::binary) << content;
    return path;
  }

  std::filesystem::path _directory;
  DataFileIndexCache _cache;
};

using Values = std::vector<std::string>;

TEST_F(ValueGeneratorsTest, ListIsReadInOrderAndWraps) {
  EXPECT_EQ(valuesOf(ListOf("dog\ncat\nbird"), 5), (Values{"dog", "cat", "bird", "dog", "cat"}));
}

TEST_F(ValueGeneratorsTest, ListSkipsBlankLinesAndWindowsLineEnds) {
  EXPECT_EQ(valuesOf(ListOf("dog\r\n\r\n\ncat\n"), 2), (Values{"dog", "cat"}));
}

TEST_F(ValueGeneratorsTest, ListKeepsSpacesAndAnyCharacter) {
  EXPECT_EQ(valuesOf(ListOf(" a b \nc&d=e"), 2), (Values{" a b ", "c&d=e"}));
}

TEST_F(ValueGeneratorsTest, EmptyListIsAnError) { EXPECT_EQ(errorOf(ListOf("\n\n")), "The list is empty"); }

TEST_F(ValueGeneratorsTest, IntegerRange) {
  EXPECT_EQ(valuesOf(IntegerRange(1, 3), 4), (Values{"1", "2", "3", "1"}));
  EXPECT_EQ(valuesOf(IntegerRange(0, 1000, 500), 3), (Values{"0", "500", "1000"}));
}

TEST_F(ValueGeneratorsTest, IntegerRangeStopsBeforeOverrunning) {
  EXPECT_EQ(valuesOf(IntegerRange(0, 10, 4), 4), (Values{"0", "4", "8", "0"}));
}

TEST_F(ValueGeneratorsTest, IntegerRangeCanGoDown) {
  EXPECT_EQ(valuesOf(IntegerRange(10, 0, -5), 3), (Values{"10", "5", "0"}));
  EXPECT_EQ(valuesOf(IntegerRange(-1, -3, -1), 3), (Values{"-1", "-2", "-3"}));
}

TEST_F(ValueGeneratorsTest, IntegerRangeZeroPadding) {
  LoadParameter parameter = IntegerRange(9, 11);
  parameter.integerPadding = 5;
  EXPECT_EQ(valuesOf(parameter, 3), (Values{"00009", "00010", "00011"}));
  parameter = IntegerRange(-2, -2);
  parameter.integerPadding = 3;
  EXPECT_EQ(valuesOf(parameter, 1), (Values{"-002"}));
}

TEST_F(ValueGeneratorsTest, IntegerRangeErrors) {
  EXPECT_EQ(errorOf(IntegerRange(1, 10, 0)), "The step is zero");
  EXPECT_EQ(errorOf(IntegerRange(10, 1, 1)), "The range is empty");
  EXPECT_EQ(errorOf(IntegerRange(1, 10, -1)), "The range is empty");
}

TEST_F(ValueGeneratorsTest, HugeIntegerRangeDoesNotOverflow) {
  const std::int64_t min = std::numeric_limits<std::int64_t>::min();
  const std::int64_t max = std::numeric_limits<std::int64_t>::max();
  ValueGenerators generators(CombineMode::independent, kSeed);
  const std::uint32_t variable = add(generators, IntegerRange(min, max));
  generators.finalize();
  EXPECT_EQ(generators.valueCount(variable), std::numeric_limits<std::uint64_t>::max());
  ValueGenerators::Cursor cursor(generators);
  ASSERT_TRUE(cursor.fill(1));
  EXPECT_EQ(cursor.values()[0], "-9223372036854775807");
}

LoadParameter DecimalRange(double from, double to, double step, std::uint8_t decimals) {
  LoadParameter parameter;
  parameter.source = ValueSource::decimalRange;
  parameter.decimalFrom = from;
  parameter.decimalTo = to;
  parameter.decimalStep = step;
  parameter.decimals = decimals;
  return parameter;
}

TEST_F(ValueGeneratorsTest, DecimalRange) {
  EXPECT_EQ(valuesOf(DecimalRange(0.5, 2.0, 0.25, 2), 7),
            (Values{"0.50", "0.75", "1.00", "1.25", "1.50", "1.75", "2.00"}));
}

TEST_F(ValueGeneratorsTest, DecimalRangeCountIsNotFooledByRounding) {
  ValueGenerators generators(CombineMode::independent, kSeed);
  const std::uint32_t variable = add(generators, DecimalRange(0.1, 0.7, 0.1, 1));
  generators.finalize();
  EXPECT_EQ(generators.valueCount(variable), 7U);  // (0.7 - 0.1) / 0.1 is just under 6
}

TEST_F(ValueGeneratorsTest, DecimalRangeErrors) {
  EXPECT_EQ(errorOf(DecimalRange(0, 1, 0, 2)), "The step is zero");
  EXPECT_EQ(errorOf(DecimalRange(2, 1, 0.5, 2)), "The range is empty");
  EXPECT_EQ(errorOf(DecimalRange(0, 1, 0.5, 200)), "Too many decimals");
}

LoadParameter DateRange(std::string_view from, std::string_view to, std::int64_t stepSeconds = 86400,
                        std::string_view format = {}) {
  LoadParameter parameter;
  parameter.source = ValueSource::dateRange;
  parameter.dateFrom.assign(from);
  parameter.dateTo.assign(to);
  parameter.dateStepSeconds = stepSeconds;
  parameter.dateFormat.assign(format);
  return parameter;
}

TEST_F(ValueGeneratorsTest, DateRangeEachDayOfAMonth) {
  ValueGenerators generators(CombineMode::independent, kSeed);
  const std::uint32_t variable = add(generators, DateRange("2026-01-01", "2026-01-31"));
  generators.finalize();
  EXPECT_EQ(generators.valueCount(variable), 31U);
  EXPECT_EQ(valuesOf(DateRange("2026-01-30", "2026-02-01"), 3), (Values{"2026-01-30", "2026-01-31", "2026-02-01"}));
}

TEST_F(ValueGeneratorsTest, DateRangeCustomFormatAndTime) {
  EXPECT_EQ(valuesOf(DateRange("2026-03-05T22:30", "2026-03-06T00:30", 3600, "%d/%m/%Y %H:%M:%S (%j) 100%%"), 2),
            (Values{"05/03/2026 22:30:00 (064) 100%", "05/03/2026 23:30:00 (064) 100%"}));
  EXPECT_EQ(valuesOf(DateRange("1970-01-02", "1970-01-02", 86400, "%s"), 1), (Values{"86400"}));
}

TEST_F(ValueGeneratorsTest, DateRangeLeapYear) {
  EXPECT_EQ(valuesOf(DateRange("2024-02-28", "2024-03-01"), 3), (Values{"2024-02-28", "2024-02-29", "2024-03-01"}));
}

TEST_F(ValueGeneratorsTest, DateRangeErrors) {
  EXPECT_EQ(errorOf(DateRange("2026-13-01", "2026-12-31")).substr(0, 12), "Invalid date");
  EXPECT_EQ(errorOf(DateRange("2026-02-30", "2026-03-01")).substr(0, 12), "Invalid date");
  EXPECT_EQ(errorOf(DateRange("soon", "later")).substr(0, 12), "Invalid date");
  EXPECT_EQ(errorOf(DateRange("2026-01-02", "2026-01-01")), "The range is empty");
  EXPECT_EQ(errorOf(DateRange("2026-01-01", "2026-01-02", 0)), "The step is zero");
  EXPECT_EQ(errorOf(DateRange("2026-01-01", "2026-01-02", 86400, "%Q")), "Unknown conversion in the date format");
  EXPECT_EQ(errorOf(DateRange("2026-01-01", "2026-01-02", 86400, "ends with %")),
            "Unknown conversion in the date format");
}

TEST_F(ValueGeneratorsTest, RandomStringUsesItsAlphabetAndLength) {
  LoadParameter parameter;
  parameter.source = ValueSource::randomString;
  parameter.randomLength = 12;
  parameter.randomCharacters.assign(std::string_view{"xy"});
  for (const std::string& value : valuesOf(parameter, 50)) {
    EXPECT_EQ(value.size(), 12U);
    EXPECT_EQ(value.find_first_not_of("xy"), std::string::npos);
  }
}

TEST_F(ValueGeneratorsTest, RandomStringDefaultsToLowerCaseLetters) {
  LoadParameter parameter;
  parameter.source = ValueSource::randomString;
  for (const std::string& value : valuesOf(parameter, 20)) {
    EXPECT_EQ(value.size(), 8U);
    EXPECT_TRUE(std::ranges::all_of(value, [](char ch) { return ch >= 'a' && ch <= 'z'; }));
  }
}

TEST_F(ValueGeneratorsTest, RandomStringIsAPureFunctionOfTheSeedAndTheRequest) {
  LoadParameter parameter;
  parameter.source = ValueSource::randomString;
  const Values first = valuesOf(parameter, 20);
  EXPECT_EQ(first, valuesOf(parameter, 20));
  EXPECT_NE(first, valuesOf(parameter, 20, kSeed + 1));
  EXPECT_NE(first[0], first[1]);
}

TEST_F(ValueGeneratorsTest, RandomStringErrors) {
  LoadParameter parameter;
  parameter.source = ValueSource::randomString;
  parameter.randomLength = 0;
  EXPECT_EQ(errorOf(parameter), "Invalid length of the random string");
}

TEST_F(ValueGeneratorsTest, UuidsAreVersion4AndSeeded) {
  LoadParameter parameter;
  parameter.source = ValueSource::uuid;
  const Values first = valuesOf(parameter, 20);
  EXPECT_EQ(first, valuesOf(parameter, 20));
  EXPECT_EQ(std::set<std::string>(first.begin(), first.end()).size(), first.size());
  for (const std::string& value : first) {
    ASSERT_EQ(value.size(), 36U);
    EXPECT_EQ(value[8], '-');
    EXPECT_EQ(value[14], '4');
    EXPECT_TRUE(value[19] == '8' || value[19] == '9' || value[19] == 'a' || value[19] == 'b');
  }
}

TEST_F(ValueGeneratorsTest, TimestampFormats) {
  LoadParameter parameter;
  parameter.source = ValueSource::timestamp;
  parameter.timestampFormat = TimestampFormat::seconds;
  EXPECT_GE(std::stoll(valuesOf(parameter, 1)[0]), 1'700'000'000LL);
  parameter.timestampFormat = TimestampFormat::milliseconds;
  EXPECT_GE(std::stoll(valuesOf(parameter, 1)[0]), 1'700'000'000'000LL);
  parameter.timestampFormat = TimestampFormat::iso8601;
  const std::string iso = valuesOf(parameter, 1)[0];
  EXPECT_EQ(iso[4], '-');
  EXPECT_EQ(iso[10], 'T');
  EXPECT_EQ(iso.back(), 'Z');
}

TEST_F(ValueGeneratorsTest, DynamicVariables) {
  ValueGenerators generators(CombineMode::independent, kSeed);
  const std::uint32_t uuid = generators.addDynamic("$randomUUID");
  const std::uint32_t integer = generators.addDynamic("$randomInt");
  const std::uint32_t clock = generators.addDynamic("$timestamp");
  generators.finalize();
  EXPECT_EQ(generators.passLength(), 0U);
  EXPECT_EQ(generators.valueCount(uuid), 0U);
  ValueGenerators::Cursor cursor(generators);
  ASSERT_TRUE(cursor.fill(3));
  EXPECT_EQ(cursor.values()[uuid].size(), 36U);
  EXPECT_LE(std::stoi(std::string(cursor.values()[integer])), 1000);
  EXPECT_GE(std::stoll(std::string(cursor.values()[clock])), 1'700'000'000LL);
}

TEST_F(ValueGeneratorsTest, RandomOrderStaysInTheValuesAndIsReproducible) {
  const LoadParameter parameter = IntegerRange(1, 5, 1, ValueOrder::random);
  const Values first = valuesOf(parameter, 200);
  EXPECT_EQ(first, valuesOf(parameter, 200));
  EXPECT_NE(first, valuesOf(parameter, 200, kSeed + 1));
  const std::set<std::string> seen(first.begin(), first.end());
  EXPECT_EQ(seen, (std::set<std::string>{"1", "2", "3", "4", "5"}));
}

TEST_F(ValueGeneratorsTest, ShuffledSendsEachValueOncePerPass) {
  for (const std::int64_t count : {1, 2, 3, 5, 7, 8, 16, 100, 1000, 4097}) {
    const Values values = valuesOf(IntegerRange(1, count, 1, ValueOrder::shuffled), static_cast<std::uint64_t>(count));
    EXPECT_EQ(std::set<std::string>(values.begin(), values.end()).size(), static_cast<std::size_t>(count)) << count;
  }
}

TEST_F(ValueGeneratorsTest, ShuffledIsNotTheOriginalOrderAndChangesAtEachPass) {
  const LoadParameter parameter = IntegerRange(1, 100, 1, ValueOrder::shuffled);
  const Values values = valuesOf(parameter, 200);
  const Values inOrder = valuesOf(IntegerRange(1, 100), 100);
  const Values firstPass(values.begin(), values.begin() + 100);
  const Values secondPass(values.begin() + 100, values.end());
  EXPECT_NE(firstPass, inOrder);
  EXPECT_NE(firstPass, secondPass);
  EXPECT_EQ(std::set<std::string>(secondPass.begin(), secondPass.end()).size(), 100U);
  EXPECT_EQ(values, valuesOf(parameter, 200));
}

// The values of two variables for the requests 0 to count - 1, joined as "a/b".
Values Pairs(const ValueGenerators& generators, std::uint64_t count) {
  ValueGenerators::Cursor cursor(generators);
  Values values;
  for (std::uint64_t number = 0; number < count; ++number) {
    EXPECT_TRUE(cursor.fill(number));
    values.push_back(std::string(cursor.values()[0]) + "/" + std::string(cursor.values()[1]));
  }
  return values;
}

TEST_F(ValueGeneratorsTest, IndependentParametersAdvanceOnTheirOwn) {
  ValueGenerators generators(CombineMode::independent, kSeed);
  add(generators, ListOf("a\nb"));
  add(generators, IntegerRange(1, 3));
  generators.finalize();
  EXPECT_EQ(generators.passLength(), 3U);
  EXPECT_EQ(Pairs(generators, 5), (Values{"a/1", "b/2", "a/3", "b/1", "a/2"}));
}

TEST_F(ValueGeneratorsTest, TogetherStopsAtTheShortestAndKeepsThePairs) {
  ValueGenerators generators(CombineMode::together, kSeed);
  add(generators, ListOf("a\nb"));
  add(generators, IntegerRange(1, 3));
  generators.finalize();
  EXPECT_EQ(generators.passLength(), 2U);
  EXPECT_EQ(Pairs(generators, 4), (Values{"a/1", "b/2", "a/1", "b/2"}));
}

TEST_F(ValueGeneratorsTest, TogetherShuffledKeepsThePairsAligned) {
  ValueGenerators generators(CombineMode::together, kSeed);
  add(generators, IntegerRange(1, 50, 1, ValueOrder::shuffled));
  add(generators, IntegerRange(101, 150));  // its own order does not matter: the first parameter leads
  generators.finalize();
  ValueGenerators::Cursor cursor(generators);
  std::set<std::int64_t> seen;
  bool isShuffled = false;
  for (std::uint64_t number = 0; number < 50; ++number) {
    ASSERT_TRUE(cursor.fill(number));
    const std::int64_t first = std::stoll(std::string(cursor.values()[0]));
    EXPECT_EQ(std::stoll(std::string(cursor.values()[1])), first + 100);
    isShuffled = isShuffled || first != static_cast<std::int64_t>(number) + 1;
    seen.insert(first);
  }
  EXPECT_EQ(seen.size(), 50U);
  EXPECT_TRUE(isShuffled);
}

TEST_F(ValueGeneratorsTest, AllCombinationsLikeNestedLoops) {
  ValueGenerators generators(CombineMode::allCombinations, kSeed);
  add(generators, ListOf("a\nb"));
  add(generators, IntegerRange(1, 3));
  generators.finalize();
  EXPECT_EQ(generators.passLength(), 6U);
  EXPECT_EQ(Pairs(generators, 7), (Values{"a/1", "a/2", "a/3", "b/1", "b/2", "b/3", "a/1"}));
}

TEST_F(ValueGeneratorsTest, AllCombinationsShuffledVisitsEachOnce) {
  ValueGenerators generators(CombineMode::allCombinations, kSeed);
  add(generators, IntegerRange(1, 12, 1, ValueOrder::shuffled));
  add(generators, IntegerRange(1, 9));
  generators.finalize();
  const Values values = Pairs(generators, 108);
  EXPECT_EQ(std::set<std::string>(values.begin(), values.end()).size(), 108U);
  EXPECT_NE(values[0], "1/1");
}

TEST_F(ValueGeneratorsTest, AHugeProductSaturates) {
  ValueGenerators generators(CombineMode::allCombinations, kSeed);
  for (int idx = 0; idx < 4; ++idx) {
    add(generators, IntegerRange(1, 1'000'000'000'000LL));
  }
  generators.finalize();
  EXPECT_EQ(generators.passLength(), std::numeric_limits<std::uint64_t>::max());
  ValueGenerators::Cursor cursor(generators);
  EXPECT_TRUE(cursor.fill(5));
  EXPECT_EQ(cursor.values()[3], "6");
}

TEST_F(ValueGeneratorsTest, UnboundedParametersHaveNoPass) {
  ValueGenerators generators(CombineMode::allCombinations, kSeed);
  LoadParameter parameter;
  parameter.source = ValueSource::uuid;
  add(generators, parameter);
  generators.finalize();
  EXPECT_EQ(generators.passLength(), 0U);
  ValueGenerators::Cursor cursor(generators);
  EXPECT_TRUE(cursor.fill(0));
}

TEST_F(ValueGeneratorsTest, UnboundedParametersDoNotCountInThePass) {
  ValueGenerators generators(CombineMode::together, kSeed);
  LoadParameter parameter;
  parameter.source = ValueSource::uuid;
  add(generators, parameter);
  add(generators, IntegerRange(1, 4));
  generators.finalize();
  EXPECT_EQ(generators.passLength(), 4U);
}

LoadParameter FileColumn(std::string_view path, std::string_view column, ValueOrder order = ValueOrder::inOrder) {
  LoadParameter parameter;
  parameter.source = ValueSource::dataFile;
  parameter.filePath.assign(path);
  parameter.fileColumn.assign(column);
  parameter.order = order;
  return parameter;
}

TEST_F(ValueGeneratorsTest, TheColumnsOfAFileStayTogether) {
  writeFile("users.csv", "user,token\nann,t1\nbob,t2\ncid,t3\n");
  ValueGenerators generators(CombineMode::independent, kSeed);
  const std::uint32_t user = add(generators, FileColumn("users.csv", "user", ValueOrder::shuffled));
  const std::uint32_t token = add(generators, FileColumn("users.csv", "token"));
  generators.finalize();
  EXPECT_EQ(generators.passLength(), 3U);
  EXPECT_EQ(generators.valueCount(token), 3U);
  EXPECT_EQ(generators.dataFiles().size(), 1U);
  ValueGenerators::Cursor cursor(generators);
  ASSERT_TRUE(cursor.error().empty());
  std::set<std::string> seen;
  for (std::uint64_t number = 0; number < 3; ++number) {
    ASSERT_TRUE(cursor.fill(number));
    seen.insert(std::string(cursor.values()[user]) + "/" + std::string(cursor.values()[token]));
  }
  EXPECT_EQ(seen, (std::set<std::string>{"ann/t1", "bob/t2", "cid/t3"}));
}

TEST_F(ValueGeneratorsTest, ADataFileWithoutColumnNameUsesTheFirstOne) {
  writeFile("words.txt", "alpha\nbeta\n");
  LoadParameter parameter = FileColumn("words.txt", "");
  parameter.fileFormat = DataFileFormat::text;
  EXPECT_EQ(valuesOf(parameter, 3), (Values{"alpha", "beta", "alpha"}));
}

TEST_F(ValueGeneratorsTest, DataFileErrors) {
  writeFile("users.csv", "user\nann\n");
  EXPECT_EQ(errorOf(FileColumn("", "user")), "No file");
  EXPECT_EQ(errorOf(FileColumn("users.csv", "nope")), "No column 'nope' in the file");
  EXPECT_TRUE(errorOf(FileColumn("missing.csv", "user")).starts_with("Cannot open"));
}

TEST_F(ValueGeneratorsTest, AFixedParameterIsNotGenerated) {
  LoadParameter parameter;
  parameter.source = ValueSource::fixed;
  EXPECT_FALSE(errorOf(parameter).empty());
}

TEST_F(ValueGeneratorsTest, CursorsOfDifferentThreadsGiveTheSameValues) {
  ValueGenerators generators(CombineMode::independent, kSeed);
  add(generators, IntegerRange(1, 1000, 1, ValueOrder::shuffled));
  generators.finalize();
  ValueGenerators::Cursor first(generators);
  ValueGenerators::Cursor second(generators);
  ASSERT_TRUE(first.fill(123));
  ASSERT_TRUE(second.fill(123));
  EXPECT_EQ(first.values()[0], second.values()[0]);
}

}  // namespace
}  // namespace zalie
