// The request panel (tabs, URL bar, request editor, response), driven as the user does.

#include <gtest/gtest.h>
#include <imgui.h>

#include <aeronet/http-method.hpp>
#include <aeronet/http-request-view.hpp>
#include <aeronet/http-server-config.hpp>
#include <aeronet/router.hpp>
#include <aeronet/single-http-server.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "app-test-fixture.hpp"
#include "zalie/collection.hpp"
#include "zalie/exchange.hpp"
#include "zalie/executor.hpp"
#include "zalie/method.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/request.hpp"
#include "zalie/ui/app.hpp"
#include "zalie/workspace.hpp"

namespace zalie {
namespace {

class RequestPanelTest : public AppTest {
 protected:
  RequestPanelTest() { frame(3); }

  // The label of the tab of a request: "<method> <name>###tab<uid>".
  [[nodiscard]] std::string tabLabel(std::size_t index) {
    ui::RequestTab& tab = app->tab(index);
    const Request& request = *app->requestOf(tab);
    return std::string(tab.perf ? "Perf \xC2\xB7 " : "") + std::string(MethodToStr(request.method)) + " " +
           std::string(View(request.name)) + "###tab" + std::to_string(tab.uid);
  }

  // Opens a request of the example collection and returns it.
  Request& openExample(std::size_t index) {
    app->openRequest(exampleRequestId(index));
    frame(3);
    return *app->requestOf(*app->activeTab());
  }

