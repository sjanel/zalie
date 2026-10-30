// Left panel: collections tree and history.

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string_view>
#include <utility>

#include "zalie/collection.hpp"
#include "zalie/exchange.hpp"
#include "zalie/file-io.hpp"
#include "zalie/history.hpp"
#include "zalie/method.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/request.hpp"
#include "zalie/resolved-request.hpp"
#include "zalie/snippets.hpp"
#include "zalie/string-utils.hpp"
#include "zalie/ui/app.hpp"
#include "zalie/ui/icons.hpp"
#include "zalie/ui/theme.hpp"
#include "zalie/ui/widgets.hpp"
#include "zalie/url.hpp"
#include "zalie/variables.hpp"
#include "zalie/workspace.hpp"

namespace zalie::ui {

namespace {

constexpr const char* kRequestPayload = "ZALIE_REQUEST";

int Size(std::string_view text) noexcept { return static_cast<int>(text.size()); }

// Payload of a dragged request.
struct DraggedRequest {
  std::uint64_t requestId;
};

// Puts the method and the URL (as sent) of a history entry in 'request'.
void LoadHistoryEntry(const HistoryEntry& entry, Request& request) {
  request.method = entry.method;
  request.url.assign(std::string_view(entry.url));
  SyncParamsFromUrl(request);
}

}  // namespace

bool App::matchesFilter(const Request& request) const {
  const std::string_view filter = Trim(_filter);
  return filter.empty() || FindIgnoreCase(request.name, filter) != std::string_view::npos ||
         FindIgnoreCase(request.url, filter) != std::string_view::npos;
}

bool App::folderMatchesFilter(const Folder& folder) const {
  if (Trim(_filter).empty() || FindIgnoreCase(folder.name, Trim(_filter)) != std::string_view::npos) {
    return true;
  }
  return std::ranges::any_of(folder.requests, [this](const Request& request) { return matchesFilter(request); }) ||
         std::ranges::any_of(folder.folders, [this](const Folder& child) { return folderMatchesFilter(child); });
}

void App::createRequest(OpenCollection& collection, Folder& folder, std::string_view name) {
  Request request;
  request.name.assign(name);
  request.id = NewId();
  request.auth.mode = AuthMode::inherit;
  const std::uint64_t id = request.id;
  folder.requests.push_back(std::move(request));
  touch(&collection);
  openRequest(id);
  if (RequestTab* pTab = activeTab(); pTab != nullptr) {
    pTab->focusUrl = true;
  }
}

void App::deleteRequest(OpenCollection& collection, std::uint64_t requestId) {
  for (std::uint32_t i = 0; i < _tabs.size(); ++i) {
    if (_tabs[i]->requestId == requestId) {
      closeTab(i);
      break;
    }
  }
  RemoveRequest(collection.collection.root, requestId);
  touch(&collection);
}

void App::drawSidebar(ImVec2 size) {
  const Palette& pal = CurrentPalette();
  const ImGuiStyle& style = ImGui::GetStyle();
  ImGui::PushStyleColor(ImGuiCol_ChildBg, pal.panel);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(style.WindowPadding.x * 0.8F, style.WindowPadding.y));
  ImGui::BeginChild("##sidebar", size, ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
  ImGui::PopStyleVar();
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x * 0.75F, style.ItemSpacing.y));

  // Filter + "new" menu.
  const float frameHeight = ImGui::GetFrameHeight();
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - (frameHeight * 2) - (style.ItemSpacing.x * 2));
  if (_focusFilter) {
    ImGui::SetKeyboardFocusHere();
    _focusFilter = false;
  }
  InputText("##filter", _filter, 0, "Filter requests  (Ctrl+K)");
  ImGui::SameLine();
  if (IconButton("##import", Icon::import, "Import a Bruno / Postman collection or a curl command")) {
    _pendingDialog = Dialog::import;
  }
  ImGui::SameLine();
  if (IconButton("##newMenu", Icon::plus, "New request, collection, import...")) {
    ImGui::OpenPopup("##newPopup");
  }
  if (ImGui::BeginPopup("##newPopup")) {
    if (ImGui::MenuItem("New request", "Ctrl+T")) {
      newScratchTab();
    }
    if (ImGui::MenuItem("New collection...")) {
      _pendingDialog = Dialog::newCollection;
    }
    if (ImGui::MenuItem("Import collection / curl...")) {
      _pendingDialog = Dialog::import;
    }
    ImGui::EndPopup();
  }

  // Collections / History switch.
  ImGui::Spacing();
  if (ImGui::BeginTabBar("##sideTabs", ImGuiTabBarFlags_NoTooltip)) {
    const auto sideTab = [this](const char* label, bool history) {
      const bool requested = _selectSideTab && _showHistory == history;
      const bool open = ImGui::BeginTabItem(label, nullptr, requested ? ImGuiTabItemFlags_SetSelected : 0);
      if (requested) {
        _selectSideTab = false;
      } else if (open && !_selectSideTab) {
        _showHistory = history;
      }
      if (open) {
        ImGui::EndTabItem();
      }
    };
    sideTab("Collections", false);
    sideTab("History", true);
    ImGui::EndTabBar();
  }
  ImGui::BeginChild("##sideContent", ImVec2(0, 0), ImGuiChildFlags_None);
  if (_showHistory) {
    drawHistory();
  } else {
    drawCollectionsTree();
  }
  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::EndChild();
  ImGui::PopStyleColor();
}

