#include "zalie/collection.hpp"

#include <gtest/gtest.h>

#include <amc/type_traits.hpp>
#include <amc/vector.hpp>
#include <cstdint>
#include <set>
#include <string_view>
#include <utility>

#include "zalie/key-value.hpp"
#include "zalie/method.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/request.hpp"

namespace zalie {
namespace {

// The model types are trivially relocatable: amc containers move them with memcpy.
static_assert(amc::is_trivially_relocatable_v<KeyValue>);
static_assert(amc::is_trivially_relocatable_v<Auth>);
static_assert(amc::is_trivially_relocatable_v<Request>);
static_assert(amc::is_trivially_relocatable_v<Environment>);
static_assert(amc::is_trivially_relocatable_v<Folder>);

Request MakeRequest(std::string_view name) {
  Request req;
  req.name.assign(name);
  return req;
}

class CollectionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    Folder users;
    users.name.assign(std::string_view{"users"});
    users.requests.push_back(MakeRequest("list"));
    users.requests.push_back(MakeRequest("get"));
    Folder admin;
    admin.name.assign(std::string_view{"admin"});
    admin.requests.push_back(MakeRequest("purge"));
    users.folders.push_back(std::move(admin));
    collection.root.folders.push_back(std::move(users));
    collection.root.requests.push_back(MakeRequest("health"));
    EnsureUniqueIds(collection);
  }

  std::uint64_t idOf(std::string_view name) {
    std::uint64_t id = 0;
    ForEachRequest(collection.root, [&](const Request& req) {
      if (View(req.name) == name) {
        id = req.id;
      }
    });
    return id;
  }

  Folder& users() { return collection.root.folders[0]; }

  Collection collection;
};

TEST_F(CollectionTest, UniqueIds) {
  std::set<std::uint64_t> ids;
  ForEachRequest(collection.root, [&ids](const Request& req) { ids.insert(req.id); });
  ids.insert(collection.root.id);
  ids.insert(users().id);
  ids.insert(users().folders[0].id);
  EXPECT_EQ(ids.size(), 7U);
  EXPECT_FALSE(ids.contains(0));

  // Duplicates (e.g. a hand-merged file) are renumbered, the first occurrence keeps its id.
  const std::uint64_t listId = idOf("list");
  users().requests[1].id = listId;
  EnsureUniqueIds(collection);
  EXPECT_EQ(users().requests[0].id, listId);
  EXPECT_NE(users().requests[1].id, listId);
}

TEST_F(CollectionTest, Find) {
  EXPECT_EQ(View(FindRequest(collection.root, idOf("purge"))->name), "purge");
  const Folder& constRoot = collection.root;
  EXPECT_EQ(View(FindRequest(constRoot, idOf("health"))->name), "health");
  EXPECT_EQ(FindRequest(collection.root, 12345), nullptr);
  EXPECT_EQ(FindFolder(collection.root, users().folders[0].id), users().folders.data());
  EXPECT_EQ(FindFolder(collection.root, collection.root.id), &collection.root);
  EXPECT_EQ(FindFolder(collection.root, 12345), nullptr);
  EXPECT_EQ(FindParentOfRequest(collection.root, idOf("get")), &users());
  EXPECT_EQ(FindParentOfRequest(collection.root, idOf("health")), &collection.root);
  EXPECT_EQ(FindParentOfRequest(collection.root, 12345), nullptr);
  EXPECT_EQ(FindParentOfFolder(collection.root, users().folders[0].id), &users());
  EXPECT_EQ(FindParentOfFolder(collection.root, 12345), nullptr);
  EXPECT_EQ(CountRequests(collection.root), 4U);
}

