#include "zalie/ui/app.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>

#include "zalie/bruno-import.hpp"
#include "zalie/collection.hpp"
#include "zalie/exchange.hpp"
#include "zalie/executor.hpp"
#include "zalie/file-io.hpp"
#include "zalie/history.hpp"
#include "zalie/persistence.hpp"
#include "zalie/postman-import.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/request.hpp"
#include "zalie/resolved-request.hpp"
#include "zalie/settings.hpp"
#include "zalie/snippets.hpp"
#include "zalie/ui/assets.hpp"
#include "zalie/ui/icons.hpp"
#include "zalie/ui/theme.hpp"
#include "zalie/ui/widgets.hpp"
#include "zalie/variables.hpp"
#include "zalie/workspace.hpp"

namespace zalie::ui {

namespace {

constexpr auto kToastDuration = std::chrono::milliseconds{3500};
constexpr auto kToastFade = std::chrono::milliseconds{400};
constexpr auto kAutosaveDelay = std::chrono::milliseconds{800};
constexpr float kMinScale = 0.6F;
constexpr float kMaxScale = 2.5F;

}  // namespace

App::App(Workspace& workspace, Executor& executor)
    : _workspace(workspace),
      _executor(executor),
      _planCompiler(executor.options().wakeFn, executor.options().pWakeUserData),
      _startTime(std::chrono::steady_clock::now()) {
  const Settings& settings = _workspace.settings();
  _theme = ParseTheme(settings.theme);
  if (const auto language = ParseSnippetLanguage(settings.snippetLanguage)) {
    _snippetLanguage = *language;
  }
}

App::~App() {
  // All the runs stop together, then their threads are joined with the tabs.
  for (auto& pTab : _tabs) {
    Executor::Cancel(pTab->ticket);
    if (pTab->perf && pTab->perf->runner) {
      pTab->perf->runner->stop();
    }
  }
}

void App::init(float dpiScale) {
  _dpiScale = dpiScale;
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.ConfigWindowsMoveFromTitleBarOnly = true;
  // Ctrl+Tab switches request tabs instead of Dear ImGui windows.
  ImGui::GetCurrentContext()->ConfigNavWindowingKeyNext = ImGuiKey_None;
  ImGui::GetCurrentContext()->ConfigNavWindowingKeyPrev = ImGuiKey_None;
  io.ConfigInputTextCursorBlink = true;
  _assets.load(_textures);
  applyTheme();

  // Restore the open tabs.
  const WorkspaceState& state = _workspace.state();
  for (const std::uint64_t requestId : state.openTabs) {
    if (_workspace.findRequest(requestId).valid() && findTab(requestId) == nullptr) {
      auto pTab = std::make_unique<RequestTab>();
      pTab->requestId = requestId;
      pTab->uid = _nextTabUid++;
      _tabs.push_back(std::move(pTab));
    }
  }
  for (std::uint32_t i = 0; i < _tabs.size(); ++i) {
    if (_tabs[i]->requestId == state.activeTab) {
      setActiveTab(i);
    }
  }
  for (std::string_view error : _workspace.errors()) {
    toast(error, true);
  }
}

void App::applyTheme() {
  const float scale = std::clamp(_dpiScale * _workspace.settings().uiScale, kMinScale, kMaxScale);
  ApplyTheme(_theme, scale);
  _themeDirty = false;
}

void App::beforeNewFrame() {
  if (_themeDirty) {
    applyTheme();
  }
  _textures.collect();
}

void App::setDpiScale(float dpiScale) noexcept {
  if (dpiScale != _dpiScale) {
    _dpiScale = dpiScale;
    _themeDirty = true;
  }
}

bool App::animating() const noexcept {
  if (!_toasts.empty()) {
    return true;
  }
  return std::ranges::any_of(_tabs, [](const auto& pTab) { return pTab->pending(); });
}

void App::shutdown() {
  for (auto& pTab : _tabs) {
    Executor::Cancel(pTab->ticket);
    if (pTab->perf && pTab->perf->runner) {
      pTab->perf->runner->stop();
    }
  }
  WorkspaceState& state = _workspace.state();
  state.openTabs.clear();
  state.activeTab = 0;
  // Performance tabs are not restored: only the ids of the requests are saved.
  for (std::uint32_t i = 0; i < _tabs.size(); ++i) {
    if (_tabs[i]->requestId != 0 && !_tabs[i]->perf) {
      state.openTabs.push_back(_tabs[i]->requestId);
      if (i == _activeTab) {
        state.activeTab = _tabs[i]->requestId;
      }
    }
  }
  Settings& settings = _workspace.settings();
  settings.theme.assign(ThemeId(_theme));
  settings.snippetLanguage.assign(SnippetLanguageId(_snippetLanguage));
  _workspace.saveAll();
}

void App::toast(std::string_view message, bool error) {
  static constexpr std::size_t kMaxToasts = 5;
  if (_toasts.size() == kMaxToasts) {
    _toasts.erase(_toasts.begin());
  }
  _toasts.push_back({.message = RawChars32(message), .shownAt = std::chrono::steady_clock::now(), .error = error});
}

void App::touch(OpenCollection* pCollection) noexcept {
  ++_revision;
  if (pCollection != nullptr) {
    Workspace::MarkDirty(*pCollection);
  }
}

RequestTab* App::activeTab() noexcept {
  return _activeTab < _tabs.size() ? _tabs[static_cast<std::uint32_t>(_activeTab)].get() : nullptr;
}

RequestTab* App::findTab(std::uint64_t requestId) noexcept {
  for (auto& pTab : _tabs) {
    if (pTab->requestId == requestId && requestId != 0 && !pTab->perf) {
      return pTab.get();
    }
  }
  return nullptr;
}

RequestTab* App::findTabByUid(std::uint64_t uid) noexcept {
  for (auto& pTab : _tabs) {
    if (pTab->uid == uid) {
      return pTab.get();
    }
  }
  return nullptr;
}

void App::setActiveTab(std::size_t index) noexcept {
  _activeTab = index;
  _requestedTab = index;
  ++_revision;
}

Request* App::requestOf(RequestTab& tab) noexcept {
  if (tab.requestId == 0 && tab.perf) {
    // The Performance tab of an unsaved request: the request lives in its tab, until it is saved.
    RequestTab* pSource = findTabByUid(tab.perf->sourceTabUid);
    if (pSource == nullptr) {
      return nullptr;
    }
    if (pSource->requestId == 0) {
      return &pSource->scratch;
    }
    tab.requestId = pSource->requestId;
  }
  if (tab.requestId == 0) {
    return &tab.scratch;
  }
  return _workspace.findRequest(tab.requestId).pRequest;
}

OpenCollection* App::collectionOf(const RequestTab& tab) noexcept {
  return tab.requestId == 0 ? nullptr : _workspace.findRequest(tab.requestId).pCollection;
}

void App::openRequest(std::uint64_t requestId) {
  for (std::uint32_t i = 0; i < _tabs.size(); ++i) {
    if (_tabs[i]->requestId == requestId && !_tabs[i]->perf) {
      setActiveTab(i);
      return;
    }
  }
  if (!_workspace.findRequest(requestId).valid()) {
    return;
  }
  auto pTab = std::make_unique<RequestTab>();
  pTab->requestId = requestId;
  pTab->uid = _nextTabUid++;
  pTab->prettyBody = _workspace.settings().prettyJson;
  _tabs.push_back(std::move(pTab));
  setActiveTab(_tabs.size() - 1);
}

RequestTab& App::newScratchTab() {
  auto pTab = std::make_unique<RequestTab>();
  pTab->uid = _nextTabUid++;
  pTab->scratch.name.assign(std::string_view{"Untitled request"});
  pTab->focusUrl = true;
  pTab->prettyBody = _workspace.settings().prettyJson;
  RequestTab& tab = *_tabs.emplace_back(std::move(pTab));
  setActiveTab(_tabs.size() - 1);
  return tab;
}

RequestTab& App::openPerformanceTab(std::size_t sourceIndex) {
  assert(sourceIndex < _tabs.size());
  RequestTab& source = *_tabs[static_cast<std::uint32_t>(sourceIndex)];
  if (source.perf) {
    setActiveTab(sourceIndex);
    return source;
  }
  for (std::uint32_t i = 0; i < _tabs.size(); ++i) {
    const RequestTab& other = *_tabs[i];
    if (other.perf && ((source.requestId != 0 && other.requestId == source.requestId) ||
                       (source.requestId == 0 && other.requestId == 0 && other.perf->sourceTabUid == source.uid))) {
      setActiveTab(i);
      return *_tabs[i];
    }
  }
  auto pTab = std::make_unique<RequestTab>();
  pTab->uid = _nextTabUid++;
  pTab->requestId = source.requestId;
  pTab->perf = std::make_unique<PerfTab>();
  pTab->perf->sourceTabUid = source.requestId == 0 ? source.uid : 0;
  pTab->perf->previewSeed = NewId();
  // Right after its request.
  const auto position = static_cast<std::uint32_t>(sourceIndex + 1);
  _tabs.insert(_tabs.begin() + position, std::move(pTab));
  setActiveTab(position);
  return *_tabs[position];
}

void App::releaseImage(RequestTab& tab) {
  if (tab.pImage != nullptr) {
    _textures.release(tab.pImage);
    tab.pImage = nullptr;
  }
  tab.imageTried = false;
}

void App::closeTab(std::size_t index) {
  if (index >= _tabs.size()) {
    return;
  }
  RequestTab& tab = *_tabs[static_cast<std::uint32_t>(index)];
  Executor::Cancel(tab.ticket);
  releaseImage(tab);
  if (tab.perf && tab.perf->runner) {
    stopRunner(std::move(tab.perf->runner));
  }
  _tabs.erase(_tabs.begin() + static_cast<std::ptrdiff_t>(index));
  if (_activeTab >= _tabs.size()) {
    _activeTab = _tabs.empty() ? 0 : _tabs.size() - 1;
  } else if (index < _activeTab) {
    --_activeTab;
  }
  setActiveTab(_activeTab);
}

void App::sendActiveTab() {
  RequestTab* pTab = activeTab();
  if (pTab == nullptr || pTab->pending()) {
    return;
  }
  if (pTab->perf) {
    pTab->perf->runRequested = !pTab->perf->running();
    return;
  }
  Request* pRequest = requestOf(*pTab);
  if (pRequest == nullptr || pRequest->url.empty()) {
    toast("Enter a URL first", true);
    return;
  }
  OpenCollection* pCollection = collectionOf(*pTab);
  static const Collection kNoCollection;
  const Collection& collection = pCollection == nullptr ? kNoCollection : pCollection->collection;
  const VariableScope scope = MakeScope(collection, pCollection == nullptr ? -1 : pCollection->activeEnvironment);
  pTab->ticket = _executor.submit(ResolveRequest(*pRequest, collection, scope), pTab->requestId);
}

void App::cancelActiveTab() {
  if (RequestTab* pTab = activeTab(); pTab != nullptr) {
    Executor::Cancel(pTab->ticket);
    if (pTab->perf && pTab->perf->runner) {
      pTab->perf->runner->stop();
    }
  }
}

void App::showExchange(RequestTab& tab, Exchange exchange) {
  tab.ticket = {};
  releaseImage(tab);
  tab.bodyView.reset();
  tab.sentText.clear();
  tab.filterDirty = true;
  tab.exchange = std::make_unique<Exchange>(std::move(exchange));
  ++_revision;
}

void App::pollExchanges() {
  _completed.clear();
  if (_executor.poll(_completed) == 0) {
    return;
  }
  for (Exchange& exchange : _completed) {
    _workspace.history().add(exchange);
    for (auto& pTab : _tabs) {
      if (pTab->ticket.id == exchange.ticket) {
        showExchange(*pTab, std::move(exchange));
        break;
      }
    }
  }
  _completed.clear();
}

bool App::importPath(const std::filesystem::path& path) {
  std::error_code errc;
  const RawChars32 pathStr = PathToUtf8(path);
  // A Bruno collection: its folder, or its bruno.json file.
  const bool brunoJson = path.filename() == "bruno.json" && std::filesystem::is_regular_file(path, errc);
  if (brunoJson || std::filesystem::is_directory(path, errc)) {
    auto imported = ImportBrunoCollection(brunoJson ? path.parent_path() : path);
    if (!imported) {
      toast(imported.error(), true);
      return false;
    }
    const std::size_t nbRequests = CountRequests(imported->collection.root);
    OpenCollection& collection = _workspace.addCollection(std::move(imported->collection));
    RawChars32 message;
    AppendAll(message, "Imported Bruno collection '", collection.collection.name, "': ", nbRequests, " requests");
    if (!imported->warnings.empty()) {
      AppendAll(message, ", ", imported->warnings.size(), " warnings");
    }
    toast(message);
    for (std::string_view warning : imported->warnings) {
      toast(warning, true);
    }
    return true;
  }
  if (View(pathStr).ends_with(".zalie.json")) {
    auto opened = _workspace.openCollection(path);
    if (!opened) {
      toast(opened.error(), true);
      return false;
    }
    toast(Concat({"Opened collection '", (*opened)->collection.name, "'"}));
    return true;
  }
  if (View(pathStr).ends_with(".bru")) {
    // One Bruno request: opened in a new tab, unsaved (like a pasted curl command). "Save to..." keeps it.
    auto request = ImportBruRequestFile(path);
    if (!request) {
      toast(request.error(), true);
      return false;
    }
    const bool usesVariables = View(request->url).contains("{{") || View(request->body).contains("{{");
    RequestTab& tab = newScratchTab();
    tab.scratch = std::move(*request);
    tab.focusUrl = false;
    toast(Concat({"Opened '", tab.scratch.name, "' (unsaved: Save to... keeps it in a collection)"}));
    if (usesVariables) {
      toast("It uses {{variables}}: import its whole collection folder to get its environments", true);
    }
    return true;
  }
  auto content = ReadFile(path);
  if (!content) {
    toast(content.error(), true);
    return false;
  }
  if (LooksLikePostmanCollection(*content)) {
    auto imported = ImportPostmanCollection(*content);
    if (!imported) {
      toast(imported.error(), true);
      return false;
    }
    OpenCollection& collection = _workspace.addCollection(std::move(imported->collection));
    toast(Concat({"Imported Postman collection '", collection.collection.name, "'"}));
    for (std::string_view warning : imported->warnings) {
      toast(warning, true);
    }
    return true;
  }
  auto opened = _workspace.openCollection(path);
  if (!opened) {
    toast(Concat({"Not a Zalie, Postman or Bruno collection: ", pathStr}), true);
    return false;
  }
  return true;
}

void App::onFilesDropped(std::span<const std::filesystem::path> paths) {
  // On a Performance tab, a file goes to the parameter row under the mouse at the next frame (a data file).
  RequestTab* pTab = activeTab();
  if (pTab != nullptr && pTab->perf && !paths.empty() && !pTab->perf->rows.empty()) {
    pTab->perf->droppedPath = PathToUtf8(paths.front());
    paths = paths.subspan(1);
  }
  for (const std::filesystem::path& path : paths) {
    importPath(path);
  }
}

void App::handleShortcuts() {
  static constexpr ImGuiInputFlags kGlobal = ImGuiInputFlags_RouteGlobal | ImGuiInputFlags_RouteOverActive;
  Settings& settings = _workspace.settings();
  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Enter, kGlobal) ||
      ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_KeypadEnter, kGlobal)) {
    sendActiveTab();
  }
  if (!_tabs.empty() && (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Enter, kGlobal) ||
                         ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_KeypadEnter, kGlobal))) {
    openPerformanceTab(_activeTab);
  }
  if (ImGui::Shortcut(ImGuiKey_Escape, ImGuiInputFlags_RouteGlobal)) {
    cancelActiveTab();
  }
  // While a request is in flight, Esc cancels it even from a text field (the field keeps its focus).
  if (RequestTab* pActive = activeTab();
      pActive != nullptr && pActive->pending() && ImGui::Shortcut(ImGuiKey_Escape, kGlobal)) {
    cancelActiveTab();
  }
  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, kGlobal)) {
    _workspace.saveAll();
    toast("Saved");
  }
  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_T, kGlobal)) {
    newScratchTab();
  }
  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_W, kGlobal)) {
    closeTab(_activeTab);
  }
  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_L, kGlobal)) {
    if (RequestTab* pTab = activeTab(); pTab != nullptr) {
      pTab->focusUrl = true;
    }
  }
  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_F, kGlobal)) {
    _focusFind = true;
  }
  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_K, kGlobal)) {
    settings.sidebarVisible = true;
    _focusFilter = true;
  }
  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_B, kGlobal)) {
    settings.sidebarVisible = !settings.sidebarVisible;
  }
  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_J, kGlobal)) {
    settings.inspectorVisible = !settings.inspectorVisible;
  }
  if (!_tabs.empty()) {
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Tab, kGlobal)) {
      setActiveTab((_activeTab + 1) % _tabs.size());
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Tab, kGlobal)) {
      setActiveTab((_activeTab + _tabs.size() - 1) % _tabs.size());
    }
  }
  static constexpr float kZoomStep = 0.1F;
  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Equal, kGlobal) ||
      ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_KeypadAdd, kGlobal)) {
    settings.uiScale = std::min(settings.uiScale + kZoomStep, 2.0F);
    _themeDirty = true;
  }
  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Minus, kGlobal) ||
      ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_KeypadSubtract, kGlobal)) {
    settings.uiScale = std::max(settings.uiScale - kZoomStep, 0.7F);
    _themeDirty = true;
  }
  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_0, kGlobal)) {
    settings.uiScale = 1.0F;
    _themeDirty = true;
  }
}

