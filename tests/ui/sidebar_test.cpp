// The sidebar, driven as the user does: clicks on its buttons, context menus, drag and drop.

#include <gtest/gtest.h>
#include <imgui.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "app-test-fixture.hpp"
#include "zalie/collection.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/request.hpp"
#include "zalie/ui/app.hpp"
#include "zalie/workspace.hpp"

namespace zalie {
namespace {

class SidebarTest : public AppTest {
 protected:
  SidebarTest() { frame(3); }

  // The label of the node of the example collection: a star follows the name of a modified collection.
  [[nodiscard]] std::string collectionLabel() {
    return std::string(View(exampleCollection().name)) + (workspace[0].dirty ? " *" : "");
  }

  [[nodiscard]] Folder& basics() { return exampleCollection().root.folders[0]; }

  static constexpr std::string_view kBasicsLabel = "     Basics";
  // The requests of the tree, in order: the three of "Basics", then "I'm a teapot".
  static constexpr std::string_view kRequestLabel = "##request";
  static constexpr std::size_t kTeapot = 3;
};

TEST_F(SidebarTest, TheNewMenuCreatesARequestOrOpensADialog) {
  ASSERT_TRUE(click("##newMenu"));
  ASSERT_TRUE(click("New request"));
  EXPECT_EQ(app->tabCount(), 1U);
  EXPECT_EQ(app->activeTab()->requestId, 0U);

  ASSERT_TRUE(click("##newMenu"));
  ASSERT_TRUE(click("New collection..."));
  EXPECT_TRUE(ModalOpen());
  EXPECT_TRUE(hasItem("Create"));
  ASSERT_TRUE(click("Cancel"));
  EXPECT_FALSE(ModalOpen());

  ASSERT_TRUE(click("##newMenu"));
  ASSERT_TRUE(click("Import collection / curl..."));
  EXPECT_TRUE(hasItem("Import collection"));
  ASSERT_TRUE(click("Close"));
  EXPECT_FALSE(ModalOpen());

  ASSERT_TRUE(click("##import"));
  EXPECT_TRUE(hasItem("Import collection"));
}

TEST_F(SidebarTest, ClickingARequestOpensItAndADoubleClickRenamesIt) {
  ASSERT_TRUE(click(kRequestLabel, 1));
  ASSERT_EQ(app->tabCount(), 1U);
  EXPECT_EQ(app->activeTab()->requestId, basics().requests[1].id);
  // The same request again: its tab is focused, not duplicated.
  ASSERT_TRUE(click(kRequestLabel, kTeapot));
  ASSERT_TRUE(click(kRequestLabel, 1));
  EXPECT_EQ(app->tabCount(), 2U);
  EXPECT_EQ(app->activeTabIndex(), 0U);

  ASSERT_TRUE(doubleClick(kRequestLabel, kTeapot));
  ASSERT_TRUE(ModalOpen());
  typeText("Short and stout");
  press(ImGuiKey_Enter);
  EXPECT_FALSE(ModalOpen());
  EXPECT_EQ(View(exampleCollection().root.requests[0].name), "Short and stout");
  EXPECT_TRUE(workspace[0].dirty);
}

TEST_F(SidebarTest, TheMenuOfACollectionAddsRequestsAndFolders) {
  ASSERT_TRUE(contextMenu(collectionLabel(), "New request"));
  ASSERT_EQ(exampleCollection().root.requests.size(), 2U);
  const Request& created = exampleCollection().root.requests[1];
  EXPECT_EQ(View(created.name), "New request");
  EXPECT_EQ(created.auth.mode, AuthMode::inherit);
  ASSERT_EQ(app->tabCount(), 1U);
  EXPECT_EQ(app->activeTab()->requestId, created.id);

  ASSERT_TRUE(contextMenu(collectionLabel(), "New folder"));
  ASSERT_EQ(exampleCollection().root.folders.size(), 2U);
  EXPECT_EQ(View(exampleCollection().root.folders[1].name), "New folder");
  EXPECT_NE(exampleCollection().root.folders[1].id, 0U);
}

TEST_F(SidebarTest, TheMenuOfACollectionRenamesItAndOpensItsSettings) {
  ASSERT_TRUE(contextMenu(collectionLabel(), "Rename..."));
  ASSERT_TRUE(ModalOpen());
  typeText("  Renamed API ");
  ASSERT_TRUE(click("Rename"));
  EXPECT_FALSE(ModalOpen());
  EXPECT_EQ(View(exampleCollection().name), "Renamed API");

  ASSERT_TRUE(contextMenu(collectionLabel(), "Rename..."));
  typeText("Not kept");
  ASSERT_TRUE(click("Cancel"));
  EXPECT_EQ(View(exampleCollection().name), "Renamed API");

  ASSERT_TRUE(contextMenu(collectionLabel(), "Variables, environments, auth..."));
  EXPECT_TRUE(ModalOpen());
  EXPECT_TRUE(hasItem("Environments"));
  ASSERT_TRUE(click("Done"));
  EXPECT_FALSE(ModalOpen());
}

TEST_F(SidebarTest, ClosingACollectionClosesItsTabsAndKeepsTheOthers) {
  app->openRequest(basics().requests[0].id);
  app->newScratchTab();
  frame(2);
  ASSERT_EQ(app->tabCount(), 2U);
  ASSERT_TRUE(contextMenu(collectionLabel(), "Close collection"));
  EXPECT_EQ(workspace.size(), 0U);
  ASSERT_EQ(app->tabCount(), 1U);
  EXPECT_EQ(app->activeTab()->requestId, 0U);

  // Without collection, the sidebar proposes to create or import one.
  ASSERT_TRUE(click("Create a collection"));
  EXPECT_TRUE(hasItem("Create"));
  ASSERT_TRUE(click("Cancel"));
  ASSERT_TRUE(click("Import (Bruno, Postman)"));
  EXPECT_TRUE(hasItem("Import collection"));
}

TEST_F(SidebarTest, TheMenuOfAFolder) {
  ASSERT_TRUE(contextMenu(kBasicsLabel, "New request"));
  ASSERT_EQ(basics().requests.size(), 4U);
  EXPECT_EQ(app->activeTab()->requestId, basics().requests[3].id);

  ASSERT_TRUE(contextMenu(kBasicsLabel, "New sub-folder"));
  ASSERT_EQ(basics().folders.size(), 1U);
  EXPECT_EQ(View(basics().folders[0].name), "New folder");

  ASSERT_TRUE(contextMenu(kBasicsLabel, "Rename..."));
  ASSERT_TRUE(ModalOpen());
  typeText("Essentials");
  press(ImGuiKey_Enter);
  EXPECT_EQ(View(basics().name), "Essentials");

  // Deleting the folder closes the tabs of its requests.
  app->openRequest(exampleCollection().root.requests[0].id);
  frame(2);
  ASSERT_EQ(app->tabCount(), 2U);
  ASSERT_TRUE(contextMenu("     Essentials", "Delete folder"));
  EXPECT_TRUE(exampleCollection().root.folders.empty());
  ASSERT_EQ(app->tabCount(), 1U);
  EXPECT_EQ(app->activeTab()->requestId, exampleCollection().root.requests[0].id);
}

TEST_F(SidebarTest, TheMenuOfARequest) {
  const std::uint64_t postId = basics().requests[1].id;
  ASSERT_TRUE(contextMenu(kRequestLabel, "Open", 1));
  ASSERT_EQ(app->tabCount(), 1U);
  EXPECT_EQ(app->activeTab()->requestId, postId);

  ASSERT_TRUE(contextMenu(kRequestLabel, "Performance test", 1));
  ASSERT_EQ(app->tabCount(), 2U);
  EXPECT_NE(app->activeTab()->perf, nullptr);
  EXPECT_EQ(app->activeTab()->requestId, postId);

  ASSERT_TRUE(contextMenu(kRequestLabel, "Rename...", 1));
  ASSERT_TRUE(ModalOpen());
  typeText("Create a dog");
  ASSERT_TRUE(click("Rename"));
  EXPECT_EQ(View(basics().requests[1].name), "Create a dog");

  // The variables of the collection are resolved in the copied command.
  ASSERT_TRUE(contextMenu(kRequestLabel, "Copy as cURL", 1));
  const std::string_view copied = ImGui::GetClipboardText();
  EXPECT_TRUE(copied.starts_with("curl -X POST 'https://httpbin.org/anything?source=zalie'")) << copied;
  EXPECT_EQ(lastToast(), "cURL command copied");

  ASSERT_TRUE(contextMenu(kRequestLabel, "Duplicate", 1));
  ASSERT_EQ(basics().requests.size(), 4U);
  EXPECT_EQ(basics().requests[2].url, basics().requests[1].url);
  EXPECT_NE(basics().requests[2].id, postId);
  EXPECT_EQ(app->activeTab()->requestId, basics().requests[2].id);
  EXPECT_EQ(app->tabCount(), 3U);

  // Deleting a request closes its tab and its Performance tab.
  ASSERT_TRUE(contextMenu(kRequestLabel, "Delete", 1));
  ASSERT_EQ(basics().requests.size(), 3U);
  EXPECT_FALSE(workspace.findRequest(postId).valid());
  ASSERT_EQ(app->tabCount(), 1U);
  EXPECT_EQ(app->activeTab()->requestId, basics().requests[1].id);
}

TEST_F(SidebarTest, DraggingARequestMovesItToAFolderOrToTheRootOfItsCollection) {
  const std::uint64_t teapotId = exampleCollection().root.requests[0].id;
  ASSERT_TRUE(drag(kRequestLabel, kBasicsLabel, kTeapot));
  EXPECT_TRUE(exampleCollection().root.requests.empty());
  ASSERT_EQ(basics().requests.size(), 4U);
  EXPECT_EQ(basics().requests[3].id, teapotId);
  EXPECT_TRUE(workspace[0].dirty);

  const std::uint64_t getId = basics().requests[0].id;
  ASSERT_TRUE(drag(kRequestLabel, collectionLabel(), 0));
  ASSERT_EQ(exampleCollection().root.requests.size(), 1U);
  EXPECT_EQ(exampleCollection().root.requests[0].id, getId);
  EXPECT_EQ(basics().requests.size(), 3U);
}

TEST_F(SidebarTest, TooltipsShowWhatTheRowsHide) {
  ASSERT_TRUE(hover(kRequestLabel, 0));
  EXPECT_FALSE(tooltipShown());  // not before the mouse rests on the row
  waitTooltip();
  EXPECT_TRUE(tooltipShown());
  EXPECT_TRUE(hasTooltip(collectionLabel()));
  EXPECT_TRUE(hasTooltip("##newMenu"));
  // A request without URL has nothing more to show.
  basics().requests[0].url.clear();
  EXPECT_FALSE(hasTooltip(kRequestLabel, 0));
}

TEST_F(SidebarTest, TheFilterKeepsTheMatchingRequestsAndTheirFolders) {
  press(ImGuiMod_Ctrl | ImGuiKey_K);
  Type("teapot");
  frame(3);
  // The folder has no matching request: it is hidden, with its requests.
  EXPECT_FALSE(hasItem(kBasicsLabel));
  EXPECT_NE(findItem(kRequestLabel, 0), nullptr);
  EXPECT_EQ(findItem(kRequestLabel, 1), nullptr);

  typeText("HEADERS");  // the URL matches too, whatever the case
  EXPECT_TRUE(hasItem(kBasicsLabel));
  EXPECT_NE(findItem(kRequestLabel, 0), nullptr);
  EXPECT_EQ(findItem(kRequestLabel, 1), nullptr);

  typeText("basics");  // a matching folder shows nothing more than its matching requests
  EXPECT_TRUE(hasItem(kBasicsLabel));
  EXPECT_EQ(findItem(kRequestLabel, 0), nullptr);

  typeText("nothing matches");
  EXPECT_FALSE(hasItem(collectionLabel()));

  // A request deep in sub-folders shows all of them.
  Folder inner;
  inner.name.assign(std::string_view{"Inner"});
  Request deep;
  deep.name.assign(std::string_view{"Deep one"});
  inner.requests.push_back(std::move(deep));
  basics().folders.push_back(std::move(inner));
  typeText("deep");
  EXPECT_TRUE(hasItem(kBasicsLabel));
  EXPECT_TRUE(hasItem("     Inner"));
  EXPECT_NE(findItem(kRequestLabel, 0), nullptr);
  EXPECT_EQ(findItem(kRequestLabel, 1), nullptr);
}

TEST_F(SidebarTest, TheHistoryIsFilteredAndCleared) {
  for (std::string_view url : {"http://127.0.0.1:1/first", "http://127.0.0.1:1/second"}) {
    ui::RequestTab& tab = app->newScratchTab();
    tab.scratch.url.assign(url);
    app->sendActiveTab();
    ASSERT_TRUE(waitResponse());
  }
  ASSERT_EQ(workspace.history().entries().size(), 2U);
  ASSERT_TRUE(click("History"));
  EXPECT_NE(findItem("##entry", 1), nullptr);

  EXPECT_TRUE(hasTooltip("##entry", 0));

  press(ImGuiMod_Ctrl | ImGuiKey_K);
  Type("FIRST");
  frame(3);
  EXPECT_NE(findItem("##entry", 0), nullptr);
  EXPECT_EQ(findItem("##entry", 1), nullptr);
  // The entry shown is the one of the first request, although it is the second of the history.
  const std::size_t nbTabs = app->tabCount();
  ASSERT_TRUE(click("##entry"));
  ASSERT_EQ(app->tabCount(), nbTabs + 1);
  EXPECT_EQ(View(app->activeTab()->scratch.url), "http://127.0.0.1:1/first");

  ASSERT_TRUE(click("##filter"));
  typeText("");
  EXPECT_NE(findItem("##entry", 1), nullptr);
  ASSERT_TRUE(click("Clear history"));
  EXPECT_TRUE(workspace.history().entries().empty());
  EXPECT_FALSE(hasItem("##entry"));

  ASSERT_TRUE(click("Collections"));
  EXPECT_TRUE(hasItem(collectionLabel()));
}

}  // namespace
}  // namespace zalie
