#include "zalie/load-plan.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include "zalie/collection.hpp"
#include "zalie/data-file.hpp"
#include "zalie/key-value.hpp"
#include "zalie/load-runner.hpp"
#include "zalie/load-settings.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/request.hpp"
#include "zalie/resolved-request.hpp"
#include "zalie/variables.hpp"

namespace zalie {
namespace {

LoadParameter ListParameter(std::string_view name, std::string_view values) {
  LoadParameter parameter;
  parameter.name.assign(name);
  parameter.source = ValueSource::list;
  parameter.listValues.assign(values);
  return parameter;
}

LoadParameter FixedParameter(std::string_view name, std::string_view value) {
  LoadParameter parameter;
  parameter.name.assign(name);
  parameter.source = ValueSource::fixed;
  parameter.fixedValue.assign(value);
  return parameter;
}

class LoadPlanTest : public ::testing::Test {
 protected:
  void SetUp() override {
    _settings.seed = 42;
    _request.url.assign(std::string_view{"http://example.com/items"});
  }

  [[nodiscard]] LoadPlan compile() {
    const VariableScope scope = MakeScope(_collection, -1);
    return LoadPlan(_request, _collection, scope, _settings, std::filesystem::temp_directory_path(), _cache);
  }

  // The request 'number' of a valid plan.
  [[nodiscard]] ResolvedRequest filled(std::uint64_t number = 0) {
    const LoadPlan plan = compile();
    EXPECT_TRUE(plan.valid()) << (plan.valid() ? std::string_view{} : View(plan.issues()[0].message));
    auto preview = plan.preview(number);
    EXPECT_TRUE(preview.has_value()) << (preview.has_value() ? std::string_view{} : View(preview.error()));
    return std::move(*preview);
  }

  void setBody(BodyMode mode, std::string_view body) {
    _request.bodyMode = mode;
    _request.body.assign(body);
  }

  void addVariable(std::string_view name, std::string_view value) {
    _collection.variables.push_back(MakeKeyValue(name, value));
  }

  Collection _collection;
  Request _request;
  LoadSettings _settings;
  DataFileIndexCache _cache;
};

TEST_F(LoadPlanTest, ARequestWithoutGeneratedValueIsConstant) {
  const LoadPlan plan = compile();
  EXPECT_TRUE(plan.valid());
  EXPECT_TRUE(plan.isConstant());
  EXPECT_EQ(plan.url().literal(), "http://example.com/items");
  EXPECT_EQ(plan.passLength(), 0U);
}

TEST_F(LoadPlanTest, ValuesAreEncodedInThePathAndTheQuery) {
  _request.url.assign(std::string_view{"http://example.com/items/{{term}}?q={{term}}&x=1"});
  _settings.parameters.push_back(ListParameter("term", "a b/c&d"));
  EXPECT_EQ(filled().url, "http://example.com/items/a%20b%2Fc%26d?q=a%20b%2Fc%26d&x=1");
}

TEST_F(LoadPlanTest, AValueInTheAuthorityIsWrittenAsItIs) {
  _request.url.assign(std::string_view{"http://{{host}}/items?q={{host}}"});
  _settings.parameters.push_back(ListParameter("host", "a.local:8080"));
  EXPECT_EQ(filled().url, "http://a.local:8080/items?q=a.local%3A8080");
}

TEST_F(LoadPlanTest, AValueCanBeAWholeUrl) {
  _request.url.assign(std::string_view{"{{target}}"});
  _settings.parameters.push_back(ListParameter("target", "https://a.example/x?y=1\nb.example/z"));
  EXPECT_EQ(filled(0).url, "https://a.example/x?y=1");
  EXPECT_EQ(filled(1).url, "http://b.example/z");  // no scheme: like any URL of Zalie
}

TEST_F(LoadPlanTest, ALineBreakInTheAuthorityIsRefused) {
  _request.url.assign(std::string_view{"http://{{host}}/items"});
  LoadParameter parameter;  // a list cannot hold a line break: it splits on them
  parameter.name.assign(std::string_view{"host"});
  parameter.source = ValueSource::randomString;
  parameter.randomLength = 3;
  parameter.randomCharacters.assign(std::string_view{"\n"});
  _settings.parameters.push_back(std::move(parameter));
  const LoadPlan plan = compile();
  ASSERT_TRUE(plan.valid());
  EXPECT_FALSE(plan.preview(0).has_value());
}

TEST_F(LoadPlanTest, FormFieldsAreFormEncoded) {
  _request.method = Method::post;
  _request.bodyMode = BodyMode::formUrlEncoded;
  _request.form.push_back(MakeKeyValue("q", "{{term}}"));
  _settings.parameters.push_back(ListParameter("term", "a b&c"));
  EXPECT_EQ(View(filled().body), "q=a+b%26c");
}

TEST_F(LoadPlanTest, JsonValuesAreEscapedOnlyInsideStrings) {
  _request.method = Method::post;
  setBody(BodyMode::json, R"({"q": "{{term}}", "page": {{page}}, "note": "\"{{page}}"})");
  _settings.parameters.push_back(ListParameter("term", "say \"hi\"\\"));
  _settings.parameters.push_back(ListParameter("page", "3"));
  EXPECT_EQ(View(filled().body), R"({"q": "say \"hi\"\\", "page": 3, "note": "\"3"})");
}

