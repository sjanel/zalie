#include "zalie/data-file.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include "zalie/file-io.hpp"
#include "zalie/load-settings.hpp"
#include "zalie/raw-chars.hpp"

namespace zalie {
namespace {

class DataFileTest : public ::testing::Test {
 protected:
  void SetUp() override {
    _directory = std::filesystem::temp_directory_path() /
                 ("zalie-data-file-" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
    std::filesystem::create_directories(_directory);
  }

  void TearDown() override { std::filesystem::remove_all(_directory); }

  std::filesystem::path write(std::string_view content, std::string_view name = "data") const {
    const std::filesystem::path path = _directory / name;
    std::ofstream(path, std::ios::binary | std::ios::trunc) << content;
    return path;
  }

  // The index of a file holding 'content', which must be valid.
  [[nodiscard]] std::shared_ptr<const DataFileIndex> indexOf(std::string_view content, DataFileFormat format,
                                                             std::uint32_t maxOffsets = DataFileIndex::kMaxOffsets) {
    auto pIndex = std::make_shared<const DataFileIndex>(write(content), format, maxOffsets);
    EXPECT_TRUE(pIndex->valid()) << pIndex->error();
    return pIndex;
  }

  std::filesystem::path _directory;
};

TEST_F(DataFileTest, TextOneValuePerLine) {
  const auto pIndex = indexOf("alpha\nbeta\ngamma\n", DataFileFormat::text);
  EXPECT_EQ(pIndex->nbRows(), 3U);
  ASSERT_EQ(pIndex->columns().size(), 1U);
  DataFileReader reader(*pIndex);
  ASSERT_TRUE(reader.error().empty());
  ASSERT_TRUE(reader.read(2));
  EXPECT_EQ(reader.value(0), "gamma");
  ASSERT_TRUE(reader.read(0));
  EXPECT_EQ(reader.value(0), "alpha");
  EXPECT_FALSE(reader.read(3));
}

TEST_F(DataFileTest, TextWithoutFinalLineBreak) {
  const auto pIndex = indexOf("alpha\nbeta", DataFileFormat::text);
  EXPECT_EQ(pIndex->nbRows(), 2U);
  DataFileReader reader(*pIndex);
  ASSERT_TRUE(reader.read(1));
  EXPECT_EQ(reader.value(0), "beta");
}

TEST_F(DataFileTest, BlankLinesAreNotRows) {
  const auto pIndex = indexOf("\nalpha\n\n\r\nbeta\n\n", DataFileFormat::text);
  EXPECT_EQ(pIndex->nbRows(), 2U);
  DataFileReader reader(*pIndex);
  ASSERT_TRUE(reader.read(0));
  EXPECT_EQ(reader.value(0), "alpha");
  ASSERT_TRUE(reader.read(1));
  EXPECT_EQ(reader.value(0), "beta");
}

TEST_F(DataFileTest, WindowsLineEndsAndByteOrderMark) {
  const auto pIndex = indexOf("\xEF\xBB\xBFid,name\r\n1,ann\r\n2,bob", DataFileFormat::csv);
  EXPECT_EQ(pIndex->nbRows(), 2U);
  ASSERT_EQ(pIndex->columns().size(), 2U);
  EXPECT_EQ(View(pIndex->columns()[0]), "id");
  EXPECT_EQ(View(pIndex->columns()[1]), "name");
  DataFileReader reader(*pIndex);
  ASSERT_TRUE(reader.read(0));
  EXPECT_EQ(reader.value(1), "ann");
  ASSERT_TRUE(reader.read(1));
  EXPECT_EQ(reader.value(1), "bob");
}

TEST_F(DataFileTest, CsvHeaderIsNotARow) {
  const auto pIndex = indexOf("id,name\n1,ann\n", DataFileFormat::csv);
  EXPECT_EQ(pIndex->nbRows(), 1U);
  EXPECT_EQ(pIndex->findColumn("name"), 1U);
  EXPECT_EQ(pIndex->findColumn("nope"), 2U);
}

TEST_F(DataFileTest, CsvQuotedFields) {
  const auto pIndex = indexOf("a,b\n\"x,y\",\"he said \"\"hi\"\"\"\n\"\",plain\n", DataFileFormat::csv);
  EXPECT_EQ(pIndex->nbRows(), 2U);
  EXPECT_EQ(pIndex->firstMalformedLine(), 0U);
  DataFileReader reader(*pIndex);
  ASSERT_TRUE(reader.read(0));
  EXPECT_EQ(reader.value(0), "x,y");
  EXPECT_EQ(reader.value(1), "he said \"hi\"");
  ASSERT_TRUE(reader.read(1));
  EXPECT_EQ(reader.value(0), "");
  EXPECT_EQ(reader.value(1), "plain");
}

TEST_F(DataFileTest, CsvLineBreakInsideQuotesStaysInTheRow) {
  const auto pIndex = indexOf("a,b\n\"line1\nline2\",x\n\"p\"\"\nq\",y\nz,t\n", DataFileFormat::csv);
  EXPECT_EQ(pIndex->nbRows(), 3U);
  DataFileReader reader(*pIndex);
  ASSERT_TRUE(reader.read(0));
  EXPECT_EQ(reader.value(0), "line1\nline2");
  EXPECT_EQ(reader.value(1), "x");
  ASSERT_TRUE(reader.read(1));
  EXPECT_EQ(reader.value(0), "p\"\nq");
  ASSERT_TRUE(reader.read(2));
  EXPECT_EQ(reader.value(1), "t");
}

TEST_F(DataFileTest, CsvEmptyFieldsAndTrailingComma) {
  const auto pIndex = indexOf("a,b,c\n,,\nx,,z\n", DataFileFormat::csv);
  EXPECT_EQ(pIndex->firstMalformedLine(), 0U);
  DataFileReader reader(*pIndex);
  ASSERT_TRUE(reader.read(0));
  EXPECT_EQ(reader.value(2), "");
  ASSERT_TRUE(reader.read(1));
  EXPECT_EQ(reader.value(0), "x");
  EXPECT_EQ(reader.value(1), "");
  EXPECT_EQ(reader.value(2), "z");
}

TEST_F(DataFileTest, CsvMalformedRowIsReportedAndRefused) {
  const auto pIndex = indexOf("a,b\n1,2\n\n3\n4,5\n", DataFileFormat::csv);
  EXPECT_EQ(pIndex->nbRows(), 3U);
  EXPECT_EQ(pIndex->firstMalformedLine(), 4U);  // the blank line counts
  DataFileReader reader(*pIndex);
  EXPECT_TRUE(reader.read(0));
  EXPECT_FALSE(reader.read(1));
  ASSERT_TRUE(reader.read(2));
  EXPECT_EQ(reader.value(1), "5");
}

TEST_F(DataFileTest, CsvLineNumbersCountTheLineBreaksInQuotes) {
  EXPECT_EQ(indexOf("a,b\n\"x\ny\",1\nbad\n", DataFileFormat::csv)->firstMalformedLine(), 4U);
}

TEST_F(DataFileTest, CsvTextAfterAClosingQuoteIsMalformed) {
  EXPECT_EQ(indexOf("a\n\"x\"y\nz\n", DataFileFormat::csv)->firstMalformedLine(), 2U);
}

TEST_F(DataFileTest, CsvUnterminatedQuoteIsMalformed) {
  EXPECT_EQ(indexOf("a\nok\n\"never closed\nend\n", DataFileFormat::csv)->firstMalformedLine(), 3U);
}

TEST_F(DataFileTest, CsvWithoutRowsIsRefused) {
  const DataFileIndex index(write("a,b\n"), DataFileFormat::csv);
  EXPECT_FALSE(index.valid());
  EXPECT_TRUE(index.error().starts_with("No value"));
}

TEST_F(DataFileTest, JsonLinesColumnsAreTheKeysOfTheFirstLine) {
  const auto pIndex = indexOf(R"({"user":"ann","id":1,"active":true})"
                              "\n"
                              R"({"id":2,"active":false,"user":"bob"})"
                              "\n",
                              DataFileFormat::jsonLines);
  EXPECT_EQ(pIndex->nbRows(), 2U);
  ASSERT_EQ(pIndex->columns().size(), 3U);
  EXPECT_EQ(View(pIndex->columns()[1]), "id");
  DataFileReader reader(*pIndex);
  ASSERT_TRUE(reader.read(0));
  EXPECT_EQ(reader.value(0), "ann");
  EXPECT_EQ(reader.value(1), "1");
  EXPECT_EQ(reader.value(2), "true");
  ASSERT_TRUE(reader.read(1));  // any order of the keys
  EXPECT_EQ(reader.value(0), "bob");
  EXPECT_EQ(reader.value(2), "false");
}

TEST_F(DataFileTest, JsonLinesValuesAreDecoded) {
  const auto pIndex = indexOf(R"({"a":"x\ny\"zé","b":null,"c":{"n":[1,2]},"d":[3]})"
                              "\n",
                              DataFileFormat::jsonLines);
  DataFileReader reader(*pIndex);
  ASSERT_TRUE(reader.read(0));
  EXPECT_EQ(reader.value(0), "x\ny\"z\xC3\xA9");
  EXPECT_EQ(reader.value(1), "");
  EXPECT_EQ(reader.value(2), R"({"n":[1,2]})");
  EXPECT_EQ(reader.value(3), "[3]");
}

TEST_F(DataFileTest, JsonLinesEscapedKeys) {
  const auto pIndex = indexOf(R"({"a\"b":1})"
                              "\n"
                              R"({"a\"b":2})"
                              "\n",
                              DataFileFormat::jsonLines);
  EXPECT_EQ(pIndex->findColumn("a\"b"), 0U);
  EXPECT_EQ(pIndex->firstMalformedLine(), 0U);
  DataFileReader reader(*pIndex);
  ASSERT_TRUE(reader.read(1));
  EXPECT_EQ(reader.value(0), "2");
}