void App::frame() {
  pollExchanges();
  pollRuns();
  if (_workspace.settings().autosave) {
    _workspace.autosave(kAutosaveDelay);
  }
  handleShortcuts();

  const ImGuiViewport* pViewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(pViewport->WorkPos);
  ImGui::SetNextWindowSize(pViewport->WorkSize);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0F);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
  static constexpr ImGuiWindowFlags kMainFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                                                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
                                                 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
  ImGui::Begin("##zalie", nullptr, kMainFlags);
  ImGui::PopStyleVar(4);

  const float scale = ImGui::GetStyle().FontScaleDpi;
  const float topBarHeight = std::floor(48.0F * scale);
  const float statusBarHeight = std::floor(26.0F * scale);
  const float splitter = std::floor(6.0F * scale);
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  const float bodyHeight = std::max(10.0F, avail.y - topBarHeight - statusBarHeight);

  drawTopBar(topBarHeight);

  Settings& settings = _workspace.settings();
  const float minPanel = 180.0F * scale;
  const float minCenter = 360.0F * scale;
  ImGui::SetCursorPos(ImVec2(0, topBarHeight));
  float remaining = avail.x;
  if (settings.sidebarVisible) {
    settings.sidebarWidth = std::clamp(settings.sidebarWidth, minPanel, std::max(minPanel, remaining * 0.45F));
    drawSidebar(ImVec2(settings.sidebarWidth, bodyHeight));
    ImGui::SameLine(0, 0);
    Splitter("##sidebarSplit", true, splitter, settings.sidebarWidth, minPanel, remaining * 0.45F, bodyHeight);
    ImGui::SameLine(0, 0);
    remaining -= settings.sidebarWidth + splitter;
  }
  float inspectorWidth = 0;
  if (settings.inspectorVisible) {
    settings.inspectorWidth =
        std::clamp(settings.inspectorWidth, minPanel, std::max(minPanel, remaining - minCenter - splitter));
    inspectorWidth = settings.inspectorWidth;
  }
  const float centerWidth = std::max(100.0F, remaining - (settings.inspectorVisible ? inspectorWidth + splitter : 0));
  drawRequestPanel(ImVec2(centerWidth, bodyHeight));
  if (settings.inspectorVisible) {
    ImGui::SameLine(0, 0);
    float negativeWidth = -settings.inspectorWidth;
    Splitter("##inspectorSplit", true, splitter, negativeWidth, -(remaining - minCenter - splitter), -minPanel,
             bodyHeight);
    settings.inspectorWidth = -negativeWidth;
    ImGui::SameLine(0, 0);
    drawInspector(ImVec2(inspectorWidth, bodyHeight));
  }

  ImGui::SetCursorPos(ImVec2(0, topBarHeight + bodyHeight));
  drawStatusBar(statusBarHeight);
  ImGui::End();

  drawDialogs();
  drawToasts();
}

