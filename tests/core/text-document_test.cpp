#include "zalie/text-document.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>

#include "zalie/syntax.hpp"

namespace zalie {
namespace {

using namespace std::string_view_literals;

TEST(ContentKind, FromContentType) {
  EXPECT_EQ(DetectContentKind("application/json; charset=utf-8", "x"), ContentKind::json);
  EXPECT_EQ(DetectContentKind("application/problem+json", "x"), ContentKind::json);
  EXPECT_EQ(DetectContentKind("text/html", "x"), ContentKind::html);
  EXPECT_EQ(DetectContentKind("application/xml", "x"), ContentKind::xml);
  EXPECT_EQ(DetectContentKind("text/javascript", "x"), ContentKind::javascript);
  EXPECT_EQ(DetectContentKind("image/png", "\x89PNG\r\n"), ContentKind::image);
  EXPECT_EQ(DetectContentKind("image/webp", "RIFF...."), ContentKind::binary);  // not decodable
  EXPECT_EQ(DetectContentKind("text/plain", "hello"), ContentKind::text);
  EXPECT_EQ(DetectContentKind("application/json", ""), ContentKind::empty);
}

TEST(ContentKind, Sniffing) {
  EXPECT_EQ(DetectContentKind("", "  {\"a\":1}"), ContentKind::json);
  EXPECT_EQ(DetectContentKind("text/plain", "[1,2]"), ContentKind::json);
  EXPECT_EQ(DetectContentKind("", "<!DOCTYPE html><html>"), ContentKind::html);
  EXPECT_EQ(DetectContentKind("", "<?xml version=\"1.0\"?><a/>"), ContentKind::xml);
  EXPECT_EQ(DetectContentKind("application/octet-stream", "ab\0cd"sv), ContentKind::binary);
  EXPECT_EQ(DetectContentKind("application/octet-stream", "GIF89a\0"sv), ContentKind::image);
  EXPECT_EQ(DetectContentKind("", "plain words"), ContentKind::text);
  EXPECT_EQ(ContentKindName(ContentKind::json), "JSON");
  EXPECT_EQ(SyntaxFor(ContentKind::html), SyntaxLanguage::xml);
  EXPECT_EQ(SyntaxFor(ContentKind::text), SyntaxLanguage::plain);
}

TEST(LineIndex, Lines) {
  LineIndex index;
  constexpr std::string_view kText = "first\r\nsecond\n\nlast";
  index.build(kText);
  ASSERT_EQ(index.size(), 4U);
  EXPECT_EQ(index.line(kText, 0), "first");  // CR stripped
  EXPECT_EQ(index.line(kText, 1), "second");
  EXPECT_EQ(index.line(kText, 2), "");
  EXPECT_EQ(index.line(kText, 3), "last");
  EXPECT_EQ(index.longestLine(), 6U);
  EXPECT_EQ(index.lineOf(0), 0U);
  EXPECT_EQ(index.lineOf(7), 1U);
  EXPECT_EQ(index.lineOf(kText.size() - 1), 3U);
  EXPECT_EQ(index.lineOf(1000), 3U);

  index.build("a\n");
  EXPECT_EQ(index.size(), 2U);  // a trailing newline ends with an empty line
  index.build("");
  EXPECT_TRUE(index.empty());
  EXPECT_EQ(index.lineOf(5), 0U);
}

TEST(LineIndex, LongLinesAreSplitOnUtf8Boundaries) {
  std::string text(10, 'a');
  text.append("é");  // 2 bytes at offsets 10-11
  text.append(10, 'b');
  LineIndex index;
  index.build(text, SyntaxLanguage::plain, 11);
  ASSERT_EQ(index.size(), 3U);
  EXPECT_EQ(index.line(text, 0), std::string(10, 'a'));  // backs off before the 2-byte sequence
  EXPECT_EQ(index.line(text, 1), "é" + std::string(9, 'b'));
  EXPECT_EQ(index.line(text, 2), "b");
  EXPECT_EQ(index.longestLine(), 11U);
  EXPECT_EQ(index.lineBegin(1), 10U);
}

TEST(LineIndex, RecordsSyntaxStatesForStatefulLanguages) {
  constexpr std::string_view kXml = "<a>\n<!-- multi\nline -->\n<b/>";
  LineIndex index;
  index.build(kXml, SyntaxLanguage::xml);
  ASSERT_EQ(index.size(), 4U);
  EXPECT_EQ(index.stateAt(0), 0);
  EXPECT_EQ(index.stateAt(1), 0);
  EXPECT_NE(index.stateAt(2), 0);  // inside the comment
  EXPECT_EQ(index.stateAt(3), 0);

  index.build("{\n}", SyntaxLanguage::json);  // stateless: no states stored
  EXPECT_EQ(index.stateAt(1), 0);
}

TEST(LineIndex, ManyLines) {
  std::string text;
  static constexpr std::size_t kNbLines = 200000;
  for (std::size_t i = 0; i < kNbLines; ++i) {
    text.append("line ").append(std::to_string(i)).push_back('\n');
  }
  LineIndex index;
  index.build(text);
  EXPECT_EQ(index.size(), kNbLines + 1);
  EXPECT_EQ(index.line(text, 123456), "line 123456");
  EXPECT_EQ(index.lineOf(index.lineBegin(123456) + 3), 123456U);
}

}  // namespace
}  // namespace zalie
