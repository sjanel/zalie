#include "zalie/string-utils.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "zalie/raw-chars.hpp"

namespace zalie {
namespace {

TEST(StringUtils, Trim) {
  EXPECT_EQ(Trim("  \t a b \r\n"), "a b");
  EXPECT_EQ(Trim("\v\fx\v\f"), "x");
  EXPECT_EQ(Trim("   "), "");
  EXPECT_EQ(Trim(""), "");
  EXPECT_EQ(Trim("untouched"), "untouched");
  static constexpr std::string_view kTrimmed = Trim(" compile time ");
  EXPECT_EQ(kTrimmed, "compile time");
}

TEST(StringUtils, CaseInsensitiveSearch) {
  EXPECT_TRUE(StartsWithIgnoreCase("Bearer xyz", "bearer"));
  EXPECT_TRUE(StartsWithIgnoreCase("Bearer", ""));
  EXPECT_FALSE(StartsWithIgnoreCase("Bear", "bearer"));
  EXPECT_FALSE(StartsWithIgnoreCase("Basic xyz", "bearer"));

  EXPECT_EQ(FindIgnoreCase("application/JSON", "json"), 12U);
  EXPECT_EQ(FindIgnoreCase("abc", "x"), std::string_view::npos);
  EXPECT_EQ(FindIgnoreCase("abc", ""), 0U);
  EXPECT_EQ(FindIgnoreCase("abc", "", 3), 3U);
  EXPECT_EQ(FindIgnoreCase("abc", "", 4), std::string_view::npos);
  EXPECT_EQ(FindIgnoreCase("ab", "abc"), std::string_view::npos);
  // A first character matching before the needle does: the search goes on.
  EXPECT_EQ(FindIgnoreCase("xmas XML xml", "XML"), 5U);
  EXPECT_EQ(FindIgnoreCase("xmas XML xml", "xml", 6), 9U);
  EXPECT_EQ(FindIgnoreCase("xmas XML xml", "xml", 10), std::string_view::npos);

  EXPECT_TRUE(ContainsIgnoreCase("text/HTML; charset=utf-8", "html"));
  EXPECT_FALSE(ContainsIgnoreCase("text/plain", "html"));
  // Needles longer than a machine word, and than a vector register.
  const std::string haystack = "Content-Type: APPLICATION/X-WWW-FORM-URLENCODED; charset=UTF-8";
  EXPECT_TRUE(ContainsIgnoreCase(haystack, "x-www-form-urlencoded"));
  EXPECT_TRUE(ContainsIgnoreCase(haystack, "application/x-www-form-urlencoded; charset=utf-8"));
  EXPECT_FALSE(ContainsIgnoreCase(haystack, "application/x-www-form-urlencoded; charset=utf-9"));
}

TEST(StringUtils, AssignLowerReplacesTheContent) {
  RawChars32 out(std::string_view{"previous content, longer than the next one"});
  AssignLower(out, "X-Api-KEY");
  EXPECT_EQ(View(out), "x-api-key");
  const std::string_view mixed = "Strict-Transport-Security_AND_A_Few_More_Bytes[]{}@`~0123456789";
  AssignLower(out, mixed);
  EXPECT_EQ(View(out), "strict-transport-security_and_a_few_more_bytes[]{}@`~0123456789");
  AssignLower(out, {});
  EXPECT_TRUE(out.empty());
  RawChars big;
  AssignLower(big, "ABC");
  EXPECT_EQ(View(big), "abc");
}

TEST(StringUtils, HttpTokens) {
  EXPECT_TRUE(IsHttpToken("X-Custom_Header.1"));
  EXPECT_TRUE(IsHttpToken("!#$%&'*+-.^_`|~"));
  EXPECT_FALSE(IsHttpToken("Bad Header"));
  EXPECT_FALSE(IsHttpToken(""));
  EXPECT_FALSE(IsHttpToken("a:b"));
  EXPECT_FALSE(IsHttpToken("caf\xC3\xA9"));
  EXPECT_FALSE(IsHttpToken(std::string_view("a\0b", 3)));
}

}  // namespace
}  // namespace zalie
