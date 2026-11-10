// The top bar and the dialogs, driven as the user does.

#include <gtest/gtest.h>
#include <imgui.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "app-test-fixture.hpp"
#include "zalie/collection.hpp"
#include "zalie/file-io.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/request.hpp"
#include "zalie/settings.hpp"
#include "zalie/ui/app.hpp"
#include "zalie/workspace.hpp"

namespace zalie {
namespace {

class DialogsTest : public AppTest {
 protected:
  DialogsTest() { frame(3); }

  void open(ui::App::Dialog dialog) {
    app->openDialog(dialog, 0);
    frame(4);
    ASSERT_TRUE(ModalOpen());
  }

  [[nodiscard]] static std::string DataPath(std::string_view name) {
    return (std::filesystem::path(ZALIE_TEST_DATA_DIR) / name).string();
  }
};

TEST_F(DialogsTest, TheMainMenu) {
  ASSERT_TRUE(choose("##menu", "New request"));
  EXPECT_EQ(app->tabCount(), 1U);

  ASSERT_TRUE(choose("##menu", "New collection..."));
  ASSERT_TRUE(hasItem("Create"));
  typeText("Pets API");
  ASSERT_TRUE(click("Create"));
  EXPECT_FALSE(ModalOpen());
  ASSERT_EQ(workspace.size(), 2U);
  EXPECT_EQ(View(workspace[1].collection.name), "Pets API");
  // The collection starts with a request, opened and ready to be typed.
  ASSERT_EQ(workspace[1].collection.root.requests.size(), 1U);
  EXPECT_EQ(app->activeTab()->requestId, workspace[1].collection.root.requests[0].id);
  EXPECT_EQ(workspace[1].path.filename(), "pets-api.zalie.json");

  ASSERT_TRUE(choose("##menu", "Import..."));
  EXPECT_TRUE(hasItem("Import collection"));
  ASSERT_TRUE(click("Close"));

  ASSERT_TRUE(choose("##menu", "Save all"));
  EXPECT_EQ(lastToast(), "Saved");
  EXPECT_FALSE(workspace[1].dirty);
  EXPECT_TRUE(std::filesystem::exists(workspace[1].path));

  ASSERT_TRUE(choose("##menu", "Settings..."));
  EXPECT_TRUE(hasItem("Nuit (dark)"));
  ASSERT_TRUE(click("Close"));

  ASSERT_TRUE(choose("##menu", "About Zalie"));
  EXPECT_TRUE(ModalOpen());
  ASSERT_TRUE(click("Close"));
  EXPECT_FALSE(ModalOpen());
}

TEST_F(DialogsTest, TheButtonsOfTheTopBarToggleThePanelsAndTheTheme) {
  Settings& settings = workspace.settings();
  ASSERT_TRUE(settings.sidebarVisible);
  ASSERT_TRUE(click("##toggleSidebar"));
  EXPECT_FALSE(settings.sidebarVisible);
  EXPECT_EQ(ActiveWindow("/##sidebar_"), nullptr);
  ASSERT_TRUE(click("##toggleSidebar"));
  EXPECT_TRUE(settings.sidebarVisible);
  EXPECT_NE(ActiveWindow("/##sidebar_"), nullptr);

  const bool beside = settings.responseBeside;
  ASSERT_TRUE(click("##toggleLayout"));
  EXPECT_EQ(settings.responseBeside, !beside);

  const bool inspector = settings.inspectorVisible;
  ASSERT_TRUE(click("##toggleInspector"));
  EXPECT_EQ(settings.inspectorVisible, !inspector);

  ASSERT_TRUE(click("##theme"));
  EXPECT_EQ(View(settings.theme), "nuit");
  ASSERT_TRUE(click("##theme"));
  EXPECT_EQ(View(settings.theme), "coton");
  EXPECT_TRUE(hasTooltip("##theme"));
}

TEST_F(DialogsTest, TheEnvironmentOfTheActiveRequest) {
  app->newScratchTab();
  frame(2);
  EXPECT_FALSE(hasItem("##environment"));  // an unsaved request has no collection, hence no environment

  app->openRequest(exampleRequestId(0));
  frame(2);
  ASSERT_EQ(workspace[0].activeEnvironment, -1);
  ASSERT_TRUE(choose("##environment", "local"));
  EXPECT_EQ(workspace[0].activeEnvironment, 0);
  ASSERT_TRUE(choose("##environment", "No environment"));
  EXPECT_EQ(workspace[0].activeEnvironment, -1);

  ASSERT_TRUE(choose("##environment", "Manage environments..."));
  ASSERT_TRUE(ModalOpen());
  EXPECT_TRUE(hasItem("Environments"));
}

TEST_F(DialogsTest, ImportingACollectionFromItsPath) {
  open(ui::App::Dialog::import);
  // Nothing typed: nothing imported, the dialog stays.
  ASSERT_TRUE(click("Import collection"));
  EXPECT_TRUE(ModalOpen());
  EXPECT_EQ(workspace.size(), 1U);

  ASSERT_TRUE(click("##path"));
  typeText("/no/such/collection.json");
  ASSERT_TRUE(click("Import collection"));
  EXPECT_TRUE(ModalOpen());
  ASSERT_FALSE(app->toasts().empty());
  EXPECT_TRUE(app->toasts().back().error);

  ASSERT_TRUE(click("##path"));
  typeText("  " + DataPath("postman-collection.json") + " ");
  press(ImGuiKey_Enter);
  EXPECT_FALSE(ModalOpen());
  EXPECT_EQ(workspace.size(), 2U);
}

TEST_F(DialogsTest, CreatingARequestFromACurlCommand) {
  open(ui::App::Dialog::import);
  ASSERT_TRUE(click("Create request"));  // nothing pasted
  EXPECT_TRUE(ModalOpen());
  EXPECT_EQ(app->tabCount(), 0U);

  ASSERT_TRUE(click("##curl"));
  typeText("wget https://example.com");
  ASSERT_TRUE(click("Create request"));
  EXPECT_TRUE(ModalOpen());
  EXPECT_EQ(lastToast(), "not a curl command");
  EXPECT_TRUE(app->toasts().back().error);

  // A command with an option Zalie has no equivalent for: imported, with a warning.
  ASSERT_TRUE(click("##curl"));
  typeText("curl -X PUT https://example.com/pets/7 -H 'X-Pet: zalie' --proxy http://localhost:3128");
  ASSERT_TRUE(click("Create request"));
  EXPECT_FALSE(ModalOpen());
  ASSERT_EQ(app->tabCount(), 1U);
  const Request& request = app->activeTab()->scratch;
  EXPECT_EQ(request.method, Method::put);
  EXPECT_EQ(View(request.url), "https://example.com/pets/7");
  ASSERT_EQ(request.headers.size(), 1U);
  EXPECT_EQ(View(request.headers[0].value), "zalie");
  ASSERT_GE(app->toasts().size(), 2U);
  EXPECT_EQ(lastToast(), "Request created from the curl command");
  EXPECT_TRUE(app->toasts()[app->toasts().size() - 2].error);
}

TEST_F(DialogsTest, ANewCollectionNeedsAName) {
  open(ui::App::Dialog::newCollection);
  typeText("   ");
  ASSERT_TRUE(click("Create"));
  EXPECT_TRUE(ModalOpen());
  EXPECT_EQ(workspace.size(), 1U);
  ASSERT_TRUE(click("##name"));
  typeText("Orders");
  press(ImGuiKey_Enter);
  EXPECT_FALSE(ModalOpen());
  ASSERT_EQ(workspace.size(), 2U);
  EXPECT_EQ(View(workspace[1].collection.name), "Orders");
}

TEST_F(DialogsTest, TheEnvironmentsOfACollection) {
  OpenCollection& collection = workspace[0];
  auto& environments = collection.collection.environments;
  open(ui::App::Dialog::collectionSettings);
  ASSERT_TRUE(click("Environments"));
  ASSERT_TRUE(hasItem("local"));

  ASSERT_TRUE(click("+ Add environment"));
  ASSERT_EQ(environments.size(), 2U);
  EXPECT_EQ(View(environments[1].name), "new environment");
  ASSERT_TRUE(click("Name"));
  typeText("staging");
  EXPECT_EQ(View(environments[1].name), "staging");

  ASSERT_TRUE(click("Activate"));
  EXPECT_EQ(collection.activeEnvironment, 1);
  EXPECT_TRUE(hasItem("staging  (active)"));
  ASSERT_TRUE(click("Deactivate"));
  EXPECT_EQ(collection.activeEnvironment, -1);
  ASSERT_TRUE(click("Activate"));

  // A variable of the environment, typed in the empty row of its table.
  ASSERT_TRUE(click("##key"));
  typeText("baseUrl");
  ASSERT_EQ(environments[1].variables.size(), 1U);
  ASSERT_TRUE(click("##value"));
  typeText("https://staging.example.com");
  EXPECT_EQ(View(environments[1].variables[0].value), "https://staging.example.com");

  // Deleting an environment before the active one keeps the active one.
  ASSERT_TRUE(click("local"));
  ASSERT_TRUE(click("Delete"));
  ASSERT_EQ(environments.size(), 1U);
  EXPECT_EQ(View(environments[0].name), "staging");
  EXPECT_EQ(collection.activeEnvironment, 0);
  // Deleting the active one leaves no active environment.
  ASSERT_TRUE(click("Delete"));
  EXPECT_TRUE(environments.empty());
  EXPECT_EQ(collection.activeEnvironment, -1);
  EXPECT_FALSE(hasItem("Delete"));
  EXPECT_TRUE(collection.dirty);

  ASSERT_TRUE(click("Done"));
  EXPECT_FALSE(ModalOpen());
}

TEST_F(DialogsTest, TheVariablesHeadersAuthAndDocsOfACollection) {
  Collection& collection = exampleCollection();
  open(ui::App::Dialog::collectionSettings);
  // Variables: disable the existing one, remove it.
  ASSERT_EQ(collection.variables.size(), 1U);
  ASSERT_TRUE(click("##on"));
  EXPECT_FALSE(collection.variables[0].enabled);
  ASSERT_TRUE(click("##del"));
  EXPECT_TRUE(collection.variables.empty());

  ASSERT_TRUE(click("Headers"));
  ASSERT_TRUE(click("##key"));
  typeText("X-Team");
  ASSERT_EQ(collection.headers.size(), 1U);
  EXPECT_EQ(View(collection.headers[0].key), "X-Team");

  ASSERT_TRUE(click("Auth"));
  ASSERT_TRUE(click("Type"));
  EXPECT_FALSE(hasItem("Inherit from the collection"));  // a collection has nothing to inherit from
  ASSERT_TRUE(click("Bearer token"));
  EXPECT_EQ(collection.auth.mode, AuthMode::bearer);
  ASSERT_TRUE(click("Token"));
  typeText("{{token}}");
  EXPECT_EQ(View(collection.auth.token), "{{token}}");

  ASSERT_TRUE(click("Docs"));
  ASSERT_TRUE(click("##collectionDocs"));
  typeText("Everything about pets.");
  EXPECT_EQ(View(collection.docs), "Everything about pets.");
  EXPECT_TRUE(workspace[0].dirty);

  // The dialog of a collection closed meanwhile closes itself.
  workspace.closeCollection(0);
  frame(2);
  EXPECT_FALSE(ModalOpen());
}

TEST_F(DialogsTest, TheSettings) {
  Settings& settings = workspace.settings();
  open(ui::App::Dialog::settings);
  ASSERT_TRUE(click("Nuit (dark)"));
  EXPECT_EQ(View(settings.theme), "nuit");
  ASSERT_TRUE(click("Coton (light)"));
  EXPECT_EQ(View(settings.theme), "coton");

  const bool autosave = settings.autosave;
  ASSERT_TRUE(click("Save collections automatically"));
  EXPECT_EQ(settings.autosave, !autosave);
  const bool pretty = settings.prettyJson;
  ASSERT_TRUE(click("Pretty-print JSON responses by default"));
  EXPECT_EQ(settings.prettyJson, !pretty);

  // Out of range values are brought back in range.
  ASSERT_TRUE(click("Parallel requests"));
  typeText("500");
  press(ImGuiKey_Enter);
  EXPECT_EQ(settings.workers, 32U);

  // Closing the dialog saves the settings.
  ASSERT_TRUE(click("Close"));
  EXPECT_FALSE(ModalOpen());
  const auto saved = ReadFile(workspace.home() / "settings.json");
  ASSERT_TRUE(saved.has_value());
  EXPECT_TRUE(View(*saved).contains("\"workers\": 32")) << View(*saved);
}

TEST_F(DialogsTest, TheZoomIsAppliedOnceTheSliderIsReleased) {
  Settings& settings = workspace.settings();
  open(ui::App::Dialog::settings);
  const float uiScale = settings.uiScale;
  ASSERT_TRUE(hover("Zoom (Ctrl+= / Ctrl+-)"));
  MouseButton(true);
  frame(2);
  EXPECT_EQ(settings.uiScale, uiScale);  // the interface does not rescale under the mouse
  MouseButton(false);
  frame(2);
  EXPECT_GT(settings.uiScale, uiScale);  // the middle of the slider
  EXPECT_LE(settings.uiScale, 2.0F);
  // Escape closes the dialog, and saves too.
  press(ImGuiKey_Escape);
  EXPECT_FALSE(ModalOpen());
  EXPECT_TRUE(std::filesystem::exists(workspace.home() / "settings.json"));
}

}  // namespace
}  // namespace zalie
