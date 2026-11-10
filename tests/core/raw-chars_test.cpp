#include "zalie/raw-chars.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace zalie {
namespace {

// A part writing fewer bytes than the maximum it announces.
struct Stars {
  [[nodiscard]] std::size_t maxSize() const noexcept { return count + 8; }
  [[nodiscard]] char* writeTo(char* pData) const noexcept {
    for (std::size_t idx = 0; idx < count; ++idx) {
      *pData++ = '*';
    }
    return pData;
  }

  std::size_t count;
};

TEST(RawCharsHelpers, View) {
  const RawChars32 empty;
  EXPECT_EQ(View(empty), "");
  EXPECT_EQ(View(RawChars(std::string_view{"body"})), "body");
  EXPECT_EQ(View(std::string("std")), "std");
}

TEST(RawCharsHelpers, ConcatAllocatesTheExactSize) {
  const RawChars32 out = Concat({"a", "bc", "", std::string_view{"def"}});
  EXPECT_EQ(View(out), "abcdef");
  EXPECT_EQ(out.capacity(), 6U);
  EXPECT_TRUE(Concat({}).empty());
  EXPECT_EQ(View(Concat<RawChars>({"big", " body"})), "big body");
}

TEST(RawCharsHelpers, AppendAllTakesStringsLiteralsAndChars) {
  RawChars32 out;
  AppendAll(out);
  EXPECT_TRUE(out.empty());
  const RawChars32 buffer(std::string_view{"buffer"});
  const char* pText = "pointer";
  AppendAll(out, "literal", ' ', std::string_view{"view"}, ' ', buffer, ' ', std::string("string"), ' ', pText, "");
  EXPECT_EQ(View(out), "literal view buffer string pointer");
}

TEST(RawCharsHelpers, AppendAllWritesIntegersInDecimal) {
  RawChars32 out;
  AppendAll(out, 0, ' ', -42, ' ', std::uint8_t{255}, ' ', std::int16_t{-32768}, ' ', 4294967295U);
  EXPECT_EQ(View(out), "0 -42 255 -32768 4294967295");
  out.clear();
  AppendAll(out, std::numeric_limits<std::uint64_t>::max(), '/', std::numeric_limits<std::int64_t>::min(), '/',
            std::int64_t{1234567890123});
  EXPECT_EQ(View(out), "18446744073709551615/-9223372036854775808/1234567890123");
}

TEST(RawCharsHelpers, AppendAllWritesCustomParts) {
  RawChars32 out(std::string_view{"["});
  AppendAll(out, Stars{3}, '|', Stars{0}, '|', Stars{40}, ']');
  EXPECT_EQ(View(out), "[***||" + std::string(40, '*') + "]");
}

TEST(RawCharsHelpers, AppendAllGrowsTheBufferOnceForManyCalls) {
  RawChars out;
  std::string expected;
  std::size_t nbGrowths = 0;
  for (int idx = 0; idx < 2000; ++idx) {
    const auto capacity = out.capacity();
    AppendAll(out, "item ", idx, ',');
    nbGrowths += out.capacity() != capacity ? 1U : 0U;
    expected += "item " + std::to_string(idx) + ",";
  }
  EXPECT_EQ(View(out), expected);
  EXPECT_LT(nbGrowths, 20U);  // exponential growth, not one reallocation per call
}

TEST(RawCharsHelpers, AssignSafeAcceptsAViewOfTheBuffer) {
  RawChars32 out(std::string_view{"abc-de-42"});
  AssignSafe(out, std::string_view(out).substr(4));
  EXPECT_EQ(View(out), "de-42");
  AssignSafe(out, {});
  EXPECT_TRUE(out.empty());
}

}  // namespace
}  // namespace zalie
