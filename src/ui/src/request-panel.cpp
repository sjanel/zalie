// Center panel: request tabs, URL bar, request editor and response viewer.

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string_view>
#include <utility>

#include "zalie/collection.hpp"
#include "zalie/curl-import.hpp"
#include "zalie/exchange.hpp"
#include "zalie/executor.hpp"
#include "zalie/json-format.hpp"
#include "zalie/json-path.hpp"
#include "zalie/key-value.hpp"
#include "zalie/method.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/request.hpp"
#include "zalie/resolved-request.hpp"
#include "zalie/settings.hpp"
#include "zalie/snippets.hpp"
#include "zalie/string-utils.hpp"
#include "zalie/syntax.hpp"
#include "zalie/text-document.hpp"
#include "zalie/ui/app.hpp"
#include "zalie/ui/assets.hpp"
#include "zalie/ui/icons.hpp"
#include "zalie/ui/text-view.hpp"
#include "zalie/ui/theme.hpp"
#include "zalie/ui/widgets.hpp"
#include "zalie/url.hpp"
#include "zalie/variables.hpp"
#include "zalie/workspace.hpp"

namespace zalie::ui {

namespace {

constexpr std::size_t kNoTab = static_cast<std::size_t>(-1);

int Size(std::string_view text) noexcept { return static_cast<int>(text.size()); }

void CenteredText(std::string_view text, ImU32 color) {
  const float width = ImGui::CalcTextSize(text.data(), text.data() + text.size()).x;
  ImGui::SetCursorPosX(std::max(0.0F, (ImGui::GetContentRegionAvail().x - width) * 0.5F) + ImGui::GetCursorPosX());
  TextColored(color, text);
}

}  // namespace

void App::drawTabs() {
  static constexpr ImGuiTabBarFlags kFlags = ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_FittingPolicyScroll |
                                             ImGuiTabBarFlags_TabListPopupButton | ImGuiTabBarFlags_NoTooltip;
  if (!ImGui::BeginTabBar("##requestTabs", kFlags)) {
    return;
  }
  const std::size_t requested = _requestedTab;
  std::size_t toClose = kNoTab;
  std::array<char, 160> label{};
  for (std::uint32_t i = 0; i < _tabs.size(); ++i) {
    RequestTab& tab = *_tabs[i];
    const Request* pRequest = requestOf(tab);
    if (pRequest == nullptr) {
      toClose = i;  // its request was deleted
      continue;
    }
    const std::string_view name =
        pRequest->name.empty() ? std::string_view{"Untitled"} : std::string_view(pRequest->name);
    const std::string_view method = MethodToStr(pRequest->method);
    bool open = true;
    const ImGuiTabItemFlags flags = i == requested ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
    ImGui::PushStyleColor(ImGuiCol_Text, i == _activeTab ? CurrentPalette().text : CurrentPalette().textDim);
    const bool busy = tab.pending() || (tab.perf && tab.perf->running());
    const bool selected = ImGui::BeginTabItem(Label(label, "{}{}{} {}###tab{}", busy ? "\xE2\x80\xA2 " : "",
                                                    tab.perf ? "Perf \xC2\xB7 " : "", method, name, tab.uid),
                                              &open, flags);
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && !pRequest->url.empty()) {
      ImGui::SetTooltip("%.*s", Size(pRequest->url), pRequest->url.data());
    }
    if (selected) {
      if (requested == kNoTab && _activeTab != i) {
        _activeTab = i;
        ++_revision;
      }
      ImGui::EndTabItem();
    }
    if (!open) {
      toClose = i;
    }
  }
  if (ImGui::TabItemButton("+", ImGuiTabItemFlags_Trailing | ImGuiTabItemFlags_NoTooltip)) {
    newScratchTab();
  }
  ImGui::EndTabBar();
  if (requested != kNoTab && _requestedTab == requested) {
    _requestedTab = kNoTab;
  }
  if (toClose != kNoTab) {
    closeTab(toClose);
  }
}

