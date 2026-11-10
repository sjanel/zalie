#include "zalie/syntax.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace zalie {
namespace {

// Text and kind of each token of one line.
std::vector<std::pair<std::string, TokenKind>> Tokens(SyntaxLanguage language, std::string_view line,
                                                      SyntaxState& state) {
  LineTokens tokens;
  TokenizeLine(language, line, state, tokens);
  std::vector<std::pair<std::string, TokenKind>> ret;
  std::size_t previousEnd = 0;
  for (const Token& token : tokens) {
    EXPECT_GE(token.begin, previousEnd) << "tokens must be ordered and not overlap";
    EXPECT_LE(token.begin + token.length, line.size());
    previousEnd = token.begin + token.length;
    ret.emplace_back(std::string(line.substr(token.begin, token.length)), token.kind);
  }
  return ret;
}

std::vector<std::pair<std::string, TokenKind>> Tokens(SyntaxLanguage language, std::string_view line) {
  SyntaxState state = 0;
  return Tokens(language, line, state);
}

using enum TokenKind;
using Expected = std::vector<std::pair<std::string, TokenKind>>;

TEST(Syntax, Json) {
  EXPECT_EQ(Tokens(SyntaxLanguage::json, R"(  "name" : "Zalie \"z\"", "n": -1.5e3, "ok": true)"),
            (Expected{{R"("name")", key},
                      {":", punctuation},
                      {R"("Zalie \"z\"")", string},
                      {",", punctuation},
                      {R"("n")", key},
                      {":", punctuation},
                      {"-1.5e3", number},
                      {",", punctuation},
                      {R"("ok")", key},
                      {":", punctuation},
                      {"true", keyword}}));
  EXPECT_EQ(Tokens(SyntaxLanguage::json, R"(["a", {)"),
            (Expected{{"[", punctuation}, {R"("a")", string}, {",", punctuation}, {"{", punctuation}}));
  EXPECT_EQ(Tokens(SyntaxLanguage::json, R"("unterminated)"), (Expected{{R"("unterminated)", string}}));
}

TEST(Syntax, XmlAcrossLines) {
  SyntaxState state = 0;
  EXPECT_EQ(Tokens(SyntaxLanguage::xml, R"(<user id="42" name='a)", state), (Expected{{"<user", tag},
                                                                                      {"id", attribute},
                                                                                      {"=", punctuation},
                                                                                      {R"("42")", string},
                                                                                      {"name", attribute},
                                                                                      {"=", punctuation},
                                                                                      {"'a", string}}));
  EXPECT_NE(state, 0);
  EXPECT_EQ(Tokens(SyntaxLanguage::xml, R"(b'>text<!-- note)", state),
            (Expected{{"b'", string}, {">", tag}, {"<!-- note", comment}}));
  EXPECT_EQ(Tokens(SyntaxLanguage::xml, "still --> <br/></user>", state),
            (Expected{{"still -->", comment}, {"<br", tag}, {"/>", tag}, {"</user", tag}, {">", tag}}));
  EXPECT_EQ(state, 0);
}

TEST(Syntax, Shell) {
  SyntaxState state = 0;
  EXPECT_EQ(Tokens(SyntaxLanguage::shell, "curl -X POST 'https://h' \\", state),
            (Expected{{"curl", keyword}, {"-X", attribute}, {"'https://h'", string}, {"\\", punctuation}}));
  EXPECT_EQ(Tokens(SyntaxLanguage::shell, "  --data-raw '{\"a\":", state),
            (Expected{{"--data-raw", attribute}, {"'{\"a\":", string}}));
  EXPECT_NE(state, 0);
  EXPECT_EQ(Tokens(SyntaxLanguage::shell, "1}' # done", state), (Expected{{"1}'", string}, {"# done", comment}}));
  EXPECT_EQ(state, 0);
}

