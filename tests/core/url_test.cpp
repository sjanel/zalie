#include "zalie/url.hpp"

#include <gtest/gtest.h>

#include <string_view>

#include "zalie/key-value.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/request.hpp"

namespace zalie {
namespace {

TEST(Url, SplitUrlQuery) {
  UrlQuerySplit split = SplitUrlQuery("https://host/path?a=1&b=2#frag");
  EXPECT_EQ(split.base, "https://host/path");
  EXPECT_EQ(split.query, "a=1&b=2");
  EXPECT_EQ(split.fragment, "frag");
  EXPECT_TRUE(split.hasQuery);

  split = SplitUrlQuery("{{baseUrl}}/users");
  EXPECT_EQ(split.base, "{{baseUrl}}/users");
  EXPECT_FALSE(split.hasQuery);
  EXPECT_TRUE(split.query.empty());

  split = SplitUrlQuery("http://h/p?");
  EXPECT_TRUE(split.hasQuery);
  EXPECT_TRUE(split.query.empty());

  // A '?' after the '#' belongs to the fragment.
  split = SplitUrlQuery("http://h/p#x?y");
  EXPECT_EQ(split.base, "http://h/p");
  EXPECT_FALSE(split.hasQuery);
  EXPECT_EQ(split.fragment, "x?y");
}

TEST(Url, ParseQuery) {
  KeyValues params;
  ParseQuery("a=1&flag&&b=x%20y&c=", params);
  ASSERT_EQ(params.size(), 4U);
  EXPECT_EQ(View(params[0].key), "a");
  EXPECT_EQ(View(params[0].value), "1");
  EXPECT_EQ(View(params[1].key), "flag");
  EXPECT_TRUE(params[1].value.empty());
  EXPECT_EQ(View(params[2].value), "x%20y");  // raw, not decoded
  EXPECT_EQ(View(params[3].key), "c");
  EXPECT_TRUE(params[3].value.empty());
  for (const KeyValue& param : params) {
    EXPECT_TRUE(param.enabled);
  }
}

TEST(Url, ComposeUrlKeepsFragmentAndSkipsDisabled) {
  KeyValues params;
  params.push_back(MakeKeyValue("a", "1"));
  params.push_back(MakeKeyValue("off", "x", false));
  params.push_back(MakeKeyValue("flag", ""));
  EXPECT_EQ(View(ComposeUrl("http://h/p?old=1#top", params)), "http://h/p?a=1&flag#top");
  EXPECT_EQ(View(ComposeUrl("http://h/p", {})), "http://h/p");
  EXPECT_EQ(View(ComposeUrl("http://h/p?x=1", {})), "http://h/p");
}

TEST(Url, TwoWaySync) {
  Request req;
  req.url.assign(std::string_view{"{{base}}/search?q=zalie&page=2"});
  req.params.push_back(MakeKeyValue("debug", "1", false));
  SyncParamsFromUrl(req);
  ASSERT_EQ(req.params.size(), 3U);
  EXPECT_EQ(View(req.params[0].key), "q");
  EXPECT_EQ(View(req.params[1].key), "page");
  EXPECT_EQ(View(req.params[2].key), "debug");  // disabled params are kept
  EXPECT_FALSE(req.params[2].enabled);

  req.params[1].value.assign(std::string_view{"3"});
  req.params[2].enabled = true;
  SyncUrlFromParams(req);
  EXPECT_EQ(View(req.url), "{{base}}/search?q=zalie&page=3&debug=1");
}

TEST(Url, AppendUrlEncoded) {
  RawChars32 out;
  AppendUrlEncoded("a b&c=d/é~._-", out);
  EXPECT_EQ(View(out), "a%20b%26c%3Dd%2F%C3%A9~._-");
  out.clear();
  AppendUrlEncoded("a b+c", out, true);
  EXPECT_EQ(View(out), "a+b%2Bc");
}

TEST(Url, UrlDecode) {
  EXPECT_EQ(View(UrlDecode("a%20b%2Fc")), "a b/c");
  EXPECT_EQ(View(UrlDecode("a+b", true)), "a b");
  EXPECT_EQ(View(UrlDecode("a+b", false)), "a+b");
  EXPECT_EQ(View(UrlDecode("100%")), "100%");    // truncated sequence kept
  EXPECT_EQ(View(UrlDecode("%zz%4")), "%zz%4");  // invalid sequences kept
  EXPECT_EQ(View(UrlDecode("%C3%A9")), "é");
}

TEST(Url, HasScheme) {
  EXPECT_TRUE(HasScheme("http://x"));
  EXPECT_TRUE(HasScheme("HTTPS://x"));
  EXPECT_TRUE(HasScheme("svn+ssh://x"));
  EXPECT_FALSE(HasScheme("localhost:8080/x"));
  EXPECT_FALSE(HasScheme("{{base}}/x"));
  EXPECT_FALSE(HasScheme("://x"));
  EXPECT_FALSE(HasScheme("1http://x"));
}

TEST(Url, SplitUrlTarget) {
  UrlTarget target = SplitUrlTarget("https://api.example.com:8443/v1/users?id=3#x");
  EXPECT_EQ(target.scheme, "https");
  EXPECT_EQ(target.authority, "api.example.com:8443");
  EXPECT_EQ(target.target, "/v1/users?id=3");

  target = SplitUrlTarget("http://host");
  EXPECT_EQ(target.authority, "host");
  EXPECT_EQ(target.target, "/");

  target = SplitUrlTarget("http://host?x=1");
  EXPECT_EQ(target.authority, "host");
  EXPECT_EQ(target.target, "?x=1");

  target = SplitUrlTarget("host/p");
  EXPECT_TRUE(target.scheme.empty());
  EXPECT_EQ(target.authority, "host");
  EXPECT_EQ(target.target, "/p");
}

TEST(Url, UrlHost) {
  EXPECT_EQ(UrlHost("https://api.example.com:8443/v1?x=1"), "api.example.com");
  EXPECT_EQ(UrlHost("http://user:secret@db.local:5984/x"), "db.local");
  EXPECT_EQ(UrlHost("http://[::1]:8080/"), "::1");
  EXPECT_EQ(UrlHost("http://[fe80::1"), "fe80::1");
  EXPECT_EQ(UrlHost("localhost:3000/health"), "localhost");
  EXPECT_EQ(UrlHost("http://h?q=a@b"), "h");
  EXPECT_EQ(UrlHost(""), "");
}

TEST(Url, IsLocalHost) {
  for (const char* host : {"localhost", "LocalHost", "api.localhost", "127.0.0.1", "127.1.2.3", "::1"}) {
    EXPECT_TRUE(IsLocalHost(host)) << host;
  }
  for (const char* host : {"", "example.com", "localhost.example.com", "notlocalhost", ".localhost", "127.0.0",
                           "127.0.0.1.", "127.0.0.1x", "128.0.0.1", "10.0.0.1", "::2", "0.0.0.0"}) {
    EXPECT_FALSE(IsLocalHost(host)) << host;
  }
}

}  // namespace
}  // namespace zalie