void App::drawCollectionsTree() {
  const Palette& pal = CurrentPalette();
  if (_workspace.size() == 0) {
    ImGui::Spacing();
    TextDim("No collection yet.");
    if (ImGui::Button("Create a collection")) {
      _pendingDialog = Dialog::newCollection;
    }
    if (ImGui::Button("Import (Bruno, Postman)")) {
      _pendingDialog = Dialog::import;
    }
    TextDim("Tip: drop a Bruno collection folder or a Postman export on the window.");
    return;
  }
  const bool filtering = !Trim(_filter).empty();
  for (std::size_t i = 0; i < _workspace.size(); ++i) {
    OpenCollection& collection = _workspace[i];
    if (filtering && !folderMatchesFilter(collection.collection.root)) {
      continue;
    }
    ImGui::PushID(&collection);
    if (filtering) {
      ImGui::SetNextItemOpen(true);
    }
    const std::string_view name = collection.collection.name;
    ImGui::PushStyleColor(ImGuiCol_Text, pal.text);
    const bool open = ImGui::TreeNodeEx(
        "##collection",
        ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding, "%.*s%s",
        Size(name), name.data(), collection.dirty ? " *" : "");
    ImGui::PopStyleColor();
    if (ImGui::BeginDragDropTarget()) {
      if (const ImGuiPayload* pPayload = ImGui::AcceptDragDropPayload(kRequestPayload); pPayload != nullptr) {
        const auto* pDragged = static_cast<const DraggedRequest*>(pPayload->Data);
        if (MoveRequest(collection.collection.root, pDragged->requestId, collection.collection.root.id,
                        collection.collection.root.requests.size())) {
          touch(&collection);
        }
      }
      ImGui::EndDragDropTarget();
    }
    if (ImGui::BeginPopupContextItem("##collectionMenu")) {
      if (ImGui::MenuItem("New request")) {
        createRequest(collection, collection.collection.root, "New request");
      }
      if (ImGui::MenuItem("New folder")) {
        Folder folder;
        folder.name.assign(std::string_view{"New folder"});
        folder.id = NewId();
        collection.collection.root.folders.push_back(std::move(folder));
        touch(&collection);
      }
      ImGui::Separator();
      if (ImGui::MenuItem("Variables, environments, auth...")) {
        _dialogCollection = i;
        _collectionSettingsEnv = std::max(0, collection.activeEnvironment);
        _pendingDialog = Dialog::collectionSettings;
      }
      if (ImGui::MenuItem("Rename...")) {
        _dialogCollection = i;
        _renameTarget = RenameTarget::collection;
        _dialogText.assign(name);
        _pendingDialog = Dialog::rename;
      }
      ImGui::Separator();
      if (ImGui::MenuItem("Close collection")) {
        // Closing keeps the file: tabs of its requests are closed first.
        for (std::uint32_t tabIdx = _tabs.size(); tabIdx-- > 0;) {
          if (collectionOf(*_tabs[tabIdx]) == &collection) {
            closeTab(tabIdx);
          }
        }
        ImGui::EndPopup();
        if (open) {
          ImGui::TreePop();
        }
        ImGui::PopID();
        _workspace.closeCollection(i);
        return;  // the list changed: draw it again next frame
      }
      ImGui::EndPopup();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      const RawChars32 path = PathToUtf8(collection.path);
      ImGui::SetTooltip("%.*s", Size(path), path.data());
    }
    if (open) {
      drawFolder(collection, collection.collection.root, 0);
      ImGui::TreePop();
    }
    ImGui::PopID();
  }
}