TEST_F(DataFileTest, JsonLinesMalformedAndIncompleteRows) {
  const auto pIndex = indexOf("{\"a\":1,\"b\":2}\n{\"a\":3}\nnot json\n{\"a\":5,\"b\":6}\n", DataFileFormat::jsonLines);
  EXPECT_EQ(pIndex->nbRows(), 4U);
  EXPECT_EQ(pIndex->firstMalformedLine(), 2U);
  DataFileReader reader(*pIndex);
  EXPECT_FALSE(reader.read(1));  // a key is missing
  EXPECT_FALSE(reader.read(2));
  ASSERT_TRUE(reader.read(3));
  EXPECT_EQ(reader.value(1), "6");
}

TEST_F(DataFileTest, JsonLinesFirstLineMustBeAnObject) {
  const DataFileIndex index(write("[1,2]\n"), DataFileFormat::jsonLines);
  EXPECT_FALSE(index.valid());
}

TEST_F(DataFileTest, PreviewHoldsTheFirstRows) {
  const auto pIndex = indexOf("a,b\n1,2\n3,4\n5,6\n7,8\n9,10\n11,12\n", DataFileFormat::csv);
  ASSERT_EQ(pIndex->previewValues().size(), DataFileIndex::kNbPreviewRows * 2U);
  EXPECT_EQ(View(pIndex->previewValues()[0]), "1");
  EXPECT_EQ(View(pIndex->previewValues()[9]), "10");
}