TEST_F(LoadPlanTest, JsonEscapesControlCharacters) {
  _request.method = Method::post;
  setBody(BodyMode::json, R"({"q": "{{term}}"})");
  LoadParameter parameter;
  parameter.name.assign(std::string_view{"term"});
  parameter.source = ValueSource::randomString;
  parameter.randomLength = 1;
  parameter.randomCharacters.assign(std::string_view{"\t"});
  _settings.parameters.push_back(std::move(parameter));
  EXPECT_EQ(View(filled().body), R"({"q": "\t"})");
  _settings.parameters[0].randomCharacters.assign(std::string_view{"\x01"});
  EXPECT_EQ(View(filled().body), R"({"q": "\u0001"})");
}

TEST_F(LoadPlanTest, TextBodiesAreRaw) {
  _request.method = Method::post;
  setBody(BodyMode::xml, "<a>{{term}}</a>");
  _settings.parameters.push_back(ListParameter("term", "<b>&"));
  EXPECT_EQ(View(filled().body), "<a><b>&</a>");
}

TEST_F(LoadPlanTest, ContentTypeOfTheBodyMode) {
  _request.method = Method::post;
  setBody(BodyMode::json, "{}");
  EXPECT_EQ(filled().headerValue("content-type"), "application/json");
}

TEST_F(LoadPlanTest, HeaderValuesAreFilled) {
  _request.headers.push_back(MakeKeyValue("X-Term", "pre-{{term}}"));
  _settings.parameters.push_back(ListParameter("term", "dog\ncat"));
  EXPECT_EQ(filled(0).headerValue("x-term"), "pre-dog");
  EXPECT_EQ(filled(1).headerValue("x-term"), "pre-cat");
}

TEST_F(LoadPlanTest, ALineBreakInAHeaderValueIsRefused) {
  _request.headers.push_back(MakeKeyValue("X-Term", "{{term}}"));
  LoadParameter parameter;
  parameter.name.assign(std::string_view{"term"});
  parameter.source = ValueSource::randomString;
  parameter.randomLength = 2;
  parameter.randomCharacters.assign(std::string_view{"\r"});
  _settings.parameters.push_back(std::move(parameter));
  const LoadPlan plan = compile();
  ASSERT_TRUE(plan.valid());
  EXPECT_FALSE(plan.isConstant());
  const auto preview = plan.preview(0);
  ASSERT_FALSE(preview.has_value());
  EXPECT_TRUE(View(preview.error()).contains("line break"));
}

TEST_F(LoadPlanTest, AGeneratedHeaderNameIsAnIssue) {
  _request.headers.push_back(MakeKeyValue("X-{{term}}", "1"));
  _settings.parameters.push_back(ListParameter("term", "a"));
  const LoadPlan plan = compile();
  ASSERT_FALSE(plan.valid());
  EXPECT_TRUE(View(plan.issues()[0].message).contains("name of a header"));
}

TEST_F(LoadPlanTest, AHeaderNameStartingLikeASentinelIsNotAnIssue) {
  _request.headers.push_back(MakeKeyValue("zalie-trace", "{{term}}"));
  _settings.parameters.push_back(ListParameter("term", "a"));
  EXPECT_EQ(filled().headerValue("zalie-trace"), "a");
}

