#pragma once

#include <imgui.h>

#include <amc/vector.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>

#include "zalie/collection.hpp"
#include "zalie/exchange.hpp"
#include "zalie/executor.hpp"
#include "zalie/load-runner.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/request.hpp"
#include "zalie/snippets.hpp"
#include "zalie/text-document.hpp"
#include "zalie/ui/assets.hpp"
#include "zalie/ui/perf-tab.hpp"
#include "zalie/ui/text-view.hpp"
#include "zalie/ui/theme.hpp"
#include "zalie/workspace.hpp"

namespace zalie::ui {

// Sections of the request editor and of the response viewer.
enum class RequestSection : std::uint8_t { params, headers, body, auth, settings, docs };
enum class ResponseSection : std::uint8_t { body, headers, sent };

// An open tab: a request, or the Performance tab of a request when 'perf' is set.
struct RequestTab {
  [[nodiscard]] bool pending() const noexcept { return ticket.valid(); }

  // The request when it is not saved in a collection (requestId == 0).
  Request scratch;
  // Last exchange of the tab.
  std::unique_ptr<Exchange> exchange;
  // The run settings, the run and the dashboard of a Performance tab, null for a request tab.
  std::unique_ptr<PerfTab> perf;
  // The request as sent, as raw HTTP (built on demand for the "Sent" view).
  RawChars32 sentText;
  LineIndex sentLines;
  // The request in flight, if any.
  Executor::Ticket ticket;
  TextViewState bodyView;
  TextViewState sentView;
  // JSONPath filter of a JSON response body: the query typed, the one 'filtered' was computed for, its outcome.
  RawChars32 jsonFilter;
  RawChars32 appliedFilter;
  RawChars32 filterMessage;
  // The matches, compact (joined) and pretty-printed, with their line indexes.
  RawChars filtered;
  RawChars filteredPretty;
  LineIndex filteredLines;
  LineIndex filteredPrettyLines;
  // Decoded image body (owned by the TextureStore).
  ImTextureData* pImage{nullptr};
  // Id of the request of a collection shown in this tab, 0 for a scratch request.
  std::uint64_t requestId{0};
  // Stable identity of the tab (Dear ImGui ids).
  std::uint64_t uid{0};
  RequestSection requestSection{RequestSection::params};
  ResponseSection responseSection{ResponseSection::body};
  // Set to select 'requestSection' programmatically at the next frame.
  bool selectRequestSection{false};
  bool prettyBody{true};
  bool focusUrl{false};
  bool imageTried{false};
  // 'filtered' must be recomputed (new response).
  bool filterDirty{true};
  // The last filter was invalid ('filterMessage' tells why): the whole body is shown.
  bool filterError{false};
};

// A transient notification, bottom right.
struct Toast {
  RawChars32 message;
  std::chrono::steady_clock::time_point shownAt;
  bool error{false};
};

// The whole user interface. Independent of the platform / renderer: the host (src/app) feeds Dear ImGui with events,
// calls beforeNewFrame() + frame() between NewFrame() and Render(), and sleeps while !animating().
class App {
 public:
  // Modal dialogs.
  enum class Dialog : std::uint8_t { none, import, newCollection, rename, collectionSettings, settings, about };

  App(Workspace& workspace, Executor& executor);
  App(const App&) = delete;
  App& operator=(const App&) = delete;
  ~App();

  // Loads fonts, textures and theme. Requires a Dear ImGui context; call once before the first frame.
  void init(float dpiScale);

  // Applies pending theme / scale changes. Call before ImGui::NewFrame().
  void beforeNewFrame();

  // Builds the UI of one frame.
  void frame();

  // Whether something moves on screen (request in flight, toast): the host must keep rendering frames.
  [[nodiscard]] bool animating() const noexcept;

  // Files or directories dropped on the window: collections to open or import.
  void onFilesDropped(std::span<const std::filesystem::path> paths);

  // Display scale of the monitor (content scale).
  void setDpiScale(float dpiScale) noexcept;

  // Saves the open tabs and everything else. Call before exiting.
  void shutdown();

  // --- Actions, also used by the tests ---

