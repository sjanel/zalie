// The JSON scanner of the JSONPath engine and of the JSON Lines data files (a private header of zalie_core).
#include "json-scan.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace zalie {
namespace {

// The members of 'object' as "key=value;" pairs, or "malformed".
std::string Members(std::string_view object) {
  std::string out;
  const bool wellFormed = ScanJsonObject(object, [&out](std::string_view key, std::string_view value) {
    out.append(key).append("=").append(value).append(";");
    return true;
  });
  return wellFormed ? out : "malformed";
}

TEST(JsonScan, WellFormedObjects) {
  EXPECT_EQ(Members("{}"), "");
  EXPECT_EQ(Members("{ }"), "");
  EXPECT_EQ(Members(R"({"a": 1, "b" : "x\"y", "c": {"d": [1, "]"]}, "e": null})"),
            R"(a=1;b="x\"y";c={"d": [1, "]"]};e=null;)");
}

TEST(JsonScan, MalformedObjects) {
  for (std::string_view object : {
           R"({} x)",            // text after the object
           R"({"a)",             // unterminated key
           R"({"a" 1})",         // no colon
           R"({"a":)",           // no value
           R"({"a":})",          // empty value
           R"({"a":"x})",        // unterminated string value
           R"({"a":{"b":"x}})",  // unterminated string in a nested value
           R"({"a":[1,2})",      // unterminated array
           R"({"a":1)",          // no closing brace
           R"({"a":1 "b":2})",   // no comma
           R"({"a":1,})",        // a comma and no member
           R"({a:1})",           // a key without quotes
       }) {
    EXPECT_EQ(Members(object), "malformed") << object;
  }
}

TEST(JsonScan, TheCallbackStopsTheScan) {
  std::size_t nbCalls = 0;
  EXPECT_FALSE(ScanJsonObject(R"({"a":1,"b":2})", [&nbCalls](std::string_view, std::string_view) {
    ++nbCalls;
    return false;
  }));
  EXPECT_EQ(nbCalls, 1U);
}

TEST(JsonScan, ValueEnds) {
  EXPECT_EQ(ValueEnd("123,", 0), 3U);
  EXPECT_EQ(ValueEnd(R"("a\"b" ,)", 0), 6U);
  EXPECT_EQ(ValueEnd("[1,[2]]x", 0), 7U);
  EXPECT_EQ(ValueEnd("", 0), std::string_view::npos);
  EXPECT_EQ(ValueEnd(",", 0), std::string_view::npos);
  EXPECT_EQ(SkipSpaces(" \t\r\nx", 0), 4U);
}

}  // namespace
}  // namespace zalie