TEST_F(LoadPlanTest, HeadersTheClientManagesAreReportedOnce) {
  _request.headers.push_back(MakeKeyValue("Connection", "close"));
  _request.headers.push_back(MakeKeyValue("Bad Name", "x"));
  const LoadPlan plan = compile();
  EXPECT_TRUE(plan.valid());
  ASSERT_EQ(plan.warnings().size(), 2U);
  EXPECT_TRUE(View(plan.warnings()[0]).contains("Connection"));
  EXPECT_TRUE(View(plan.warnings()[1]).contains("Bad Name"));
  EXPECT_TRUE(plan.headers().empty());
}

TEST_F(LoadPlanTest, ARequestHeaderReplacesACollectionHeader) {
  _collection.headers.push_back(MakeKeyValue("X-Term", "collection"));
  _request.headers.push_back(MakeKeyValue("x-term", "{{term}}"));
  _settings.parameters.push_back(ListParameter("term", "request"));
  const ResolvedRequest request = filled();
  EXPECT_EQ(request.headerValue("x-term"), "request");
  EXPECT_EQ(request.headers.size(), 1U);
}

TEST_F(LoadPlanTest, BasicCredentialsAreEncodedForEachRequest) {
  _request.auth.mode = AuthMode::basic;
  _request.auth.username.assign(std::string_view{"{{user}}"});
  _request.auth.password.assign(std::string_view{"secret"});
  _settings.parameters.push_back(ListParameter("user", "ann\nbob"));
  EXPECT_EQ(filled(0).headerValue("authorization"), "Basic YW5uOnNlY3JldA==");
  EXPECT_EQ(filled(1).headerValue("authorization"), "Basic Ym9iOnNlY3JldA==");
}

TEST_F(LoadPlanTest, ConstantBasicCredentialsStayConstant) {
  _request.auth.mode = AuthMode::basic;
  _request.auth.username.assign(std::string_view{"ann"});
  _request.auth.password.assign(std::string_view{"secret"});
  const LoadPlan plan = compile();
  EXPECT_TRUE(plan.isConstant());
  EXPECT_EQ(plan.headers()[0].value.literal(), "Basic YW5uOnNlY3JldA==");
}

TEST_F(LoadPlanTest, AnExplicitAuthorizationHeaderWinsOverGeneratedCredentials) {
  _request.headers.push_back(MakeKeyValue("Authorization", "Token abc"));
  _request.auth.mode = AuthMode::basic;
  _request.auth.username.assign(std::string_view{"{{user}}"});
  _settings.parameters.push_back(ListParameter("user", "ann"));
  EXPECT_EQ(filled().headerValue("authorization"), "Token abc");
}

TEST_F(LoadPlanTest, BearerTokenIsFilled) {
  _request.auth.mode = AuthMode::bearer;
  _request.auth.token.assign(std::string_view{"{{token}}"});
  _settings.parameters.push_back(ListParameter("token", "t1\nt2"));
  EXPECT_EQ(filled(1).headerValue("authorization"), "Bearer t2");
}

TEST_F(LoadPlanTest, ApiKeyInTheQueryIsEncoded) {
  _request.auth.mode = AuthMode::apiKey;
  _request.auth.apiKeyName.assign(std::string_view{"key"});
  _request.auth.apiKeyValue.assign(std::string_view{"{{token}}"});
  _request.auth.apiKeyInQuery = true;
  _settings.parameters.push_back(ListParameter("token", "a b"));
  EXPECT_EQ(filled().url, "http://example.com/items?key=a%20b");
}

TEST_F(LoadPlanTest, CollectionAuthenticationIsInherited) {
  _collection.auth.mode = AuthMode::bearer;
  _collection.auth.token.assign(std::string_view{"{{token}}"});
  _settings.parameters.push_back(ListParameter("token", "t1"));
  EXPECT_EQ(filled().headerValue("authorization"), "Bearer t1");
}

TEST_F(LoadPlanTest, AVariedQueryParameterIsARunOnlyOverride) {
  _request.url.assign(std::string_view{"http://example.com/items?a=1&q=old&b=2#top"});
  LoadParameter parameter = ListParameter("q", "new value\nother");
  parameter.queryParameter = true;
  _settings.parameters.push_back(std::move(parameter));
  EXPECT_EQ(filled(0).url, "http://example.com/items?a=1&q=new%20value&b=2#top");
  EXPECT_EQ(filled(1).url, "http://example.com/items?a=1&q=other&b=2#top");
  EXPECT_EQ(View(_request.url), "http://example.com/items?a=1&q=old&b=2#top");  // the request does not change
}

