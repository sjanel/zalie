#include "zalie/string-pool.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace zalie {
namespace {

TEST(StringPool, ViewsStayValidAcrossGrowthAndMoves) {
  StringPool pool(8);  // tiny first block: forces several blocks
  std::vector<std::string_view> views;
  std::vector<std::string> expected;
  for (int i = 0; i < 200; ++i) {
    expected.push_back("string number " + std::to_string(i));
    views.push_back(pool.store(expected.back()));
  }
  StringPool moved = std::move(pool);
  for (std::size_t i = 0; i < views.size(); ++i) {
    EXPECT_EQ(views[i], expected[i]);
  }
  EXPECT_TRUE(moved.store("").empty());
  EXPECT_EQ(moved.concat({"a", "", "bc"}), "abc");
  EXPECT_TRUE(moved.concat({"", ""}).empty());
}

TEST(StringPool, Splice) {
  StringPool target;
  const std::string_view first = target.store("first");
  std::string_view second;
  {
    StringPool other;
    second = other.store("second");
    target.splice(other);
  }
  EXPECT_EQ(first, "first");
  EXPECT_EQ(second, "second");
}

TEST(StringList, Basics) {
  StringList list;
  EXPECT_TRUE(list.empty());
  list.push_back("warning: ");
  list.push_back({"file ", "a.bru", ": oops"});
  list.pushBackUnique("warning: ");
  list.pushBackUnique("other");
  ASSERT_EQ(list.size(), 3U);
  EXPECT_EQ(list.front(), "warning: ");
  EXPECT_EQ(list[1], "file a.bru: oops");
  EXPECT_EQ(list.back(), "other");
  EXPECT_TRUE(list.contains("other"));
  EXPECT_FALSE(list.contains("nope"));
  std::size_t count = 0;
  for (std::string_view item : list) {
    EXPECT_FALSE(item.empty());
    ++count;
  }
  EXPECT_EQ(count, 3U);
}

}  // namespace
}  // namespace zalie
