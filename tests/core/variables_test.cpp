#include "zalie/variables.hpp"

#include <gtest/gtest.h>
#include <stdlib.h>  // NOLINT(modernize-deprecated-headers): setenv is POSIX, declared there

#include <cstdlib>
#include <string>
#include <string_view>

#include "zalie/key-value.hpp"
#include "zalie/raw-chars.hpp"

namespace zalie {
namespace {

TEST(Variables, FirstLayerWins) {
  KeyValues env;
  env.push_back(MakeKeyValue("host", "localhost"));
  env.push_back(MakeKeyValue("disabled", "x", false));
  KeyValues collection;
  collection.push_back(MakeKeyValue("host", "example.com"));
  collection.push_back(MakeKeyValue("port", "8080"));
  collection.push_back(MakeKeyValue("", "ignored"));

  VariableScope scope;
  scope.addLayer(env);
  scope.addLayer(collection);
  EXPECT_EQ(scope.size(), 3U);
  ASSERT_NE(scope.find("host"), nullptr);
  EXPECT_EQ(*scope.find("host"), "localhost");
  EXPECT_EQ(*scope.find("port"), "8080");
  EXPECT_EQ(scope.find("disabled"), nullptr);
  EXPECT_EQ(scope.find("nope"), nullptr);
}

TEST(Variables, Interpolate) {
  VariableScope scope;
  scope.add("host", "localhost");
  scope.add("port", "8080");
  scope.add("empty", "");
  UnresolvedNames unresolved;
  EXPECT_EQ(View(Interpolate("http://{{host}}:{{ port }}/x", scope, &unresolved)), "http://localhost:8080/x");
  EXPECT_TRUE(unresolved.empty());
  EXPECT_EQ(View(Interpolate("[{{empty}}]", scope)), "[]");
  EXPECT_EQ(View(Interpolate("no variables", scope)), "no variables");
  EXPECT_EQ(View(Interpolate("", scope)), "");
  // Unterminated / empty references are kept verbatim.
  EXPECT_EQ(View(Interpolate("a {{host", scope)), "a {{host");
  EXPECT_EQ(View(Interpolate("{{}}", scope, &unresolved)), "{{}}");
}

TEST(Variables, UnresolvedAreKeptAndReportedOnce) {
  VariableScope scope;
  UnresolvedNames unresolved;
  const std::string_view text = "{{token}} and {{token}} and {{other}}";
  EXPECT_EQ(View(Interpolate(text, scope, &unresolved)), text);
  ASSERT_EQ(unresolved.size(), 2U);
  EXPECT_EQ(unresolved[0], "token");
  EXPECT_EQ(unresolved[1], "other");
}

TEST(Variables, NestedAndCycles) {
  VariableScope scope;
  scope.add("baseUrl", "{{scheme}}://{{host}}");
  scope.add("scheme", "https");
  scope.add("host", "api.example.com");
  scope.add("loop", "x{{loop}}");
  EXPECT_EQ(View(Interpolate("{{baseUrl}}/v1", scope)), "https://api.example.com/v1");
  // A cycle stops after a bounded depth instead of recursing forever.
  const RawChars32 looped = Interpolate("{{loop}}", scope);
  EXPECT_TRUE(View(looped).starts_with("xxxxxxxx"));
  EXPECT_TRUE(View(looped).ends_with("{{loop}}"));
}

TEST(Variables, DynamicVariables) {
  VariableScope scope;
  EXPECT_TRUE(IsDynamicVariable("$timestamp"));
  EXPECT_TRUE(IsDynamicVariable("$env.HOME"));
  EXPECT_TRUE(IsDynamicVariable("process.env.HOME"));
  EXPECT_FALSE(IsDynamicVariable("timestamp"));

  const RawChars32 timestamp = Interpolate("{{$timestamp}}", scope);
  EXPECT_EQ(timestamp.size(), 10U);  // seconds since the epoch, until 2286
  const RawChars32 timestampMs = Interpolate("{{$timestampMs}}", scope);
  EXPECT_EQ(timestampMs.size(), 13U);
  const RawChars32 iso = Interpolate("{{$isoTimestamp}}", scope);
  EXPECT_EQ(iso.size(), 24U);  // 2026-10-29T12:34:56.789Z
  EXPECT_EQ(View(iso)[10], 'T');
  EXPECT_TRUE(View(iso).ends_with('Z'));
  const RawChars32 randomInt = Interpolate("{{$randomInt}}", scope);
  EXPECT_LE(std::stoi(std::string(View(randomInt))), 1000);
  EXPECT_EQ(Interpolate("{{$guid}}", scope).size(), 36U);

  // A user variable shadows a dynamic one.
  scope.add("$timestamp", "fixed");
  EXPECT_EQ(View(Interpolate("{{$timestamp}}", scope)), "fixed");
}

TEST(Variables, ProcessEnvironment) {
#ifdef _WIN32
  _putenv_s("ZALIE_TEST_VAR", "from-env");
#else
  setenv("ZALIE_TEST_VAR", "from-env", 1);  // NOLINT(concurrency-mt-unsafe)
#endif
  VariableScope scope;
  EXPECT_EQ(View(Interpolate("{{$env.ZALIE_TEST_VAR}}|{{process.env.ZALIE_TEST_VAR}}", scope)), "from-env|from-env");
  UnresolvedNames unresolved;
  EXPECT_EQ(View(Interpolate("{{$env.ZALIE_SURELY_UNDEFINED}}", scope, &unresolved)),
            "{{$env.ZALIE_SURELY_UNDEFINED}}");
  EXPECT_EQ(unresolved.size(), 1U);
}

TEST(Variables, FindVariableRefs) {
  VariableRefs refs;
  FindVariableRefs("{{a}}/x/{{ b }}/{{unterminated", refs);
  ASSERT_EQ(refs.size(), 2U);
  EXPECT_EQ(refs[0].begin, 0U);
  EXPECT_EQ(refs[0].end, 5U);
  EXPECT_EQ(refs[0].name, "a");
  EXPECT_EQ(refs[1].name, "b");
  EXPECT_EQ(refs[1].begin, 8U);
  EXPECT_EQ(refs[1].end, 15U);
}

TEST(Variables, RandomUuid) {
  const RawChars32 uuid = RandomUuid();
  const std::string_view view = uuid;
  ASSERT_EQ(view.size(), 36U);
  EXPECT_EQ(view[8], '-');
  EXPECT_EQ(view[13], '-');
  EXPECT_EQ(view[14], '4');  // version
  EXPECT_TRUE(std::string_view{"89ab"}.contains(view[19]));
  EXPECT_NE(view, std::string_view(RandomUuid()));
}

}  // namespace
}  // namespace zalie