TEST_F(LoadPlanTest, VaryingAParameterWithoutValue) {
  _request.url.assign(std::string_view{"http://example.com/items?flag&x=1"});
  LoadParameter parameter = ListParameter("flag", "on");
  parameter.queryParameter = true;
  _settings.parameters.push_back(std::move(parameter));
  EXPECT_EQ(filled().url, "http://example.com/items?flag=on&x=1");
}

TEST_F(LoadPlanTest, AQueryParameterTheRequestDoesNotHaveIsAppendedForTheRun) {
  LoadParameter added = ListParameter("new key", "1\n2");
  added.queryParameter = true;
  _settings.parameters.push_back(std::move(added));
  EXPECT_EQ(filled(0).url, "http://example.com/items?new%20key=1");
  EXPECT_EQ(filled(1).url, "http://example.com/items?new%20key=2");

  _request.url.assign(std::string_view{"http://example.com/items?a=1#top"});
  EXPECT_EQ(filled(0).url, "http://example.com/items?a=1&new%20key=1#top");
  _request.url.assign(std::string_view{"http://example.com/items?a=1&"});
  EXPECT_EQ(filled(0).url, "http://example.com/items?a=1&new%20key=1");
  _request.url.assign(std::string_view{"http://example.com/items?"});
  EXPECT_EQ(filled(0).url, "http://example.com/items?new%20key=1");

  // A fixed value, inlined like the value of a variable.
  _settings.parameters.clear();
  LoadParameter fixed = FixedParameter("lang", "en");
  fixed.queryParameter = true;
  _settings.parameters.push_back(std::move(fixed));
  EXPECT_EQ(filled().url, "http://example.com/items?lang=en");
}

TEST_F(LoadPlanTest, AnOmittedQueryParameterIsLeftOutOfTheRun) {
  _request.url.assign(std::string_view{"http://example.com/items?a=1&q=x&b=2&q=y#top"});
  LoadParameter omitted;
  omitted.name.assign(std::string_view{"q"});
  omitted.queryParameter = true;
  omitted.source = ValueSource::omitted;
  _settings.parameters.push_back(omitted);
  EXPECT_EQ(filled().url, "http://example.com/items?a=1&b=2#top");
  EXPECT_EQ(View(_request.url), "http://example.com/items?a=1&q=x&b=2&q=y#top");  // the request does not change

  // The '?' of an empty query goes too, the fragment stays even when empty.
  _request.url.assign(std::string_view{"http://example.com/items?q=1&&q#"});
  EXPECT_EQ(filled().url, "http://example.com/items#");
  // Without the parameter, nothing changes.
  _request.url.assign(std::string_view{"http://example.com/items?a=1"});
  EXPECT_EQ(filled().url, "http://example.com/items?a=1");
  _request.url.assign(std::string_view{"http://example.com/items"});
  EXPECT_EQ(filled().url, "http://example.com/items");
}

TEST_F(LoadPlanTest, OnlyAQueryParameterCanBeOmitted) {
  _request.url.assign(std::string_view{"http://example.com/{{page}}"});
  LoadParameter omitted;
  omitted.name.assign(std::string_view{"page"});
  omitted.source = ValueSource::omitted;
  _settings.parameters.push_back(omitted);
  const LoadPlan plan = compile();
  ASSERT_FALSE(plan.valid());
  EXPECT_EQ(plan.issues()[0].parameter, 0U);
  EXPECT_EQ(View(plan.issues()[0].message), "Only a query parameter can be left out");
}

TEST_F(LoadPlanTest, AFixedParameterBehavesLikeAVariableOfANormalFetch) {
  _request.url.assign(std::string_view{"{{base}}/items"});
  addVariable("base", "http://collection.example");
  _settings.parameters.push_back(FixedParameter("base", "http://fixed.example/api/v1"));
  const LoadPlan plan = compile();
  ASSERT_TRUE(plan.valid());
  EXPECT_TRUE(plan.isConstant());
  EXPECT_EQ(plan.url().literal(), "http://fixed.example/api/v1/items");
  EXPECT_EQ(plan.valueCount(0), 1U);
}