void App::drawRequestPanel(ImVec2 size) {
  const ImGuiStyle& style = ImGui::GetStyle();
  const float scale = style.FontScaleDpi;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0F * scale, 8.0F * scale));
  ImGui::BeginChild("##center", size, ImGuiChildFlags_AlwaysUseWindowPadding,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleVar();
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0F * scale, 7.0F * scale));

  drawTabs();
  RequestTab* pTab = activeTab();
  Request* pRequest = pTab == nullptr ? nullptr : requestOf(*pTab);
  if (pTab == nullptr || pRequest == nullptr) {
    // Welcome screen.
    const float avail = ImGui::GetContentRegionAvail().y;
    ImGui::Dummy(ImVec2(0, avail * 0.22F));
    const float logoSize = 120.0F * scale;
    ImGui::SetCursorPosX(((ImGui::GetContentRegionAvail().x - logoSize) * 0.5F) + ImGui::GetCursorPosX());
    ImGui::Image(_assets.logo(), ImVec2(logoSize, logoSize));
    ImGui::Spacing();
    {
      ScopedFont title(GetFonts().pUi, Fonts::kTitleSize * 1.2F);
      CenteredText("Woof! Ready to fetch.", CurrentPalette().text);
    }
    CenteredText("Pick a request in the sidebar, or create one with Ctrl+T.", CurrentPalette().textDim);
    CenteredText("Paste a curl command in the URL bar to import it.", CurrentPalette().textDim);
    ImGui::Spacing();
    const float buttonWidth = 160.0F * scale;
    ImGui::SetCursorPosX(((ImGui::GetContentRegionAvail().x - (buttonWidth * 2) - style.ItemSpacing.x) * 0.5F) +
                         ImGui::GetCursorPosX());
    if (PrimaryButton("New request", ImVec2(buttonWidth, 0))) {
      newScratchTab();
    }
    ImGui::SameLine();
    if (ImGui::Button("Import...", ImVec2(buttonWidth, 0))) {
      _pendingDialog = Dialog::import;
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();
    return;
  }

  drawBreadcrumb(*pTab, *pRequest);
  if (pTab->perf) {
    drawPerformancePanel(*pTab, *pRequest);
    ImGui::PopStyleVar();
    ImGui::EndChild();
    return;
  }
  drawUrlBar(*pTab, *pRequest);
  Settings& settings = _workspace.settings();
  const float splitter = 8.0F * scale;
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  // Beside: only when both columns get a usable width, else below (narrow windows, inspector open...).
  const float minWidth = 320.0F * scale;
  if (settings.responseBeside && avail.x >= (2 * minWidth) + splitter) {
    float editorWidth = std::clamp(avail.x * settings.requestEditorWidthRatio, minWidth, avail.x - minWidth - splitter);
    drawRequestEditor(*pTab, *pRequest, ImVec2(editorWidth, avail.y));
    ImGui::SameLine(0, 0);
    if (Splitter("##editorSplitV", true, splitter, editorWidth, minWidth, avail.x - minWidth - splitter, avail.y)) {
      settings.requestEditorWidthRatio = editorWidth / std::max(1.0F, avail.x);
    }
    ImGui::SameLine(0, 0);
    drawResponse(*pTab, avail.y);
  } else {
    const float minHeight = 110.0F * scale;
    float editorHeight =
        std::clamp(avail.y * settings.requestEditorRatio, minHeight, std::max(minHeight, avail.y - minHeight));
    drawRequestEditor(*pTab, *pRequest, ImVec2(0, editorHeight));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    if (Splitter("##editorSplit", false, splitter, editorHeight, minHeight, avail.y - minHeight,
                 ImGui::GetContentRegionAvail().x)) {
      settings.requestEditorRatio = editorHeight / std::max(1.0F, avail.y);
    }
    ImGui::PopStyleVar();
    drawResponse(*pTab, ImGui::GetContentRegionAvail().y);
  }
  ImGui::PopStyleVar();
  ImGui::EndChild();
}

void App::drawBreadcrumb(RequestTab& tab, Request& request) {
  OpenCollection* pCollection = collectionOf(tab);
  if (_breadcrumbRevision != _revision || _breadcrumbTabUid != tab.uid) {
    // Walks the collection tree: done when something changed only, not at every frame.
    _breadcrumbRevision = _revision;
    _breadcrumbTabUid = tab.uid;
    _breadcrumb.clear();
    if (pCollection != nullptr) {
      AppendAll(_breadcrumb, pCollection->collection.name, std::string_view{" / "});
      amc::vector<const Folder*> path;
      if (FindRequestPath(pCollection->collection.root, tab.requestId, path)) {
        for (const Folder* pFolder : path) {
          AppendAll(_breadcrumb, pFolder->name, std::string_view{" / "});
        }
      }
    } else {
      _breadcrumb.append(std::string_view{"Unsaved request / "});
    }
  }
  ImGui::Spacing();
  TextDim(_breadcrumb);
  ImGui::SameLine(0, 0);
  const std::string_view name = request.name.empty() ? std::string_view{"Untitled"} : std::string_view(request.name);
  Text(name);
  if (pCollection != nullptr) {
    if (ImGui::IsItemHovered()) {
      ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
      ImGui::SetTooltip("Rename");
    }
    if (ImGui::IsItemClicked()) {
      _renameTarget = RenameTarget::request;
      _renameId = tab.requestId;
      _dialogText.assign(name);
      _pendingDialog = Dialog::rename;
    }
  }
}

