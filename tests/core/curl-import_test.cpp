#include "zalie/curl-import.hpp"

#include <gtest/gtest.h>

#include <string_view>

#include "zalie/key-value.hpp"
#include "zalie/method.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/request.hpp"

namespace zalie {
namespace {

std::string_view HeaderOf(const Request& req, std::string_view name) {
  const KeyValue* pHeader = FindEnabled(req.headers, name, true);
  return pHeader == nullptr ? std::string_view{} : std::string_view{pHeader->value};
}

TEST(SplitShellWords, Quotes) {
  auto words = SplitShellWords(R"(curl 'single quoted' "double \"quoted\" \$x" plain\ escaped mixed'a'"b"c)");
  ASSERT_TRUE(words.has_value());
  ASSERT_EQ(words->size(), 5U);
  EXPECT_EQ((*words)[0], "curl");
  EXPECT_EQ((*words)[1], "single quoted");
  EXPECT_EQ((*words)[2], "double \"quoted\" $x");
  EXPECT_EQ((*words)[3], "plain escaped");
  EXPECT_EQ((*words)[4], "mixedabc");
}

TEST(SplitShellWords, AnsiCQuotes) {
  auto words = SplitShellWords(R"(x $'it\'s\n\t\x41\u00e9\101\\')");
  ASSERT_TRUE(words.has_value());
  ASSERT_EQ(words->size(), 2U);
  EXPECT_EQ((*words)[1], "it's\n\tAéA\\");
}

TEST(SplitShellWords, LineContinuations) {
  auto words = SplitShellWords("curl \\\n  -H 'a: b' ^\r\n  --url `\n x");
  ASSERT_TRUE(words.has_value());
  ASSERT_EQ(words->size(), 5U);
  EXPECT_EQ((*words)[2], "a: b");
  EXPECT_EQ((*words)[4], "x");
}

TEST(SplitShellWords, Errors) {
  EXPECT_FALSE(SplitShellWords("curl 'unterminated").has_value());
  EXPECT_FALSE(SplitShellWords("curl \"unterminated").has_value());
  EXPECT_FALSE(SplitShellWords("curl $'unterminated").has_value());
  auto empty = SplitShellWords("   ");
  ASSERT_TRUE(empty.has_value());
  EXPECT_TRUE(empty->empty());
}

TEST(CurlImport, LooksLikeCurlCommand) {
  EXPECT_TRUE(LooksLikeCurlCommand("  curl https://x"));
  EXPECT_TRUE(LooksLikeCurlCommand("curl.exe https://x"));
  EXPECT_FALSE(LooksLikeCurlCommand("https://curl.se"));
  EXPECT_FALSE(LooksLikeCurlCommand("curly"));
}

TEST(CurlImport, SimpleGet) {
  auto imported = ParseCurlCommand("curl https://api.example.com/users/42?expand=team");
  ASSERT_TRUE(imported.has_value());
  const Request& req = imported->request;
  EXPECT_EQ(req.method, Method::get);
  EXPECT_EQ(View(req.url), "https://api.example.com/users/42?expand=team");
  ASSERT_EQ(req.params.size(), 1U);
  EXPECT_EQ(View(req.params[0].key), "expand");
  EXPECT_EQ(View(req.name), "GET users/42");
  EXPECT_EQ(req.bodyMode, BodyMode::none);
  EXPECT_TRUE(imported->warnings.empty());
}

TEST(CurlImport, ChromeCopyAsCurl) {
  constexpr std::string_view kCommand = R"(curl 'https://api.example.com/graph' \
  -H 'accept: application/json' \
  -H 'content-type: application/json' \
  -b 'session=abc; theme=dark' \
  --data-raw $'{"query":"it\'s"}' \
  --compressed)";
  auto imported = ParseCurlCommand(kCommand);
  ASSERT_TRUE(imported.has_value()) << View(imported.error());
  const Request& req = imported->request;
  EXPECT_EQ(req.method, Method::post);  // implied by the data
  EXPECT_EQ(req.bodyMode, BodyMode::json);
  EXPECT_EQ(View(req.body), R"({"query":"it's"})");
  EXPECT_EQ(HeaderOf(req, "Accept"), "application/json");
  EXPECT_EQ(HeaderOf(req, "Cookie"), "session=abc; theme=dark");
  EXPECT_TRUE(req.settings.decompress);
}

TEST(CurlImport, MethodHeadersAuthAndFlags) {
  auto imported = ParseCurlCommand(
      "curl -X PUT -HX-A:1 --header='X-B: 2' -u alice:s3cr3t -A zalie-agent -e https://ref -kLsS "
      "--max-time 2.5 --http1.1 -o /dev/null --url http://localhost:8080/items/1");
  ASSERT_TRUE(imported.has_value()) << View(imported.error());
  const Request& req = imported->request;
  EXPECT_EQ(req.method, Method::put);
  EXPECT_EQ(View(req.url), "http://localhost:8080/items/1");
  EXPECT_EQ(HeaderOf(req, "X-A"), "1");
  EXPECT_EQ(HeaderOf(req, "X-B"), "2");
  EXPECT_EQ(HeaderOf(req, "User-Agent"), "zalie-agent");
  EXPECT_EQ(HeaderOf(req, "Referer"), "https://ref");
  EXPECT_EQ(req.auth.mode, AuthMode::basic);
  EXPECT_EQ(View(req.auth.username), "alice");
  EXPECT_EQ(View(req.auth.password), "s3cr3t");
  EXPECT_FALSE(req.settings.verifyTls);
  EXPECT_TRUE(req.settings.followRedirects);
  EXPECT_EQ(req.settings.timeoutMs, 2500U);
  EXPECT_EQ(req.settings.httpVersion, HttpVersion::http1);
}

TEST(CurlImport, FormDataIsDecodedIntoFields) {
  auto imported = ParseCurlCommand("curl http://h/login -d 'user=zalie&msg=hello%20world' -d 'x=a+b'");
  ASSERT_TRUE(imported.has_value());
  const Request& req = imported->request;
  EXPECT_EQ(req.method, Method::post);
  EXPECT_EQ(req.bodyMode, BodyMode::formUrlEncoded);
  ASSERT_EQ(req.form.size(), 3U);
  EXPECT_EQ(View(req.form[1].value), "hello world");
  EXPECT_EQ(View(req.form[2].value), "a b");
}

TEST(CurlImport, DataUrlencode) {
  auto imported = ParseCurlCommand("curl http://h --data-urlencode 'q=a b&c' --data-urlencode '=raw value'");
  ASSERT_TRUE(imported.has_value());
  const Request& req = imported->request;
  ASSERT_EQ(req.form.size(), 2U);
  EXPECT_EQ(View(req.form[0].key), "q");
  EXPECT_EQ(View(req.form[0].value), "a b&c");
  EXPECT_EQ(View(req.form[1].key), "raw value");
}

TEST(CurlImport, JsonOptionAndExplicitTextType) {
  auto imported = ParseCurlCommand(R"(curl --json '{"a":1}' https://h/j)");
  ASSERT_TRUE(imported.has_value());
  EXPECT_EQ(imported->request.bodyMode, BodyMode::json);
  EXPECT_EQ(HeaderOf(imported->request, "Accept"), "application/json");

  imported = ParseCurlCommand("curl -H 'Content-Type: text/csv' --data-binary 'a,b' https://h/csv");
  ASSERT_TRUE(imported.has_value());
  EXPECT_EQ(imported->request.bodyMode, BodyMode::text);
  EXPECT_EQ(View(imported->request.body), "a,b");

  imported = ParseCurlCommand("curl -H 'Content-Type: application/xml' -d '<a/>' https://h/x");
  ASSERT_TRUE(imported.has_value());
  EXPECT_EQ(imported->request.bodyMode, BodyMode::xml);
}

TEST(CurlImport, GetModeAndHead) {
  auto imported = ParseCurlCommand("curl -G -d q=zalie -d lang=fr 'https://h/search?x=1'");
  ASSERT_TRUE(imported.has_value());
  EXPECT_EQ(imported->request.method, Method::get);
  EXPECT_EQ(View(imported->request.url), "https://h/search?x=1&q=zalie&lang=fr");
  EXPECT_EQ(imported->request.params.size(), 3U);
  EXPECT_EQ(imported->request.bodyMode, BodyMode::none);

  imported = ParseCurlCommand("curl -I https://h/");
  ASSERT_TRUE(imported.has_value());
  EXPECT_EQ(imported->request.method, Method::head);
}

TEST(CurlImport, BearerAndWarnings) {
  auto imported = ParseCurlCommand(
      "curl --oauth2-bearer tok -x http://proxy:3128 --frobnicate -F file=@a.txt -b cookies.txt https://h/ extra");
  ASSERT_TRUE(imported.has_value());
  EXPECT_EQ(imported->request.auth.mode, AuthMode::bearer);
  EXPECT_EQ(View(imported->request.auth.token), "tok");
  EXPECT_EQ(imported->warnings.size(), 5U);  // proxy, unknown option, multipart, cookie file, extra argument
}

TEST(CurlImport, Errors) {
  EXPECT_FALSE(ParseCurlCommand("wget https://h").has_value());
  EXPECT_FALSE(ParseCurlCommand("curl -X").has_value());
  EXPECT_FALSE(ParseCurlCommand("curl -H 'a: b'").has_value());  // no URL
  EXPECT_FALSE(ParseCurlCommand("curl 'oops").has_value());
  EXPECT_FALSE(ParseCurlCommand("").has_value());
}

}  // namespace
}  // namespace zalie
