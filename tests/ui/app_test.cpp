// Tests of the real user interface, rendered headless (Dear ImGui null backend) and driven by simulated input.

#include "zalie/ui/app.hpp"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <aeronet/http-method.hpp>
#include <aeronet/http-request-view.hpp>
#include <aeronet/http-server-config.hpp>
#include <aeronet/router.hpp>
#include <aeronet/single-http-server.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "test-utils.hpp"
#include "ui-test-fixture.hpp"
#include "zalie/collection.hpp"
#include "zalie/exchange.hpp"
#include "zalie/executor.hpp"
#include "zalie/history.hpp"
#include "zalie/method.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/request.hpp"
#include "zalie/text-document.hpp"
#include "zalie/ui/embedded.hpp"
#include "zalie/workspace.hpp"

namespace zalie {
namespace {

using namespace std::chrono_literals;

// The window (child windows included) of this frame whose name contains 'part' ("parent/##child_1234ABCD").
const ImGuiWindow* ActiveWindow(std::string_view part) {
  for (const ImGuiWindow* pWindow : ImGui::GetCurrentContext()->Windows) {
    if (pWindow->Active && std::string_view(pWindow->Name).contains(part)) {
      return pWindow;
    }
  }
  return nullptr;
}

class AppTest : public test::ImGuiContextFixture {
 protected:
  AppTest() : workspace(home.path()) {
    workspace.load();  // first launch: the example collection
    app = std::make_unique<ui::App>(workspace, executor);
    app->init(1.0F);
  }

  ~AppTest() override {
    app.reset();  // before the Dear ImGui context (it unregisters its textures)
  }

  void frame(int count = 1) {
    for (int i = 0; i < count; ++i) {
      app->beforeNewFrame();
      runFrame([this] { app->frame(); });
    }
  }

  void press(ImGuiKeyChord chord) {
    Key(chord, true);
    frame();
    Key(chord, false);
    frame();
  }

  // Waits (rendering frames) until the active tab has a response.
  bool waitResponse() {
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    while (std::chrono::steady_clock::now() < deadline) {
      frame();
      const ui::RequestTab* pTab = app->activeTab();
      if (pTab != nullptr && !pTab->pending() && pTab->exchange) {
        return true;
      }
      std::this_thread::sleep_for(5ms);
    }
    return false;
  }

  // A response with the given body, shown in the active tab.
  void showBody(std::string_view contentType, std::string_view body) {
    Exchange exchange;
    exchange.request.url = "http://example.com/x";
    Response& response = exchange.response;
    response.status = 200;
    response.reason = "OK";
    response.state = ExchangeState::completed;
    response.headers.push_back({"content-type", contentType});
    response.body.raw.append(body);
    response.body.analyze(contentType);
    app->showExchange(*app->activeTab(), std::move(exchange));
  }

  std::uint64_t exampleRequestId(std::size_t index) {
    return workspace[0].collection.root.folders[0].requests[static_cast<std::uint32_t>(index)].id;
  }

  // Shows the history in the sidebar and double clicks its last entry.
  void doubleClickLastHistoryEntry() {
    app->showHistory(true);
    frame(3);
    const ImGuiWindow* pList = ActiveWindow("/##sideContent_");
    ASSERT_NE(pList, nullptr);
    // The entries are the last items of the list.
    MouseMove(ImVec2(pList->Pos.x + 40.0F, pList->DC.CursorMaxPos.y - 4.0F));
    frame();
    for (int click = 0; click < 2; ++click) {
      MouseButton(true);
      frame();
      MouseButton(false);
      frame();
    }
  }

