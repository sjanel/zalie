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
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "app-test-fixture.hpp"
#include "test-utils.hpp"
#include "zalie/collection.hpp"
#include "zalie/exchange.hpp"
#include "zalie/file-io.hpp"
#include "zalie/history.hpp"
#include "zalie/latency-histogram.hpp"
#include "zalie/load-runner.hpp"
#include "zalie/load-settings.hpp"
#include "zalie/method.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/request.hpp"
#include "zalie/text-document.hpp"
#include "zalie/ui/embedded.hpp"
#include "zalie/ui/perf-tab.hpp"
#include "zalie/url.hpp"
#include "zalie/workspace.hpp"

namespace zalie {
namespace {

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
  press(ImGuiMod_Ctrl | ImGuiKey_Minus);
  EXPECT_FLOAT_EQ(workspace.settings().uiScale, 0.9F);
  press(ImGuiMod_Ctrl | ImGuiKey_0);
  // The previous tab, the URL bar, saving.
  press(ImGuiMod_Ctrl | ImGuiKey_T);
  ASSERT_EQ(app->activeTabIndex(), 1U);
  press(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Tab);
  EXPECT_EQ(app->activeTabIndex(), 0U);
  app->activeTab()->focusUrl = false;
  press(ImGuiMod_Ctrl | ImGuiKey_L);
  EXPECT_TRUE(ImGui::IsAnyItemActive());  // the URL field, focused then cleared by the frame
  press(ImGuiMod_Ctrl | ImGuiKey_S);
  EXPECT_EQ(lastToast(), "Saved");
}

TEST_F(AppTest, TabsIgnoreWhatDoesNotExist) {
  app->openRequest(424242);  // no such request
  EXPECT_EQ(app->tabCount(), 0U);
  app->sendActiveTab();  // no tab
  app->closeTab(3);
  // Closing a tab before the active one keeps the same tab active.
  app->openRequest(exampleRequestId(0));
  app->openRequest(exampleRequestId(1));
  app->openRequest(exampleRequestId(2));
  app->closeTab(0);
  EXPECT_EQ(app->activeTabIndex(), 1U);
  EXPECT_EQ(app->activeTab()->requestId, exampleRequestId(2));
}

TEST_F(AppTest, ThePerformanceTabOfAnUnsavedRequestFollowsIt) {
  ui::RequestTab& perf = perfTabFor("http://127.0.0.1:1/x");
  ui::RequestTab& source = app->tab(0);
  ASSERT_EQ(app->requestOf(perf), &source.scratch);
  // Saved: the Performance tab follows the request into its collection (as Save to... does).
  source.requestId = exampleRequestId(1);
  EXPECT_EQ(app->requestOf(perf), app->requestOf(source));
  EXPECT_EQ(perf.requestId, exampleRequestId(1));
  // Without its request tab, the Performance tab of an unsaved request has no request.
  ui::RequestTab& orphan = perfTabFor("http://127.0.0.1:1/y");
  app->closeTab(app->tabCount() - 2);
  EXPECT_EQ(app->requestOf(orphan), nullptr);
  frame(2);
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

TEST_F(AppTest, ImportsReportWhatTheyCannotRead) {
  // A folder that is not a Bruno collection.
  const std::filesystem::path empty = home.path() / "empty";
  std::filesystem::create_directories(empty);
  EXPECT_FALSE(app->importPath(empty));
  EXPECT_TRUE(app->toasts().back().error);
  // A Zalie collection file: opened once.
  EXPECT_FALSE(app->importPath(home.path() / "missing.zalie.json"));
  workspace.saveAll();
  const std::size_t nbCollections = workspace.size();
  EXPECT_TRUE(app->importPath(workspace[0].path));
  EXPECT_EQ(workspace.size(), nbCollections);
  EXPECT_TRUE(lastToast().starts_with("Opened collection '")) << lastToast();
  // A collection without the .zalie.json suffix.
  const std::filesystem::path copy = home.path() / "copy.json";
  std::filesystem::copy_file(workspace[0].path, copy);
  EXPECT_TRUE(app->importPath(copy));
  EXPECT_EQ(workspace.size(), nbCollections + 1);
  // A broken Postman export.
  const std::filesystem::path postman = home.path() / "broken.postman.json";
  std::ofstream(postman)
      << R"({"info": {"schema": "https://schema.getpostman.com/json/collection/v2.1.0/collection.json"},)";
  EXPECT_FALSE(app->importPath(postman));
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
  app->newScratchTab();  // not restored: unsaved
  app->openRequest(exampleRequestId(2));
  app->openRequest(exampleRequestId(0));
  frame(2);
  app->shutdown();
  app.reset();
  std::ofstream(home.path() / "settings.json") << "{ broken";

  Workspace reloaded(home.path());
  reloaded.load();
  ASSERT_EQ(reloaded.state().openTabs.size(), 2U);
  EXPECT_EQ(reloaded.state().openTabs[0], exampleRequestId(2));
  ui::App restored(reloaded, executor);
  restored.init(1.0F);
  EXPECT_EQ(restored.tabCount(), 2U);
  EXPECT_EQ(restored.activeTabIndex(), 1U);
  ASSERT_FALSE(restored.toasts().empty());  // the settings could not be read
  EXPECT_TRUE(restored.toasts().back().error);
  app = std::make_unique<ui::App>(workspace, executor);  // for the fixture teardown
}

TEST_F(AppTest, ShutdownStopsTheRuns) {
  aeronet::Router router;
  router.setPath(aeronet::http::Method::GET, "/x", [](const aeronet::HttpRequestView& req) {
    std::this_thread::sleep_for(5ms);
    return req.makeResponse(200, "ok", "text/plain");
  });
  aeronet::SingleHttpServer server(aeronet::HttpServerConfig{}.withPort(0).withPollInterval(20ms), std::move(router));
  server.start();
  ui::RequestTab& tab = perfTabFor(LocalUrl(server, "/x"));
  app->requestOf(tab)->load.emplace().durationMs = 0;  // no limit
  app->requestOf(tab)->load->connections = 1;
  refresh();
  ASSERT_TRUE(waitPlan(tab));
  app->sendActiveTab();
  const auto deadline = std::chrono::steady_clock::now() + 20s;
  while (std::chrono::steady_clock::now() < deadline && !tab.perf->running()) {
    frame();
    std::this_thread::sleep_for(5ms);
  }
  ASSERT_TRUE(tab.perf->running());
  app->shutdown();
  app.reset();  // stops it again, then waits for its threads
  app = std::make_unique<ui::App>(workspace, executor);
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

TEST_F(AppTest, PerformanceTabOfASavedRequest) {
  app->openRequest(exampleRequestId(0));  // {{baseUrl}}/json
  frame(2);
  ui::RequestTab& tab = app->openPerformanceTab(app->activeTabIndex());
  ASSERT_TRUE(tab.perf);
  EXPECT_EQ(app->tabCount(), 2U);
  EXPECT_EQ(app->activeTabIndex(), 1U);
  EXPECT_EQ(tab.requestId, exampleRequestId(0));
  // Opened again from the request or from itself: focused, not duplicated.
  app->openPerformanceTab(0);
  app->openPerformanceTab(1);
  EXPECT_EQ(app->tabCount(), 2U);
  EXPECT_EQ(app->activeTabIndex(), 1U);
  ASSERT_TRUE(waitPlan(tab));
  ASSERT_EQ(tab.perf->rows.size(), 1U);
  EXPECT_EQ(View(tab.perf->rows[0].name), "baseUrl");
  EXPECT_EQ(View(tab.perf->rows[0].defaultValue), "https://httpbin.org");
  EXPECT_EQ(tab.perf->preview->plan->host(), "httpbin.org");
  EXPECT_FALSE(app->animating());
  // Opening the request focuses its request tab, not its Performance tab.
  app->openRequest(exampleRequestId(0));
  EXPECT_EQ(app->activeTabIndex(), 0U);
  // Not saved with the open tabs: only its request.
  app->shutdown();
  ASSERT_EQ(workspace.state().openTabs.size(), 1U);
  EXPECT_EQ(workspace.state().openTabs[0], exampleRequestId(0));
}

TEST_F(AppTest, PerformanceRunAgainstALocalServer) {
  std::atomic<int> hits{0};
  aeronet::Router router;
  router.setPath(aeronet::http::Method::GET, "/items", [&hits](const aeronet::HttpRequestView& req) {
    hits.fetch_add(1, std::memory_order_relaxed);
    return req.makeResponse(200, R"({"ok":true})", "application/json");
  });
  aeronet::SingleHttpServer server(aeronet::HttpServerConfig{}.withPort(0).withPollInterval(20ms), std::move(router));
  server.start();

  ui::RequestTab& source = app->newScratchTab();
  source.scratch.name.assign(std::string_view{"Items"});
  source.scratch.url.assign(LocalUrl(server, "/items?page=1&q=dog"));
  SyncParamsFromUrl(source.scratch);
  LoadSettings& settings = source.scratch.load.emplace();
  LoadParameter page;
  page.name.assign(std::string_view{"page"});
  page.target = ParameterTarget::queryParameter;
  page.source = ValueSource::integerRange;
  page.integerFrom = 1;
  page.integerTo = 50;
  settings.parameters.push_back(std::move(page));
  settings.maxRequests = 200;
  settings.connections = 2;
  settings.seed = 42;
  frame();
  ui::RequestTab& tab = app->openPerformanceTab(app->activeTabIndex());
  ASSERT_TRUE(waitPlan(tab));
  // The query parameters are rows: the varied page and the fixed term.
  ASSERT_EQ(tab.perf->rows.size(), 2U);
  EXPECT_EQ(tab.perf->rows[0].parameter, 0U);
  EXPECT_EQ(tab.perf->rows[1].target, ParameterTarget::queryParameter);
  EXPECT_EQ(View(tab.perf->preview->samples[0]), "1, 2, 3, 4, 5");
  ASSERT_FALSE(tab.perf->preview->requests.empty());
  EXPECT_TRUE(View(tab.perf->preview->requests[0].line).ends_with("/items?page=1&q=dog"));

  frame(2);
  press(ImGuiMod_Ctrl | ImGuiKey_Enter);  // runs the active Performance tab
  ASSERT_TRUE(waitRun(tab));
  const std::shared_ptr<const LoadSnapshot> snapshot = tab.perf->snapshot;
  EXPECT_TRUE(snapshot->setupError.empty()) << View(snapshot->setupError);
  EXPECT_EQ(snapshot->counts[static_cast<std::size_t>(LoadClass::status2xx)], 200U);
  EXPECT_EQ(hits.load(), 200);
  EXPECT_EQ(tab.perf->runSettings.seed, 42U);
  EXPECT_EQ(tab.perf->charts.pSnapshot, snapshot.get());  // drawn
  EXPECT_FALSE(app->animating());

  // Without limit, until stopped (Esc).
  source.scratch.load->maxRequests = 0;
  source.scratch.load->durationMs = 0;
  refresh();
  ASSERT_TRUE(waitPlan(tab));
  app->sendActiveTab();
  const auto deadline = std::chrono::steady_clock::now() + 20s;
  while (std::chrono::steady_clock::now() < deadline && (!tab.perf->running() || tab.perf->snapshot->requests() < 10)) {
    frame();
    std::this_thread::sleep_for(5ms);
  }
  ASSERT_TRUE(tab.perf->running());
  EXPECT_FALSE(app->animating());  // a run wakes the interface, it does not animate it
  app->cancelActiveTab();
  ASSERT_TRUE(waitRun(tab));
  EXPECT_GE(tab.perf->snapshot->requests(), 10U);

  // The run as JSON.
  const std::filesystem::path report = home.path() / "runs" / "items.json";
  ASSERT_TRUE(app->exportRun(tab, report));
  auto content = ReadFile(report);
  ASSERT_TRUE(content.has_value());
  EXPECT_TRUE(View(*content).contains(R"("name": "Items")"));
  EXPECT_TRUE(View(*content).contains(R"("durationMs": 0)"));
  EXPECT_TRUE(View(*content).contains(R"("status": 200)"));
  EXPECT_FALSE(app->exportRun(tab, home.path()));  // a directory
}

TEST_F(AppTest, PerformanceRunsNeedAConfirmationToChangeOrLeaveTheMachine) {
  aeronet::Router router;
  router.setPath(aeronet::http::Method::POST, "/orders",
                 [](const aeronet::HttpRequestView& req) { return req.makeResponse(201, "created", "text/plain"); });
  aeronet::SingleHttpServer server(aeronet::HttpServerConfig{}.withPort(0).withPollInterval(20ms), std::move(router));
  server.start();
  ui::RequestTab& tab = perfTabFor(LocalUrl(server, "/orders"), Method::post);
  ASSERT_TRUE(waitPlan(tab));
  // Asked with Ctrl+Enter, whose Enter does not confirm it at once. Refused: nothing is sent.
  frame(2);
  press(ImGuiMod_Ctrl | ImGuiKey_Enter);
  frame();
  ASSERT_TRUE(ModalOpen());
  EXPECT_FALSE(tab.perf->running());
  press(ImGuiKey_Escape);
  frame();
  EXPECT_FALSE(ModalOpen());
  EXPECT_FALSE(tab.perf->running());
  EXPECT_FALSE(tab.perf->snapshot);
  // Accepted with Enter.
  Request& request = *app->requestOf(tab);
  request.load.emplace().maxRequests = 5;
  refresh();
  ASSERT_TRUE(waitPlan(tab));
  app->sendActiveTab();
  frame(2);
  ASSERT_TRUE(ModalOpen());
  press(ImGuiKey_Enter);
  ASSERT_TRUE(waitRun(tab));
  EXPECT_EQ(tab.perf->snapshot->counts[static_cast<std::size_t>(LoadClass::status2xx)], 5U);

  // Another host than the local machine, even with GET.
  ui::RequestTab& remote = perfTabFor("http://example.invalid/x");
  ASSERT_TRUE(waitPlan(remote));
  EXPECT_FALSE(IsLocalHost(remote.perf->preview->plan->host()));
  app->sendActiveTab();
  frame(2);
  EXPECT_TRUE(ModalOpen());
  press(ImGuiKey_Escape);
  EXPECT_FALSE(remote.perf->running());
}

TEST_F(AppTest, ClosingARunningPerformanceTabDoesNotWaitForTheRun) {
  aeronet::Router router;
  router.setPath(aeronet::http::Method::GET, "/slow", [](const aeronet::HttpRequestView& req) {
    std::this_thread::sleep_for(300ms);
    return req.makeResponse(200, "late", "text/plain");
  });
  aeronet::SingleHttpServer server(aeronet::HttpServerConfig{}.withPort(0).withPollInterval(20ms), std::move(router));
  server.start();
  ui::RequestTab& tab = perfTabFor(LocalUrl(server, "/slow"));
  app->requestOf(tab)->load.emplace().durationMs = 0;  // no limit
  app->requestOf(tab)->load->connections = 2;
  refresh();
  ASSERT_TRUE(waitPlan(tab));
  app->sendActiveTab();
  const auto deadline = std::chrono::steady_clock::now() + 20s;
  while (std::chrono::steady_clock::now() < deadline && (!tab.perf->running() || tab.perf->snapshot->inFlight == 0)) {
    frame();
    std::this_thread::sleep_for(5ms);
  }
  ASSERT_TRUE(tab.perf->running());
  const std::size_t nbTabs = app->tabCount();
  const auto start = std::chrono::steady_clock::now();
  app->closeTab(app->activeTabIndex());
  EXPECT_LT(std::chrono::steady_clock::now() - start, 150ms);
  EXPECT_EQ(app->tabCount(), nbTabs - 1);
  // Released once its requests in flight end.
  for (int idx = 0; idx < 50; ++idx) {
    frame();
    std::this_thread::sleep_for(10ms);
  }
}

TEST_F(AppTest, ADroppedFileBecomesTheDataFileOfItsRow) {
  const std::filesystem::path csv = home.path() / "users.csv";
  {
    std::ofstream file(csv);
    file << "id,name\n1,zalie\n2,physalie\n";
  }
  ui::RequestTab& tab = perfTabFor("http://127.0.0.1:1/users/{{user}}");
  ASSERT_TRUE(waitPlan(tab));
  ASSERT_EQ(tab.perf->rows.size(), 1U);
  EXPECT_TRUE(tab.perf->rows[0].undefined);
  EXPECT_FALSE(tab.perf->preview->plan->valid());

  // No data file row under the mouse nor selected: imported as a collection, which it is not.
  const std::filesystem::path dropped[] = {csv};
  app->onFilesDropped(dropped);
  frame(2);
  EXPECT_TRUE(app->toasts().back().error);
  Request& request = *app->requestOf(tab);
  EXPECT_FALSE(request.load.has_value());

  // The selected row reads a data file: it receives the file.
  LoadParameter users;
  users.name.assign(std::string_view{"user"});
  users.source = ValueSource::dataFile;
  request.load.emplace().parameters.push_back(std::move(users));
  refresh();
  frame();
  app->onFilesDropped(dropped);
  frame(2);
  const LoadParameter& parameter = request.load->parameters[0];
  EXPECT_EQ(View(parameter.filePath), View(PathToUtf8(csv)));  // outside the base directory: absolute
  EXPECT_EQ(parameter.fileFormat, DataFileFormat::csv);
  ASSERT_TRUE(waitPlan(tab));
  ASSERT_TRUE(tab.perf->preview->plan->valid()) << View(tab.perf->preview->plan->issues()[0].message);
  ASSERT_NE(tab.perf->preview->dataFiles[0], nullptr);
  EXPECT_EQ(tab.perf->preview->dataFiles[0]->nbRows(), 2U);
  EXPECT_EQ(View(tab.perf->preview->samples[0]), "1, 2");
  frame(2);  // the editor shows the first rows of the file

  // Every source has its editor.
  for (int source = 0; source <= static_cast<int>(ValueSource::dataFile); ++source) {
    request.load->parameters[0].source = static_cast<ValueSource>(source);
    refresh();
    frame(2);
  }
}

TEST_F(AppTest, DashboardOfALongRunStaysFast) {
  app->openRequest(exampleRequestId(0));
  frame();
  ui::RequestTab& tab = app->openPerformanceTab(app->activeTabIndex());
  auto snapshot = std::make_shared<LoadSnapshot>();
  snapshot->phase = LoadPhase::finished;
  snapshot->connections = 8;
  snapshot->elapsedSeconds = 3600.0;
  for (std::uint32_t second = 0; second < 3600; ++second) {
    LoadSecond point{.counts = {1000 + (second % 100), second % 7, second % 13, second % 5, second % 3},
                     .bodyBytes = 512'000,
                     .p50Ns = 2'000'000 + (second * 100),
                     .p90Ns = 4'000'000,
                     .p99Ns = 9'000'000,
                     .maxNs = 40'000'000};
    snapshot->series.push_back(point);
    for (std::size_t outcome = 0; outcome < kNbLoadClasses; ++outcome) {
      snapshot->counts[outcome] += point.counts[outcome];
    }
  }
  for (std::uint64_t idx = 0; idx < 10'000; ++idx) {
    snapshot->totalHistogram.record(1'000'000 + (idx * 1000));
    snapshot->headHistogram.record(500'000 + (idx * 1000));
  }
  snapshot->total.count = 10'000;
  snapshot->total.p50Ns = 6'000'000;
  snapshot->total.p99Ns = 10'900'000;  // p90 and p99.9 left at 0: no marker on the logarithmic axis
  snapshot->statuses.push_back({.count = 3'600'000, .status = 200});
  snapshot->statuses.push_back({.count = 21'600, .status = 404});
  snapshot->errors[static_cast<std::size_t>(LoadErrorKind::timeout)] = {.count = 3600, .firstMessage = "timeout"};
  snapshot->meanRequestsPerSecond = 1050.0;
  snapshot->clientSaturated = true;
  snapshot->clientLoad = 0.95;
  app->showLoadSnapshot(tab, snapshot);
  frame(2);
  EXPECT_EQ(tab.perf->charts.times.size(), 3600U);
  EXPECT_GT(tab.perf->charts.rateChart.nbPoints, 0U);
  EXPECT_LT(tab.perf->charts.rateChart.nbPoints, 3600U);  // one point per pixel at most
  const auto start = std::chrono::steady_clock::now();
  static constexpr int kFrames = 10;
  frame(kFrames);
  EXPECT_LT((std::chrono::steady_clock::now() - start) / kFrames, 50ms);
  EXPECT_GT(LastFrameVertices(), 5000);
  EXPECT_EQ(LastFrameNonFiniteVertices(), 0);
  // A fake run exports too.
  EXPECT_TRUE(app->exportRun(tab, home.path() / "fake.json"));
}

}  // namespace
}  // namespace zalie
