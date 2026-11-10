#pragma once

// The application driven headless by the tests: a workspace in a temporary directory (the example collection of a
// first launch), the real executor and the real interface.

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <aeronet/single-http-server.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
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
#include "zalie/load-runner.hpp"
#include "zalie/method.hpp"
#include "zalie/request.hpp"
#include "zalie/ui/app.hpp"
#include "zalie/ui/perf-tab.hpp"
#include "zalie/url.hpp"
#include "zalie/workspace.hpp"

namespace zalie {

using namespace std::chrono_literals;

// The window (child windows included) of this frame whose name contains 'part' ("parent/##child_1234ABCD").
inline const ImGuiWindow* ActiveWindow(std::string_view part) {
  for (const ImGuiWindow* pWindow : ImGui::GetCurrentContext()->Windows) {
    if (pWindow->Active && std::string_view(pWindow->Name).contains(part)) {
      return pWindow;
    }
  }
  return nullptr;
}

// Whether a modal popup is open.
inline bool ModalOpen() { return ImGui::GetTopMostPopupModal() != nullptr; }

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

  // Renders frames until the Performance tab has the plan of its request as it is now.
  bool waitPlan(ui::RequestTab& tab) {
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    while (std::chrono::steady_clock::now() < deadline) {
      frame();
      if (tab.perf->planCurrent()) {
        return true;
      }
      app->planCompiler().waitIdle(50ms);
    }
    return false;
  }

  // Renders frames until the last run of the Performance tab is over.
  bool waitRun(ui::RequestTab& tab) {
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    while (std::chrono::steady_clock::now() < deadline) {
      frame();
      if (tab.perf->snapshot && tab.perf->snapshot->phase == LoadPhase::finished && !tab.perf->running()) {
        return true;
      }
      std::this_thread::sleep_for(5ms);
    }
    return false;
  }

  // Focusing a tab makes the Performance tabs list their rows and compile their plan again: the tests edit the
  // requests directly, without the interface.
  void refresh() { app->openPerformanceTab(app->activeTabIndex()); }

  // An unsaved request to 'path' of the local server, and its Performance tab.
  ui::RequestTab& perfTabFor(std::string_view url, Method method = Method::get) {
    ui::RequestTab& source = app->newScratchTab();
    source.scratch.url.assign(url);
    source.scratch.method = method;
    SyncParamsFromUrl(source.scratch);
    frame();
    return app->openPerformanceTab(app->activeTabIndex());
  }

  [[nodiscard]] static std::string LocalUrl(const aeronet::SingleHttpServer& server, std::string_view path) {
    return "http://127.0.0.1:" + std::to_string(server.port()) + std::string(path);
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

  // Interactions with the items of the last frame, found by their label (ImGuiContextFixture::findItem).
  [[nodiscard]] bool hasItem(std::string_view label) const noexcept { return findItem(label) != nullptr; }

  [[nodiscard]] bool hover(std::string_view label, std::size_t index = 0) {
    return hoverItem(label, [this] { frame(); }, index);
  }

  [[nodiscard]] bool click(std::string_view label, std::size_t index = 0) {
    return clickItem(label, [this] { frame(); }, ImGuiMouseButton_Left, index);
  }

  [[nodiscard]] bool rightClick(std::string_view label, std::size_t index = 0) {
    return clickItem(label, [this] { frame(); }, ImGuiMouseButton_Right, index);
  }

  [[nodiscard]] bool middleClick(std::string_view label, std::size_t index = 0) {
    return clickItem(label, [this] { frame(); }, ImGuiMouseButton_Middle, index);
  }

  // Clicks a point of the window: for what has no label (a text, a row drawn by hand).
  void clickAt(ImVec2 pos) {
    MouseMove(pos);
    frame();
    clickHere([this] { frame(); });
  }

  // Clicks an entry of the popup opened by an item: a combo box, a menu button.
  [[nodiscard]] bool choose(std::string_view label, std::string_view entry, std::size_t entryIndex = 0) {
    return click(label) && click(entry, entryIndex);
  }

  [[nodiscard]] bool tooltipShown() const { return ActiveWindow("##Tooltip") != nullptr; }

  // Two clicks in a row: a double click for Dear ImGui.
  [[nodiscard]] bool doubleClick(std::string_view label, std::size_t index = 0) {
    if (!hover(label, index)) {
      return false;
    }
    for (int nbClicks = 0; nbClicks < 2; ++nbClicks) {
      MouseButton(true);
      frame();
      MouseButton(false);
      frame();
    }
    frame();
    return true;
  }

  // Opens the context menu of an item (right click) and clicks one of its entries.
  [[nodiscard]] bool contextMenu(std::string_view label, std::string_view entry, std::size_t index = 0) {
    return rightClick(label, index) && click(entry);
  }

  [[nodiscard]] bool drag(std::string_view from, std::string_view to, std::size_t fromIndex = 0,
                          std::size_t toIndex = 0) {
    return dragItem(from, to, [this] { frame(); }, fromIndex, toIndex);
  }

  // Replaces the text of the field having the keyboard focus.
  void typeText(std::string_view text) {
    press(ImGuiMod_Ctrl | ImGuiKey_A);
    press(ImGuiKey_Delete);
    Type(text);
    frame(2);
  }

  // Leaves the mouse where it is for longer than the delay of the tooltips.
  void waitTooltip() { frame(45); }

  // Whether the tooltip of an item shows up once the mouse rests on it.
  [[nodiscard]] bool hasTooltip(std::string_view label, std::size_t index = 0) {
    if (!hover(label, index)) {
      return false;
    }
    waitTooltip();
    return tooltipShown();
  }

  // The text of the last notification, empty if there is none.
  [[nodiscard]] std::string_view lastToast() const {
    return app->toasts().empty() ? std::string_view{} : std::string_view(app->toasts().back().message);
  }

  [[nodiscard]] Collection& exampleCollection() { return workspace[0].collection; }

  test::TempDir home;
  Workspace workspace;
  Executor executor;
  std::unique_ptr<ui::App> app;
};

}  // namespace zalie