void App::drawUrlBar(RequestTab& tab, Request& request) {
  const Palette& pal = CurrentPalette();
  const ImGuiStyle& style = ImGui::GetStyle();
  const float scale = style.FontScaleDpi;
  OpenCollection* pCollection = collectionOf(tab);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(style.FramePadding.x * 1.2F, style.FramePadding.y * 1.35F));
  if (MethodCombo("##method", request.method, 104.0F * scale)) {
    touch(pCollection);
  }
  ImGui::SameLine(0, 4.0F * scale);
  const float sendWidth = 96.0F * scale;
  const float menuWidth = ImGui::GetFrameHeight() * 0.8F;
  const float menuGap = 1.0F * scale;
  const bool canSave = tab.requestId == 0 && _workspace.size() != 0;
  const float saveWidth = canSave ? 80.0F * scale : 0.0F;
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - sendWidth - menuGap - menuWidth - saveWidth -
                          (style.ItemSpacing.x * (canSave ? 2.0F : 1.0F)));
  if (tab.focusUrl) {
    ImGui::SetKeyboardFocusHere();
    tab.focusUrl = false;
  }
  {
    ScopedFont mono(GetFonts().pMono, Fonts::kMonoSize);
    const std::size_t sizeBefore = request.url.size();
    const bool edited = InputText("##url", request.url, 0, "https://api.example.com/...  or paste a curl command");
    const bool deactivated = ImGui::IsItemDeactivated();
    // A curl command is imported when pasted, or when the edit ends (Enter / focus lost) for a typed one: not at every
    // keystroke of a command being typed.
    const bool pasted = edited && request.url.size() > sizeBefore + 1;
    if ((pasted || deactivated) && LooksLikeCurlCommand(request.url)) {
      auto imported = ParseCurlCommand(request.url);
      if (imported) {
        const std::uint64_t id = request.id;
        RawChars32 name = std::move(request.name);
        request = std::move(imported->request);
        request.id = id;
        if (!name.empty() && View(name) != "New request" && View(name) != "Untitled request") {
          request.name = std::move(name);
        }
        toast(imported->warnings.empty() ? std::string_view{"curl command imported"}
                                         : std::string_view{"curl command imported (some options were ignored)"});
        for (std::string_view warning : imported->warnings) {
          toast(warning, true);
        }
        ImGui::ClearActiveID();  // the input shows the imported URL, not its edit buffer
      } else {
        toast(imported.error(), true);
      }
      touch(pCollection);
    } else if (edited) {
      SyncParamsFromUrl(request);
      touch(pCollection);
    }
  }
  ImGui::SameLine();
  if (canSave) {
    if (ImGui::Button("Save to...", ImVec2(saveWidth, 0))) {
      ImGui::OpenPopup("##saveTo");
    }
    if (ImGui::BeginPopup("##saveTo")) {
      SectionTitle("SAVE IN COLLECTION");
      for (std::size_t i = 0; i < _workspace.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        RawChars32 name(View(_workspace[i].collection.name));
        name.push_back('\0');
        if (ImGui::Selectable(name.data())) {
          OpenCollection& target = _workspace[i];
          Request saved = std::move(tab.scratch);
          saved.id = NewId();
          tab.requestId = saved.id;
          tab.scratch = Request{};
          target.collection.root.requests.push_back(std::move(saved));
          touch(&target);
          toast(View(Concat({"Saved in '", target.collection.name, "'"})));
        }
        ImGui::PopID();
      }
      ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (tab.requestId != 0) {
      ImGui::PopStyleVar();
      return;  // 'request' was the scratch request, moved into the collection
    }
  }
  if (tab.pending()) {
    ImGui::PushStyleColor(ImGuiCol_Button, WithAlpha(pal.danger, 0.15F));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, WithAlpha(pal.danger, 0.3F));
    ImGui::PushStyleColor(ImGuiCol_Text, pal.danger);
    if (ImGui::Button("Cancel", ImVec2(sendWidth, 0))) {
      Executor::Cancel(tab.ticket);
    }
    ImGui::PopStyleColor(3);
  } else {
    // "Fetch", with Zalie's paw.
    if (PrimaryButton("##fetch", ImVec2(sendWidth, 0))) {
      sendActiveTab();
    }
    DrawItemIconLabel(Icon::paw, ImGui::GetFontSize() * 0.95F, "Fetch", pal.onAccent);
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip(tab.pending() ? "Cancel (Esc)" : "Fetch (Ctrl+Enter)");
  }
  // The menu of the Fetch button, joined to it.
  ImGui::SameLine(0, menuGap);
  ImGui::PushStyleColor(ImGuiCol_Button, pal.accent);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, pal.accentHover);
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, pal.accentActive);
  ImGui::PushStyleColor(ImGuiCol_Border, pal.accent);
  if (ImGui::Button("##fetchMenu", ImVec2(menuWidth, 0))) {
    ImGui::OpenPopup("##fetchMenuPopup");
  }
  ImGui::PopStyleColor(4);
  {
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    DrawIcon(ImGui::GetWindowDrawList(), Icon::chevronDown, ImVec2((min.x + max.x) * 0.5F, (min.y + max.y) * 0.5F),
             ImGui::GetFontSize() * 0.7F, pal.onAccent);
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("More ways to send the request");
  }
  ImGui::PopStyleVar();
  if (ImGui::BeginPopup("##fetchMenuPopup")) {
    if (ImGui::MenuItem("Fetch", "Ctrl+Enter", false, !tab.pending())) {
      sendActiveTab();
    }
    if (ImGui::MenuItem("Performance test...")) {
      openPerformanceTab(_activeTab);
    }
    ImGui::EndPopup();
  }

  // Resolved URL preview when variables are used.
  if (View(request.url).contains("{{")) {
    if (_urlPreviewRevision != _revision) {
      _urlPreviewRevision = _revision;
      static const Collection kNoCollection;
      const Collection& collection = pCollection == nullptr ? kNoCollection : pCollection->collection;
      const VariableScope scope = MakeScope(collection, pCollection == nullptr ? -1 : pCollection->activeEnvironment);
      UnresolvedNames unresolved;
      _urlPreview.clear();
      Interpolate(request.url, scope, _urlPreview, &unresolved);
      _urlMissing.clear();
      for (std::string_view name : unresolved) {
        if (!_urlMissing.empty()) {
          _urlMissing.append(std::string_view{", "});
        }
        _urlMissing.append(name);
      }
    }
    ScopedFont small(GetFonts().pMono, Fonts::kMonoSize * 0.9F);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (108.0F * scale));
    TextDim("=> ");
    ImGui::SameLine(0, 0);
    TextDim(_urlPreview);
    if (!_urlMissing.empty()) {
      ImGui::SameLine();
      TextColored(pal.warning, View(Concat({"undefined: ", _urlMissing})));
    }
  }
}

