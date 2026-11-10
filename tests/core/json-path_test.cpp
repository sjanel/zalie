#include "zalie/json-path.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "zalie/raw-chars.hpp"

namespace zalie {
namespace {

// The example document of RFC 9535 (section 1.5), with a few additions.
constexpr std::string_view kStore = R"({ "store": {
    "book": [
      { "category": "reference", "author": "Nigel Rees", "title": "Sayings of the Century", "price": 8.95 },
      { "category": "fiction", "author": "Evelyn Waugh", "title": "Sword of Honour", "price": 12.99 },
      { "category": "fiction", "author": "Herman Melville", "title": "Moby Dick", "isbn": "0-553-21311-3",
        "price": 8.99 },
      { "category": "fiction", "author": "J. R. R. Tolkien", "title": "The Lord of the Rings",
        "isbn": "0-395-19395-8", "price": 22.99 }
    ],
    "bicycle": { "color": "red", "price": 399 }
  },
  "id": 12345678901234567890,
  "content-type": "json",
  "weird \"key\"": 1,
  "caf\u00e9": "unicode"
})";

// Evaluates and joins the matches (an array, or the value of a singular path).
std::string Query(std::string_view json, std::string_view path) {
  auto result = EvaluateJsonPath(json, path);
  if (!result) {
    return "error: " + std::string(View(result.error()));
  }
  RawChars out;
  JoinJsonPathMatches(*result, out);
  return std::string(View(out));
}

TEST(JsonPath, RootAndMembers) {
  EXPECT_EQ(Query(R"( {"a": 1} )", "$"), R"({"a": 1})");
  EXPECT_EQ(Query(kStore, "$.store.bicycle.color"), R"("red")");
  EXPECT_EQ(Query(kStore, "$['store']['bicycle'][\"price\"]"), "399");
  EXPECT_EQ(Query(kStore, "$.content-type"), R"("json")");
  EXPECT_EQ(Query(kStore, R"($['weird "key"'])"), "1");
  EXPECT_EQ(Query(kStore, R"($["weird \"key\""])"), "1");
  EXPECT_EQ(Query(kStore, "$['caf\u00e9']"), R"("unicode")");  // escaped key in the document
  // Numbers are copied verbatim: no precision lost.
  EXPECT_EQ(Query(kStore, "$.id"), "12345678901234567890");
  // A singular path without match gives an empty array.
  EXPECT_EQ(Query(kStore, "$.store.nope"), "[]");
  EXPECT_EQ(Query(kStore, "$.store.book.title"), "[]");  // not an object
}

TEST(JsonPath, ImplicitRoot) {
  EXPECT_EQ(Query(kStore, "store.bicycle.color"), R"("red")");
  EXPECT_EQ(Query(kStore, ".store.bicycle.color"), R"("red")");
  EXPECT_EQ(Query(kStore, "  $.store.bicycle.price  "), "399");
}

TEST(JsonPath, IndexesAndSlices) {
  constexpr std::string_view kArray = "[0, 1, 2, 3, 4, 5, 6]";
  EXPECT_EQ(Query(kArray, "$[0]"), "0");
  EXPECT_EQ(Query(kArray, "$[6]"), "6");
  EXPECT_EQ(Query(kArray, "$[-1]"), "6");
  EXPECT_EQ(Query(kArray, "$[-7]"), "0");
  EXPECT_EQ(Query(kArray, "$[7]"), "[]");
  EXPECT_EQ(Query(kArray, "$[-8]"), "[]");
  EXPECT_EQ(Query(kArray, "$[1:3]"), "[1,2]");
  EXPECT_EQ(Query(kArray, "$[5:]"), "[5,6]");
  EXPECT_EQ(Query(kArray, "$[:2]"), "[0,1]");
  EXPECT_EQ(Query(kArray, "$[1:5:2]"), "[1,3]");
  EXPECT_EQ(Query(kArray, "$[::3]"), "[0,3,6]");
  EXPECT_EQ(Query(kArray, "$[5:1:-2]"), "[5,3]");
  EXPECT_EQ(Query(kArray, "$[::-1]"), "[6,5,4,3,2,1,0]");
  EXPECT_EQ(Query(kArray, "$[-2:]"), "[5,6]");
  EXPECT_EQ(Query(kArray, "$[::0]"), "[]");
  EXPECT_EQ(Query(kArray, "$[ 1 : 3 ]"), "[1,2]");
  EXPECT_EQ(Query("[]", "$[::-1]"), "[]");
  EXPECT_EQ(Query(kStore, "$.store.book[-1].title"), R"("The Lord of the Rings")");
}