  ui::RequestTab& newScratch(std::string_view url = {}) {
    ui::RequestTab& tab = app->newScratchTab();
    tab.scratch.url.assign(url);
    frame(3);
    return tab;
  }
};

TEST_F(RequestPanelTest, TheWelcomeScreenStartsARequestOrAnImport) {
  ASSERT_TRUE(click("New request"));
  EXPECT_EQ(app->tabCount(), 1U);
  app->closeTab(0);
  frame(2);
  ASSERT_TRUE(click("Import..."));
  EXPECT_TRUE(ModalOpen());
}

TEST_F(RequestPanelTest, TabsAreSelectedClosedAndAdded) {
  for (std::size_t i = 0; i < 3; ++i) {
    openExample(i);
  }
  ASSERT_EQ(app->activeTabIndex(), 2U);
  ASSERT_TRUE(click(tabLabel(0)));
  EXPECT_EQ(app->activeTabIndex(), 0U);
  EXPECT_TRUE(hasTooltip(tabLabel(1)));  // the URL of the request

  // The middle button closes a tab, as its cross does.
  const std::uint64_t secondId = app->tab(1).requestId;
  ASSERT_TRUE(middleClick(tabLabel(1)));
  ASSERT_EQ(app->tabCount(), 2U);
  EXPECT_NE(app->tab(1).requestId, secondId);

  ASSERT_TRUE(click("+"));
  ASSERT_EQ(app->tabCount(), 3U);
  EXPECT_EQ(app->activeTab()->requestId, 0U);

  // The tab of a request deleted from its collection goes away.
  RemoveRequest(exampleCollection().root, app->tab(0).requestId);
  frame(2);
  EXPECT_EQ(app->tabCount(), 2U);
}

TEST_F(RequestPanelTest, TheMethodAndTheSaveToMenu) {
  ui::RequestTab& tab = newScratch("https://example.com/pets");
  ASSERT_TRUE(choose("##method", "DELETE"));
  EXPECT_EQ(tab.scratch.method, Method::del);

  // The collection is listed in the sidebar too: the entry of the menu is the last item with its name.
  ASSERT_TRUE(click("Save to..."));
  ASSERT_TRUE(click("Zalie examples", workspace[0].dirty ? 0 : 1));
  ASSERT_NE(tab.requestId, 0U);
  ASSERT_EQ(exampleCollection().root.requests.size(), 2U);
  const Request& saved = exampleCollection().root.requests[1];
  EXPECT_EQ(saved.id, tab.requestId);
  EXPECT_EQ(saved.method, Method::del);
  EXPECT_EQ(View(saved.url), "https://example.com/pets");
  EXPECT_EQ(lastToast(), "Saved in 'Zalie examples'");
  EXPECT_FALSE(hasItem("Save to..."));  // a saved request has no "Save to..." button
  EXPECT_EQ(app->breadcrumb(), "Zalie examples / ");
}

TEST_F(RequestPanelTest, TheFetchAndPerformanceButtons) {
  ui::RequestTab& tab = newScratch("http://127.0.0.1:1/");
  ASSERT_TRUE(click("##fetch"));
  ASSERT_TRUE(waitResponse());
  EXPECT_EQ(tab.exchange->response.state, ExchangeState::failed);
  EXPECT_EQ(workspace.history().entries().size(), 1U);

  ASSERT_TRUE(click("##performance"));
  ASSERT_EQ(app->tabCount(), 2U);
  EXPECT_NE(app->activeTab()->perf, nullptr);
  // Its shortcut, from the request: the same Performance tab.
  press(ImGuiMod_Ctrl | ImGuiKey_Tab);
  ASSERT_EQ(app->activeTabIndex(), 0U);
  press(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Enter);
  EXPECT_EQ(app->tabCount(), 2U);
  EXPECT_EQ(app->activeTabIndex(), 1U);
  EXPECT_EQ(workspace.history().entries().size(), 1U);  // nothing sent
}

TEST_F(RequestPanelTest, ARequestInFlightIsCancelledByItsButtons) {
  aeronet::Router router;
  router.setPath(aeronet::http::Method::GET, "/slow", [](const aeronet::HttpRequestView& req) {
    std::this_thread::sleep_for(400ms);
    return req.makeResponse(200, "late", "text/plain");
  });
  aeronet::SingleHttpServer server(aeronet::HttpServerConfig{}.withPort(0).withPollInterval(20ms), std::move(router));
  server.start();
  ui::RequestTab& tab = newScratch(LocalUrl(server, "/slow"));

  // The button of the URL bar, which replaces "Fetch" while the request is in flight.
  app->sendActiveTab();
  frame(3);
  ASSERT_TRUE(tab.pending());
  EXPECT_FALSE(hasItem("##fetch"));
  ASSERT_TRUE(click("Cancel"));
  ASSERT_TRUE(waitResponse());
  EXPECT_EQ(tab.exchange->response.state, ExchangeState::cancelled);

  // The button under the animation, which tells how much of the body has arrived.
  app->sendActiveTab();
  frame(3);
  ASSERT_TRUE(tab.pending());
  Progress& progress = *tab.ticket.progress;
  progress.headReceived = true;
  progress.bytesReceived = 512U << 10U;
  frame(2);  // size unknown: what was received
  progress.expectedBytes = 2U << 20U;
  frame(2);  // "512 KiB of 2 MiB", with a progress bar
  EXPECT_GT(LastFrameVertices(), 1000);
  EXPECT_EQ(LastFrameNonFiniteVertices(), 0);
  ASSERT_TRUE(click("Cancel (Esc)"));
  ASSERT_TRUE(waitResponse());
  EXPECT_EQ(tab.exchange->response.state, ExchangeState::cancelled);
}

TEST_F(RequestPanelTest, ACurlCommandInTheUrlBarIsImported) {
  Request& request = openExample(0);
  request.name.assign(std::string_view{"My request"});
  // Pasted (several characters at once): imported right away.
  ASSERT_TRUE(click("##url"));
  typeText("curl -X PATCH https://example.com/pets/1 -H 'X-A: 1' --proxy http://p:1");
  EXPECT_EQ(request.method, Method::patch);
  EXPECT_EQ(View(request.url), "https://example.com/pets/1");
  EXPECT_EQ(View(request.name), "My request");  // a name given by the user is kept
  EXPECT_EQ(request.id, app->activeTab()->requestId);
  ASSERT_GE(app->toasts().size(), 2U);
  EXPECT_TRUE(app->toasts().back().error);  // the option that was ignored
  EXPECT_EQ(View(app->toasts()[app->toasts().size() - 2].message), "curl command imported (some options were ignored)");

  // Typed key after key: imported once the edit ends, not at every keystroke of a command still incomplete.
  ASSERT_TRUE(click("##url"));
  typeText("");
  const std::size_t nbToasts = app->toasts().size();
  for (char ch : std::string_view{"curl -X"}) {
    Type(std::string_view(&ch, 1));
    frame();
  }
  EXPECT_EQ(app->toasts().size(), nbToasts);
  press(ImGuiKey_Enter);
  // This one cannot be read: reported, and left in the field.
  EXPECT_EQ(lastToast(), "missing argument for option -X");
  EXPECT_EQ(View(request.url), "curl -X");
}

TEST_F(RequestPanelTest, TheUrlPreviewNamesTheUndefinedVariables) {
  Request& request = openExample(0);
  ASSERT_TRUE(click("##url"));
  typeText("{{baseUrl}}/{{first}}/{{second}}");
  EXPECT_EQ(View(request.url), "{{baseUrl}}/{{first}}/{{second}}");
  // Nothing to check but the text drawn: the frame is laid out without a Dear ImGui error.
  EXPECT_GT(LastFrameVertices(), 1000);
}

TEST_F(RequestPanelTest, TheNameOfASavedRequestIsRenamedFromTheBreadcrumb) {
  Request& request = openExample(0);
  const UiItem* pMethod = findItem("##method");
  if (pMethod == nullptr) {
    ASSERT_TRUE(hover("##method"));  // a combo box: found once its label is watched
    pMethod = findItem("##method");
  }
  ASSERT_NE(pMethod, nullptr);
  // The name follows the breadcrumb, on the line above the URL bar.
  const float nameX = pMethod->visible.Min.x + ImGui::CalcTextSize("Zalie examples / Basics / ").x + 6.0F;
  const float nameY = pMethod->visible.Min.y - ImGui::GetStyle().ItemSpacing.y - (ImGui::GetFontSize() * 0.5F);
  MouseMove(ImVec2(nameX, nameY));
  frame(2);
  EXPECT_TRUE(tooltipShown());
  clickAt(ImVec2(nameX, nameY));
  ASSERT_TRUE(ModalOpen());
  typeText("Fetch the JSON");
  press(ImGuiKey_Enter);
  EXPECT_EQ(View(request.name), "Fetch the JSON");
}

TEST_F(RequestPanelTest, ParamsAndHeadersAreEditedInTheirTables) {
  Request& request = openExample(1);  // POST {{baseUrl}}/anything?source=zalie
  ASSERT_EQ(request.params.size(), 1U);
  // A new parameter typed in the empty row: the URL follows.
  ASSERT_TRUE(click("##key", 1));
  typeText("page");
  ASSERT_EQ(request.params.size(), 2U);
  ASSERT_TRUE(click("##value", 1));
  typeText("2");
  EXPECT_EQ(View(request.url), "{{baseUrl}}/anything?source=zalie&page=2");
  ASSERT_TRUE(click("##on", 0));
  EXPECT_EQ(View(request.url), "{{baseUrl}}/anything?page=2");
  ASSERT_TRUE(click("##del", 0));
  ASSERT_EQ(request.params.size(), 1U);
  EXPECT_EQ(View(request.params[0].key), "page");
  EXPECT_TRUE(workspace[0].dirty);

  ASSERT_TRUE(click("Headers###headers"));
  ASSERT_TRUE(click("##key"));
  typeText("X-Trace");
  ASSERT_EQ(request.headers.size(), 1U);
  EXPECT_EQ(View(request.headers[0].key), "X-Trace");
  EXPECT_TRUE(hasItem("Headers 1###headers"));  // the tab counts the enabled headers

  // The headers of the collection are announced under the table.
  exampleCollection().headers.push_back(MakeKeyValue("X-Team", "pets"));
  frame(2);
  EXPECT_GT(LastFrameVertices(), 1000);
}

TEST_F(RequestPanelTest, TheBodyEditor) {
  Request& request = openExample(1);
  ASSERT_TRUE(click("Body"));
  ASSERT_TRUE(click("Beautify"));
  EXPECT_TRUE(View(request.body).starts_with("{\n  \"name\": \"Zalie\","));

  ASSERT_TRUE(click("##body"));
  typeText("{\"broken\": ");
  frame(2);
  ASSERT_TRUE(click("Beautify"));
  EXPECT_EQ(lastToast(), "The body is not valid JSON");
  EXPECT_EQ(View(request.body), "{\"broken\": ");

  ASSERT_TRUE(click("Text"));
  EXPECT_EQ(request.bodyMode, BodyMode::text);
  EXPECT_FALSE(hasItem("Beautify"));
  ASSERT_TRUE(click("XML"));
  EXPECT_EQ(request.bodyMode, BodyMode::xml);
  ASSERT_TRUE(click("Form URL-encoded"));
  EXPECT_EQ(request.bodyMode, BodyMode::formUrlEncoded);
  ASSERT_TRUE(click("##key"));
  typeText("name");
  ASSERT_EQ(request.form.size(), 1U);
  ASSERT_TRUE(click("None"));
  EXPECT_EQ(request.bodyMode, BodyMode::none);
  EXPECT_FALSE(hasItem("##body"));
  ASSERT_TRUE(click("JSON"));
  EXPECT_EQ(request.bodyMode, BodyMode::json);
}

TEST_F(RequestPanelTest, TheAuthEditor) {
  Request& request = openExample(0);
  ASSERT_TRUE(click("Auth###auth"));
  ASSERT_TRUE(choose("Type", "Basic"));
  EXPECT_EQ(request.auth.mode, AuthMode::basic);
  ASSERT_TRUE(click("Username"));
  typeText("zalie");
  ASSERT_TRUE(click("Password"));
  typeText("woof");
  EXPECT_EQ(View(request.auth.username), "zalie");
  EXPECT_EQ(View(request.auth.password), "woof");
  EXPECT_TRUE(hasItem("Auth \xE2\x80\xA2###auth"));  // the tab tells that the request has its own authentication

  ASSERT_TRUE(choose("Type", "Bearer token"));
  ASSERT_TRUE(click("Token"));
  typeText("{{token}}");
  EXPECT_EQ(View(request.auth.token), "{{token}}");

  ASSERT_TRUE(choose("Type", "API key"));
  ASSERT_TRUE(click("Key"));
  typeText("X-Api-Key");
  ASSERT_TRUE(click("Value"));
  typeText("secret");
  ASSERT_TRUE(click("Send in the query string instead of a header"));
  EXPECT_EQ(View(request.auth.apiKeyName), "X-Api-Key");
  EXPECT_EQ(View(request.auth.apiKeyValue), "secret");
  EXPECT_TRUE(request.auth.apiKeyInQuery);
  // The other schemes keep what was typed.
  EXPECT_EQ(View(request.auth.username), "zalie");

  ASSERT_TRUE(choose("Type", "No authentication"));
  EXPECT_EQ(request.auth.mode, AuthMode::none);
  ASSERT_TRUE(choose("Type", "Inherit from the collection"));
  EXPECT_EQ(request.auth.mode, AuthMode::inherit);
}

TEST_F(RequestPanelTest, TheSettingsAndDocsOfARequest) {
  Request& request = openExample(0);
  ASSERT_TRUE(click("Settings"));
  ASSERT_TRUE(click("Follow redirects"));
  ASSERT_TRUE(click("Verify TLS certificate"));
  ASSERT_TRUE(click("Decompress responses (gzip, deflate, zstd, br)"));
  EXPECT_FALSE(request.settings.followRedirects);
  EXPECT_FALSE(request.settings.verifyTls);
  EXPECT_FALSE(request.settings.decompress);
  ASSERT_TRUE(choose("HTTP version", "HTTP/1.1"));
  EXPECT_EQ(request.settings.httpVersion, HttpVersion::http1);
  ASSERT_TRUE(choose("HTTP version", "HTTP/2 (h2 / h2c)"));
  EXPECT_EQ(request.settings.httpVersion, HttpVersion::http2);
  ASSERT_TRUE(click("Timeout (ms)"));
  typeText("0");
  press(ImGuiKey_Enter);
  EXPECT_EQ(request.settings.timeoutMs, 1U);  // never zero: it would mean no timeout at all
  ASSERT_TRUE(click("+", 1));                 // the step button of the timeout (the first "+" opens a new tab)
  EXPECT_EQ(request.settings.timeoutMs, 1001U);

  ASSERT_TRUE(click("Docs###docs"));
  ASSERT_TRUE(click("##name"));
  typeText("Renamed here");
  EXPECT_EQ(View(request.name), "Renamed here");
  ASSERT_TRUE(click("##docs"));
  typeText("What it does.");
  EXPECT_EQ(View(request.docs), "What it does.");
  EXPECT_TRUE(hasItem("Docs \xE2\x80\xA2###docs"));
  EXPECT_TRUE(workspace[0].dirty);
}

TEST_F(RequestPanelTest, TheSectionsAndTheToolbarOfAResponse) {
  ui::RequestTab& tab = newScratch("http://example.com/x");
  showBody("application/json", R"({"pets":[{"name":"Zalie"},{"name":"Physalie"},{"name":"zalie junior"}]})");
  tab.exchange->response.warnings.push_back("a warning of the exchange");
  frame(3);

  ASSERT_TRUE(click("Headers 1###headers"));
  EXPECT_EQ(tab.responseSection, ui::ResponseSection::headers);
  // A click on the value of a header copies it: the first row, under the title of its column.
  const UiItem* pValueColumn = findItem("Value");
  ASSERT_NE(pValueColumn, nullptr);
  clickAt(ImVec2(pValueColumn->visible.Min.x + 20.0F, pValueColumn->visible.Max.y + (ImGui::GetFontSize() * 0.8F)));
  EXPECT_STREQ(ImGui::GetClipboardText(), "application/json");
  EXPECT_EQ(lastToast(), "Header value copied");

  ASSERT_TRUE(click("Sent"));
  EXPECT_EQ(tab.responseSection, ui::ResponseSection::sent);
  ASSERT_TRUE(click("Body", 1));  // the first "Body" is the section of the request editor
  EXPECT_EQ(tab.responseSection, ui::ResponseSection::body);

  ASSERT_TRUE(click("Raw"));
  EXPECT_FALSE(tab.prettyBody);
  ASSERT_TRUE(click("Pretty"));
  EXPECT_TRUE(tab.prettyBody);
  EXPECT_TRUE(hasTooltip("##jsonFilter"));

  // Find: Enter goes to the next match, Shift+Enter and the arrows move too.
  press(ImGuiMod_Ctrl | ImGuiKey_F);
  Type("zalie");
  frame(2);
  ASSERT_EQ(tab.bodyView.matches.size(), 2U);
  EXPECT_EQ(tab.bodyView.currentMatch, 0U);
  press(ImGuiKey_Enter);
  EXPECT_EQ(tab.bodyView.currentMatch, 1U);
  ASSERT_TRUE(click("##next"));
  EXPECT_EQ(tab.bodyView.currentMatch, 0U);
  ASSERT_TRUE(click("##prev"));
  EXPECT_EQ(tab.bodyView.currentMatch, 1U);

  ASSERT_TRUE(click("##copyBody"));
  EXPECT_EQ(lastToast(), "Body copied");
  EXPECT_TRUE(std::string_view(ImGui::GetClipboardText()).contains("\"Physalie\""));

  // With a filter, the matches are what is copied.
  ASSERT_TRUE(click("##jsonFilter"));
  typeText("$.pets[1].name");
  press(ImGuiKey_Enter);
  ASSERT_TRUE(click("##copyBody"));
  EXPECT_EQ(lastToast(), "Matches copied");
  EXPECT_STREQ(ImGui::GetClipboardText(), "\"Physalie\"");
}

TEST_F(RequestPanelTest, AnImageThatCannotBeDecodedIsShownInHexadecimal) {
  ui::RequestTab& tab = newScratch("http://example.com/x");
  showBody("image/png", std::string_view("\x89PNG\r\n\x1a\n not an image at all", 28));
  frame(3);
  EXPECT_EQ(tab.exchange->response.body.kind, ContentKind::image);
  EXPECT_EQ(tab.pImage, nullptr);
  EXPECT_NE(ActiveWindow("##hex"), nullptr);
}

}  // namespace
}  // namespace zalie