  test::TempDir home;
  Workspace workspace;
  Executor executor;
  std::unique_ptr<ui::App> app;
};

TEST_F(AppTest, WelcomeScreen) {
  frame(3);
  EXPECT_EQ(app->tabCount(), 0U);
  EXPECT_GT(LastFrameVertices(), 1000);
  EXPECT_FALSE(app->animating());
}

TEST_F(AppTest, OpenRequestsAndEverySection) {
  for (std::size_t i = 0; i < 3; ++i) {
    app->openRequest(exampleRequestId(i));
    frame(2);
  }
  ASSERT_EQ(app->tabCount(), 3U);
  app->openRequest(exampleRequestId(1));  // already open: focused, not duplicated
  frame();
  EXPECT_EQ(app->tabCount(), 3U);
  EXPECT_EQ(app->activeTabIndex(), 1U);

  ui::RequestTab& tab = *app->activeTab();
  for (ui::RequestSection section :
       {ui::RequestSection::headers, ui::RequestSection::body, ui::RequestSection::auth, ui::RequestSection::settings,
        ui::RequestSection::docs, ui::RequestSection::params}) {
    tab.requestSection = section;
    tab.selectRequestSection = true;
    frame(2);
    EXPECT_EQ(tab.requestSection, section);
  }
  // Every authentication scheme renders its fields.
  Request& request = *app->requestOf(tab);
  for (AuthMode mode : {AuthMode::basic, AuthMode::bearer, AuthMode::apiKey, AuthMode::none, AuthMode::inherit}) {
    request.auth.mode = mode;
    tab.requestSection = ui::RequestSection::auth;
    tab.selectRequestSection = true;
    frame(2);
  }
  // Every body mode too.
  for (BodyMode mode : {BodyMode::text, BodyMode::xml, BodyMode::formUrlEncoded, BodyMode::none, BodyMode::json}) {
    request.bodyMode = mode;
    tab.requestSection = ui::RequestSection::body;
    tab.selectRequestSection = true;
    frame(2);
  }
}

TEST_F(AppTest, ShortcutsManageTabs) {
  frame(2);  // shortcut routes are registered during a frame and resolved at the next one
  press(ImGuiMod_Ctrl | ImGuiKey_T);
  EXPECT_EQ(app->tabCount(), 1U);
  press(ImGuiMod_Ctrl | ImGuiKey_T);
  EXPECT_EQ(app->tabCount(), 2U);
  EXPECT_EQ(app->activeTabIndex(), 1U);
  press(ImGuiMod_Ctrl | ImGuiKey_Tab);
  EXPECT_EQ(app->activeTabIndex(), 0U);
  press(ImGuiMod_Ctrl | ImGuiKey_W);
  EXPECT_EQ(app->tabCount(), 1U);
  // Sidebar / inspector toggles.
  const bool sidebar = workspace.settings().sidebarVisible;
  press(ImGuiMod_Ctrl | ImGuiKey_B);
  EXPECT_NE(workspace.settings().sidebarVisible, sidebar);
  press(ImGuiMod_Ctrl | ImGuiKey_J);
  EXPECT_FALSE(workspace.settings().inspectorVisible);
  press(ImGuiMod_Ctrl | ImGuiKey_B);
  press(ImGuiMod_Ctrl | ImGuiKey_J);
  // Zoom in and back.
  press(ImGuiMod_Ctrl | ImGuiKey_Equal);
  EXPECT_FLOAT_EQ(workspace.settings().uiScale, 1.1F);
  press(ImGuiMod_Ctrl | ImGuiKey_0);
  EXPECT_FLOAT_EQ(workspace.settings().uiScale, 1.0F);
}

TEST_F(AppTest, ShortcutsUseCmdWithMacOSBehaviors) {
  // The default on macOS, checked on every platform: Dear ImGui swaps Cmd and Ctrl, the ImGuiMod_Ctrl shortcuts are
  // pressed with Cmd.
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigMacOSXBehaviors = true;
  frame(2);
  io.AddKeyEvent(ImGuiMod_Ctrl, true);  // the physical Ctrl key
  io.AddKeyEvent(ImGuiKey_T, true);
  frame();
  io.AddKeyEvent(ImGuiKey_T, false);
  io.AddKeyEvent(ImGuiMod_Ctrl, false);
  frame();
  EXPECT_EQ(app->tabCount(), 0U);
  press(ImGuiMod_Ctrl | ImGuiKey_T);  // Cmd+T (Key() presses Cmd with these behaviors)
  EXPECT_EQ(app->tabCount(), 1U);
}

TEST_F(AppTest, SendToLocalServerAndShowEveryResponseView) {
  aeronet::Router router;
  router.setPath(aeronet::http::Method::GET, "/pets", [](const aeronet::HttpRequestView& req) {
    std::this_thread::sleep_for(100ms);  // long enough to render the waiting animation
    return req.makeResponse(200, R"([{"name":"Zalie","tricks":["fetch","sit"]},{"name":"Physalie"}])",
                            "application/json");
  });
  aeronet::SingleHttpServer server(aeronet::HttpServerConfig{}.withPort(0).withPollInterval(20ms), std::move(router));
  server.start();

  ui::RequestTab& tab = app->newScratchTab();
  tab.scratch.url.assign(View(Concat({"http://127.0.0.1:", std::string_view(std::to_string(server.port())), "/pets"})));
  frame();
  press(ImGuiMod_Ctrl | ImGuiKey_Enter);
  EXPECT_TRUE(app->animating());
  ASSERT_TRUE(waitResponse());
  const Response& response = tab.exchange->response;
  ASSERT_TRUE(response.ok()) << response.error;
  EXPECT_EQ(response.status, 200);
  EXPECT_EQ(response.body.kind, ContentKind::json);
  EXPECT_EQ(workspace.history().entries().size(), 1U);

  for (ui::ResponseSection section :
       {ui::ResponseSection::headers, ui::ResponseSection::sent, ui::ResponseSection::body}) {
    tab.responseSection = section;
    frame(2);
  }
  EXPECT_FALSE(tab.sentText.empty());
  EXPECT_TRUE(View(tab.sentText).starts_with("GET /pets HTTP/1.1"));
  tab.prettyBody = false;
  frame(2);

  // Find in the body.
  tab.bodyView.query.assign(std::string_view{"zalie"});
  frame(2);
  EXPECT_EQ(tab.bodyView.matches.size(), 1U);
}

TEST_F(AppTest, CancelPendingRequestWithEscape) {
  aeronet::Router router;
  router.setPath(aeronet::http::Method::GET, "/slow", [](const aeronet::HttpRequestView& req) {
    std::this_thread::sleep_for(300ms);
    return req.makeResponse(200, "late", "text/plain");
  });
  aeronet::SingleHttpServer server(aeronet::HttpServerConfig{}.withPort(0).withPollInterval(20ms), std::move(router));
  server.start();
  ui::RequestTab& tab = app->newScratchTab();
  tab.scratch.url.assign(View(Concat({"http://127.0.0.1:", std::string_view(std::to_string(server.port())), "/slow"})));
  app->sendActiveTab();
  frame(3);
  ASSERT_TRUE(tab.pending());
  press(ImGuiKey_Escape);
  ASSERT_TRUE(waitResponse());
  EXPECT_EQ(tab.exchange->response.state, ExchangeState::cancelled);
}

TEST_F(AppTest, FailedRequestShowsTheError) {
  ui::RequestTab& tab = app->newScratchTab();
  tab.scratch.url.assign(std::string_view{"http://127.0.0.1:1/"});
  app->sendActiveTab();
  ASSERT_TRUE(waitResponse());
  EXPECT_EQ(tab.exchange->response.state, ExchangeState::failed);
  frame(2);
  // An empty URL is refused with a toast, nothing is sent.
  app->newScratchTab();
  app->sendActiveTab();
  EXPECT_FALSE(app->activeTab()->pending());
  EXPECT_TRUE(app->toasts().back().error);
}

TEST_F(AppTest, PasteCurlCommandInUrlBar) {
  ui::RequestTab& tab = app->newScratchTab();
  frame(3);  // the URL input takes the focus
  ImGui::SetClipboardText("curl -X PUT 'https://api.example.com/items/7?force=1' -H 'X-Team: core' --data-raw 'a=b'");
  press(ImGuiMod_Ctrl | ImGuiKey_V);
  frame(2);
  const Request& request = tab.scratch;
  EXPECT_EQ(request.method, Method::put);
  EXPECT_EQ(View(request.url), "https://api.example.com/items/7?force=1");
  ASSERT_EQ(request.params.size(), 1U);
  ASSERT_EQ(request.headers.size(), 1U);
  EXPECT_EQ(request.bodyMode, BodyMode::formUrlEncoded);
}

TEST_F(AppTest, TypingInTheUrlBarUpdatesTheParams) {
  ui::RequestTab& tab = app->newScratchTab();
  frame(3);
  Type("localhost:8080/search?q=zalie");
  frame(2);
  EXPECT_EQ(View(tab.scratch.url), "localhost:8080/search?q=zalie");
  ASSERT_EQ(tab.scratch.params.size(), 1U);
  EXPECT_EQ(View(tab.scratch.params[0].value), "zalie");
}

TEST_F(AppTest, EditingASavedRequestMarksItsCollectionDirty) {
  app->openRequest(exampleRequestId(0));
  frame(2);
  app->activeTab()->focusUrl = true;
  frame(2);
  Type("/more");
  frame(2);
  EXPECT_TRUE(workspace[0].dirty);
  EXPECT_TRUE(View(workspace[0].collection.root.folders[0].requests[0].url).ends_with("/more"));
}

TEST_F(AppTest, LargeBodiesStayFast) {
  app->newScratchTab();
  std::string json = "[";
  for (int i = 0; i < 60000; ++i) {
    json.append(i == 0 ? "" : ",").append(R"({"id":)").append(std::to_string(i)).append(R"(,"name":"item"})");
  }
  json.push_back(']');
  showBody("application/json", json);  // about 1.5 MB, 300000 pretty lines
  frame(2);
  const auto start = std::chrono::steady_clock::now();
  static constexpr int kFrames = 10;
  frame(kFrames);
  const auto perFrame = (std::chrono::steady_clock::now() - start) / kFrames;
  // Only the visible lines are laid out: a frame costs the same for 1 KB or 100 MB (generous bound for debug builds).
  EXPECT_LT(perFrame, 50ms);
  EXPECT_GT(app->activeTab()->exchange->response.body.lines(true).size(), 100000U);
}

TEST_F(AppTest, ImageBinaryAndTextBodies) {
  app->newScratchTab();
  const auto png = embedded::LogoSmallPng();
  showBody("image/png", std::string_view(reinterpret_cast<const char*>(png.data()), png.size()));
  frame(3);
  EXPECT_NE(app->activeTab()->pImage, nullptr);

  showBody("application/octet-stream", std::string_view("\x01\x02\0binary\xff", 10));
  frame(3);
  EXPECT_EQ(app->activeTab()->pImage, nullptr);  // released when the exchange changes

  showBody("text/html", "<html>\n<body class=\"x\">Hi</body>\n</html>");
  frame(2);
  showBody("text/plain", "");
  frame(2);
}

TEST_F(AppTest, ImportsAndDialogs) {
  const std::filesystem::path data(ZALIE_TEST_DATA_DIR);
  EXPECT_TRUE(app->importPath(data / "bruno-collection"));
  EXPECT_TRUE(app->importPath(data / "postman-collection.json"));
  EXPECT_FALSE(app->importPath(data / "does-not-exist.json"));
  EXPECT_EQ(workspace.size(), 3U);
  // A Bruno collection through its bruno.json file.
  EXPECT_TRUE(app->importPath(data / "bruno-collection" / "bruno.json"));
  EXPECT_EQ(workspace.size(), 4U);
  // A single Bruno request: a new unsaved tab, nothing added to the workspace.
  const std::size_t nbTabs = app->tabCount();
  EXPECT_TRUE(app->importPath(data / "bruno-collection" / "users" / "Create user.bru"));
  EXPECT_EQ(app->tabCount(), nbTabs + 1);
  ASSERT_NE(app->activeTab(), nullptr);
  EXPECT_EQ(app->activeTab()->requestId, 0U);
  EXPECT_EQ(View(app->activeTab()->scratch.name), "Create user");
  EXPECT_TRUE(app->toasts().back().error);  // it uses {{variables}}: hint to import the whole collection
  EXPECT_FALSE(app->importPath(data / "bruno-collection" / "users" / "folder.bru"));
  EXPECT_EQ(workspace.size(), 4U);
  frame(3);

  for (ui::App::Dialog dialog :
       {ui::App::Dialog::import, ui::App::Dialog::newCollection, ui::App::Dialog::rename,
        ui::App::Dialog::collectionSettings, ui::App::Dialog::settings, ui::App::Dialog::about}) {
    app->openDialog(dialog, 1);
    frame(3);
    press(ImGuiKey_Escape);
    frame();
  }
  // Dropped files go through the same importers.
  const std::filesystem::path dropped[] = {data / "nope"};
  app->onFilesDropped(dropped);
  EXPECT_TRUE(app->toasts().back().error);
}

TEST_F(AppTest, FilterTreeAndHistory) {
  ui::RequestTab& tab = app->newScratchTab();
  tab.scratch.url.assign(std::string_view{"http://127.0.0.1:1/"});
  app->sendActiveTab();
  ASSERT_TRUE(waitResponse());
  press(ImGuiMod_Ctrl | ImGuiKey_K);  // focuses the filter
  Type("json");
  frame(3);
  EXPECT_EQ(workspace.history().entries().size(), 1U);
}

TEST_F(AppTest, DoubleClickOnHistoryPutsTheSentRequestBack) {
  // The example request was sent with another method and URL, edited since.
  app->openRequest(exampleRequestId(0));
  workspace.history().add(HistoryEntry{.url = RawChars32(std::string_view{"http://localhost:34609/old?q=1"}),
                                       .requestId = exampleRequestId(0),
                                       .method = Method::post});
  doubleClickLastHistoryEntry();
  ASSERT_EQ(app->tabCount(), 1U);
  EXPECT_EQ(app->activeTab()->requestId, exampleRequestId(0));
  const Request& request = workspace[0].collection.root.folders[0].requests[0];
  EXPECT_EQ(request.method, Method::post);
  EXPECT_EQ(View(request.url), "http://localhost:34609/old?q=1");
  ASSERT_EQ(request.params.size(), 1U);
  EXPECT_EQ(View(request.params[0].key), "q");
  EXPECT_TRUE(workspace[0].dirty);
}

TEST_F(AppTest, DoubleClickOnAnUnsavedHistoryEntryOpensItOnce) {
  workspace.history().add(
      HistoryEntry{.url = RawChars32(std::string_view{"http://127.0.0.1:1/x?a=b"}), .method = Method::put});
  doubleClickLastHistoryEntry();
  ASSERT_EQ(app->tabCount(), 1U);
  const ui::RequestTab& tab = *app->activeTab();
  EXPECT_EQ(tab.requestId, 0U);
  EXPECT_EQ(tab.scratch.method, Method::put);
  EXPECT_EQ(View(tab.scratch.url), "http://127.0.0.1:1/x?a=b");
  EXPECT_EQ(tab.scratch.params.size(), 1U);
}

TEST_F(AppTest, CopyingAHistoryEntryKeepsTheVariablesOfARequestSendingIt) {
  app->openRequest(exampleRequestId(0));  // {{baseUrl}}/json
  workspace.history().add(
      HistoryEntry{.url = RawChars32(std::string_view{"https://httpbin.org/json"}), .requestId = exampleRequestId(0)});
  frame(2);
  app->copyHistoryEntry(0);
  EXPECT_EQ(View(workspace[0].collection.root.folders[0].requests[0].url), "{{baseUrl}}/json");
  EXPECT_FALSE(workspace[0].dirty);
  // Without any tab: a new unsaved request.
  app->closeTab(0);
  app->copyHistoryEntry(0);
  ASSERT_EQ(app->tabCount(), 1U);
  EXPECT_EQ(View(app->activeTab()->scratch.url), "https://httpbin.org/json");
  app->copyHistoryEntry(1);  // no such entry
  app->openHistoryEntry(1);
  EXPECT_EQ(app->tabCount(), 1U);
}

TEST_F(AppTest, ShutdownRestoresOpenTabs) {
  app->openRequest(exampleRequestId(2));
  app->openRequest(exampleRequestId(0));
  app->newScratchTab();  // not restored: unsaved
  frame(2);
  app->shutdown();
  app.reset();

  Workspace reloaded(home.path());
  reloaded.load();
  ASSERT_EQ(reloaded.state().openTabs.size(), 2U);
  EXPECT_EQ(reloaded.state().openTabs[0], exampleRequestId(2));
  ui::App restored(reloaded, executor);
  restored.init(1.0F);
  EXPECT_EQ(restored.tabCount(), 2U);
  app = std::make_unique<ui::App>(workspace, executor);  // for the fixture teardown
}

TEST_F(AppTest, JsonPathFilter) {
  ui::RequestTab& tab = app->newScratchTab();
  showBody("application/json", R"({"items": [{"id": 1, "name": "a"}, {"id": 2, "name": "b"}]})");
  frame(2);
  tab.jsonFilter.assign(std::string_view{"$.items[*].id"});
  frame(2);
  EXPECT_FALSE(tab.filterError);
  EXPECT_EQ(View(tab.filterMessage), "2 matches for $.items[*].id");
  EXPECT_EQ(View(tab.filtered), "[1,2]");
  EXPECT_EQ(View(tab.filteredPretty), "[\n  1,\n  2\n]");
  // Find searches the filtered text.
  tab.bodyView.query.assign(std::string_view{"2"});
  frame(2);
  EXPECT_EQ(tab.bodyView.matches.size(), 1U);

  // An invalid filter tells why, and the whole body is shown.
  tab.jsonFilter.assign(std::string_view{"$.items["});
  frame(2);
  EXPECT_TRUE(tab.filterError);
  EXPECT_TRUE(View(tab.filterMessage).starts_with("missing ']'")) << View(tab.filterMessage);

  // The filter is kept, and applied to the next responses.
  tab.jsonFilter.assign(std::string_view{"  $.items[-1].name "});
  frame(2);
  EXPECT_EQ(View(tab.filtered), R"("b")");
  showBody("application/json", R"({"items": [{"name": "c"}]})");
  frame(2);
  EXPECT_EQ(View(tab.filtered), R"("c")");
  EXPECT_EQ(View(tab.filterMessage), "1 match for $.items[-1].name");

  // Cleared: the whole body again.
  tab.jsonFilter.clear();
  frame(2);
  EXPECT_TRUE(tab.filtered.empty());
  EXPECT_TRUE(tab.filterMessage.empty());
}

TEST_F(AppTest, JsonPathFilterOfALargeBodyWaitsForEnter) {
  ui::RequestTab& tab = app->newScratchTab();
  std::string big = "[";
  for (int i = 0; i < 300000; ++i) {
    big += (i == 0 ? "" : ",") + std::to_string(i);
  }
  big += ']';
  showBody("application/json", big);
  frame(2);
  tab.jsonFilter.assign(std::string_view{"$[-1]"});
  frame(2);
  // Not applied while typing (the body is larger than 1 MiB)...
  EXPECT_EQ(View(tab.filterMessage), "Press Enter to apply the filter");
  EXPECT_TRUE(tab.filtered.empty());
  // ... but applied at once to a new response.
  showBody("application/json", big);
  frame(2);
  EXPECT_EQ(View(tab.filtered), "299999");
}

TEST_F(AppTest, BreadcrumbAndResponseBesideTheRequest) {
  app->openRequest(exampleRequestId(0));
  frame(2);
  EXPECT_EQ(app->breadcrumb(), "Zalie examples / Basics / ");
  showBody("application/json", R"({"a": 1})");

  workspace.settings().responseBeside = true;
  frame(3);
  const ImGuiWindow* pEditor = ActiveWindow("/##editor_");
  const ImGuiWindow* pResponse = ActiveWindow("/##response_");
  ASSERT_NE(pEditor, nullptr);
  ASSERT_NE(pResponse, nullptr);
  EXPECT_GE(pResponse->Pos.x, pEditor->Pos.x + pEditor->Size.x);
  EXPECT_FLOAT_EQ(pResponse->Pos.y, pEditor->Pos.y);

  // Not enough room for two columns (a very wide inspector): below again.
  workspace.settings().inspectorWidth = 5000.0F;
  frame(3);
  EXPECT_FLOAT_EQ(pResponse->Pos.x, pEditor->Pos.x);
  EXPECT_GE(pResponse->Pos.y, pEditor->Pos.y + pEditor->Size.y);

  app->newScratchTab();
  frame(2);
  EXPECT_EQ(app->breadcrumb(), "Unsaved request / ");
}

TEST_F(AppTest, DarkThemeAndHiDpi) {
  workspace.settings().theme.assign(std::string_view{"nuit"});
  app = std::make_unique<ui::App>(workspace, executor);
  app->init(2.0F);
  app->openRequest(exampleRequestId(1));
  frame(3);
  EXPECT_GT(LastFrameVertices(), 1000);
  app->setDpiScale(1.5F);
  frame(2);
  EXPECT_FLOAT_EQ(ImGui::GetStyle().FontScaleDpi, 1.5F);
}

}  // namespace
}  // namespace zalie