void App::drawRequestEditor(RequestTab& tab, Request& request, ImVec2 size) {
  OpenCollection* pCollection = collectionOf(tab);
  ImGui::BeginChild("##editor", size, ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
  std::array<char, 48> label{};
  const auto enabledCount = [](const KeyValues& rows) {
    return std::ranges::count_if(rows, [](const KeyValue& row) { return row.enabled && !row.key.empty(); });
  };
  if (ImGui::BeginTabBar("##requestSections")) {
    const auto section = [&](const char* text, RequestSection value) {
      const bool requested = tab.selectRequestSection && tab.requestSection == value;
      const bool open = ImGui::BeginTabItem(text, nullptr, requested ? ImGuiTabItemFlags_SetSelected : 0);
      if (requested) {
        tab.selectRequestSection = false;
      } else if (open && !tab.selectRequestSection) {
        tab.requestSection = value;
      }
      return open;
    };
    const auto paramsCount = enabledCount(request.params);
    if (section(paramsCount == 0 ? "Params###params" : Label(label, "Params {}###params", paramsCount),
                RequestSection::params)) {
      ImGui::BeginChild("##paramsContent");
      if (KeyValueTable("##params", request.params, {.keyHint = "Parameter", .valueHint = "Value"})) {
        SyncUrlFromParams(request);
        touch(pCollection);
      }
      TextDim("Query parameters are kept in sync with the URL. Values may use {{variables}}.");
      ImGui::EndChild();
      ImGui::EndTabItem();
    }
    const auto headersCount = enabledCount(request.headers);
    if (section(headersCount == 0 ? "Headers###headers" : Label(label, "Headers {}###headers", headersCount),
                RequestSection::headers)) {
      ImGui::BeginChild("##headersContent");
      if (KeyValueTable("##headers", request.headers, {.keyHint = "Header", .valueHint = "Value"})) {
        touch(pCollection);
      }
      if (pCollection != nullptr && !pCollection->collection.headers.empty()) {
        TextDim(View(Concat({"+ the headers of the collection '", pCollection->collection.name, "'"})));
      }
      ImGui::EndChild();
      ImGui::EndTabItem();
    }
    if (section("Body", RequestSection::body)) {
      if (drawBodyEditor(tab, request)) {
        touch(pCollection);
      }
      ImGui::EndTabItem();
    }
    if (section(request.auth.mode == AuthMode::inherit || request.auth.mode == AuthMode::none
                    ? "Auth###auth"
                    : "Auth \xE2\x80\xA2###auth",
                RequestSection::auth)) {
      ImGui::BeginChild("##authContent");
      if (DrawAuthEditor(request.auth, true)) {
        touch(pCollection);
      }
      ImGui::EndChild();
      ImGui::EndTabItem();
    }
    if (section("Settings", RequestSection::settings)) {
      ImGui::BeginChild("##settingsContent");
      if (DrawSettingsEditor(request.settings)) {
        touch(pCollection);
      }
      ImGui::EndChild();
      ImGui::EndTabItem();
    }
    if (section(request.docs.empty() ? "Docs###docs" : "Docs \xE2\x80\xA2###docs", RequestSection::docs)) {
      if (pCollection != nullptr) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (InputText("##name", request.name, 0, "Request name")) {
          touch(pCollection);
        }
      }
      ScopedFont mono(GetFonts().pMono, Fonts::kMonoSize);
      if (InputTextMultiline("##docs", request.docs, ImVec2(-FLT_MIN, -FLT_MIN))) {
        touch(pCollection);
      }
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }
  ImGui::EndChild();
}

bool App::drawBodyEditor(RequestTab& tab, Request& request) {
  bool modified = false;
  static constexpr std::array kModes{std::pair{BodyMode::none, "None"}, std::pair{BodyMode::json, "JSON"},
                                     std::pair{BodyMode::text, "Text"}, std::pair{BodyMode::xml, "XML"},
                                     std::pair{BodyMode::formUrlEncoded, "Form URL-encoded"}};
  for (const auto& [mode, name] : kModes) {
    if (ImGui::RadioButton(name, request.bodyMode == mode)) {
      request.bodyMode = mode;
      modified = true;
    }
    ImGui::SameLine();
  }
  ImGui::NewLine();
  switch (request.bodyMode) {
    case BodyMode::none:
      TextDim("This request has no body.");
      break;
    case BodyMode::formUrlEncoded:
      ImGui::BeginChild("##formContent");
      modified |= KeyValueTable("##form", request.form, {.keyHint = "Field", .valueHint = "Value"});
      TextDim("Sent as application/x-www-form-urlencoded, values percent-encoded.");
      ImGui::EndChild();
      break;
    case BodyMode::json:
    case BodyMode::text:
    case BodyMode::xml: {
      if (request.bodyMode == BodyMode::json) {
        if (_bodyCheckRevision != _revision) {
          _bodyCheckRevision = _revision;
          const std::string_view body = Trim(request.body);
          _bodyValidJson = body.empty() || View(request.body).contains("{{") || IsValidJson(body);
        }
        if (ImGui::SmallButton("Beautify")) {
          RawChars pretty;
          if (PrettyPrintJson(request.body, pretty)) {
            request.body.assign(std::string_view(pretty));
            modified = true;
          } else {
            toast("The body is not valid JSON", true);
          }
        }
        ImGui::SameLine();
        if (_bodyValidJson) {
          TextColored(CurrentPalette().success, "valid JSON");
        } else {
          TextColored(CurrentPalette().warning, "invalid JSON");
        }
        ImGui::SameLine();
      }
      TextDim(View(Concat({"Content-Type: ", DefaultContentType(request.bodyMode), " (unless set in the headers)"})));
      ScopedFont mono(GetFonts().pMono, Fonts::kMonoSize);
      modified |=
          InputTextMultiline("##body", request.body, ImVec2(-FLT_MIN, -FLT_MIN), ImGuiInputTextFlags_AllowTabInput);
      break;
    }
  }
  (void)tab;
  return modified;
}

bool App::DrawAuthEditor(Auth& auth, bool allowInherit) {
  bool modified = false;
  static constexpr std::array kModes{std::pair{AuthMode::inherit, "Inherit from the collection"},
                                     std::pair{AuthMode::none, "No authentication"},
                                     std::pair{AuthMode::basic, "Basic"}, std::pair{AuthMode::bearer, "Bearer token"},
                                     std::pair{AuthMode::apiKey, "API key"}};
  const char* current = "No authentication";
  for (const auto& [mode, name] : kModes) {
    if (mode == auth.mode) {
      current = name;
    }
  }
  const float scale = ImGui::GetStyle().FontScaleDpi;
  ImGui::SetNextItemWidth(260.0F * scale);
  if (ImGui::BeginCombo("Type", current)) {
    for (const auto& [mode, name] : kModes) {
      if (mode == AuthMode::inherit && !allowInherit) {
        continue;
      }
      if (ImGui::Selectable(name, mode == auth.mode)) {
        auth.mode = mode;
        modified = true;
      }
    }
    ImGui::EndCombo();
  }
  ImGui::Spacing();
  const float fieldWidth = std::min(ImGui::GetContentRegionAvail().x - (100.0F * scale), 520.0F * scale);
  switch (auth.mode) {
    case AuthMode::basic:
      ImGui::SetNextItemWidth(fieldWidth);
      modified |= InputText("Username", auth.username);
      ImGui::SetNextItemWidth(fieldWidth);
      modified |= InputText("Password", auth.password, ImGuiInputTextFlags_Password);
      break;
    case AuthMode::bearer:
      ImGui::SetNextItemWidth(fieldWidth);
      modified |= InputText("Token", auth.token, ImGuiInputTextFlags_Password, "{{token}}");
      TextDim("Sent as 'Authorization: Bearer <token>'.");
      break;
    case AuthMode::apiKey:
      ImGui::SetNextItemWidth(fieldWidth);
      modified |= InputText("Key", auth.apiKeyName, 0, "X-Api-Key");
      ImGui::SetNextItemWidth(fieldWidth);
      modified |= InputText("Value", auth.apiKeyValue, ImGuiInputTextFlags_Password);
      modified |= ImGui::Checkbox("Send in the query string instead of a header", &auth.apiKeyInQuery);
      break;
    case AuthMode::inherit:
      TextDim("Uses the authentication of the collection (right click on it > Variables, environments, auth).");
      break;
    case AuthMode::none:
      break;
  }
  if (auth.mode != AuthMode::none && auth.mode != AuthMode::inherit) {
    TextDim("Values may use {{variables}}: keep secrets in an environment.");
  }
  return modified;
}

bool App::DrawSettingsEditor(RequestSettings& settings) {
  bool modified = false;
  const float scale = ImGui::GetStyle().FontScaleDpi;
  ImGui::SetNextItemWidth(160.0F * scale);
  static constexpr std::uint32_t kStep = 1000;
  modified |= ImGui::InputScalar("Timeout (ms)", ImGuiDataType_U32, &settings.timeoutMs, &kStep);
  settings.timeoutMs = std::max(settings.timeoutMs, 1U);
  modified |= ImGui::Checkbox("Follow redirects", &settings.followRedirects);
  modified |= ImGui::Checkbox("Verify TLS certificate", &settings.verifyTls);
  modified |= ImGui::Checkbox("Decompress responses (gzip, deflate, zstd, br)", &settings.decompress);
  static constexpr std::array kVersions{std::pair{HttpVersion::automatic, "Automatic (ALPN)"},
                                        std::pair{HttpVersion::http1, "HTTP/1.1"},
                                        std::pair{HttpVersion::http2, "HTTP/2 (h2 / h2c)"}};
  ImGui::SetNextItemWidth(220.0F * scale);
  const char* current = kVersions[static_cast<std::size_t>(settings.httpVersion)].second;
  if (ImGui::BeginCombo("HTTP version", current)) {
    for (const auto& [version, name] : kVersions) {
      if (ImGui::Selectable(name, version == settings.httpVersion)) {
        settings.httpVersion = version;
        modified = true;
      }
    }
    ImGui::EndCombo();
  }
  return modified;
}

void App::drawResponse(RequestTab& tab, float height) {
  if (tab.pending()) {
    drawPending(tab, height);
    return;
  }
  if (!tab.exchange) {
    drawEmptyResponse(height);
    return;
  }
  const Palette& pal = CurrentPalette();
  const Exchange& exchange = *tab.exchange;
  const Response& response = exchange.response;
  const float scale = ImGui::GetStyle().FontScaleDpi;
  ImGui::BeginChild("##response", ImVec2(0, height), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);

  // Summary line: status, time, size, clock.
  ImGui::AlignTextToFramePadding();
  StatusPill(response.status, response.reason, response.state);
  std::array<char, 48> buffer{};
  if (response.state != ExchangeState::cancelled) {
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    Text(FormatDuration(response.timings.total, buffer));
    ImGui::SameLine();
    TextDim("\xC2\xB7");
    ImGui::SameLine();
    Text(FormatBytes(response.body.raw.size(), buffer));
    ImGui::SameLine();
    TextDim("\xC2\xB7");
    ImGui::SameLine();
    TextDim(FormatClock(response.timings.startedAt, buffer));
  }
  // Section selector, right aligned.
  {
    std::array<char, 32> headersLabel{};
    const char* sections[] = {"Body", Label(headersLabel, "Headers {}###headers", response.headers.size()), "Sent"};
    float total = 0;
    for (const char* section : sections) {
      total += ImGui::CalcTextSize(section, nullptr, true).x + (ImGui::GetStyle().FramePadding.x * 2) +
               ImGui::GetStyle().ItemSpacing.x;
    }
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + (8.0F * scale),
                             ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - total));
    for (std::size_t i = 0; i < 3; ++i) {
      const bool active = static_cast<std::size_t>(tab.responseSection) == i;
      ImGui::PushStyleColor(ImGuiCol_Button, active ? pal.accentSoft : 0U);
      ImGui::PushStyleColor(ImGuiCol_Text, active ? pal.accent : pal.textDim);
      ImGui::PushStyleColor(ImGuiCol_Border, 0U);
      if (ImGui::Button(sections[i])) {
        tab.responseSection = static_cast<ResponseSection>(i);
      }
      ImGui::PopStyleColor(3);
      if (i != 2) {
        ImGui::SameLine();
      }
    }
  }

  if (!response.error.empty()) {
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float iconSize = ImGui::GetFontSize() * 1.1F;
    const ImU32 color = response.state == ExchangeState::cancelled ? pal.textDim : pal.danger;
    DrawIcon(ImGui::GetWindowDrawList(), Icon::warning, ImVec2(pos.x + (iconSize * 0.5F), pos.y + (iconSize * 0.55F)),
             iconSize, color);
    ImGui::Indent(iconSize + (8.0F * scale));
    ImGui::PushTextWrapPos(0);
    TextColored(color, response.error);
    ImGui::PopTextWrapPos();
    ImGui::Unindent(iconSize + (8.0F * scale));
  }
  for (std::string_view warning : response.warnings) {
    TextColored(pal.warning, warning);
  }

  switch (tab.responseSection) {
    case ResponseSection::body:
      drawResponseBody(tab, ImGui::GetContentRegionAvail().y);
      break;
    case ResponseSection::headers:
      if (ImGui::BeginTable("##responseHeaders", 2,
                            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY |
                                ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 220.0F * scale);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (std::uint32_t i = 0; i < response.headers.size(); ++i) {
          const HeaderView& header = response.headers[i];
          ImGui::PushID(static_cast<int>(i));
          ImGui::TableNextRow();
          ImGui::TableSetColumnIndex(0);
          TextColored(SyntaxColor(TokenKind::key), header.name);
          ImGui::TableSetColumnIndex(1);
          ImGui::PushTextWrapPos(0);
          Text(header.value);
          ImGui::PopTextWrapPos();
          if (ImGui::IsItemClicked()) {
            CopyToClipboard(header.value);
            toast("Header value copied");
          }
          ImGui::PopID();
        }
        ImGui::EndTable();
      }
      break;
    case ResponseSection::sent:
      DrawSentRequest(tab);
      break;
  }
  ImGui::EndChild();
}