void App::drawTopBar(float height) {
  const Palette& pal = CurrentPalette();
  const ImGuiStyle& style = ImGui::GetStyle();
  const float scale = style.FontScaleDpi;
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  const float width = ImGui::GetContentRegionAvail().x;
  ImDrawList* pDrawList = ImGui::GetWindowDrawList();
  pDrawList->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), pal.panel);
  pDrawList->AddLineH(pos.x, pos.x + width, pos.y + height - 1, pal.border);

  // Logo + name.
  const float logoSize = std::floor(30.0F * scale);
  const float margin = std::floor(12.0F * scale);
  pDrawList->AddImage(_assets.logoSmall(), ImVec2(pos.x + margin, pos.y + ((height - logoSize) * 0.5F)),
                      ImVec2(pos.x + margin + logoSize, pos.y + ((height + logoSize) * 0.5F)));
  {
    ScopedFont title(GetFonts().pUi, Fonts::kTitleSize);
    const float textHeight = ImGui::GetFontSize();
    pDrawList->AddText(ImVec2(pos.x + margin + logoSize + (8.0F * scale), pos.y + ((height - textHeight) * 0.5F)),
                       pal.text, "Zalie");
  }

  // Environment of the collection of the active request.
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x, 0));
  const float frameHeight = ImGui::GetFrameHeight();
  const float rowY = (height - frameHeight) * 0.5F;
  RequestTab* pTab = activeTab();
  OpenCollection* pCollection = pTab == nullptr ? nullptr : collectionOf(*pTab);
  const float envX = std::max(180.0F * scale, (width * 0.5F) - (130.0F * scale));
  ImGui::SetCursorPos(ImVec2(envX, rowY));
  if (pCollection != nullptr) {
    Collection& collection = pCollection->collection;
    ImGui::AlignTextToFramePadding();
    TextDim("Environment");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(180.0F * scale);
    const int active = pCollection->activeEnvironment;
    RawChars32 preview(active >= 0 && std::cmp_less(active, collection.environments.size())
                           ? std::string_view(collection.environments[static_cast<std::uint32_t>(active)].name)
                           : std::string_view{"No environment"});
    preview.push_back('\0');
    if (ImGui::BeginCombo("##environment", preview.data())) {
      if (ImGui::Selectable("No environment", active < 0)) {
        pCollection->activeEnvironment = -1;
        touch(nullptr);
      }
      for (std::uint32_t i = 0; i < collection.environments.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        RawChars32 name(View(collection.environments[i].name));
        name.push_back('\0');
        if (ImGui::Selectable(name.data(), std::cmp_equal(i, active))) {
          pCollection->activeEnvironment = static_cast<std::int32_t>(i);
          touch(nullptr);
        }
        ImGui::PopID();
      }
      ImGui::Separator();
      if (ImGui::Selectable("Manage environments...")) {
        for (std::size_t i = 0; i < _workspace.size(); ++i) {
          if (&_workspace[i] == pCollection) {
            _dialogCollection = i;
          }
        }
        _collectionSettingsEnv = std::max(0, active);
        _pendingDialog = Dialog::collectionSettings;
      }
      ImGui::EndCombo();
    }
  } else if (pTab != nullptr) {
    ImGui::AlignTextToFramePadding();
    TextDim("Unsaved request - no environment");
  }

  // Right side buttons.
  Settings& settings = _workspace.settings();
  const float buttonsWidth = (frameHeight * 5.0F) + (style.ItemSpacing.x * 4.0F) + margin;
  ImGui::SetCursorPos(ImVec2(width - buttonsWidth, rowY));
  if (IconButton("##toggleSidebar", Icon::sidebar, "Toggle the sidebar (Ctrl+B)", settings.sidebarVisible)) {
    settings.sidebarVisible = !settings.sidebarVisible;
  }
  ImGui::SameLine();
  if (IconButton("##toggleLayout", Icon::columns, "Response beside the request", settings.responseBeside)) {
    settings.responseBeside = !settings.responseBeside;
  }
  ImGui::SameLine();
  if (IconButton("##toggleInspector", Icon::inspector, "Toggle the inspector (Ctrl+J)", settings.inspectorVisible)) {
    settings.inspectorVisible = !settings.inspectorVisible;
  }
  ImGui::SameLine();
  if (IconButton("##theme", _theme == ThemeKind::coton ? Icon::moon : Icon::sun,
                 _theme == ThemeKind::coton ? "Nuit (dark theme)" : "Coton (light theme)")) {
    _theme = _theme == ThemeKind::coton ? ThemeKind::nuit : ThemeKind::coton;
    settings.theme.assign(ThemeId(_theme));
    _themeDirty = true;
  }
  ImGui::SameLine();
  if (IconButton("##menu", Icon::gear, "Settings")) {
    ImGui::OpenPopup("##mainMenu");
  }
  if (ImGui::BeginPopup("##mainMenu")) {
    if (ImGui::MenuItem("New request", "Ctrl+T")) {
      newScratchTab();
    }
    if (ImGui::MenuItem("New collection...")) {
      _pendingDialog = Dialog::newCollection;
    }
    if (ImGui::MenuItem("Import...")) {
      _pendingDialog = Dialog::import;
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Save all", "Ctrl+S")) {
      _workspace.saveAll();
      toast("Saved");
    }
    if (ImGui::MenuItem("Settings...")) {
      _pendingDialog = Dialog::settings;
    }
    if (ImGui::MenuItem("About Zalie")) {
      _pendingDialog = Dialog::about;
    }
    ImGui::EndPopup();
  }
  ImGui::PopStyleVar();
}