TEST_F(CollectionTest, FindRequestPath) {
  amc::vector<const Folder*> path;
  EXPECT_TRUE(FindRequestPath(collection.root, idOf("purge"), path));
  ASSERT_EQ(path.size(), 2U);
  EXPECT_EQ(path[0], &users());
  EXPECT_EQ(View(path[1]->name), "admin");

  path.clear();
  EXPECT_TRUE(FindRequestPath(collection.root, idOf("health"), path));  // in the root: no folder
  EXPECT_TRUE(path.empty());
  EXPECT_TRUE(FindRequestPath(collection.root, idOf("list"), path));
  ASSERT_EQ(path.size(), 1U);
  EXPECT_FALSE(FindRequestPath(collection.root, 12345, path));
  EXPECT_EQ(path.size(), 1U);  // unchanged
}

TEST_F(CollectionTest, RemoveRequestAndFolder) {
  EXPECT_TRUE(RemoveRequest(collection.root, idOf("get")));
  EXPECT_FALSE(RemoveRequest(collection.root, 12345));
  EXPECT_EQ(CountRequests(collection.root), 3U);
  EXPECT_TRUE(RemoveFolder(collection.root, users().folders[0].id));
  EXPECT_FALSE(RemoveFolder(collection.root, 12345));
  EXPECT_EQ(CountRequests(collection.root), 2U);
}

TEST_F(CollectionTest, Duplicate) {
  const std::uint64_t listId = idOf("list");
  Request* pCopy = DuplicateRequest(collection.root, listId);
  ASSERT_NE(pCopy, nullptr);
  EXPECT_EQ(View(pCopy->name), "list copy");
  EXPECT_NE(pCopy->id, listId);
  ASSERT_EQ(users().requests.size(), 3U);
  EXPECT_EQ(&users().requests[1], pCopy);  // right after the original
  EXPECT_EQ(DuplicateRequest(collection.root, 12345), nullptr);
}

TEST_F(CollectionTest, Move) {
  // Within a folder, to the end.
  EXPECT_TRUE(MoveRequest(collection.root, idOf("list"), users().id, 99));
  EXPECT_EQ(View(users().requests[0].name), "get");
  EXPECT_EQ(View(users().requests[1].name), "list");
  // Within a folder, before another one (index counted before the removal).
  EXPECT_TRUE(MoveRequest(collection.root, idOf("list"), users().id, 0));
  EXPECT_EQ(View(users().requests[0].name), "list");
  // Across folders.
  EXPECT_TRUE(MoveRequest(collection.root, idOf("health"), users().folders[0].id, 0));
  EXPECT_TRUE(collection.root.requests.empty());
  EXPECT_EQ(View(users().folders[0].requests[0].name), "health");
  EXPECT_FALSE(MoveRequest(collection.root, 12345, users().id, 0));
  EXPECT_FALSE(MoveRequest(collection.root, idOf("list"), 12345, 0));
}

TEST(Collection, Example) {
  const Collection example = MakeExampleCollection();
  EXPECT_FALSE(example.name.empty());
  EXPECT_EQ(CountRequests(example.root), 4U);
  EXPECT_EQ(example.environments.size(), 1U);
  EXPECT_NE(FindEnabled(example.variables, "baseUrl"), nullptr);
}

TEST(KeyValue, FindEnabled) {
  KeyValues entries;
  entries.push_back(MakeKeyValue("Accept", "a", false));
  entries.push_back(MakeKeyValue("Accept", "b"));
  EXPECT_EQ(View(FindEnabled(entries, "Accept")->value), "b");
  EXPECT_EQ(FindEnabled(entries, "accept"), nullptr);
  EXPECT_EQ(View(FindEnabled(entries, "accept", true)->value), "b");
}

TEST(Method, Parse) {
  for (Method method : kMethods) {
    EXPECT_EQ(ParseMethod(MethodToStr(method)), method);
  }
  EXPECT_EQ(ParseMethod(" delete "), Method::del);
  EXPECT_FALSE(ParseMethod("PROPFIND").has_value());
  EXPECT_TRUE(MethodUsuallyHasBody(Method::patch));
  EXPECT_FALSE(MethodUsuallyHasBody(Method::get));
}

}  // namespace
}  // namespace zalie
