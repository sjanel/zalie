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

TEST(Syntax, PlainHighlightsVariables) {
  EXPECT_EQ(Tokens(SyntaxLanguage::plain, "{{base}}/users/{{ id }}?x={{"),
            (Expected{{"{{base}}", variable}, {"{{ id }}", variable}}));
}

}  // namespace
}  // namespace zalie
