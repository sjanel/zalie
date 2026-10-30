#include "zalie/json-format.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "zalie/raw-chars.hpp"

namespace zalie {
namespace {

std::string Pretty(std::string_view json, std::uint32_t indent = 2) {
  RawChars out;
  EXPECT_TRUE(PrettyPrintJson(json, out, indent)) << json;
  return std::string(View(out));
}

TEST(JsonFormat, Scalars) {
  EXPECT_EQ(Pretty("  42 "), "42");
  EXPECT_EQ(Pretty("-0.5e+10"), "-0.5e+10");
  EXPECT_EQ(Pretty("true"), "true");
  EXPECT_EQ(Pretty("null"), "null");
  EXPECT_EQ(Pretty(R"("a \"quoted\" \u00e9 \\ string")"), R"("a \"quoted\" \u00e9 \\ string")");
}

TEST(JsonFormat, ObjectsAndArrays) {
  EXPECT_EQ(Pretty(R"({"a":1,"b":[true,null,{"c":"d"}],"e":{},"f":[]})"),
            "{\n"
            "  \"a\": 1,\n"
            "  \"b\": [\n"
            "    true,\n"
            "    null,\n"
            "    {\n"
            "      \"c\": \"d\"\n"
            "    }\n"
            "  ],\n"
            "  \"e\": {},\n"
            "  \"f\": []\n"
            "}");
  EXPECT_EQ(Pretty("[ 1 , 2 ]", 4), "[\n    1,\n    2\n]");
  EXPECT_EQ(Pretty("{ }"), "{}");
  // Already pretty documents are normalized.
  EXPECT_EQ(Pretty("{\n\t\"x\" :\r\n [ ]\n}"), "{\n  \"x\": []\n}");
}

TEST(JsonFormat, Invalid) {
  RawChars out;
  for (std::string_view invalid : {"",
                                   "   ",
                                   "{",
                                   "[1,]",
                                   "{\"a\" 1}",
                                   "{\"a\":}",
                                   "{1:2}",
                                   "[1 2]",
                                   "tru",
                                   "nul",
                                   "01",
                                   "1.",
                                   "1e",
                                   "-",
                                   "\"unterminated",
                                   R"("bad \x escape")",
                                   R"("bad \u12 escape")",
                                   "\"raw\ncontrol\"",
                                   "[1]]",
                                   "{} {}",
                                   "[1}",
                                   "{\"a\":1]",
                                   "+1",
                                   ".5"}) {
    EXPECT_FALSE(PrettyPrintJson(invalid, out)) << invalid;
    EXPECT_FALSE(IsValidJson(invalid)) << invalid;
  }
}

TEST(JsonFormat, IsValidJson) {
  EXPECT_TRUE(IsValidJson(R"({"a":[1,2,{"b":null}],"c":"\u0041"})"));
  EXPECT_TRUE(IsValidJson(" [] "));
}

TEST(JsonFormat, DepthLimit) {
  std::string deep(1024, '[');
  deep.append(1024, ']');
  EXPECT_TRUE(IsValidJson(deep));
  std::string tooDeep(1025, '[');
  tooDeep.append(1025, ']');
  EXPECT_FALSE(IsValidJson(tooDeep));
}

TEST(JsonFormat, LargeDocument) {
  std::string json = "[";
  static constexpr std::size_t kNbItems = 100000;
  for (std::size_t i = 0; i < kNbItems; ++i) {
    if (i != 0) {
      json.push_back(',');
    }
    json.append(R"({"id":)").append(std::to_string(i)).append(R"(,"name":"item","tags":["a","b"]})");
  }
  json.push_back(']');
  RawChars out;
  ASSERT_TRUE(PrettyPrintJson(json, out));
  EXPECT_GT(out.size(), json.size());
  EXPECT_TRUE(View(out).starts_with("[\n  {\n    \"id\": 0,\n    \"name\": \"item\",\n    \"tags\": [\n      \"a\""));
  EXPECT_TRUE(View(out).ends_with("    ]\n  }\n]"));
  // Pretty-printing is idempotent.
  RawChars again;
  ASSERT_TRUE(PrettyPrintJson(out, again));
  EXPECT_EQ(View(again), View(out));
}

}  // namespace
}  // namespace zalie