TEST(Syntax, Code) {
  SyntaxState state = 0;
  EXPECT_EQ(Tokens(SyntaxLanguage::code, R"(const response = await fetch("https://h"); // go)", state),
            (Expected{{"const", keyword},
                      {"=", punctuation},
                      {"await", keyword},
                      {"fetch", tag},
                      {"(", punctuation},
                      {R"("https://h")", string},
                      {")", punctuation},
                      {";", punctuation},
                      {"// go", comment}}));
  EXPECT_EQ(Tokens(SyntaxLanguage::code, "#include <x> # comment"),
            (Expected{{"#include", keyword}, {"<", punctuation}, {">", punctuation}, {"# comment", comment}}));
  // Raw strings spanning lines.
  EXPECT_EQ(Tokens(SyntaxLanguage::code, R"(body(R"zalie({)", state),
            (Expected{{"body", tag}, {"(", punctuation}, {"R\"", string}, {"zalie({", string}}));
  EXPECT_EQ(state, 1);
  EXPECT_EQ(
      Tokens(SyntaxLanguage::code, R"(})zalie", 42);)", state),
      (Expected{{R"(})zalie")", string}, {",", punctuation}, {"42", number}, {")", punctuation}, {";", punctuation}}));
  EXPECT_EQ(state, 0);
}

TEST(Syntax, Http) {
  SyntaxState state = 0;
  EXPECT_EQ(Tokens(SyntaxLanguage::http, "POST /x HTTP/1.1\r", state),
            (Expected{{"POST", keyword}, {"HTTP/1.1", comment}}));
  EXPECT_EQ(Tokens(SyntaxLanguage::http, "Host: h", state),
            (Expected{{"Host", key}, {":", punctuation}, {" h", string}}));
  EXPECT_TRUE(Tokens(SyntaxLanguage::http, "", state).empty());
  EXPECT_TRUE(Tokens(SyntaxLanguage::http, "body: not a header", state).empty());
}

TEST(Syntax, JsonKeywordsAndNumbers) {
  EXPECT_EQ(Tokens(SyntaxLanguage::json, "[null,false,12e+3,0.5]"), (Expected{{"[", punctuation},
                                                                              {"null", keyword},
                                                                              {",", punctuation},
                                                                              {"false", keyword},
                                                                              {",", punctuation},
                                                                              {"12e+3", number},
                                                                              {",", punctuation},
                                                                              {"0.5", number},
                                                                              {"]", punctuation}}));
  // A backslash before the closing quote escapes it, and a key may be followed by blanks before its colon.
  EXPECT_EQ(Tokens(SyntaxLanguage::json, "\"a\\\\\" \t: \"b\\\""),
            (Expected{{"\"a\\\\\"", key}, {":", punctuation}, {"\"b\\\"", string}}));
}

TEST(Syntax, XmlDoubleQuotedValueAcrossLines) {
  SyntaxState state = 0;
  EXPECT_EQ(Tokens(SyntaxLanguage::xml, R"(<?xml version="1.0 ?>)", state),
            (Expected{{"<?xml", tag}, {"version", attribute}, {"=", punctuation}, {R"("1.0 ?>)", string}}));
  EXPECT_EQ(Tokens(SyntaxLanguage::xml, "still in the value", state), (Expected{{"still in the value", string}}));
  EXPECT_EQ(Tokens(SyntaxLanguage::xml, R"(end" ?>)", state), (Expected{{R"(end")", string}, {"?>", tag}}));
  EXPECT_EQ(state, 0);
  // Text without tag, then a lone '/' and '?' in a tag.
  EXPECT_TRUE(Tokens(SyntaxLanguage::xml, "plain text", state).empty());
  EXPECT_EQ(Tokens(SyntaxLanguage::xml, "<a / ?", state), (Expected{{"<a", tag}, {"/", attribute}, {"?", attribute}}));
}

TEST(Syntax, ShellQuotes) {
  SyntaxState state = 0;
  // ANSI-C quotes honor escapes, single quotes do not.
  EXPECT_EQ(Tokens(SyntaxLanguage::shell, R"(echo $'it\'s' 'a\' x#y)", state),
            (Expected{{"echo", keyword}, {R"($'it\'s')", string}, {R"('a\')", string}}));
  EXPECT_EQ(Tokens(SyntaxLanguage::shell, R"(printf "a\"b)", state),
            (Expected{{"printf", keyword}, {R"("a\"b)", string}}));
  EXPECT_EQ(state, 2);
  // The continuation of a double quoted argument honors escapes too.
  EXPECT_EQ(Tokens(SyntaxLanguage::shell, R"(c\"d" -v)", state), (Expected{{R"(c\"d")", string}, {"-v", attribute}}));
  EXPECT_EQ(state, 0);
  // A word glued to a quote, a backslash that does not end the line.
  EXPECT_EQ(Tokens(SyntaxLanguage::shell, R"(a"b"c \x)", state), (Expected{{"a", keyword}, {R"("b")", string}}));
}

TEST(Syntax, CodeBackquotesAndVariables) {
  SyntaxState state = 0;
  EXPECT_EQ(Tokens(SyntaxLanguage::code, "body := `{", state),
            (Expected{{":", punctuation}, {"=", punctuation}, {"`", string}, {"{", string}}));
  EXPECT_EQ(state, 2);
  EXPECT_EQ(Tokens(SyntaxLanguage::code, "\"a\": 1", state), (Expected{{"\"a\": 1", string}}));
  EXPECT_EQ(Tokens(SyntaxLanguage::code, "}` + 'c' - $x", state),
            (Expected{{"}`", string}, {"'c'", string}, {"$x", attribute}}));
  EXPECT_EQ(state, 0);
  // A ')' in a raw string that is not followed by the delimiter and '"' does not end it.
  EXPECT_EQ(Tokens(SyntaxLanguage::code, R"(R"x(a) b)x")", state),
            (Expected{{"R\"", string}, {R"(x(a) b)x")", string}}));
  EXPECT_EQ(state, 0);
  EXPECT_EQ(Tokens(SyntaxLanguage::code, R"(R"x(a) b)", state), (Expected{{"R\"", string}, {"x(a) b", string}}));
  EXPECT_EQ(state, 1);
}

TEST(Syntax, HttpStatusLineAndBody) {
  SyntaxState state = 0;
  EXPECT_EQ(Tokens(SyntaxLanguage::http, "HTTP/1.1 200 OK", state), (Expected{{"HTTP/1.1", keyword}}));
  EXPECT_EQ(Tokens(SyntaxLanguage::http, "X-Empty:", state), (Expected{{"X-Empty", key}, {":", punctuation}}));
  EXPECT_TRUE(Tokens(SyntaxLanguage::http, "\r", state).empty());
  EXPECT_EQ(state, 2);
  EXPECT_TRUE(Tokens(SyntaxLanguage::http, "Not: a header", state).empty());
  // A request line without space is a lone method.
  state = 0;
  EXPECT_EQ(Tokens(SyntaxLanguage::http, "GET", state), (Expected{{"GET", keyword}}));
}

TEST(Syntax, PlainHighlightsVariables) {
  EXPECT_EQ(Tokens(SyntaxLanguage::plain, "{{base}}/users/{{ id }}?x={{"),
            (Expected{{"{{base}}", variable}, {"{{ id }}", variable}}));
}

}  // namespace
}  // namespace zalie