void App::drawFolder(OpenCollection& collection, Folder& folder, int depth) {
  const Palette& pal = CurrentPalette();
  const bool filtering = !Trim(_filter).empty();
  for (Folder& child : folder.folders) {
    if (filtering && !folderMatchesFilter(child)) {
      continue;
    }
    PushId(child.id);
    if (filtering) {
      ImGui::SetNextItemOpen(true);
    }
    const std::string_view name = child.name;
    const bool open = ImGui::TreeNodeEx("##folder", ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen,
                                        "     %.*s", Size(name), name.data());
    const ImVec2 min = ImGui::GetItemRectMin();
    const float iconSize = ImGui::GetFontSize();
    DrawIcon(ImGui::GetWindowDrawList(), Icon::folder,
             ImVec2(min.x + ImGui::GetTreeNodeToLabelSpacing() + (iconSize * 0.55F),
                    min.y + (ImGui::GetItemRectSize().y * 0.5F)),
             iconSize * 0.95F, pal.accent);
    if (ImGui::BeginDragDropTarget()) {
      if (const ImGuiPayload* pPayload = ImGui::AcceptDragDropPayload(kRequestPayload); pPayload != nullptr) {
        const auto* pDragged = static_cast<const DraggedRequest*>(pPayload->Data);
        if (MoveRequest(collection.collection.root, pDragged->requestId, child.id, child.requests.size())) {
          touch(&collection);
        }
      }
      ImGui::EndDragDropTarget();
    }
    bool deleted = false;
    if (ImGui::BeginPopupContextItem("##folderMenu")) {
      if (ImGui::MenuItem("New request")) {
        createRequest(collection, child, "New request");
      }
      if (ImGui::MenuItem("New sub-folder")) {
        Folder sub;
        sub.name.assign(std::string_view{"New folder"});
        sub.id = NewId();
        child.folders.push_back(std::move(sub));
        touch(&collection);
      }
      if (ImGui::MenuItem("Rename...")) {
        _renameTarget = RenameTarget::folder;
        _renameId = child.id;
        _dialogText.assign(name);
        _pendingDialog = Dialog::rename;
      }
      ImGui::Separator();
      if (ImGui::MenuItem("Delete folder")) {
        deleted = true;
      }
      ImGui::EndPopup();
    }
    if (open) {
      if (!deleted) {
        drawFolder(collection, child, depth + 1);
      }
      ImGui::TreePop();
    }
    ImGui::PopID();
    if (deleted) {
      // Close the tabs of the requests of the folder, then remove it.
      ForEachRequest(child, [this](const Request& request) {
        for (std::uint32_t tabIdx = 0; tabIdx < _tabs.size(); ++tabIdx) {
          if (_tabs[tabIdx]->requestId == request.id) {
            closeTab(tabIdx);
            break;
          }
        }
      });
      RemoveFolder(collection.collection.root, child.id);
      touch(&collection);
      return;
    }
  }
  for (std::uint32_t i = 0; i < folder.requests.size(); ++i) {
    Request& request = folder.requests[i];
    if (filtering && !matchesFilter(request)) {
      continue;
    }
    const std::size_t sizeBefore = folder.requests.size();
    drawRequestItem(collection, folder, request);
    if (folder.requests.size() != sizeBefore) {
      return;  // modified (deleted / duplicated): references are invalidated
    }
  }
}