  // Opens the request in a tab (or focuses its tab).
  void openRequest(std::uint64_t requestId);
  // Opens a new tab with an unsaved request.
  RequestTab& newScratchTab();
  // Opens the Performance tab of the request of tab 'sourceIndex', or focuses it if it is open.
  RequestTab& openPerformanceTab(std::size_t sourceIndex);
  // Click on a history entry: opens its request, or a new unsaved request with its method and URL.
  void openHistoryEntry(std::size_t index);
  // Double click on a history entry: puts its method and URL (as sent) in the request of the active tab.
  void copyHistoryEntry(std::size_t index);
  // Shows the history (or the collections) in the sidebar.
  void showHistory(bool show) noexcept;
  // Sends / cancels the request of the active tab, or runs / stops a Performance tab.
  void sendActiveTab();
  void cancelActiveTab();
  void closeTab(std::size_t index);
  // Imports a Bruno directory, a Postman / Zalie collection file. Returns false (with an error toast) on failure.
  bool importPath(const std::filesystem::path& path);
  // Collects the completed exchanges (called by frame()).
  void pollExchanges();
  // Shows an exchange in a tab as if it had just completed (used by pollExchanges, the demo mode and the tests).
  void showExchange(RequestTab& tab, Exchange exchange);
  // Shows a run in a Performance tab as if it had just ended (demo mode and tests).
  void showLoadSnapshot(RequestTab& tab, std::shared_ptr<const LoadSnapshot> snapshot);
  // Writes the report of the last run of a Performance tab. Returns false (with an error toast) on failure.
  bool exportRun(RequestTab& tab, const std::filesystem::path& path);
  // Picks up the snapshots of the runs, ends the finished ones (called by frame()).
  void pollRuns();

  [[nodiscard]] std::size_t tabCount() const noexcept { return _tabs.size(); }
  [[nodiscard]] RequestTab& tab(std::size_t index) noexcept { return *_tabs[static_cast<std::uint32_t>(index)]; }
  [[nodiscard]] RequestTab* activeTab() noexcept;
  [[nodiscard]] std::size_t activeTabIndex() const noexcept { return _activeTab; }
  // The request shown by a tab (nullptr if its request was deleted).
  [[nodiscard]] Request* requestOf(RequestTab& tab) noexcept;
  // The collection of a tab's request (nullptr for a scratch request).
  [[nodiscard]] OpenCollection* collectionOf(const RequestTab& tab) noexcept;
  [[nodiscard]] std::span<const Toast> toasts() const noexcept { return {_toasts.data(), _toasts.size()}; }
  void toast(std::string_view message, bool error = false);
  [[nodiscard]] Workspace& workspace() noexcept { return _workspace; }
  [[nodiscard]] PlanCompiler& planCompiler() noexcept { return _planCompiler; }
  // Location of the active request ("Collection / Folder / "), as shown above the URL bar.
  [[nodiscard]] std::string_view breadcrumb() const noexcept { return _breadcrumb; }
  // Opens a dialog at the next frame ('collectionIndex': collection of the collection settings dialog).
  void openDialog(Dialog dialog, std::size_t collectionIndex = 0) noexcept {
    _pendingDialog = dialog;
    _dialogCollection = collectionIndex;
  }

 private:
  // Layout pieces (app.cpp).
  void drawTopBar(float height);
  void drawStatusBar(float height);
  void drawToasts();
  void handleShortcuts();
  void applyTheme();

  // Sidebar (sidebar.cpp).
  void drawSidebar(ImVec2 size);
  void drawCollectionsTree();
  void drawFolder(OpenCollection& collection, Folder& folder, int depth);
  void drawRequestItem(OpenCollection& collection, Folder& folder, Request& request);
  void drawHistory();
  [[nodiscard]] bool matchesFilter(const Request& request) const;
  [[nodiscard]] bool folderMatchesFilter(const Folder& folder) const;

  // Center (request-panel.cpp).
  void drawTabs();
  void drawRequestPanel(ImVec2 size);
  void drawBreadcrumb(RequestTab& tab, Request& request);
  void drawUrlBar(RequestTab& tab, Request& request);
  void drawRequestEditor(RequestTab& tab, Request& request, ImVec2 size);
  bool drawBodyEditor(RequestTab& tab, Request& request);
  static bool DrawAuthEditor(Auth& auth, bool allowInherit);
  static bool DrawSettingsEditor(RequestSettings& settings);
  void drawResponse(RequestTab& tab, float height);
  void drawPending(RequestTab& tab, float height);
  void drawEmptyResponse(float height);
  void drawResponseBody(RequestTab& tab, float height);
  static void UpdateJsonFilter(RequestTab& tab, bool submitted);
  static void DrawSentRequest(RequestTab& tab);

