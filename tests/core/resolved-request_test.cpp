#include "zalie/resolved-request.hpp"

#include <gtest/gtest.h>

#include <string_view>
#include <utility>

#include "zalie/collection.hpp"
#include "zalie/key-value.hpp"
#include "zalie/method.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/request.hpp"
#include "zalie/variables.hpp"

namespace zalie {
namespace {

class ResolveRequestTest : public ::testing::Test {
 protected:
  void SetUp() override {
    collection.variables.push_back(MakeKeyValue("host", "api.example.com"));
    collection.variables.push_back(MakeKeyValue("token", "collection-token"));
    Environment staging;
    staging.name.assign(std::string_view{"staging"});
    staging.variables.push_back(MakeKeyValue("host", "staging.example.com"));
    collection.environments.push_back(std::move(staging));
  }

  ResolvedRequest resolve(int environment = -1) {
    const VariableScope scope = MakeScope(collection, environment);
    return ResolveRequest(request, collection, scope);
  }

  Collection collection;
  Request request;
};

TEST_F(ResolveRequestTest, UrlVariablesAndEnvironment) {
  request.url.assign(std::string_view{"  https://{{host}}/users?id=1  "});
  EXPECT_EQ(resolve().url, "https://api.example.com/users?id=1");
  EXPECT_EQ(resolve(0).url, "https://staging.example.com/users?id=1");
  EXPECT_EQ(resolve(7).url, "https://api.example.com/users?id=1");  // invalid index: no environment
}

TEST_F(ResolveRequestTest, DefaultSchemeIsHttp) {
  request.url.assign(std::string_view{"localhost:8080/health"});
  EXPECT_EQ(resolve().url, "http://localhost:8080/health");
  request.url.assign(std::string_view{"{{host}}/x"});
  EXPECT_EQ(resolve().url, "http://api.example.com/x");
  request.url.clear();
  EXPECT_TRUE(resolve().url.empty());
}

TEST_F(ResolveRequestTest, HeadersMergeAndOverride) {
  collection.headers.push_back(MakeKeyValue("X-Client", "zalie"));
  collection.headers.push_back(MakeKeyValue("Accept", "*/*"));
  request.headers.push_back(MakeKeyValue("accept", "application/json"));
  request.headers.push_back(MakeKeyValue("X-Off", "1", false));
  request.headers.push_back(MakeKeyValue("  ", "blank name"));
  request.headers.push_back(MakeKeyValue("X-Host", "{{host}}"));
  const ResolvedRequest resolved = resolve();
  ASSERT_EQ(resolved.headers.size(), 3U);
  EXPECT_EQ(resolved.headers[0], (HeaderView{"X-Client", "zalie"}));
  EXPECT_EQ(resolved.headers[1], (HeaderView{"Accept", "application/json"}));  // collection position, request value
  EXPECT_EQ(resolved.headers[2], (HeaderView{"X-Host", "api.example.com"}));
  EXPECT_EQ(resolved.headerValue("ACCEPT"), "application/json");
  EXPECT_TRUE(resolved.headerValue("X-Off").empty());
}

TEST_F(ResolveRequestTest, BodyModesAndContentType) {
  request.method = Method::post;
  request.bodyMode = BodyMode::json;
  request.body.assign(std::string_view{R"({"host": "{{host}}"})"});
  ResolvedRequest resolved = resolve();
  EXPECT_EQ(View(resolved.body), R"({"host": "api.example.com"})");
  EXPECT_EQ(resolved.headerValue("Content-Type"), "application/json");

  request.headers.push_back(MakeKeyValue("Content-Type", "application/vnd.api+json"));
  resolved = resolve();
  EXPECT_EQ(resolved.headerValue("content-type"), "application/vnd.api+json");
  EXPECT_EQ(resolved.headers.size(), 1U);

  request.headers.clear();
  request.bodyMode = BodyMode::xml;
  EXPECT_EQ(resolve().headerValue("Content-Type"), "application/xml");
  request.bodyMode = BodyMode::text;
  EXPECT_EQ(resolve().headerValue("Content-Type"), "text/plain");

  request.bodyMode = BodyMode::none;
  resolved = resolve();
  EXPECT_TRUE(resolved.body.empty());
  EXPECT_TRUE(resolved.headers.empty());
}

TEST_F(ResolveRequestTest, FormUrlEncoded) {
  request.method = Method::post;
  request.bodyMode = BodyMode::formUrlEncoded;
  request.form.push_back(MakeKeyValue("name", "Zalie la chienne"));
  request.form.push_back(MakeKeyValue("skip", "x", false));
  request.form.push_back(MakeKeyValue("host", "{{host}}"));
  request.form.push_back(MakeKeyValue("sym", "a&b=c"));
  const ResolvedRequest resolved = resolve();
  EXPECT_EQ(View(resolved.body), "name=Zalie+la+chienne&host=api.example.com&sym=a%26b%3Dc");
  EXPECT_EQ(resolved.headerValue("Content-Type"), "application/x-www-form-urlencoded");
}

TEST_F(ResolveRequestTest, BasicAuth) {
  request.auth.mode = AuthMode::basic;
  request.auth.username.assign(std::string_view{"Aladdin"});
  request.auth.password.assign(std::string_view{"open sesame"});
  EXPECT_EQ(resolve().headerValue("Authorization"), "Basic QWxhZGRpbjpvcGVuIHNlc2FtZQ==");
}

TEST_F(ResolveRequestTest, BearerAuthAndInheritance) {
  collection.auth.mode = AuthMode::bearer;
  collection.auth.token.assign(std::string_view{"{{token}}"});
  request.auth.mode = AuthMode::inherit;
  EXPECT_EQ(resolve().headerValue("Authorization"), "Bearer collection-token");

  request.auth.mode = AuthMode::bearer;
  request.auth.token.assign(std::string_view{"own"});
  EXPECT_EQ(resolve().headerValue("Authorization"), "Bearer own");

  request.auth.mode = AuthMode::none;
  EXPECT_TRUE(resolve().headerValue("Authorization").empty());

  // An explicit Authorization header wins over the auth settings.
  request.auth.mode = AuthMode::bearer;
  request.headers.push_back(MakeKeyValue("Authorization", "Custom abc"));
  const ResolvedRequest resolved = resolve();
  EXPECT_EQ(resolved.headerValue("Authorization"), "Custom abc");
  EXPECT_EQ(resolved.headers.size(), 1U);
}

TEST_F(ResolveRequestTest, ApiKeyHeaderAndQuery) {
  request.url.assign(std::string_view{"https://h/p?x=1#frag"});
  request.auth.mode = AuthMode::apiKey;
  request.auth.apiKeyName.assign(std::string_view{"X-Api-Key"});
  request.auth.apiKeyValue.assign(std::string_view{"s3cr3t"});
  ResolvedRequest resolved = resolve();
  EXPECT_EQ(resolved.headerValue("x-api-key"), "s3cr3t");
  EXPECT_EQ(resolved.url, "https://h/p?x=1#frag");

  request.auth.apiKeyInQuery = true;
  request.auth.apiKeyName.assign(std::string_view{"api key"});
  resolved = resolve();
  EXPECT_EQ(resolved.url, "https://h/p?x=1&api%20key=s3cr3t#frag");
  EXPECT_TRUE(resolved.headerValue("x-api-key").empty());

  request.url.assign(std::string_view{"https://h/p"});
  EXPECT_EQ(resolve().url, "https://h/p?api%20key=s3cr3t");

  request.auth.apiKeyName.clear();
  EXPECT_EQ(resolve().url, "https://h/p");
}

TEST_F(ResolveRequestTest, UnresolvedVariablesOutliveTheModel) {
  request.url.assign(std::string_view{"https://{{missing}}/{{host}}/{{missing}}"});
  request.headers.push_back(MakeKeyValue("X-{{other}}", "v"));
  ResolvedRequest resolved = resolve();
  request = Request{};  // the views of the resolved request must not depend on the model
  collection = Collection{};
  ASSERT_EQ(resolved.unresolved.size(), 2U);
  EXPECT_EQ(resolved.unresolved[0], "missing");
  EXPECT_EQ(resolved.unresolved[1], "other");
  EXPECT_EQ(resolved.url, "https://{{missing}}/api.example.com/{{missing}}");
  EXPECT_EQ(resolved.headers[0].name, "X-{{other}}");

  // Moving keeps the views valid (the strings live in the pool blocks, not in the object).
  const ResolvedRequest moved = std::move(resolved);
  EXPECT_EQ(moved.url, "https://{{missing}}/api.example.com/{{missing}}");
}

TEST_F(ResolveRequestTest, SettingsAndMethodAreCopied) {
  request.method = Method::del;
  request.settings.timeoutMs = 1234;
  request.settings.verifyTls = false;
  const ResolvedRequest resolved = resolve();
  EXPECT_EQ(resolved.method, Method::del);
  EXPECT_EQ(resolved.settings, request.settings);
}

}  // namespace
}  // namespace zalie