void App::drawStatusBar(float height) {
  const Palette& pal = CurrentPalette();
  const float scale = ImGui::GetStyle().FontScaleDpi;
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  const float width = ImGui::GetContentRegionAvail().x;
  ImDrawList* pDrawList = ImGui::GetWindowDrawList();
  pDrawList->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), pal.panel);
  pDrawList->AddLineH(pos.x, pos.x + width, pos.y, pal.border);

  const float margin = 10.0F * scale;
  const std::size_t nbPending =
      static_cast<std::size_t>(std::ranges::count_if(_tabs, [](const auto& pTab) { return pTab->pending(); }));
  const std::size_t nbRuns = static_cast<std::size_t>(
      std::ranges::count_if(_tabs, [](const auto& pTab) { return pTab->perf && pTab->perf->running(); }));
  std::array<char, 96> buffer{};
  ScopedFont font(GetFonts().pUi, Fonts::kUiSize * 0.87F);
  const float textY = pos.y + ((height - ImGui::GetFontSize()) * 0.5F);
  float x = pos.x + margin;
  if (nbPending != 0) {
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - _startTime).count();
    const float spriteHeight = height - (4.0F * scale);
    x += _assets.drawSprite(pDrawList, Animation::run, seconds, ImVec2(x, pos.y + (2.0F * scale)), spriteHeight).x;
    const std::string_view text = FormatTo(buffer, "{} request{} in flight", nbPending, nbPending > 1 ? "s" : "");
    pDrawList->AddText(ImVec2(x + (4.0F * scale), textY), pal.text, text.data(), text.data() + text.size());
    x += (20.0F * scale) + ImGui::CalcTextSize(text.data(), text.data() + text.size()).x;
  }
  if (nbRuns != 0) {
    // No animation: a run refreshes the interface about 10 times per second, not at the rate of the screen.
    const std::string_view text = FormatTo(buffer, "{} performance run{} in progress", nbRuns, nbRuns > 1 ? "s" : "");
    DrawIcon(pDrawList, Icon::send, ImVec2(x + (6.0F * scale), pos.y + (height * 0.5F)), 12.0F * scale, pal.accent);
    pDrawList->AddText(ImVec2(x + (16.0F * scale), textY), pal.text, text.data(), text.data() + text.size());
  } else if (nbPending == 0) {
    DrawIcon(pDrawList, Icon::check, ImVec2(x + (6.0F * scale), pos.y + (height * 0.5F)), 12.0F * scale, pal.success);
    pDrawList->AddText(ImVec2(x + (16.0F * scale), textY), pal.textDim, "Ready");
  }

  // Save state, then the version on the right.
  std::size_t nbDirty = 0;
  for (std::size_t i = 0; i < _workspace.size(); ++i) {
    nbDirty += _workspace[i].dirty ? 1 : 0;
  }
  const std::string_view saveState = nbDirty == 0 ? "All changes saved" : "Saving...";
  const std::string_view version = "Zalie " ZALIE_VERSION_STR " - aeronet";
  const float versionWidth = ImGui::CalcTextSize(version.data(), version.data() + version.size()).x;
  const float saveWidth = ImGui::CalcTextSize(saveState.data(), saveState.data() + saveState.size()).x;
  const float rightX = pos.x + width - margin - versionWidth;
  pDrawList->AddText(ImVec2(rightX, textY), pal.textDim, version.data(), version.data() + version.size());
  pDrawList->AddText(ImVec2(rightX - (24.0F * scale) - saveWidth, textY), nbDirty == 0 ? pal.textDim : pal.accent,
                     saveState.data(), saveState.data() + saveState.size());
  ImGui::Dummy(ImVec2(width, height));
}

