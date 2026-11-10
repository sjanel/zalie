#include "zalie/bruno-import.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <string_view>

#include "test-utils.hpp"
#include "zalie/collection.hpp"
#include "zalie/file-io.hpp"
#include "zalie/key-value.hpp"
#include "zalie/method.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/request.hpp"

namespace zalie {
namespace {

std::filesystem::path CollectionDir() { return std::filesystem::path(ZALIE_TEST_DATA_DIR) / "bruno-collection"; }

TEST(BruParser, Request) {
  constexpr std::string_view kBru = R"(meta {
  name: Update pet
  seq: 7
}

put {
  url: https://petstore/pets/1?x=1
  body: text
  auth: bearer
}

headers {
  Accept: */*
  ~X-Off: 1
}

auth:bearer {
  token: abc:def
}

body:text {
  line one
    indented line
  }
}
)";
  auto parsed = ParseBruRequest(kBru);
  ASSERT_TRUE(parsed.has_value()) << View(parsed.error());
  const Request& req = parsed->request;
  EXPECT_EQ(parsed->sequence, 7);
  EXPECT_EQ(View(req.name), "Update pet");
  EXPECT_EQ(req.method, Method::put);
  EXPECT_EQ(View(req.url), "https://petstore/pets/1?x=1");
  ASSERT_EQ(req.params.size(), 1U);
  EXPECT_EQ(req.bodyMode, BodyMode::text);
  EXPECT_EQ(View(req.body), "line one\n  indented line\n}");  // only Bruno's 2 spaces of indentation are removed
  ASSERT_EQ(req.headers.size(), 2U);
  EXPECT_FALSE(req.headers[1].enabled);
  EXPECT_EQ(View(req.headers[1].key), "X-Off");
  EXPECT_EQ(req.auth.mode, AuthMode::bearer);
  EXPECT_EQ(View(req.auth.token), "abc:def");  // value split on the first colon only
}

TEST(BruParser, Errors) {
  EXPECT_FALSE(ParseBruRequest("meta {\n  name: x\n}\n").has_value());
  EXPECT_FALSE(ParseBruRequest("").has_value());
}

TEST(BruParser, Environment) {
  const Environment env = ParseBruEnvironment("dev", "vars {\n  a: 1\n  ~b: 2\n}\nvars:secret [\n  s1,\n  ~s2\n]\n");
  EXPECT_EQ(View(env.name), "dev");
  ASSERT_EQ(env.variables.size(), 4U);
  EXPECT_EQ(View(env.variables[0].value), "1");
  EXPECT_FALSE(env.variables[1].enabled);
  EXPECT_EQ(View(env.variables[2].key), "s1");
  EXPECT_TRUE(env.variables[2].value.empty());
  EXPECT_FALSE(env.variables[3].enabled);
}

TEST(BrunoImport, Collection) {
  auto imported = ImportBrunoCollection(CollectionDir());
  ASSERT_TRUE(imported.has_value()) << View(imported.error());
  const Collection& collection = imported->collection;
  EXPECT_EQ(View(collection.name), "Coton Kennel \"API\"");
  EXPECT_EQ(View(collection.docs), "# Coton Kennel\nCollection docs.");

  // Collection level headers, auth and variables.
  ASSERT_EQ(collection.headers.size(), 2U);
  EXPECT_FALSE(collection.headers[1].enabled);
  EXPECT_EQ(collection.auth.mode, AuthMode::bearer);
  EXPECT_EQ(View(collection.auth.token), "{{token}}");
  ASSERT_EQ(collection.variables.size(), 1U);
  EXPECT_EQ(View(collection.variables[0].key), "apiVersion");

  // Environments, sorted by name, secrets imported empty.
  ASSERT_EQ(collection.environments.size(), 2U);
  EXPECT_EQ(View(collection.environments[0].name), "local");
  EXPECT_EQ(collection.environments[0].variables.size(), 4U);
  EXPECT_EQ(View(collection.environments[1].name), "prod");

  // Tree: folder 'Users' (from folder.bru) first, then the root requests by sequence.
  ASSERT_EQ(collection.root.folders.size(), 1U);
  const Folder& users = collection.root.folders[0];
  EXPECT_EQ(View(users.name), "Users");
  ASSERT_EQ(users.requests.size(), 3U);
  EXPECT_EQ(View(users.requests[0].name), "List users");
  EXPECT_EQ(View(users.requests[1].name), "Create user");
  EXPECT_EQ(View(users.requests[2].name), "Login");
  ASSERT_EQ(collection.root.requests.size(), 1U);
  EXPECT_EQ(View(collection.root.requests[0].name), "Health");
  EXPECT_EQ(collection.root.requests[0].auth.mode, AuthMode::none);
  EXPECT_EQ(collection.root.requests[0].settings.timeoutMs, 2500U);                     // settings { timeout: 2500 }
  EXPECT_EQ(users.requests[0].settings.timeoutMs, RequestSettings::kDefaultTimeoutMs);  // no settings block

  const Request& list = users.requests[0];
  EXPECT_EQ(list.auth.mode, AuthMode::basic);
  EXPECT_EQ(View(list.auth.username), "admin");
  EXPECT_EQ(View(list.auth.password), "{{password}}");

  const Request& create = users.requests[1];
  EXPECT_EQ(create.method, Method::post);
  EXPECT_EQ(create.auth.mode, AuthMode::inherit);
  EXPECT_EQ(create.bodyMode, BodyMode::json);
  EXPECT_EQ(View(create.body), "{\n  \"name\": \"Zalie\",\n  \"tags\": {\n    \"breed\": \"Coton de Tulear\"\n  }\n}");
  ASSERT_EQ(create.params.size(), 2U);  // enabled from the URL, then the disabled one of the table
  EXPECT_TRUE(create.params[0].enabled);
  EXPECT_EQ(View(create.params[1].key), "dryRun");
  EXPECT_FALSE(create.params[1].enabled);
  EXPECT_EQ(View(create.docs), "Creates a **user**.");

  const Request& login = users.requests[2];
  EXPECT_EQ(login.bodyMode, BodyMode::formUrlEncoded);
  ASSERT_EQ(login.form.size(), 2U);
  EXPECT_FALSE(login.form[1].enabled);
  EXPECT_EQ(login.auth.mode, AuthMode::apiKey);
  EXPECT_TRUE(login.auth.apiKeyInQuery);
  EXPECT_EQ(View(login.auth.apiKeyName), "X-Api-Key");

  // The broken file is reported, not fatal. users/folder.bru only has the default 'auth { mode: inherit }': no
  // warning for it.
  ASSERT_EQ(imported->warnings.size(), 1U) << imported->warnings.back();
  EXPECT_TRUE(imported->warnings[0].contains("broken.bru"));
}

TEST(BruParser, SettingsTimeout) {
  constexpr std::string_view kBase = "get {\n  url: http://h\n  body: none\n  auth: none\n}\n";
  auto withTimeout = ParseBruRequest(std::string(kBase) + "settings {\n  encodeUrl: true\n  timeout: 1200\n}\n");
  ASSERT_TRUE(withTimeout.has_value());
  EXPECT_EQ(withTimeout->request.settings.timeoutMs, 1200U);
  // 0 is Bruno's "default timeout": Zalie's default.
  auto zero = ParseBruRequest(std::string(kBase) + "settings {\n  timeout: 0\n}\n");
  ASSERT_TRUE(zero.has_value());
  EXPECT_EQ(zero->request.settings.timeoutMs, RequestSettings::kDefaultTimeoutMs);
}

TEST(BrunoImport, FolderLevelOverridesAreReported) {
  const test::TempDir dir;
  ASSERT_TRUE(WriteFileAtomically(dir.path() / "bruno.json", R"({"version": "1", "name": "x", "type": "collection"})"));
  ASSERT_TRUE(
      WriteFileAtomically(dir.path() / "f" / "folder.bru", "meta {\n  name: F\n}\n\nauth {\n  mode: bearer\n}\n"));
  ASSERT_TRUE(WriteFileAtomically(dir.path() / "g" / "folder.bru", "headers {\n  ~X-Off: 1\n}\n"));  // disabled only
  ASSERT_TRUE(WriteFileAtomically(dir.path() / "h" / "folder.bru", "headers {\n  X-On: 1\n}\n"));
  auto imported = ImportBrunoCollection(dir.path());
  ASSERT_TRUE(imported.has_value());
  ASSERT_EQ(imported->warnings.size(), 2U);
  EXPECT_TRUE(imported->warnings[0].contains("folder level")) << imported->warnings[0];
}

TEST(BrunoImport, UnusualFilesAndBodies) {
  const test::TempDir dir;
  const std::filesystem::path root = dir.path() / "My API";
  // A name that cannot be decoded: the collection is named after its directory.
  ASSERT_TRUE(WriteFileAtomically(root / "bruno.json", R"({"name": "bad \uZZZZ"})"));
  ASSERT_TRUE(WriteFileAtomically(root / "collection.bru", "auth {\n  mode: inherit\n}\n\nscript:pre-request {\n}\n"));
  ASSERT_TRUE(WriteFileAtomically(root / "f" / "folder.bru", "auth:bearer {\n  token: t\n}\n"));
  // Windows line endings, no newline at the end, no name, an XML body, a key without value.
  ASSERT_TRUE(WriteFileAtomically(root / "Xml.bru",
                                  "post {\r\n  url: http://h/x\r\n  body: xml\r\n}\r\n\r\nheaders {\r\n\r\n"
                                  "  X-Flag\r\n}\r\n\r\nbody:xml {\r\n  <a/>\r\n}"));
  ASSERT_TRUE(WriteFileAtomically(root / "Form.bru",
                                  "post {\n  url: http://h/f\n  body: multipartForm\n}\n\nbody:multipart-form {\n"
                                  "  name: zalie\n}\n"));
  // Files that cannot be read: links to nothing.
  std::error_code errc;
  std::filesystem::create_symlink(dir.path() / "nothing", root / "Broken.bru", errc);
  const bool links = !errc;
  if (links) {
    std::filesystem::create_directories(root / "environments");
    std::filesystem::create_symlink(dir.path() / "nothing", root / "environments" / "local.bru", errc);
  }

  auto imported = ImportBrunoCollection(root);
  ASSERT_TRUE(imported.has_value()) << View(imported.error());
  const Collection& collection = imported->collection;
  EXPECT_EQ(View(collection.name), "My API");
  EXPECT_EQ(collection.auth.mode, AuthMode::none);
  ASSERT_EQ(collection.root.requests.size(), 2U);
  const Request& form = collection.root.requests[0];
  EXPECT_EQ(View(form.name), "Form");
  EXPECT_EQ(form.bodyMode, BodyMode::formUrlEncoded);
  ASSERT_EQ(form.form.size(), 1U);
  EXPECT_EQ(View(form.form[0].value), "zalie");
  const Request& xml = collection.root.requests[1];
  EXPECT_EQ(View(xml.name), "Xml");
  EXPECT_EQ(xml.bodyMode, BodyMode::xml);
  EXPECT_EQ(View(xml.body), "<a/>");
  ASSERT_EQ(xml.headers.size(), 1U);
  EXPECT_EQ(View(xml.headers[0].key), "X-Flag");
  EXPECT_TRUE(xml.headers[0].value.empty());
  // Scripts, the folder level authentication, and the files that could not be read.
  EXPECT_EQ(imported->warnings.size(), links ? 4U : 2U);
}

TEST(BrunoImport, SingleRequestFile) {
  auto request = ImportBruRequestFile(CollectionDir() / "users" / "Create user.bru");
  ASSERT_TRUE(request.has_value()) << View(request.error());
  EXPECT_EQ(View(request->name), "Create user");
  EXPECT_EQ(request->method, Method::post);
  EXPECT_NE(request->id, 0U);

  // The name defaults to the file name.
  const test::TempDir dir;
  ASSERT_TRUE(WriteFileAtomically(dir.path() / "My ping.bru", "get {\n  url: http://h/ping\n}\n"));
  request = ImportBruRequestFile(dir.path() / "My ping.bru");
  ASSERT_TRUE(request.has_value());
  EXPECT_EQ(View(request->name), "My ping");

  // Not requests.
  EXPECT_FALSE(ImportBruRequestFile(CollectionDir() / "users" / "folder.bru").has_value());
  EXPECT_FALSE(ImportBruRequestFile(CollectionDir() / "environments" / "local.bru").has_value());
  EXPECT_FALSE(ImportBruRequestFile(CollectionDir() / "nope.bru").has_value());
}

TEST(BrunoImport, NotACollection) {
  EXPECT_FALSE(ImportBrunoCollection(CollectionDir() / "users").has_value());
  EXPECT_FALSE(ImportBrunoCollection(CollectionDir() / "does-not-exist").has_value());
}

}  // namespace
}  // namespace zalie