void App::DrawSentRequest(RequestTab& tab) {
  if (tab.sentText.empty() && tab.exchange) {
    GenerateSnippet(SnippetLanguage::rawHttp, tab.exchange->request, tab.sentText);
    tab.sentLines.build(tab.sentText, SyntaxLanguage::http);
    tab.sentView.reset();
  }
  TextDim("As built by Zalie (the client adds User-Agent, Accept-Encoding and the connection headers):");
  TextView("##sent", tab.sentText, tab.sentLines, tab.sentView,
           {.language = SyntaxLanguage::http, .size = ImVec2(0, 0), .lineNumbers = false, .border = false});
}

void App::drawResponseBody(RequestTab& tab, float height) {
  const Palette& pal = CurrentPalette();
  const Response& response = tab.exchange->response;
  const ResponseBody& body = response.body;
  const float scale = ImGui::GetStyle().FontScaleDpi;

  // Toolbar: Pretty | Raw, kind, JSONPath filter (JSON bodies), find, copy.
  const ImGuiStyle& style = ImGui::GetStyle();
  if (body.hasPretty()) {
    static constexpr std::array<const char*, 2> kModes{"Pretty", "Raw"};
    std::size_t mode = tab.prettyBody ? 0 : 1;
    if (SegmentedControl("##bodyMode", kModes, mode)) {
      tab.prettyBody = mode == 0;
      tab.bodyView.reset();
    }
    ImGui::SameLine();
  }
  ImGui::AlignTextToFramePadding();
  TextDim(ContentKindName(body.kind));
  const bool textual =
      body.kind != ContentKind::image && body.kind != ContentKind::binary && body.kind != ContentKind::empty;
  const bool filterable = body.kind == ContentKind::json;
  std::size_t nbMatches = 0;
  if (textual) {
    ImGui::SameLine();
    const float buttonsWidth = (ImGui::GetFrameHeight() * 3) + (style.ItemSpacing.x * 3);
    const float findWidth = std::min(220.0F * scale, ImGui::GetContentRegionAvail().x * 0.35F);
    if (filterable) {
      ImGui::SetNextItemWidth(
          std::max(80.0F * scale, ImGui::GetContentRegionAvail().x - findWidth - buttonsWidth - style.ItemSpacing.x));
      {
        ScopedFont mono(GetFonts().pMono, Fonts::kMonoSize);
        InputText("##jsonFilter", tab.jsonFilter, 0, "$.filter  (JSONPath)");
      }
      const bool submitted = ImGui::IsItemDeactivated();
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("Filter the body with a JSONPath: $.items[*].id, $..name, $.list[-1], $.list[0:5]...");
      }
      UpdateJsonFilter(tab, submitted);
      ImGui::SameLine();
    } else {
      ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                           std::max(0.0F, ImGui::GetContentRegionAvail().x - findWidth - buttonsWidth));
    }
    ImGui::SetNextItemWidth(findWidth);
    if (_focusFind) {
      ImGui::SetKeyboardFocusHere();
      _focusFind = false;
    }
    if (InputText("##find", tab.bodyView.query, ImGuiInputTextFlags_EnterReturnsTrue, "Find  (Ctrl+F)")) {
      GoToMatch(tab.bodyView, !ImGui::GetIO().KeyShift);
    }
  }
  // The filtered matches when a valid filter is applied, the whole body otherwise.
  const bool filtered = filterable && !tab.filterError && !tab.appliedFilter.empty();
  std::string_view text = body.text(tab.prettyBody);
  const LineIndex* pLines = &body.lines(tab.prettyBody);
  if (filtered) {
    text = tab.prettyBody ? std::string_view(tab.filteredPretty) : std::string_view(tab.filtered);
    pLines = tab.prettyBody ? &tab.filteredPrettyLines : &tab.filteredLines;
  }
  if (textual) {
    nbMatches = UpdateMatches(text, tab.bodyView);
    ImGui::SameLine();
    if (IconButton("##prev", Icon::chevronUp, "Previous match (Shift+Enter)")) {
      GoToMatch(tab.bodyView, false);
    }
    ImGui::SameLine();
    if (IconButton("##next", Icon::chevronDown, "Next match (Enter)")) {
      GoToMatch(tab.bodyView, true);
    }
    ImGui::SameLine();
    if (IconButton("##copyBody", Icon::copy, filtered ? "Copy the matches" : "Copy the body")) {
      CopyToClipboard(text);
      toast(filtered ? "Matches copied" : "Body copied");
    }
    // Outcome of the filter and of the find, on one line.
    bool sameLine = false;
    if (filterable && !tab.filterMessage.empty()) {
      TextColored(tab.filterError ? pal.warning : pal.textDim, tab.filterMessage);
      sameLine = true;
    }
    if (!tab.bodyView.query.empty()) {
      if (sameLine) {
        ImGui::SameLine();
        TextDim("\xC2\xB7");
        ImGui::SameLine();
      }
      std::array<char, 48> buffer{};
      TextDim(nbMatches == 0
                  ? std::string_view{"No match"}
                  : std::string_view(Label(buffer, "Match {} of {}", tab.bodyView.currentMatch + 1, nbMatches)));
    }
  }

  const float contentHeight = ImGui::GetContentRegionAvail().y;
  (void)height;
  switch (body.kind) {
    case ContentKind::empty:
      ImGui::Spacing();
      TextDim("No body.");
      break;
    case ContentKind::image: {
      if (!tab.imageTried) {
        tab.imageTried = true;
        tab.pImage = _textures.createFromImage(
            {reinterpret_cast<const unsigned char*>(body.raw.data()), static_cast<std::size_t>(body.raw.size())});
      }
      if (tab.pImage == nullptr) {
        TextColored(pal.warning, "This image format cannot be previewed.");
        HexView("##hex", body.raw, ImVec2(0, contentHeight));
        break;
      }
      const float imageWidth = static_cast<float>(tab.pImage->Width);
      const float imageHeight = static_cast<float>(tab.pImage->Height);
      const ImVec2 avail = ImGui::GetContentRegionAvail();
      const float fit = std::min({1.0F, avail.x / imageWidth, (avail.y - ImGui::GetFrameHeight()) / imageHeight});
      std::array<char, 48> buffer{};
      TextDim(Label(buffer, "{} x {} px", tab.pImage->Width, tab.pImage->Height));
      ImGui::Image(tab.pImage->GetTexRef(), ImVec2(imageWidth * fit, imageHeight * fit));
      break;
    }
    case ContentKind::binary:
      HexView("##hex", body.raw, ImVec2(0, contentHeight));
      break;
    default:
      TextView(
          "##body", text, *pLines, tab.bodyView,
          {.language = SyntaxFor(body.kind), .size = ImVec2(0, contentHeight), .lineNumbers = true, .border = false});
      break;
  }
}