TEST(JsonPath, WildcardsUnionsAndDescendants) {
  EXPECT_EQ(Query(kStore, "$.store.book[*].author"),
            R"(["Nigel Rees","Evelyn Waugh","Herman Melville","J. R. R. Tolkien"])");
  EXPECT_EQ(Query(kStore, "$..author"), R"(["Nigel Rees","Evelyn Waugh","Herman Melville","J. R. R. Tolkien"])");
  EXPECT_EQ(Query(kStore, "$.store.*.color"), R"(["red"])");
  EXPECT_EQ(Query(kStore, "$.store..price"), "[8.95,12.99,8.99,22.99,399]");
  EXPECT_EQ(Query(kStore, "$..book[2].title"), R"(["Moby Dick"])");
  EXPECT_EQ(Query(kStore, "$..book[-1:].isbn"), R"(["0-395-19395-8"])");
  EXPECT_EQ(Query(kStore, "$..book[0,1].price"), "[8.95,12.99]");
  EXPECT_EQ(Query(kStore, "$.store.book[0]['author','title']"), R"(["Nigel Rees","Sayings of the Century"])");
  EXPECT_EQ(Query(kStore, "$..isbn"), R"(["0-553-21311-3","0-395-19395-8"])");
  EXPECT_EQ(Query(R"({"a": {"b": 1}, "c": [2, {"d": 3}]})", "$..*"), R"([{"b": 1},[2, {"d": 3}],1,2,{"d": 3},3])");
  EXPECT_EQ(Query(R"({"a": [1, 2]})", "$..[0]"), "[1]");
  // A wildcard on a scalar selects nothing.
  EXPECT_EQ(Query("42", "$.*"), "[]");
}

TEST(JsonPath, Errors) {
  EXPECT_EQ(Query(kStore, ""), "error: empty path");
  EXPECT_EQ(Query(kStore, "$.store["), "error: missing ']' (at position 9)");
  EXPECT_EQ(Query(kStore, "$.store[?@.price < 10]"), "error: filter expressions are not supported (at position 9)");
  EXPECT_EQ(Query(kStore, "$.store['unterminated]"), "error: unterminated string (at position 9)");
  EXPECT_EQ(Query(kStore, "$store"), "error: expected '.' or '[' (at position 2)");
  EXPECT_EQ(Query(kStore, "$.."), "error: expected a member name (at position 4)");
  EXPECT_EQ(Query(kStore, "$[1 2]"), "error: expected ',' or ']' (at position 5)");
  EXPECT_EQ(Query(kStore, "$[99999999999999999999]"), "error: invalid integer (at position 3)");
  EXPECT_EQ(Query(kStore, "#a"), "error: a path starts with '$' (at position 1)");
  EXPECT_EQ(Query("{\"a\": ", "$.a"), "error: not a valid JSON document");
  EXPECT_EQ(Query(kStore, "$[0"), "error: missing ']' (at position 4)");
  EXPECT_EQ(Query(kStore, "$['a'"), "error: missing ']' (at position 6)");
  EXPECT_EQ(Query(kStore, "$[x]"), "error: expected a name, an index, a slice or '*' (at position 3)");
  EXPECT_EQ(Query(kStore, "$[1:-]"), "error: invalid integer (at position 5)");
  EXPECT_EQ(Query(kStore, "$[1:2:-]"), "error: invalid integer (at position 7)");
  EXPECT_EQ(Query(kStore, R"($['\x'])"), "error: invalid escape in a name (at position 6)");
}

TEST(JsonPath, SpacesAroundThePathAreIgnored) {
  EXPECT_EQ(Query(kStore, " \t$.store.bicycle.color\n"), R"("red")");
  EXPECT_EQ(Query(kStore, "$[ 'id' , 'content-type' ]"), R"([12345678901234567890,"json"])");
}

TEST(JsonPath, LimitStopsUnionsAndDescendants) {
  auto result = EvaluateJsonPath(R"([1, 2, 3])", "$[0,1,2]", 2);
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result->truncated);
  EXPECT_EQ(result->matches.size(), 2U);

  result = EvaluateJsonPath(kStore, "$..price", 3);
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result->truncated);
  ASSERT_EQ(result->matches.size(), 3U);
  EXPECT_EQ(result->matches[2], "8.99");

  result = EvaluateJsonPath(R"({"a": [[1], [2]], "b": {"c": 3}})", "$..*", 3);
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result->truncated);
  EXPECT_EQ(result->matches.size(), 3U);
}

TEST(JsonPath, SingularMatchLargerThanTheLimit) {
  auto result = EvaluateJsonPath(kStore, "$.store.bicycle");
  ASSERT_TRUE(result.has_value());
  ASSERT_TRUE(result->singular);
  RawChars out;
  EXPECT_EQ(JoinJsonPathMatches(*result, out, 10), 0U);
  EXPECT_TRUE(out.empty());
}

TEST(JsonPath, LimitsAndTruncation) {
  std::string big = "[";
  for (int i = 0; i < 1000; ++i) {
    big += (i == 0 ? "" : ",") + std::to_string(i);
  }
  big += ']';
  auto result = EvaluateJsonPath(big, "$[*]", 10);
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result->truncated);
  EXPECT_EQ(result->matches.size(), 10U);
  EXPECT_FALSE(result->singular);

  RawChars out;
  EXPECT_EQ(JoinJsonPathMatches(*result, out, 12), 5U);  // "[0,1,2,3,4]"
  EXPECT_EQ(View(out), "[0,1,2,3,4]");

  auto all = EvaluateJsonPath(big, "$[*]");
  ASSERT_TRUE(all.has_value());
  EXPECT_FALSE(all->truncated);
  EXPECT_EQ(all->matches.size(), 1000U);
}

TEST(JsonPath, DoesNotReadPastTheView) {
  // The document is a view of a larger buffer (no null terminator after it).
  const std::string buffer = R"({"a": [1, 2, 3]}garbage")";
  const std::string_view json = std::string_view(buffer).substr(0, 16);
  EXPECT_EQ(Query(json, "$.a[*]"), "[1,2,3]");
}

}  // namespace
}  // namespace zalie
