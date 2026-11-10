// Modal dialogs: import, new collection, rename, collection settings, settings, about.

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

#include "zalie/collection.hpp"
#include "zalie/curl-import.hpp"
#include "zalie/file-io.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/settings.hpp"
#include "zalie/string-utils.hpp"
#include "zalie/ui/app.hpp"
#include "zalie/ui/theme.hpp"
#include "zalie/ui/widgets.hpp"
#include "zalie/workspace.hpp"

namespace zalie::ui {

namespace {

constexpr const char* kImportPopup = "Import###import";
constexpr const char* kNewCollectionPopup = "New collection###newCollection";
constexpr const char* kRenamePopup = "Rename###rename";
constexpr const char* kCollectionPopup = "Collection settings###collectionSettings";
constexpr const char* kSettingsPopup = "Settings###settings";
constexpr const char* kAboutPopup = "About Zalie###about";

bool BeginModal(const char* name, ImVec2 size) {
  const ImGuiViewport* pViewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(pViewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5F, 0.5F));
  ImGui::SetNextWindowSize(size, ImGuiCond_Appearing);
  return ImGui::BeginPopupModal(name, nullptr, ImGuiWindowFlags_NoSavedSettings);
}

// Closes the current modal on Escape.
bool EscapePressed() { return ImGui::IsKeyPressed(ImGuiKey_Escape) && !ImGui::IsAnyItemActive(); }

}  // namespace

void App::drawDialogs() {
  switch (_pendingDialog) {
    case Dialog::import:
      _dialogText.clear();
      _dialogText2.clear();
      ImGui::OpenPopup(kImportPopup);
      break;
    case Dialog::newCollection:
      _dialogText.assign(std::string_view{"My API"});
      ImGui::OpenPopup(kNewCollectionPopup);
      break;
    case Dialog::rename:
      ImGui::OpenPopup(kRenamePopup);
      break;
    case Dialog::collectionSettings:
      ImGui::OpenPopup(kCollectionPopup);
      break;
    case Dialog::settings:
      ImGui::OpenPopup(kSettingsPopup);
      break;
    case Dialog::about:
      ImGui::OpenPopup(kAboutPopup);
      break;
    case Dialog::none:
      break;
  }
  _pendingDialog = Dialog::none;
  drawImportDialog();
  drawNewCollectionDialog();
  drawRenameDialog();
  drawCollectionSettingsDialog();
  drawSettingsDialog();
  drawAboutDialog();
}