void App::UpdateJsonFilter(RequestTab& tab, bool submitted) {
  // Up to this size the filter follows the typing; above, it is applied on Enter (or when the field loses focus).
  static constexpr std::size_t kLiveFilterMaxBytes = std::size_t{1} << 20U;
  static constexpr std::size_t kMaxFilteredBytes = std::size_t{256} << 20U;
  const std::string_view query = Trim(tab.jsonFilter);
  if (!tab.filterDirty && query == View(tab.appliedFilter)) {
    return;
  }
  const ResponseBody& body = tab.exchange->response.body;
  if (!submitted && !tab.filterDirty && body.raw.size() > kLiveFilterMaxBytes) {
    static constexpr std::string_view kHint = "Press Enter to apply the filter";
    if (View(tab.filterMessage) != kHint) {
      tab.filterMessage.assign(kHint);
    }
    return;
  }
  tab.filterDirty = false;
  tab.appliedFilter.assign(query);
  tab.filterError = false;
  tab.filterMessage.clear();
  tab.filtered.clear();
  tab.filteredPretty.clear();
  tab.filteredLines.clear();
  tab.filteredPrettyLines.clear();
  tab.bodyView.reset();
  if (query.empty()) {
    return;
  }
  auto result = EvaluateJsonPath(body.raw, query);
  if (!result) {
    tab.filterError = true;
    tab.filterMessage = std::move(result.error());
    return;
  }
  const std::size_t written = JoinJsonPathMatches(*result, tab.filtered, kMaxFilteredBytes);
  if (!PrettyPrintJson(tab.filtered, tab.filteredPretty)) {
    tab.filteredPretty.clear();  // cannot happen: the matches are valid JSON values
  }
  tab.filteredLines.build(tab.filtered, SyntaxLanguage::json);
  tab.filteredPrettyLines.build(tab.filteredPretty, SyntaxLanguage::json);

  const std::size_t nbMatches = result->matches.size();
  AppendAll(tab.filterMessage, nbMatches, std::string_view{nbMatches == 1 ? " match" : " matches"},
            std::string_view{result->truncated ? " (or more)" : ""}, " for ", query);
  if (written < nbMatches) {
    AppendAll(tab.filterMessage, " - the first ", written, " are shown");
  }
}

