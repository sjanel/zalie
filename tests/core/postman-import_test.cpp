#include "zalie/postman-import.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <string_view>

#include "zalie/collection.hpp"
#include "zalie/file-io.hpp"
#include "zalie/key-value.hpp"
#include "zalie/method.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/request.hpp"

namespace zalie {
namespace {

TEST(PostmanImport, Collection) {
  auto content = ReadFile(std::filesystem::path(ZALIE_TEST_DATA_DIR) / "postman-collection.json");
  ASSERT_TRUE(content.has_value());
  EXPECT_TRUE(LooksLikePostmanCollection(*content));
  auto imported = ImportPostmanCollection(*content);
  ASSERT_TRUE(imported.has_value()) << View(imported.error());
  const Collection& collection = imported->collection;
  EXPECT_EQ(View(collection.name), "Pet store");
  EXPECT_EQ(View(collection.docs), "The pet store API.");
  EXPECT_EQ(collection.auth.mode, AuthMode::bearer);
  EXPECT_EQ(View(collection.auth.token), "{{token}}");
  ASSERT_EQ(collection.variables.size(), 2U);
  EXPECT_EQ(View(collection.variables[1].value), "t0k3n");

  ASSERT_EQ(collection.root.folders.size(), 1U);
  const Folder& pets = collection.root.folders[0];
  EXPECT_EQ(View(pets.name), "Pets");
  ASSERT_EQ(pets.requests.size(), 2U);

  const Request& list = pets.requests[0];
  EXPECT_EQ(list.method, Method::get);
  EXPECT_EQ(View(list.url), "{{baseUrl}}/pets?limit=10");
  ASSERT_EQ(list.params.size(), 2U);
  EXPECT_FALSE(list.params[1].enabled);
  ASSERT_EQ(list.headers.size(), 2U);
  EXPECT_FALSE(list.headers[1].enabled);
  EXPECT_EQ(list.auth.mode, AuthMode::inherit);
  EXPECT_EQ(View(list.docs), "Lists the pets.");

  const Request& create = pets.requests[1];
  EXPECT_EQ(create.method, Method::post);
  EXPECT_EQ(create.bodyMode, BodyMode::json);
  EXPECT_EQ(View(create.body), "{\n  \"name\": \"Zalie\"\n}");
  EXPECT_EQ(create.auth.mode, AuthMode::basic);
  EXPECT_EQ(View(create.auth.password), "pw");

  ASSERT_EQ(collection.root.requests.size(), 2U);
  const Request& login = collection.root.requests[0];
  EXPECT_EQ(login.bodyMode, BodyMode::formUrlEncoded);
  ASSERT_EQ(login.form.size(), 2U);
  EXPECT_EQ(View(login.form[1].value), "7");  // numbers are converted
  EXPECT_EQ(login.auth.mode, AuthMode::apiKey);
  EXPECT_TRUE(login.auth.apiKeyInQuery);
  EXPECT_EQ(View(login.auth.apiKeyName), "api_key");

  EXPECT_EQ(View(collection.root.requests[1].url), "https://example.com/ping");

  ASSERT_EQ(imported->warnings.size(), 1U);  // the test script
  EXPECT_TRUE(imported->warnings[0].contains("scripts"));
}

TEST(PostmanImport, Errors) {
  EXPECT_FALSE(ImportPostmanCollection("not json").has_value());
  EXPECT_FALSE(ImportPostmanCollection(R"({"a": 1})").has_value());
  EXPECT_FALSE(LooksLikePostmanCollection(R"({"a": 1})"));
  auto minimal = ImportPostmanCollection(R"({"info": {}, "item": []})");
  ASSERT_TRUE(minimal.has_value());
  EXPECT_EQ(View(minimal->collection.name), "Postman collection");
}

}  // namespace
}  // namespace zalie