void App::drawImportDialog() {
  const float scale = ImGui::GetStyle().FontScaleDpi;
  if (!BeginModal(kImportPopup, ImVec2(640.0F * scale, 0))) {
    return;
  }
  SectionTitle("COLLECTION");
  TextDim("A Bruno collection folder (or its bruno.json), a Bruno request (.bru), a Postman collection export (.json)");
  TextDim("or a Zalie collection (.zalie.json). Tip: you can also drop them on the window.");
  ImGui::SetNextItemWidth(-FLT_MIN);
  if (ImGui::IsWindowAppearing()) {
    ImGui::SetKeyboardFocusHere();
  }
  const bool enter = InputText("##path", _dialogText, ImGuiInputTextFlags_EnterReturnsTrue, "/path/to/collection");
  if ((PrimaryButton("Import collection") || enter) && !Trim(_dialogText).empty()) {
    if (importPath(PathFromUtf8(Trim(_dialogText)))) {
      ImGui::CloseCurrentPopup();
    }
  }
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();
  SectionTitle("CURL COMMAND");
  TextDim("Paste a curl command (\"Copy as cURL\" in the browser devtools works great):");
  {
    ScopedFont mono(GetFonts().pMono, Fonts::kMonoSize);
    InputTextMultiline("##curl", _dialogText2, ImVec2(-FLT_MIN, 140.0F * scale));
  }
  if (PrimaryButton("Create request") && !Trim(_dialogText2).empty()) {
    auto imported = ParseCurlCommand(_dialogText2);
    if (imported) {
      RequestTab& tab = newScratchTab();
      tab.scratch = std::move(imported->request);
      for (std::string_view warning : imported->warnings) {
        toast(warning, true);
      }
      toast("Request created from the curl command");
      ImGui::CloseCurrentPopup();
    } else {
      toast(imported.error(), true);
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Close") || EscapePressed()) {
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

void App::drawNewCollectionDialog() {
  const float scale = ImGui::GetStyle().FontScaleDpi;
  if (!BeginModal(kNewCollectionPopup, ImVec2(420.0F * scale, 0))) {
    return;
  }
  TextDim("Name of the collection:");
  ImGui::SetNextItemWidth(-FLT_MIN);
  if (ImGui::IsWindowAppearing()) {
    ImGui::SetKeyboardFocusHere();
  }
  const bool enter = InputText("##name", _dialogText, ImGuiInputTextFlags_EnterReturnsTrue);
  TextDim(View(Concat({"Saved in ", PathToUtf8(_workspace.collectionsDirectory())})));
  if ((PrimaryButton("Create") || enter) && !Trim(_dialogText).empty()) {
    OpenCollection& collection = _workspace.createCollection(Trim(_dialogText));
    createRequest(collection, collection.collection.root, "New request");
    ImGui::CloseCurrentPopup();
  }
  ImGui::SameLine();
  if (ImGui::Button("Cancel") || EscapePressed()) {
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

void App::drawRenameDialog() {
  const float scale = ImGui::GetStyle().FontScaleDpi;
  if (!BeginModal(kRenamePopup, ImVec2(420.0F * scale, 0))) {
    return;
  }
  ImGui::SetNextItemWidth(-FLT_MIN);
  if (ImGui::IsWindowAppearing()) {
    ImGui::SetKeyboardFocusHere();
  }
  const bool enter = InputText("##newName", _dialogText, ImGuiInputTextFlags_EnterReturnsTrue);
  if (PrimaryButton("Rename") || enter) {
    const std::string_view name = Trim(_dialogText);
    switch (_renameTarget) {
      case RenameTarget::request:
        if (const RequestLocation location = _workspace.findRequest(_renameId); location.valid()) {
          location.pRequest->name.assign(name);
          touch(location.pCollection);
        }
        break;
      case RenameTarget::folder:
        for (std::size_t i = 0; i < _workspace.size(); ++i) {
          if (Folder* pFolder = FindFolder(_workspace[i].collection.root, _renameId); pFolder != nullptr) {
            pFolder->name.assign(name);
            touch(&_workspace[i]);
          }
        }
        break;
      case RenameTarget::collection:
        if (_dialogCollection < _workspace.size()) {
          _workspace[_dialogCollection].collection.name.assign(name);
          touch(&_workspace[_dialogCollection]);
        }
        break;
    }
    ImGui::CloseCurrentPopup();
  }
  ImGui::SameLine();
  if (ImGui::Button("Cancel") || EscapePressed()) {
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

void App::drawCollectionSettingsDialog() {
  const float scale = ImGui::GetStyle().FontScaleDpi;
  if (!BeginModal(kCollectionPopup, ImVec2(820.0F * scale, 560.0F * scale))) {
    return;
  }
  if (_dialogCollection >= _workspace.size()) {
    ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return;
  }
  OpenCollection& open = _workspace[_dialogCollection];
  Collection& collection = open.collection;
  {
    ScopedFont title(GetFonts().pUi, Fonts::kTitleSize);
    Text(collection.name);
  }
  TextDim(PathToUtf8(open.path));
  const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
  ImGui::BeginChild("##collectionTabs", ImVec2(0, -footer));
  if (ImGui::BeginTabBar("##collectionSections")) {
    if (ImGui::BeginTabItem("Variables")) {
      TextDim("Available to every request as {{name}}. The active environment overrides them.");
      if (KeyValueTable("##collectionVars", collection.variables, {.keyHint = "Variable", .valueHint = "Value"})) {
        touch(&open);
      }
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Environments")) {
      const float listWidth = 200.0F * scale;
      ImGui::BeginChild("##envList", ImVec2(listWidth, 0), ImGuiChildFlags_Borders);
      for (std::uint32_t i = 0; i < collection.environments.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        const std::string_view name = collection.environments[i].name;
        RawChars32 label = Concat({name.empty() ? std::string_view{"(unnamed)"} : name,
                                   std::cmp_equal(i, open.activeEnvironment) ? "  (active)" : ""});
        label.push_back('\0');
        if (ImGui::Selectable(label.data(), std::cmp_equal(i, _collectionSettingsEnv))) {
          _collectionSettingsEnv = static_cast<int>(i);
        }
        ImGui::PopID();
      }
      if (ImGui::Button("+ Add environment", ImVec2(-FLT_MIN, 0))) {
        Environment env;
        env.name.assign(std::string_view{"new environment"});
        collection.environments.push_back(std::move(env));
        _collectionSettingsEnv = static_cast<int>(collection.environments.size()) - 1;
        touch(&open);
      }
      ImGui::EndChild();
      ImGui::SameLine();
      ImGui::BeginChild("##envDetails");
      if (_collectionSettingsEnv >= 0 && std::cmp_less(_collectionSettingsEnv, collection.environments.size())) {
        Environment& env = collection.environments[static_cast<std::uint32_t>(_collectionSettingsEnv)];
        ImGui::SetNextItemWidth(260.0F * scale);
        if (InputText("Name", env.name)) {
          touch(&open);
        }
        ImGui::SameLine();
        const bool active = open.activeEnvironment == _collectionSettingsEnv;
        if (ImGui::Button(active ? "Deactivate" : "Activate")) {
          open.activeEnvironment = active ? -1 : _collectionSettingsEnv;
          touch(&open);
        }
        ImGui::SameLine();
        if (ImGui::Button("Delete")) {
          collection.environments.erase(collection.environments.begin() + _collectionSettingsEnv);
          if (open.activeEnvironment == _collectionSettingsEnv) {
            open.activeEnvironment = -1;
          } else if (open.activeEnvironment > _collectionSettingsEnv) {
            --open.activeEnvironment;
          }
          _collectionSettingsEnv = std::max(0, _collectionSettingsEnv - 1);
          touch(&open);
        } else if (KeyValueTable("##envVars", env.variables, {.keyHint = "Variable", .valueHint = "Value"})) {
          touch(&open);
        }
      } else {
        TextDim("No environment. Environments group variables (local, staging, prod...).");
      }
      ImGui::EndChild();
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Headers")) {
      TextDim("Sent with every request of the collection (a request header with the same name wins).");
      if (KeyValueTable("##collectionHeaders", collection.headers, {.keyHint = "Header", .valueHint = "Value"})) {
        touch(&open);
      }
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Auth")) {
      TextDim("Used by the requests whose authentication is 'Inherit from the collection' (the default).");
      if (DrawAuthEditor(collection.auth, false)) {
        touch(&open);
      }
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Docs")) {
      ScopedFont mono(GetFonts().pMono, Fonts::kMonoSize);
      if (InputTextMultiline("##collectionDocs", collection.docs, ImVec2(-FLT_MIN, -FLT_MIN))) {
        touch(&open);
      }
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }
  ImGui::EndChild();
  if (PrimaryButton("Done") || EscapePressed()) {
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

void App::drawSettingsDialog() {
  const float scale = ImGui::GetStyle().FontScaleDpi;
  if (!BeginModal(kSettingsPopup, ImVec2(520.0F * scale, 0))) {
    return;
  }
  Settings& settings = _workspace.settings();
  SectionTitle("APPEARANCE");
  int theme = _theme == ThemeKind::nuit ? 1 : 0;
  if (ImGui::RadioButton("Coton (light)", &theme, 0) || ImGui::RadioButton("Nuit (dark)", &theme, 1)) {
    _theme = theme == 1 ? ThemeKind::nuit : ThemeKind::coton;
    settings.theme.assign(ThemeId(_theme));
    _themeDirty = true;
  }
  ImGui::SetNextItemWidth(220.0F * scale);
  static float zoom = 1.0F;  // edited value, applied when the slider is released (the UI rescales)
  if (ImGui::IsWindowAppearing()) {
    zoom = settings.uiScale;
  }
  ImGui::SliderFloat("Zoom (Ctrl+= / Ctrl+-)", &zoom, 0.7F, 2.0F, "%.1fx");
  if (ImGui::IsItemDeactivatedAfterEdit()) {
    settings.uiScale = zoom;
    _themeDirty = true;
  }
  ImGui::Checkbox("Pretty-print JSON responses by default", &settings.prettyJson);
  ImGui::Checkbox("Show the response beside the request (wide windows)", &settings.responseBeside);
  ImGui::Spacing();
  SectionTitle("BEHAVIOUR");
  ImGui::Checkbox("Save collections automatically", &settings.autosave);
  ImGui::SetNextItemWidth(160.0F * scale);
  static constexpr std::uint32_t kStep = 64;
  ImGui::InputScalar("Max response body (MiB)", ImGuiDataType_U32, &settings.maxBodyMiB, &kStep);
  settings.maxBodyMiB = std::clamp(settings.maxBodyMiB, 1U, 4096U);
  ImGui::SetNextItemWidth(160.0F * scale);
  static constexpr std::uint32_t kOne = 1;
  ImGui::InputScalar("Parallel requests", ImGuiDataType_U32, &settings.workers, &kOne);
  settings.workers = std::clamp(settings.workers, 1U, 32U);
  TextDim("The last two settings apply at the next start.");
  ImGui::Spacing();
  SectionTitle("PERFORMANCE RUNS");
  ImGui::SetNextItemWidth(160.0F * scale);
  ImGui::InputScalar("Connection limit", ImGuiDataType_U32, &settings.maxLoadConnections, &kOne);
  settings.maxLoadConnections = std::min(settings.maxLoadConnections, 4096U);
  TextDim("Each connection of a run is a thread. 0: the number of vCPUs.");
  ImGui::Spacing();
  SectionTitle("DATA");
  TextDim(View(Concat({"Settings, history and collections: ", PathToUtf8(_workspace.home())})));
  ImGui::Spacing();
  if (PrimaryButton("Close") || EscapePressed()) {
    _workspace.saveAll();
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

void App::drawAboutDialog() {
  const float scale = ImGui::GetStyle().FontScaleDpi;
  if (!BeginModal(kAboutPopup, ImVec2(460.0F * scale, 0))) {
    return;
  }
  const float logoSize = 96.0F * scale;
  ImGui::SetCursorPosX(((ImGui::GetContentRegionAvail().x - logoSize) * 0.5F) + ImGui::GetCursorPosX());
  ImGui::Image(_assets.logo(), ImVec2(logoSize, logoSize));
  {
    ScopedFont title(GetFonts().pUi, Fonts::kTitleSize * 1.2F);
    Text("Zalie " ZALIE_VERSION_STR);
  }
  ImGui::PushTextWrapPos(0);
  Text("A light, fast and friendly HTTP client.");
  TextDim("Named after Zalie, a fluffy Coton de Tulear who fetches everything you throw at her.");
  ImGui::Spacing();
  TextDim("Built with aeronet (HTTP/1.1 and HTTP/2 client), Dear ImGui, GLFW, glaze, amc and stb.");
  TextDim("MIT License.");
  ImGui::PopTextWrapPos();
  ImGui::Spacing();
  if (PrimaryButton("Close") || EscapePressed()) {
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

}  // namespace zalie::ui
