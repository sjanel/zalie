#include "zalie/encoding.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

#include "zalie/raw-chars.hpp"

namespace zalie {
namespace {

std::string Escape(std::string_view str, const EscapeTable& table, char quote = '\0') {
  RawChars32 out;
  AppendAll(out, Escaped(str, table, quote));
  return std::string(View(out));
}

std::string Utf8(char32_t codePoint) {
  std::array<char, kMaxUtf8Size> buffer{};
  const char* pEnd = AppendUtf8(codePoint, buffer.data());
  return {buffer.data(), static_cast<std::size_t>(pEnd - buffer.data())};
}

TEST(Escaping, JsonStrings) {
  EXPECT_EQ(Escape("a\"b\\c\nd\te\x01"
                   "\xC3\xA9",
                   kJsonEscapes, '"'),
            "\"a\\\"b\\\\c\\nd\\te\\u0001\xC3\xA9\"");
  EXPECT_EQ(Escape("\r\b\f\x1F\x7F", kJsonEscapes), "\\r\\b\\f\\u001f\x7F");
  EXPECT_EQ(Escape(std::string_view("\0", 1), kJsonEscapes), "\\u0000");
  EXPECT_EQ(Escape("", kJsonEscapes), "");
  EXPECT_EQ(Escape("", kJsonEscapes, '"'), "\"\"");
  EXPECT_EQ(kJsonEscapes.extraSize('a'), 0U);
  EXPECT_EQ(kJsonEscapes.extraSize('"'), 1U);
  EXPECT_EQ(kJsonEscapes.extraSize('\x02'), 5U);
  EXPECT_EQ(kJsonEscapes.extraSize('\xFF'), 0U);
}

TEST(Escaping, CppLiteralsUseOctalForControlCharacters) {
  // A hexadecimal escape would take the 'f' that follows as one of its digits.
  EXPECT_EQ(Escape("a\x01"
                   "f\x1F\"\\\n\r\t",
                   kCppEscapes, '"'),
            "\"a\\001f\\037\\\"\\\\\\n\\r\\t\"");
  EXPECT_EQ(Escape("\b", kCppEscapes), "\\010");
}

TEST(Escaping, Shells) {
  EXPECT_EQ(Escape("it's $HOME", kShellEscapes, '\''), R"('it'\''s $HOME')");
  EXPECT_EQ(Escape("", kShellEscapes, '\''), "''");
  EXPECT_EQ(Escape("''", kShellEscapes), R"('\'''\'')");
  EXPECT_EQ(Escape("it's", kPowerShellEscapes, '\''), "'it''s'");
  EXPECT_EQ(Escape("\"\\\n", kPowerShellEscapes), "\"\\\n");
}

TEST(Escaping, DesktopEntries) {
  EXPECT_EQ(Escape("a\\b\nc\td\re", kDesktopStringEscapes), "a\\\\b\\nc\\td\\re");
  EXPECT_EQ(Escape("/opt/my \"app\"/$x`y`\\100%", kDesktopExecEscapes, '"'),
            "\"/opt/my \\\"app\\\"/\\$x\\`y\\`\\\\100%%\"");
}

TEST(Escaping, LongRunsAndManyEscapesKeepEveryByte) {
  // Plain runs longer than the inlined copies, escapes at each end and next to each other.
  std::string input;
  std::string expected;
  for (int block = 0; block < 500; ++block) {
    const std::string run(static_cast<std::size_t>(block % 70), static_cast<char>('a' + (block % 26)));
    input += "\"" + run + "\\\n";
    expected += "\\\"" + run + "\\\\\\n";
  }
  EXPECT_EQ(Escape(input, kJsonEscapes), expected);
  EXPECT_EQ(Escape(input, kJsonEscapes, '"'), "\"" + expected + "\"");

  // Appended after existing content, followed by another part: the slack of a part is not part of the output.
  RawChars32 out(std::string_view{"x="});
  AppendAll(out, Escaped("\n", kJsonEscapes), Escaped("\t", kJsonEscapes, '\''), "!");
  EXPECT_EQ(View(out), "x=\\n'\\t'!");
}

TEST(Base64, EncodesWithPadding) {
  const auto encode = [](std::string_view data) {
    RawChars32 out;
    AppendAll(out, Base64(data));
    return std::string(View(out));
  };
  EXPECT_EQ(encode(""), "");
  EXPECT_EQ(encode("f"), "Zg==");
  EXPECT_EQ(encode("fo"), "Zm8=");
  EXPECT_EQ(encode("foo"), "Zm9v");
  EXPECT_EQ(encode("foob"), "Zm9vYg==");
  EXPECT_EQ(encode("user:pass"), "dXNlcjpwYXNz");
  EXPECT_EQ(encode(std::string_view("\xFB\xFF\x00", 3)), "+/8A");
  RawChars32 header;
  AppendAll(header, "Basic ", Base64("aladdin:opensesame"), '\n');
  EXPECT_EQ(View(header), "Basic YWxhZGRpbjpvcGVuc2VzYW1l\n");
}

TEST(Utf8, EncodesEveryLength) {
  EXPECT_EQ(Utf8(U'A'), "A");
  EXPECT_EQ(Utf8(0x7F), "\x7F");
  EXPECT_EQ(Utf8(0x80), "\xC2\x80");
  EXPECT_EQ(Utf8(U'é'), "\xC3\xA9");
  EXPECT_EQ(Utf8(0x7FF), "\xDF\xBF");
  EXPECT_EQ(Utf8(0x800), "\xE0\xA0\x80");
  EXPECT_EQ(Utf8(U'€'), "\xE2\x82\xAC");
  EXPECT_EQ(Utf8(0xFFFF), "\xEF\xBF\xBF");
  EXPECT_EQ(Utf8(0x10000), "\xF0\x90\x80\x80");
  EXPECT_EQ(Utf8(U'😀'), "\xF0\x9F\x98\x80");
  EXPECT_EQ(Utf8(0x10FFFF), "\xF4\x8F\xBF\xBF");
}

TEST(Hexadecimal, ReadsUpToAMaximumOfDigits) {
  char32_t value = 99;
  EXPECT_EQ(ReadHex("1fZ", 8, value), 2U);
  EXPECT_EQ(value, 0x1FU);
  EXPECT_EQ(ReadHex("ABCDEF", 4, value), 4U);
  EXPECT_EQ(value, 0xABCDU);
  EXPECT_EQ(ReadHex("0010ffff", 8, value), 8U);
  EXPECT_EQ(value, 0x10FFFFU);
  EXPECT_EQ(ReadHex("g1", 2, value), 0U);
  EXPECT_EQ(value, 0U);
  EXPECT_EQ(ReadHex("", 2, value), 0U);
  EXPECT_EQ(ReadHex("12", 0, value), 0U);
}

TEST(ControlEscapes, NamesOfCAndShells) {
  EXPECT_EQ(ControlEscape('n'), '\n');
  EXPECT_EQ(ControlEscape('t'), '\t');
  EXPECT_EQ(ControlEscape('r'), '\r');
  EXPECT_EQ(ControlEscape('b'), '\b');
  EXPECT_EQ(ControlEscape('f'), '\f');
  EXPECT_EQ(ControlEscape('a'), '\a');
  EXPECT_EQ(ControlEscape('v'), '\v');
  EXPECT_EQ(ControlEscape('e'), '\x1B');
  EXPECT_EQ(ControlEscape('x'), '\0');
  EXPECT_EQ(ControlEscape('N'), '\0');
  EXPECT_EQ(ControlEscape('\0'), '\0');
}

TEST(JsonUnescape, DecodesEveryEscape) {
  RawChars32 out(std::string_view{"> "});
  ASSERT_TRUE(AppendJsonUnescaped(R"(plain \" \' \\ \/ \b\f\n\r\t end)", out));
  EXPECT_EQ(View(out), "> plain \" ' \\ / \b\f\n\r\t end");
  out.clear();
  ASSERT_TRUE(AppendJsonUnescaped(R"(Aé€ 😀!)", out));
  EXPECT_EQ(View(out), "A\xC3\xA9\xE2\x82\xAC \xF0\x9F\x98\x80!");
  out.clear();
  ASSERT_TRUE(AppendJsonUnescaped("", out));
  ASSERT_TRUE(AppendJsonUnescaped("no escape at all, in a text longer than thirty-two bytes", out));
  EXPECT_EQ(View(out), "no escape at all, in a text longer than thirty-two bytes");
}

TEST(JsonUnescape, RefusesInvalidEscapesAndLeavesTheOutputUnchanged) {
  for (std::string_view invalid : {
           std::string_view{R"(end\)"},          // nothing after the backslash
           std::string_view{R"(a\qb)"},          // unknown escape
           std::string_view{R"(\u12)"},          // truncated code unit
           std::string_view{R"(\u12G4)"},        // not hexadecimal
           std::string_view{R"(\ud83d)"},        // high surrogate alone
           std::string_view{R"(\ud83d\n)"},      // high surrogate followed by another escape
           std::string_view{R"(\ud83dxxxxxx)"},  // high surrogate followed by text
           std::string_view{R"(\ud83dA)"},       // high surrogate followed by a non surrogate
           std::string_view{R"(\ud83d\ude0)"},   // truncated low surrogate
           std::string_view{R"(\ude00)"},        // low surrogate alone
       }) {
    RawChars32 out(std::string_view{"kept"});
    EXPECT_FALSE(AppendJsonUnescaped(invalid, out)) << invalid;
    EXPECT_EQ(View(out), "kept") << invalid;
  }
}

TEST(JsonUnescape, NeverGrowsARoomAsLargeAsTheInput) {
  std::string raw;
  std::string expected;
  for (int idx = 0; idx < 3000; ++idx) {
    raw += R"(line \"quoted\" é\n)";
    expected += "line \"quoted\" \xC3\xA9\n";
  }
  RawChars32 out(raw.size());
  const char* pData = out.data();
  ASSERT_TRUE(AppendJsonUnescaped(raw, out));
  EXPECT_EQ(View(out), expected);
  EXPECT_EQ(out.data(), pData);
}

}  // namespace
}  // namespace zalie