TEST_F(DataFileTest, MissingFileAndEmptyFile) {
  const DataFileIndex missing(_directory / "nope", DataFileFormat::text);
  EXPECT_FALSE(missing.valid());
  EXPECT_TRUE(missing.error().starts_with("Cannot open"));
  const DataFileIndex empty(write(""), DataFileFormat::text);
  EXPECT_FALSE(empty.valid());
  EXPECT_TRUE(empty.error().starts_with("No value"));
}

std::string ManyLines(std::size_t count) {
  std::string content;
  for (std::size_t idx = 0; idx < count; ++idx) {
    content += "value-" + std::to_string(idx) + "\n";
  }
  return content;
}

TEST_F(DataFileTest, RandomAccessInAFileLargerThanTheIndexStep) {
  const std::size_t kNbLines = 20000;
  const auto pIndex = indexOf(ManyLines(kNbLines), DataFileFormat::text);
  EXPECT_EQ(pIndex->nbRows(), kNbLines);
  DataFileReader reader(*pIndex);
  for (const std::uint64_t row : {19999ULL, 0ULL, 17ULL, 16ULL, 15ULL, 10000ULL, 9999ULL, 1ULL, 19983ULL}) {
    ASSERT_TRUE(reader.read(row)) << row;
    EXPECT_EQ(reader.value(0), "value-" + std::to_string(row));
  }
}