TEST_F(LoadPlanTest, ACollectionVariableCanHoldAGeneratedValue) {
  _request.url.assign(std::string_view{"http://example.com{{path}}"});
  addVariable("path", "/users/{{id}}");
  _settings.parameters.push_back(ListParameter("id", "a/b"));
  EXPECT_EQ(filled().url, "http://example.com/users/a%2Fb");
}

TEST_F(LoadPlanTest, DynamicVariablesAreEvaluatedForEachRequestAndEachOccurrence) {
  _request.url.assign(std::string_view{"http://example.com/items?a={{$randomUUID}}&b={{$randomUUID}}"});
  const LoadPlan plan = compile();
  ASSERT_TRUE(plan.valid());
  EXPECT_FALSE(plan.isConstant());
  const auto first = plan.preview(0);
  const auto second = plan.preview(1);
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_NE(first->url, second->url);
  const std::string_view url = first->url;
  const std::size_t aPos = url.find("a=") + 2;
  const std::size_t bPos = url.find("b=") + 2;
  EXPECT_NE(url.substr(aPos, 36), url.substr(bPos, 36));
  EXPECT_EQ(url, plan.preview(0)->url);  // seeded
}

TEST_F(LoadPlanTest, DynamicVariablesAreFoundThroughOtherVariables) {
  _request.url.assign(std::string_view{"http://example.com/{{wrapped}}"});
  addVariable("wrapped", "t{{$randomInt}}");
  const LoadPlan plan = compile();
  ASSERT_TRUE(plan.valid());
  EXPECT_FALSE(plan.isConstant());
}

TEST_F(LoadPlanTest, EnvironmentVariablesAreConstant) {
  _request.url.assign(std::string_view{"http://example.com/{{$env.HOME}}"});
  EXPECT_TRUE(compile().isConstant());
}

TEST_F(LoadPlanTest, ASeededRunIsReproducibleAndZeroPicksASeed) {
  _request.url.assign(std::string_view{"http://example.com/{{$randomUUID}}"});
  const std::string first(filled(7).url);
  EXPECT_EQ(filled(7).url, first);
  _settings.seed = 43;
  EXPECT_NE(filled(7).url, first);
  _settings.seed = 0;
  const LoadPlan plan = compile();
  EXPECT_NE(plan.seed(), 0U);
}

TEST_F(LoadPlanTest, UndefinedVariablesAreIssues) {
  _request.url.assign(std::string_view{"http://example.com/{{nope}}/{{$env.ZALIE_SURELY_UNSET}}"});
  const LoadPlan plan = compile();
  ASSERT_EQ(plan.issues().size(), 2U);
  EXPECT_TRUE(View(plan.issues()[0].message).contains("nope"));
  EXPECT_EQ(plan.issues()[0].parameter, LoadPlanIssue::kNoParameter);
}

TEST_F(LoadPlanTest, GeneratorErrorsNameTheirParameterAndAreNotAlsoUndefined) {
  _request.url.assign(std::string_view{"http://example.com/{{term}}"});
  _settings.parameters.push_back(ListParameter("term", ""));
  const LoadPlan plan = compile();
  ASSERT_EQ(plan.issues().size(), 1U);
  EXPECT_EQ(plan.issues()[0].parameter, 0U);
  EXPECT_EQ(View(plan.issues()[0].message), "The list is empty");
}

TEST_F(LoadPlanTest, ParametersNeedAName) {
  _settings.parameters.push_back(ListParameter("  ", "a"));
  const LoadPlan plan = compile();
  ASSERT_FALSE(plan.valid());
  EXPECT_EQ(plan.issues()[0].parameter, 0U);
}

TEST_F(LoadPlanTest, AnEmptyUrlIsAnIssue) {
  _request.url.clear();
  EXPECT_FALSE(compile().valid());
}