  // Performance tab (perf-panel.cpp).
  void drawPerformancePanel(RequestTab& tab, Request& request);
  void updatePerformanceTab(RequestTab& tab, Request& request);
  void submitPlan(RequestTab& tab, const Request& request);
  void startRun(RequestTab& tab, const Request& request);
  void drawPerfHeader(RequestTab& tab, const Request& request);
  void listPerfRows(RequestTab& tab, const Request& request);
  void drawPerfSettings(RequestTab& tab, Request& request);
  void drawPerfPlan(RequestTab& tab, const Request& request);
  void drawPerfParameters(RequestTab& tab, Request& request);
  void drawPerfRowEditor(RequestTab& tab, Request& request);
  void drawPerfDialogs(RequestTab& tab, const Request& request);
  void drawPerfDashboard(PerfTab& perf);
  void drawPerfTables(PerfTab& perf);
  void stopRunner(std::unique_ptr<LoadRunner> runner);
  [[nodiscard]] std::filesystem::path dataBaseDirectory(const RequestTab& tab);

  // Inspector (inspector.cpp).
  void drawInspector(ImVec2 size);
  void drawSnippetCard(RequestTab& tab, Request& request);
  static void DrawTimingsCard(RequestTab& tab);
  void drawVariablesCard(RequestTab& tab, Request& request);
  void drawResponseHeadersCard(RequestTab& tab);
  static void DrawNotesCard(Request& request);
  void drawInspectorMenu();

  // Dialogs (dialogs.cpp).
  void drawDialogs();
  void drawCollectionSettingsDialog();
  void drawImportDialog();
  void drawNewCollectionDialog();
  void drawRenameDialog();
  void drawSettingsDialog();
  void drawAboutDialog();

  // Helpers.
  void touch(OpenCollection* pCollection) noexcept;
  void createRequest(OpenCollection& collection, Folder& folder, std::string_view name);
  void deleteRequest(OpenCollection& collection, std::uint64_t requestId);
  // The request tab (not a Performance tab) of a request.
  [[nodiscard]] RequestTab* findTab(std::uint64_t requestId) noexcept;
  [[nodiscard]] RequestTab* findTabByUid(std::uint64_t uid) noexcept;
  void releaseImage(RequestTab& tab);
  void setActiveTab(std::size_t index) noexcept;

  // Pending dialog to open (dialogs are opened at a stable point of the frame).
  // What a rename dialog renames.
  enum class RenameTarget : std::uint8_t { request, folder, collection };

  Workspace& _workspace;
  Executor& _executor;
  PlanCompiler _planCompiler;
  TextureStore _textures;
  Assets _assets;
  amc::vector<std::unique_ptr<RequestTab>> _tabs;
  amc::vector<Toast> _toasts;
  amc::vector<Exchange> _completed;
  // Runs of closed Performance tabs, stopped: released once over, never waited for on the UI thread.
  amc::vector<std::unique_ptr<LoadRunner>> _stoppingRunners;
  RawChars32 _filter;
  RawChars32 _dialogText;
  RawChars32 _dialogText2;
  // Snippet of the active request, regenerated only when the request (or the language) changes.
  RawChars32 _snippet;
  LineIndex _snippetLines;
  TextViewState _snippetView;
  // URL of the active request with its variables resolved, recomputed only when something changed.
  RawChars32 _urlPreview;
  RawChars32 _urlMissing;
  // Breadcrumb of the active request, recomputed only when something changed.
  RawChars32 _breadcrumb;
  std::chrono::steady_clock::time_point _startTime;
  std::uint64_t _revision{1};
  std::uint64_t _snippetRevision{0};
  std::uint64_t _snippetTabUid{0};
  std::uint64_t _urlPreviewRevision{0};
  std::uint64_t _breadcrumbRevision{0};
  std::uint64_t _breadcrumbTabUid{0};
  std::uint64_t _bodyCheckRevision{0};
  std::uint64_t _nextTabUid{1};
  std::uint64_t _renameId{0};
  std::size_t _activeTab{0};
  std::size_t _dialogCollection{0};
  std::size_t _requestedTab{static_cast<std::size_t>(-1)};
  float _dpiScale{1.0F};
  int _collectionSettingsEnv{0};
  ThemeKind _theme{ThemeKind::coton};
  SnippetLanguage _snippetLanguage{SnippetLanguage::curl};
  Dialog _pendingDialog{Dialog::none};
  RenameTarget _renameTarget{RenameTarget::request};
  bool _themeDirty{true};
  bool _focusFilter{false};
  bool _focusFind{false};
  bool _snippetEditing{false};
  bool _showHistory{false};
  // Set to select the sidebar tab of '_showHistory' programmatically at the next frame.
  bool _selectSideTab{false};
  bool _bodyValidJson{true};
};

}  // namespace zalie::ui