void App::drawPending(RequestTab& tab, float height) {
  const Palette& pal = CurrentPalette();
  const float scale = ImGui::GetStyle().FontScaleDpi;
  ImGui::BeginChild("##pending", ImVec2(0, height));
  const Progress& progress = *tab.ticket.progress;
  const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - progress.startedAt).count();
  const float spriteHeight = std::min(120.0F * scale, height * 0.45F);
  const float spriteWidth = spriteHeight * 4.0F / 3.0F;
  const float contentHeight = spriteHeight + (ImGui::GetTextLineHeightWithSpacing() * 3) + ImGui::GetFrameHeight();
  ImGui::Dummy(ImVec2(0, std::max(0.0F, (height - contentHeight) * 0.4F)));
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const float availWidth = ImGui::GetContentRegionAvail().x;
  // Zalie runs back and forth across the panel while the request is in flight.
  const double lap = std::fmod(elapsed * 0.3, 2.0);
  const bool goingBack = lap >= 1.0;
  const float progressAlong = static_cast<float>(goingBack ? 2.0 - lap : lap);
  const float travel = std::max(0.0F, std::min(availWidth, 560.0F * scale) - spriteWidth);
  const float startX = origin.x + ((availWidth - std::min(availWidth, 560.0F * scale)) * 0.5F);
  const float x = startX + (progressAlong * travel);
  _assets.drawSprite(ImGui::GetWindowDrawList(), Animation::run, elapsed, ImVec2(x, origin.y), spriteHeight, goingBack);
  ImGui::Dummy(ImVec2(availWidth, spriteHeight));

  std::array<char, 96> buffer{};
  std::array<char, 32> sizeBuffer{};
  std::array<char, 32> totalBuffer{};
  const std::uint64_t received = progress.bytesReceived.load(std::memory_order_relaxed);
  const std::uint64_t expected = progress.expectedBytes.load(std::memory_order_relaxed);
  {
    ScopedFont title(GetFonts().pUi, Fonts::kTitleSize);
    CenteredText(progress.headReceived.load(std::memory_order_relaxed) ? "Zalie is bringing the response back..."
                                                                       : "Zalie is fetching...",
                 pal.text);
  }
  std::string_view details;
  if (received == 0) {
    details = Label(buffer, "{:.1f} s", elapsed);
  } else if (expected != 0) {
    details = Label(buffer, "{:.1f} s  \xC2\xB7  {} of {}", elapsed, FormatBytes(received, sizeBuffer),
                    FormatBytes(expected, totalBuffer));
  } else {
    details = Label(buffer, "{:.1f} s  \xC2\xB7  {} received", elapsed, FormatBytes(received, sizeBuffer));
  }
  CenteredText(details, pal.textDim);
  if (expected != 0) {
    const float barWidth = std::min(availWidth * 0.6F, 360.0F * scale);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ((availWidth - barWidth) * 0.5F));
    ImGui::ProgressBar(static_cast<float>(static_cast<double>(received) / static_cast<double>(expected)),
                       ImVec2(barWidth, 6.0F * scale), "");
  }
  const float buttonWidth = 120.0F * scale;
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ((availWidth - buttonWidth) * 0.5F));
  if (ImGui::Button("Cancel (Esc)", ImVec2(buttonWidth, 0))) {
    Executor::Cancel(tab.ticket);
  }
  ImGui::EndChild();
}

void App::drawEmptyResponse(float height) {
  const Palette& pal = CurrentPalette();
  const float scale = ImGui::GetStyle().FontScaleDpi;
  ImGui::BeginChild("##emptyResponse", ImVec2(0, height));
  const float spriteHeight = std::min(110.0F * scale, height * 0.45F);
  const float spriteWidth = spriteHeight * 4.0F / 3.0F;
  ImGui::Dummy(ImVec2(0, std::max(0.0F, (height - spriteHeight) * 0.3F)));
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const float availWidth = ImGui::GetContentRegionAvail().x;
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - _startTime).count();
  _assets.drawSprite(ImGui::GetWindowDrawList(), Animation::sleep, seconds,
                     ImVec2(origin.x + ((availWidth - spriteWidth) * 0.5F), origin.y), spriteHeight);
  ImGui::Dummy(ImVec2(availWidth, spriteHeight));
  CenteredText("Zalie is napping. Send the request to wake her up!", pal.textDim);
  CenteredText("Ctrl+Enter to send  \xC2\xB7  Esc to cancel  \xC2\xB7  Ctrl+L to edit the URL", pal.textDim);
  ImGui::EndChild();
}

}  // namespace zalie::ui