TEST_F(DataFileTest, TheIndexKeepsABoundedNumberOfOffsets) {
  const std::size_t kNbLines = 5000;
  const auto pIndex = indexOf(ManyLines(kNbLines), DataFileFormat::text, 8);
  EXPECT_GT(pIndex->rowsPerOffset(), 16U);
  EXPECT_LE(kNbLines / pIndex->rowsPerOffset(), 8U);
  DataFileReader reader(*pIndex);
  for (const std::uint64_t row : {4999ULL, 0ULL, 1234ULL, 2048ULL, 2047ULL, 4096ULL}) {
    ASSERT_TRUE(reader.read(row)) << row;
    EXPECT_EQ(reader.value(0), "value-" + std::to_string(row));
  }
}

TEST_F(DataFileTest, TheIndexWithAnOddMaximumStaysAligned) {
  const auto pIndex = indexOf(ManyLines(1000), DataFileFormat::text, 7);
  DataFileReader reader(*pIndex);
  for (const std::uint64_t row : {999ULL, 500ULL, 3ULL, 111ULL}) {
    ASSERT_TRUE(reader.read(row)) << row;
    EXPECT_EQ(reader.value(0), "value-" + std::to_string(row));
  }
}

TEST_F(DataFileTest, ARowLargerThanTheBuffers) {
  const std::string huge((3UL << 20), 'x');  // more than the scan block and the reading window
  const auto pIndex = indexOf("first\n" + huge + "\nlast\n", DataFileFormat::text);
  EXPECT_EQ(pIndex->nbRows(), 3U);
  DataFileReader reader(*pIndex);
  ASSERT_TRUE(reader.read(1));
  EXPECT_EQ(reader.value(0).size(), huge.size());
  ASSERT_TRUE(reader.read(2));
  EXPECT_EQ(reader.value(0), "last");
  ASSERT_TRUE(reader.read(0));
  EXPECT_EQ(reader.value(0), "first");
}

TEST_F(DataFileTest, RowsAcrossScanBlocksAreIntact) {
  // Lines around the size of the scan block (1 MiB), so that some rows are cut by its end.
  std::string content;
  const std::string line(70000, 'a');
  for (int idx = 0; idx < 40; ++idx) {
    content += std::to_string(idx) + line + "\n";
  }
  const auto pIndex = indexOf(content, DataFileFormat::text);
  EXPECT_EQ(pIndex->nbRows(), 40U);
  DataFileReader reader(*pIndex);
  for (const int row : {39, 0, 20, 21}) {
    ASSERT_TRUE(reader.read(static_cast<std::uint64_t>(row)));
    EXPECT_TRUE(reader.value(0).starts_with(std::to_string(row) + "a"));
    EXPECT_EQ(reader.value(0).size(), std::to_string(row).size() + line.size());
  }
}