void App::drawRequestItem(OpenCollection& collection, Folder& folder, Request& request) {
  const Palette& pal = CurrentPalette();
  PushId(request.id);
  const RequestTab* pActive = activeTab();
  const bool selected = pActive != nullptr && pActive->requestId == request.id;
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  const float rowHeight = ImGui::GetFrameHeight() * 0.92F;
  ImGui::Indent(ImGui::GetTreeNodeToLabelSpacing() * 0.35F);
  if (ImGui::Selectable("##request", selected, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, rowHeight))) {
    openRequest(request.id);
    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
      _renameTarget = RenameTarget::request;
      _renameId = request.id;
      _dialogText.assign(request.name);
      _pendingDialog = Dialog::rename;
    }
  }
  ImGui::Unindent(ImGui::GetTreeNodeToLabelSpacing() * 0.35F);
  if (ImGui::BeginDragDropSource()) {
    const DraggedRequest dragged{request.id};
    ImGui::SetDragDropPayload(kRequestPayload, &dragged, sizeof(dragged));
    MethodLabel(request.method);
    ImGui::SameLine();
    Text(request.name);
    ImGui::EndDragDropSource();
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && !request.url.empty()) {
    ImGui::SetTooltip("%.*s", Size(request.url), request.url.data());
  }

  // Method + name, drawn over the selectable.
  ImDrawList* pDrawList = ImGui::GetWindowDrawList();
  const float indent = ImGui::GetTreeNodeToLabelSpacing() * 0.55F;
  {
    ScopedFont mono(GetFonts().pMono, Fonts::kMonoSize * 0.82F);
    const std::string_view method = MethodShortName(request.method);
    const float methodY = pos.y + ((rowHeight - ImGui::GetFontSize()) * 0.5F);
    pDrawList->AddText(ImVec2(pos.x + indent, methodY), MethodColor(request.method), method.data(),
                       method.data() + method.size());
  }
  const float nameX = pos.x + indent + (ImGui::GetFontSize() * 3.1F);
  const std::string_view name = request.name.empty() ? std::string_view{"(unnamed)"} : std::string_view(request.name);
  const float clipRight = ImGui::GetWindowPos().x + ImGui::GetWindowSize().x - ImGui::GetStyle().WindowPadding.x;
  pDrawList->PushClipRect(ImVec2(nameX, pos.y), ImVec2(clipRight, pos.y + rowHeight), true);
  pDrawList->AddText(ImVec2(nameX, pos.y + ((rowHeight - ImGui::GetFontSize()) * 0.5F)),
                     selected ? pal.text : WithAlpha(pal.text, 0.88F), name.data(), name.data() + name.size());
  pDrawList->PopClipRect();
  if (findTab(request.id) != nullptr && findTab(request.id)->pending()) {
    pDrawList->AddCircleFilled(ImVec2(clipRight - 6.0F, pos.y + (rowHeight * 0.5F)), 3.0F, pal.accent);
  }

  if (ImGui::BeginPopupContextItem("##requestMenu")) {
    if (ImGui::MenuItem("Open")) {
      openRequest(request.id);
    }
    if (ImGui::MenuItem("Rename...")) {
      _renameTarget = RenameTarget::request;
      _renameId = request.id;
      _dialogText.assign(request.name);
      _pendingDialog = Dialog::rename;
    }
    if (ImGui::MenuItem("Copy as cURL")) {
      const VariableScope scope = MakeScope(collection.collection, collection.activeEnvironment);
      const ResolvedRequest resolved = ResolveRequest(request, collection.collection, scope);
      RawChars32 snippet;
      GenerateSnippet(SnippetLanguage::curl, resolved, snippet);
      CopyToClipboard(snippet);
      toast("cURL command copied");
    }
    if (ImGui::MenuItem("Duplicate")) {
      if (const Request* pCopy = DuplicateRequest(folder, request.id); pCopy != nullptr) {
        const std::uint64_t copyId = pCopy->id;
        touch(&collection);
        ImGui::EndPopup();
        ImGui::PopID();
        openRequest(copyId);
        return;
      }
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Delete")) {
      const std::uint64_t id = request.id;
      ImGui::EndPopup();
      ImGui::PopID();
      deleteRequest(collection, id);
      return;
    }
    ImGui::EndPopup();
  }
  ImGui::PopID();
}

void App::showHistory(bool show) noexcept {
  _showHistory = show;
  _selectSideTab = true;
}

void App::openHistoryEntry(std::size_t index) {
  const History& history = _workspace.history();
  if (index >= history.entries().size()) {
    return;
  }
  const HistoryEntry& entry = history.entries()[static_cast<std::uint32_t>(index)];
  if (entry.requestId != 0 && _workspace.findRequest(entry.requestId).valid()) {
    openRequest(entry.requestId);
  } else {
    LoadHistoryEntry(entry, newScratchTab().scratch);
  }
}