TEST_F(LoadPlanTest, HostOfTheRequests) {
  _request.url.assign(std::string_view{"https://user@{{host}}:8443/search?q={{term}}"});
  addVariable("host", "api.example.com");
  _settings.parameters.push_back(ListParameter("term", "dog\ncat"));
  EXPECT_EQ(compile().host(), "api.example.com");
  EXPECT_EQ(LoadPlan(ResolveRequest(_request, _collection, MakeScope(_collection, -1))).host(), "api.example.com");

  // A generated value in the authority, or a whole generated URL: the host is not known in advance.
  _settings.parameters.push_back(ListParameter("host", "a.example.com\nb.example.com"));
  EXPECT_EQ(compile().host(), "");
  _request.url.assign(std::string_view{"{{target}}"});
  _settings.parameters.clear();
  _settings.parameters.push_back(ListParameter("target", "http://localhost/a"));
  const LoadPlan plan = compile();
  ASSERT_TRUE(plan.valid());
  EXPECT_EQ(plan.host(), "");
  EXPECT_EQ(plan.parameterVariable(0), 0U);
}

TEST_F(LoadPlanTest, ValueCountAndPass) {
  _request.url.assign(std::string_view{"http://example.com/{{a}}/{{b}}/{{c}}"});
  _settings.parameters.push_back(ListParameter("a", "1\n2\n3"));
  _settings.parameters.push_back(FixedParameter("b", "x"));
  LoadParameter random;
  random.name.assign(std::string_view{"c"});
  random.source = ValueSource::uuid;
  _settings.parameters.push_back(std::move(random));
  _settings.combine = CombineMode::allCombinations;
  const LoadPlan plan = compile();
  ASSERT_TRUE(plan.valid());
  EXPECT_EQ(plan.valueCount(0), 3U);
  EXPECT_EQ(plan.valueCount(1), 1U);
  EXPECT_EQ(plan.valueCount(2), 0U);
  EXPECT_EQ(plan.passLength(), 3U);
}

TEST_F(LoadPlanTest, PlannedRequestsIsTheTightestLimit) {
  _request.url.assign(std::string_view{"http://example.com/{{a}}"});
  _settings.parameters.push_back(ListParameter("a", "1\n2\n3\n4"));
  const LoadPlan plan = compile();
  ASSERT_TRUE(plan.valid());
  EXPECT_EQ(PlannedRequests(plan, {.durationMs = 1000}), 0U);
  EXPECT_EQ(PlannedRequests(plan, {.durationMs = 1000, .onePass = true}), 4U);
  EXPECT_EQ(PlannedRequests(plan, {.maxRequests = 2, .durationMs = 1000, .onePass = true}), 2U);
  EXPECT_EQ(PlannedRequests(plan, {.maxRequests = 2000, .durationMs = 1000, .maxRequestsPerSecond = 50}), 50U);
  EXPECT_EQ(PlannedRequests(plan, {.maxRequests = 20, .durationMs = 0, .maxRequestsPerSecond = 50}), 20U);
}

TEST(LoadSettingsTest, OptionsFollowTheSavedSettings) {
  const LoadSettings settings{
      .maxRequests = 5, .durationMs = 6, .maxRequestsPerSecond = 7, .warmupMs = 8, .connections = 9, .onePass = true};
  const LoadOptions options = MakeLoadOptions(settings);
  EXPECT_EQ(options.maxRequests, 5U);
  EXPECT_EQ(options.durationMs, 6U);
  EXPECT_EQ(options.maxRequestsPerSecond, 7U);
  EXPECT_EQ(options.warmupMs, 8U);
  EXPECT_EQ(options.connections, 9U);
  EXPECT_TRUE(options.onePass);
}

TEST_F(LoadPlanTest, RelativeDataFilesStartAtTheBaseDirectory) {
  const std::filesystem::path directory = std::filesystem::temp_directory_path() / "zalie-load-plan-relative";
  std::filesystem::create_directories(directory);
  std::ofstream(directory / "users.csv", std::ios::binary) << "user\nann\nbob\n";
  _request.url.assign(std::string_view{"http://example.com/{{user}}"});
  LoadParameter parameter;
  parameter.name.assign(std::string_view{"user"});
  parameter.source = ValueSource::dataFile;
  parameter.filePath.assign(std::string_view{"users.csv"});
  parameter.fileColumn.assign(std::string_view{"user"});
  _settings.parameters.push_back(std::move(parameter));
  const VariableScope scope = MakeScope(_collection, -1);
  const LoadPlan plan(_request, _collection, scope, _settings, directory, _cache);
  ASSERT_TRUE(plan.valid());
  EXPECT_EQ(plan.generators().dataFiles().size(), 1U);
  EXPECT_EQ(plan.preview(1)->url, "http://example.com/bob");
  std::filesystem::remove_all(directory);
}

}  // namespace
}  // namespace zalie
