#include "zalie/snippets.hpp"

#include <gtest/gtest.h>

#include <string_view>

#include "zalie/method.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/request.hpp"
#include "zalie/resolved-request.hpp"

namespace zalie {
namespace {

ResolvedRequest MakePost() {
  ResolvedRequest req;
  req.method = Method::post;
  req.url = "https://api.example.com/users?team=a&b=1";
  req.headers.push_back({"Content-Type", "application/json"});
  req.headers.push_back({"X-Note", "it's"});
  req.body.assign(std::string_view{R"({"name": "Zalie"})"});
  return req;
}

ResolvedRequest MakeGet() {
  ResolvedRequest req;
  req.url = "https://api.example.com/health";
  return req;
}

std::string_view Generate(SnippetLanguage language, const ResolvedRequest& req) {
  static RawChars32 out;
  GenerateSnippet(language, req, out);
  return out;
}

TEST(Snippets, LanguageIds) {
  for (SnippetLanguage language : kSnippetLanguages) {
    EXPECT_EQ(ParseSnippetLanguage(SnippetLanguageId(language)), language);
    EXPECT_FALSE(SnippetLanguageName(language).empty());
  }
  EXPECT_FALSE(ParseSnippetLanguage("cobol").has_value());
}

TEST(Snippets, Curl) {
  EXPECT_EQ(Generate(SnippetLanguage::curl, MakePost()),
            "curl -X POST 'https://api.example.com/users?team=a&b=1' \\\n"
            "  -H 'Content-Type: application/json' \\\n"
            "  -H 'X-Note: it'\\''s' \\\n"
            "  --data-raw '{\"name\": \"Zalie\"}' \\\n"
            "  -L\n");

  ResolvedRequest get = MakeGet();
  get.settings.followRedirects = false;
  get.settings.verifyTls = false;
  get.settings.httpVersion = HttpVersion::http2;
  get.settings.timeoutMs = 2500;
  EXPECT_EQ(Generate(SnippetLanguage::curl, get),
            "curl 'https://api.example.com/health' \\\n  -k \\\n  --http2 \\\n  --max-time 2.5\n");

  ResolvedRequest head = MakeGet();
  head.method = Method::head;
  head.url = "http://h/";
  head.settings.httpVersion = HttpVersion::http2;
  EXPECT_EQ(Generate(SnippetLanguage::curl, head), "curl --head 'http://h/' \\\n  -L \\\n  --http2-prior-knowledge\n");
}

TEST(Snippets, Httpie) {
  ResolvedRequest req = MakePost();
  req.headers.push_back({"X-Empty", ""});
  EXPECT_EQ(Generate(SnippetLanguage::httpie, req),
            "http --follow POST 'https://api.example.com/users?team=a&b=1' \\\n"
            "  'Content-Type:application/json' \\\n"
            "  'X-Note:it'\\''s' \\\n"
            "  'X-Empty;' \\\n"
            "  --raw '{\"name\": \"Zalie\"}'\n");
}

TEST(Snippets, Wget) {
  ResolvedRequest req = MakeGet();
  req.settings.followRedirects = false;
  req.settings.timeoutMs = 5000;
  EXPECT_EQ(Generate(SnippetLanguage::wget, req),
            "wget --quiet \\\n  --method=GET \\\n  --max-redirect=0 \\\n  --timeout=5 \\\n"
            "  --output-document=- \\\n  'https://api.example.com/health'\n");
}

TEST(Snippets, PowerShell) {
  const std::string_view snippet = Generate(SnippetLanguage::powershell, MakePost());
  EXPECT_EQ(snippet,
            "$headers = @{\n"
            "    'X-Note' = 'it''s'\n"
            "}\n"
            "$body = '{\"name\": \"Zalie\"}'\n"
            "\n"
            "$response = Invoke-WebRequest -Uri 'https://api.example.com/users?team=a&b=1' -Method 'POST' "
            "-Headers $headers -ContentType 'application/json' -Body $body\n"
            "$response.StatusCode\n$response.Content\n");
  ResolvedRequest connect = MakeGet();
  connect.method = Method::connect;
  connect.settings.verifyTls = false;
  EXPECT_TRUE(Generate(SnippetLanguage::powershell, connect).contains("-CustomMethod 'CONNECT' -SkipCertificateCheck"));
}

TEST(Snippets, Python) {
  EXPECT_EQ(Generate(SnippetLanguage::python, MakePost()),
            "import requests\n\n"
            "url = \"https://api.example.com/users?team=a&b=1\"\n"
            "headers = {\n"
            "    \"Content-Type\": \"application/json\",\n"
            "    \"X-Note\": \"it's\",\n"
            "}\n"
            "payload = \"{\\\"name\\\": \\\"Zalie\\\"}\"\n"
            "\n"
            "response = requests.request(\"POST\", url, headers=headers, data=payload, timeout=30)\n"
            "\n"
            "print(response.status_code)\nprint(response.text)\n");
  ResolvedRequest utf8 = MakePost();
  utf8.body.assign(std::string_view{"{\"name\": \"Zalie é\"}"});
  utf8.settings.verifyTls = false;
  utf8.settings.followRedirects = false;
  const std::string_view snippet = Generate(SnippetLanguage::python, utf8);
  EXPECT_TRUE(snippet.contains("data=payload.encode(\"utf-8\")"));
  EXPECT_TRUE(snippet.contains("allow_redirects=False, verify=False"));
}

TEST(Snippets, JavaScript) {
  EXPECT_EQ(Generate(SnippetLanguage::javascript, MakeGet()),
            "const response = await fetch(\"https://api.example.com/health\");\n\n"
            "console.log(response.status);\nconsole.log(await response.text());\n");
  ResolvedRequest req = MakePost();
  req.settings.followRedirects = false;
  req.settings.timeoutMs = 1000;
  const std::string_view snippet = Generate(SnippetLanguage::javascript, req);
  EXPECT_TRUE(snippet.contains("  method: \"POST\",\n"));
  EXPECT_TRUE(snippet.contains("    \"X-Note\": \"it's\",\n"));
  EXPECT_TRUE(snippet.contains("  body: \"{\\\"name\\\": \\\"Zalie\\\"}\",\n"));
  EXPECT_TRUE(snippet.contains("  redirect: \"manual\",\n"));
  EXPECT_TRUE(snippet.contains("  signal: AbortSignal.timeout(1000),\n"));
}

TEST(Snippets, Go) {
  const std::string_view post = Generate(SnippetLanguage::go, MakePost());
  EXPECT_TRUE(
      post.starts_with("package main\n\nimport (\n\t\"fmt\"\n\t\"io\"\n\t\"net/http\"\n\t\"strings\"\n\t\"time\"\n)"));
  EXPECT_TRUE(post.contains("payload := strings.NewReader(\"{\\\"name\\\": \\\"Zalie\\\"}\")"));
  EXPECT_TRUE(post.contains("http.NewRequest(\"POST\", \"https://api.example.com/users?team=a&b=1\", payload)"));
  EXPECT_TRUE(post.contains("req.Header.Add(\"X-Note\", \"it's\")"));
  EXPECT_TRUE(post.contains("Timeout: 30 * time.Second,"));

  ResolvedRequest get = MakeGet();
  get.settings.verifyTls = false;
  get.settings.followRedirects = false;
  get.settings.timeoutMs = 1500;
  const std::string_view snippet = Generate(SnippetLanguage::go, get);
  EXPECT_TRUE(snippet.contains("\t\"crypto/tls\"\n"));
  EXPECT_FALSE(snippet.contains("strings"));
  EXPECT_TRUE(snippet.contains(", nil)"));
  EXPECT_TRUE(snippet.contains("Timeout: 1500 * time.Millisecond,"));
  EXPECT_TRUE(snippet.contains("http.ErrUseLastResponse"));
  EXPECT_TRUE(snippet.contains("InsecureSkipVerify: true"));
}

TEST(Snippets, CppAeronet) {
  ResolvedRequest req = MakePost();
  req.headers.push_back({"Host", "other"});
  const std::string_view snippet = Generate(SnippetLanguage::cppAeronet, req);
  EXPECT_TRUE(snippet.contains("  aeronet::HttpClient client;\n"));
  EXPECT_TRUE(snippet.contains(
      "client.makeRequest(aeronet::http::Method::POST, \"https://api.example.com/users?team=a&b=1\");"));
  EXPECT_TRUE(snippet.contains("request.headerAddLine(\"x-note\", \"it's\");"));  // lower-case names
  EXPECT_FALSE(snippet.contains("headerAddLine(\"content-type\""));
  EXPECT_TRUE(snippet.contains("// 'Host' is managed by the client"));
  EXPECT_TRUE(snippet.contains(R"(request.body(R"zalie({"name": "Zalie"})zalie", "application/json");)"));

  // A body containing the default delimiter picks another one; control characters force an escaped literal.
  req.body.assign(std::string_view{"a)zalie\"b"});
  EXPECT_TRUE(Generate(SnippetLanguage::cppAeronet, req).contains("R\"zalie0(a)zalie\"b)zalie0\""));
  req.body.assign(std::string_view{"a\x01z"});
  EXPECT_TRUE(Generate(SnippetLanguage::cppAeronet, req).contains("request.body(\"a\\001z\""));

  req.settings.followRedirects = false;
  req.settings.verifyTls = false;
  req.settings.timeoutMs = 100;
  req.settings.httpVersion = HttpVersion::http1;
  req.settings.decompress = false;
  const std::string_view custom = Generate(SnippetLanguage::cppAeronet, req);
  EXPECT_TRUE(custom.contains("#include <chrono>\n"));
  EXPECT_TRUE(custom.contains("config.requestTimeout = std::chrono::milliseconds{100};"));
  EXPECT_TRUE(custom.contains("config.followRedirects = false;"));
  EXPECT_TRUE(custom.contains("config.tlsVerifyPeer = false;"));
  EXPECT_TRUE(custom.contains("aeronet::HttpVersionMode::Http1_1"));
  EXPECT_TRUE(custom.contains("config.withDecompression(false);"));
  EXPECT_TRUE(custom.contains("aeronet::HttpClient client(std::move(config));"));
}

TEST(Snippets, RawHttp) {
  EXPECT_EQ(Generate(SnippetLanguage::rawHttp, MakePost()),
            "POST /users?team=a&b=1 HTTP/1.1\r\n"
            "Host: api.example.com\r\n"
            "Content-Type: application/json\r\n"
            "X-Note: it's\r\n"
            "Content-Length: 17\r\n"
            "\r\n"
            "{\"name\": \"Zalie\"}");
  ResolvedRequest get = MakeGet();
  get.url = "http://h:81?x=1";
  EXPECT_EQ(Generate(SnippetLanguage::rawHttp, get), "GET /?x=1 HTTP/1.1\r\nHost: h:81\r\n\r\n");
}

TEST(Snippets, NonDefaultSettings) {
  ResolvedRequest req = MakePost();
  req.settings.verifyTls = false;
  req.settings.followRedirects = false;
  req.settings.timeoutMs = 2500;
  req.settings.httpVersion = HttpVersion::http2;
  const std::string_view httpie = Generate(SnippetLanguage::httpie, req);
  EXPECT_TRUE(httpie.contains(" --verify=no")) << httpie;
  EXPECT_TRUE(httpie.contains(" --timeout=")) << httpie;
  const std::string_view wget = Generate(SnippetLanguage::wget, req);
  for (std::string_view option :
       {"--header='X-Note: it'\\''s'", "--body-data=", "--max-redirect=0", "--no-check-certificate", "--timeout="}) {
    EXPECT_TRUE(wget.contains(option)) << option << " in " << wget;
  }
  const std::string_view powershell = Generate(SnippetLanguage::powershell, req);
  for (std::string_view option : {" -MaximumRedirection 0", " -SkipCertificateCheck", " -TimeoutSec 3"}) {
    EXPECT_TRUE(powershell.contains(option)) << option << " in " << powershell;
  }
  EXPECT_TRUE(Generate(SnippetLanguage::javascript, req).contains("NODE_TLS_REJECT_UNAUTHORIZED=0"));
  EXPECT_TRUE(Generate(SnippetLanguage::cppAeronet, req).contains("aeronet::HttpVersionMode::Http2"));
  // Raw HTTP writes its own Host and Content-Length.
  req.headers.push_back({"Host", "other.example.com"});
  req.headers.push_back({"Content-Length", "99"});
  const std::string_view raw = Generate(SnippetLanguage::rawHttp, req);
  EXPECT_FALSE(raw.contains("other.example.com")) << raw;
  EXPECT_FALSE(raw.contains("99")) << raw;
}

}  // namespace
}  // namespace zalie