void App::copyHistoryEntry(std::size_t index) {
  const History& history = _workspace.history();
  if (index >= history.entries().size()) {
    return;
  }
  const HistoryEntry& entry = history.entries()[static_cast<std::uint32_t>(index)];
  RequestTab* pTab = activeTab();
  Request* pRequest = pTab == nullptr ? nullptr : requestOf(*pTab);
  if (pRequest == nullptr) {
    LoadHistoryEntry(entry, newScratchTab().scratch);
    return;
  }
  OpenCollection* pCollection = collectionOf(*pTab);
  static const Collection kNoCollection;
  const Collection& collection = pCollection == nullptr ? kNoCollection : pCollection->collection;
  const VariableScope scope = MakeScope(collection, pCollection == nullptr ? -1 : pCollection->activeEnvironment);
  const ResolvedRequest resolved = ResolveRequest(*pRequest, collection, scope);
  if (resolved.method == entry.method && resolved.url == std::string_view(entry.url)) {
    return;  // the request already sends this: its {{variables}} are kept
  }
  LoadHistoryEntry(entry, *pRequest);
  touch(pCollection);
}

void App::drawHistory() {
  const Palette& pal = CurrentPalette();
  History& history = _workspace.history();
  if (history.entries().empty()) {
    ImGui::Spacing();
    TextDim("Sent requests appear here.");
    return;
  }
  if (ImGui::SmallButton("Clear history")) {
    history.clear();
    return;
  }
  ImGui::Spacing();
  std::array<char, 32> agoBuffer{};
  std::array<char, 16> statusBuffer{};
  const std::string_view filter = Trim(_filter);
  const auto drawEntry = [&](std::size_t row) {
    const HistoryEntry& entry = history.entries()[static_cast<std::uint32_t>(row)];
    ImGui::PushID(static_cast<int>(row));
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float rowHeight = ImGui::GetFrameHeight() * 1.5F;
    const bool clicked =
        ImGui::Selectable("##entry", false, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, rowHeight));
    ImDrawList* pDrawList = ImGui::GetWindowDrawList();
    const float lineHeight = ImGui::GetFontSize();
    const float clipRight = ImGui::GetWindowPos().x + ImGui::GetWindowSize().x - ImGui::GetStyle().WindowPadding.x;
    {
      ScopedFont mono(GetFonts().pMono, Fonts::kMonoSize * 0.82F);
      const std::string_view method = MethodToStr(entry.method);
      pDrawList->AddText(ImVec2(pos.x + 4, pos.y + 3), MethodColor(entry.method), method.data(),
                         method.data() + method.size());
    }
    std::string_view status =
        entry.state == ExchangeState::cancelled ? std::string_view{"cancelled"} : std::string_view{"error"};
    if (entry.state == ExchangeState::completed) {
      const auto result = std::format_to_n(statusBuffer.data(), 8, "{}", entry.status);
      status = std::string_view(statusBuffer.data(), static_cast<std::size_t>(result.out - statusBuffer.data()));
    }
    pDrawList->AddText(ImVec2(pos.x + (lineHeight * 3.6F), pos.y + 2), StatusColor(entry.status, entry.state),
                       status.data(), status.data() + status.size());
    const std::string_view ago = FormatAgo(entry.timestampMs, agoBuffer);
    const float agoWidth = ImGui::CalcTextSize(ago.data(), ago.data() + ago.size()).x;
    pDrawList->AddText(ImVec2(clipRight - agoWidth, pos.y + 2), pal.textDim, ago.data(), ago.data() + ago.size());
    pDrawList->PushClipRect(ImVec2(pos.x, pos.y), ImVec2(clipRight, pos.y + rowHeight), true);
    const std::string_view url = entry.url;
    pDrawList->AddText(ImVec2(pos.x + 4, pos.y + 2 + (lineHeight * 1.15F)), WithAlpha(pal.text, 0.8F), url.data(),
                       url.data() + url.size());
    pDrawList->PopClipRect();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("%.*s", Size(url), url.data());
    }
    if (clicked) {
      // The first click of a double click opens the entry: the second one fills the request it opened.
      if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        copyHistoryEntry(row);
      } else {
        openHistoryEntry(row);
      }
    }
    ImGui::PopID();
  };
  if (!filter.empty()) {
    for (std::uint32_t row = 0; row < history.entries().size(); ++row) {
      if (FindIgnoreCase(history.entries()[row].url, filter) != std::string_view::npos) {
        drawEntry(row);
      }
    }
    return;
  }
  // Unfiltered: only the visible rows are drawn, whatever the size of the history.
  ImGuiListClipper clipper;
  clipper.Begin(static_cast<int>(history.entries().size()));
  while (clipper.Step()) {
    for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
      drawEntry(static_cast<std::size_t>(row));
    }
  }
}

}  // namespace zalie::ui