TEST_F(DataFileTest, ATruncatedFileIsARowErrorNotACrash) {
  const std::filesystem::path path = write(ManyLines(100));
  auto pIndex = std::make_shared<const DataFileIndex>(path, DataFileFormat::text);
  DataFileReader reader(*pIndex);
  ASSERT_TRUE(reader.error().empty());
  std::filesystem::resize_file(path, 20);  // during the run, after the reader was opened
  EXPECT_FALSE(reader.read(99));
  DataFileReader lateReader(*pIndex);
  EXPECT_FALSE(lateReader.error().empty());
}

TEST_F(DataFileTest, AReaderRefusesAFileChangedSinceItsIndex) {
  const std::filesystem::path path = write("alpha\nbeta\n");
  const auto pIndex = std::make_shared<const DataFileIndex>(path, DataFileFormat::text);
  write("different size\n");
  DataFileReader reader(*pIndex);
  EXPECT_TRUE(reader.error().contains("changed"));
}

TEST_F(DataFileTest, CacheReusesAnIndexUntilTheFileChanges) {
  const std::filesystem::path path = write("alpha\nbeta\n");
  DataFileIndexCache cache;
  const auto first = cache.get(path, DataFileFormat::text);
  EXPECT_TRUE(first->valid());
  EXPECT_TRUE(first->isCurrent());
  EXPECT_EQ(cache.get(path, DataFileFormat::text), first);
  EXPECT_NE(cache.get(path, DataFileFormat::csv), first);  // another format is another index

  write("alpha\nbeta\ngamma\n");
  EXPECT_FALSE(first->isCurrent());
  const auto second = cache.get(path, DataFileFormat::text);
  EXPECT_NE(second, first);
  EXPECT_EQ(second->nbRows(), 3U);
}

TEST_F(DataFileTest, CacheDetectsAChangeOfDateAlone) {
  const std::filesystem::path path = write("alpha\nbeta\n");
  DataFileIndexCache cache;
  const auto first = cache.get(path, DataFileFormat::text);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  write("gamma\ndelta\n");  // same size
  std::filesystem::last_write_time(path, std::filesystem::last_write_time(path) + std::chrono::seconds(2));
  EXPECT_NE(cache.get(path, DataFileFormat::text), first);
}

TEST_F(DataFileTest, CacheDoesNotKeepFailures) {
  const std::filesystem::path path = _directory / "late";
  DataFileIndexCache cache;
  EXPECT_FALSE(cache.get(path, DataFileFormat::text)->valid());
  write("alpha\n", "late");
  EXPECT_TRUE(cache.get(path, DataFileFormat::text)->valid());
}

TEST_F(DataFileTest, CacheEvictsItsOldestEntries) {
  DataFileIndexCache cache;
  const std::filesystem::path firstPath = write("v\n", "file-0");
  const auto first = cache.get(firstPath, DataFileFormat::text);
  for (int idx = 1; idx < 20; ++idx) {
    EXPECT_TRUE(cache.get(write("v\n", "file-" + std::to_string(idx)), DataFileFormat::text)->valid());
  }
  EXPECT_NE(cache.get(firstPath, DataFileFormat::text), first);
}

TEST_F(DataFileTest, RelativePathsStartFromTheCollection) {
  const std::filesystem::path base = std::filesystem::path("/some") / "where";
  EXPECT_EQ(ResolveDataFilePath("data/users.csv", base), base / "data" / "users.csv");
  const std::filesystem::path absolute = std::filesystem::absolute(std::filesystem::path("/data/users.csv"));
  EXPECT_EQ(ResolveDataFilePath(View(PathToUtf8(absolute)), base), absolute);
}

}  // namespace
}  // namespace zalie