void App::drawToasts() {
  const auto now = std::chrono::steady_clock::now();
  _toasts.erase(
      std::ranges::remove_if(_toasts, [now](const Toast& toast) { return now - toast.shownAt > kToastDuration; })
          .begin(),
      _toasts.end());
  if (_toasts.empty()) {
    return;
  }
  const Palette& pal = CurrentPalette();
  const float scale = ImGui::GetStyle().FontScaleDpi;
  const ImGuiViewport* pViewport = ImGui::GetMainViewport();
  float y = pViewport->WorkPos.y + pViewport->WorkSize.y - (40.0F * scale);
  for (std::uint32_t i = _toasts.size(); i-- > 0;) {
    const Toast& toast = _toasts[i];
    const auto age = now - toast.shownAt;
    const float alpha = age > kToastDuration - kToastFade
                            ? std::max(0.0F, std::chrono::duration<float>(kToastDuration - age).count() /
                                                 std::chrono::duration<float>(kToastFade).count())
                            : 1.0F;
    ImGui::SetNextWindowPos(ImVec2(pViewport->WorkPos.x + pViewport->WorkSize.x - (16.0F * scale), y), ImGuiCond_Always,
                            ImVec2(1.0F, 1.0F));
    ImGui::SetNextWindowBgAlpha(0.97F * alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    ImGui::PushStyleColor(ImGuiCol_Border, toast.error ? pal.danger : pal.accent);
    std::array<char, 32> name{};
    std::format_to_n(name.data(), static_cast<std::ptrdiff_t>(name.size() - 1), "##toast{}", i);
    static constexpr ImGuiWindowFlags kToastFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                                    ImGuiWindowFlags_NoSavedSettings |
                                                    ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                                    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs;
    if (ImGui::Begin(name.data(), nullptr, kToastFlags)) {
      ImGui::PushTextWrapPos(380.0F * scale);
      const ImVec2 cursor = ImGui::GetCursorScreenPos();
      const float iconSize = ImGui::GetFontSize();
      DrawIcon(ImGui::GetWindowDrawList(), toast.error ? Icon::warning : Icon::check,
               ImVec2(cursor.x + (iconSize * 0.5F), cursor.y + (iconSize * 0.5F)), iconSize,
               toast.error ? pal.danger : pal.success);
      ImGui::Indent(iconSize + (6.0F * scale));
      Text(toast.message);
      ImGui::Unindent(iconSize + (6.0F * scale));
      ImGui::PopTextWrapPos();
      y -= ImGui::GetWindowHeight() + (8.0F * scale);
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
  }
}

}  // namespace zalie::ui
