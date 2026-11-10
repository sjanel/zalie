// Right panel: configurable cards about the active request (code snippet, timeline, variables...).

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <amc/vector.hpp>
#include <array>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string_view>
#include <utility>

#include "zalie/collection.hpp"
#include "zalie/exchange.hpp"
#include "zalie/key-value.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/request.hpp"
#include "zalie/resolved-request.hpp"
#include "zalie/settings.hpp"
#include "zalie/snippets.hpp"
#include "zalie/string-utils.hpp"
#include "zalie/syntax.hpp"
#include "zalie/ui/app.hpp"
#include "zalie/ui/icons.hpp"
#include "zalie/ui/text-view.hpp"
#include "zalie/ui/theme.hpp"
#include "zalie/ui/widgets.hpp"
#include "zalie/variables.hpp"
#include "zalie/workspace.hpp"

namespace zalie::ui {

namespace {

const char* CardTitle(std::string_view id) noexcept {
  if (id == kCardSnippet) {
    return "Code snippet";
  }
  if (id == kCardTimings) {
    return "Timeline";
  }
  if (id == kCardVariables) {
    return "Variables";
  }
  if (id == kCardResponseHeaders) {
    return "Response headers";
  }
  return "Notes";
}

SyntaxLanguage SnippetSyntax(SnippetLanguage language) noexcept {
  switch (language) {
    case SnippetLanguage::curl:
    case SnippetLanguage::httpie:
    case SnippetLanguage::wget:
      return SyntaxLanguage::shell;
    case SnippetLanguage::rawHttp:
      return SyntaxLanguage::http;
    default:
      return SyntaxLanguage::code;
  }
}

// Variable names that probably hold secrets: their values are masked.
bool LooksSecret(std::string_view name) noexcept {
  static constexpr std::array kSecretWords{std::string_view{"token"},    std::string_view{"secret"},
                                           std::string_view{"password"}, std::string_view{"passwd"},
                                           std::string_view{"apikey"},   std::string_view{"api_key"},
                                           std::string_view{"key"},      std::string_view{"credential"}};
  return std::ranges::any_of(kSecretWords, [name](std::string_view word) { return ContainsIgnoreCase(name, word); });
}

void CollectRefs(std::string_view text, amc::vector<std::string_view>& names) {
  VariableRefs refs;
  FindVariableRefs(text, refs);
  for (const VariableRef& ref : refs) {
    if (!ref.name.empty() && !std::ranges::contains(names, ref.name)) {
      names.push_back(ref.name);
    }
  }
}

void Row(const char* name, std::string_view value) {
  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  TextDim(name);
  ImGui::TableSetColumnIndex(1);
  Text(value);
}

}  // namespace

void App::drawInspector(ImVec2 size) {
  const Palette& pal = CurrentPalette();
  const ImGuiStyle& style = ImGui::GetStyle();
  ImGui::PushStyleColor(ImGuiCol_ChildBg, pal.panel);
  ImGui::BeginChild("##inspector", size, ImGuiChildFlags_AlwaysUseWindowPadding);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x * 0.8F, style.ItemSpacing.y * 0.9F));

  ImGui::AlignTextToFramePadding();
  {
    ScopedFont title(GetFonts().pUi, Fonts::kUiSize * 1.08F);
    Text("Inspector");
  }
  ImGui::SameLine(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight());
  if (IconButton("##inspectorMenu", Icon::dots, "Choose and order the cards")) {
    ImGui::OpenPopup("##inspectorMenuPopup");
  }
  if (ImGui::BeginPopup("##inspectorMenuPopup")) {
    drawInspectorMenu();
    ImGui::EndPopup();
  }

  RequestTab* pTab = activeTab();
  Request* pRequest = pTab == nullptr ? nullptr : requestOf(*pTab);
  if (pTab == nullptr || pRequest == nullptr) {
    ImGui::Spacing();
    TextDim("Open a request to inspect it.");
  } else {
    for (InspectorCard& card : _workspace.settings().inspectorCards) {
      if (!card.visible) {
        continue;
      }
      const std::string_view id = card.id;
      ImGui::PushID(card.id.data(), card.id.data() + card.id.size());
      ImGui::Spacing();
      if (ImGui::CollapsingHeader(CardTitle(id), ImGuiTreeNodeFlags_DefaultOpen)) {
        if (id == kCardSnippet) {
          drawSnippetCard(*pTab, *pRequest);
        } else if (id == kCardTimings) {
          DrawTimingsCard(*pTab);
        } else if (id == kCardVariables) {
          drawVariablesCard(*pTab, *pRequest);
        } else if (id == kCardResponseHeaders) {
          drawResponseHeadersCard(*pTab);
        } else {
          DrawNotesCard(*pRequest);
        }
      }
      ImGui::PopID();
    }
  }
  ImGui::PopStyleVar();
  ImGui::EndChild();
  ImGui::PopStyleColor();
}

void App::drawInspectorMenu() {
  amc::vector<InspectorCard>& cards = _workspace.settings().inspectorCards;
  SectionTitle("CARDS");
  for (std::uint32_t i = 0; i < cards.size(); ++i) {
    ImGui::PushID(static_cast<int>(i));
    ImGui::Checkbox(CardTitle(cards[i].id), &cards[i].visible);
    ImGui::SameLine(200.0F * ImGui::GetStyle().FontScaleDpi);
    ImGui::BeginDisabled(i == 0);
    if (IconButton("##up", Icon::chevronUp, "Move up")) {
      std::swap(cards[i], cards[i - 1]);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(i + 1 == cards.size());
    if (IconButton("##down", Icon::chevronDown, "Move down")) {
      std::swap(cards[i], cards[i + 1]);
    }
    ImGui::EndDisabled();
    ImGui::PopID();
  }
}

void App::drawSnippetCard(RequestTab& tab, Request& request) {
  const float scale = ImGui::GetStyle().FontScaleDpi;
  const float buttons = (ImGui::GetFrameHeight() * 2) + (ImGui::GetStyle().ItemSpacing.x * 2);
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - buttons);
  // SnippetLanguageName returns views of string literals: null-terminated.
  // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
  if (ImGui::BeginCombo("##language", SnippetLanguageName(_snippetLanguage).data())) {
    for (SnippetLanguage language : kSnippetLanguages) {
      // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
      if (ImGui::Selectable(SnippetLanguageName(language).data(), language == _snippetLanguage)) {
        _snippetLanguage = language;
        _workspace.settings().snippetLanguage.assign(SnippetLanguageId(language));
        _snippetEditing = false;
        _snippetRevision = 0;
      }
    }
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  if (IconButton("##editSnippet", Icon::edit, _snippetEditing ? "Back to the generated snippet" : "Edit before copying",
                 _snippetEditing)) {
    _snippetEditing = !_snippetEditing;
    _snippetRevision = 0;
  }
  ImGui::SameLine();
  if (IconButton("##copySnippet", Icon::copy, "Copy")) {
    CopyToClipboard(_snippet);
    toast(View(Concat({SnippetLanguageName(_snippetLanguage), " snippet copied"})));
  }

  // Regenerated only when the request changed (the revision counts every edit), never every frame.
  if (!_snippetEditing && (_snippetRevision != _revision || _snippetTabUid != tab.uid)) {
    _snippetRevision = _revision;
    _snippetTabUid = tab.uid;
    OpenCollection* pCollection = collectionOf(tab);
    static const Collection kNoCollection;
    const Collection& collection = pCollection == nullptr ? kNoCollection : pCollection->collection;
    const VariableScope scope = MakeScope(collection, pCollection == nullptr ? -1 : pCollection->activeEnvironment);
    GenerateSnippet(_snippetLanguage, ResolveRequest(request, collection, scope), _snippet);
    _snippetLines.build(_snippet, SnippetSyntax(_snippetLanguage));
    _snippetView.reset();
  }
  const float lineHeight = std::floor(Fonts::kMonoSize * scale * 1.42F);
  const float height = std::clamp((static_cast<float>(_snippetLines.size()) * lineHeight) + (lineHeight * 1.2F),
                                  lineHeight * 3, 420.0F * scale);
  if (_snippetEditing) {
    ScopedFont mono(GetFonts().pMono, Fonts::kMonoSize);
    if (InputTextMultiline("##snippetEdit", _snippet, ImVec2(-FLT_MIN, height), ImGuiInputTextFlags_AllowTabInput)) {
      _snippetLines.build(_snippet, SnippetSyntax(_snippetLanguage));
    }
  } else {
    TextView(
        "##snippet", _snippet, _snippetLines, _snippetView,
        {.language = SnippetSyntax(_snippetLanguage), .size = ImVec2(0, height), .lineNumbers = false, .border = true});
  }
}

void App::DrawTimingsCard(RequestTab& tab) {
  const Palette& pal = CurrentPalette();
  const float scale = ImGui::GetStyle().FontScaleDpi;
  std::array<char, 48> buffer{};
  if (tab.pending()) {
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - tab.ticket.progress->startedAt).count();
    const auto result = std::format_to_n(buffer.data(), 40, "In flight for {:.2f} s", elapsed);
    TextColored(pal.accent, std::string_view(buffer.data(), static_cast<std::size_t>(result.out - buffer.data())));
    return;
  }
  if (!tab.exchange) {
    TextDim("Send the request to see its timeline.");
    return;
  }
  const Exchange& exchange = *tab.exchange;
  const Response& response = exchange.response;
  const Timings& timings = response.timings;

  // Head / body bar.
  const float width = ImGui::GetContentRegionAvail().x;
  const float barHeight = 8.0F * scale;
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  const double total = std::max(1.0, static_cast<double>(timings.total.count()));
  // No head received (failure before the response): the whole bar is waiting time.
  const float headWidth =
      timings.head.count() == 0
          ? width
          : std::max(2.0F, width * static_cast<float>(static_cast<double>(timings.head.count()) / total));
  ImDrawList* pDrawList = ImGui::GetWindowDrawList();
  pDrawList->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + barHeight), WithAlpha(pal.info, 0.55F), barHeight * 0.5F);
  pDrawList->AddRectFilled(pos, ImVec2(pos.x + headWidth, pos.y + barHeight), pal.accent, barHeight * 0.5F);
  ImGui::Dummy(ImVec2(width, barHeight));
  pDrawList = ImGui::GetWindowDrawList();
  const ImVec2 legend = ImGui::GetCursorScreenPos();
  const float dot = 4.0F * scale;
  pDrawList->AddCircleFilled(ImVec2(legend.x + dot, legend.y + (ImGui::GetFontSize() * 0.5F)), dot, pal.accent);
  ImGui::Indent(dot * 3);
  TextDim("waiting (connection, server)");
  ImGui::Unindent(dot * 3);
  const ImVec2 legend2 = ImGui::GetCursorScreenPos();
  pDrawList->AddCircleFilled(ImVec2(legend2.x + dot, legend2.y + (ImGui::GetFontSize() * 0.5F)), dot,
                             WithAlpha(pal.info, 0.8F));
  ImGui::Indent(dot * 3);
  TextDim("downloading the body");
  ImGui::Unindent(dot * 3);

  if (ImGui::BeginTable("##timings", 2, ImGuiTableFlags_SizingFixedFit)) {
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
    std::array<char, 48> valueBuffer{};
    Row("Sent at", FormatClock(timings.startedAt, valueBuffer));
    Row("Time to head", FormatDuration(timings.head, valueBuffer));
    Row("Body download", FormatDuration(timings.total - timings.head, valueBuffer));
    Row("Total", FormatDuration(timings.total, valueBuffer));
    Row("Head size", FormatBytes(response.headBytes, valueBuffer));
    Row("Body size", FormatBytes(response.body.raw.size(), valueBuffer));
    Row("Request body", FormatBytes(exchange.request.body.size(), valueBuffer));
    ImGui::EndTable();
  }
  ImGui::PushTextWrapPos(0);
  TextDim(exchange.request.url);
  ImGui::PopTextWrapPos();
}

void App::drawVariablesCard(RequestTab& tab, Request& request) {
  const Palette& pal = CurrentPalette();
  OpenCollection* pCollection = collectionOf(tab);
  amc::vector<std::string_view> names;
  CollectRefs(request.url, names);
  for (const KeyValues* pRows : {&request.headers, &request.form}) {
    for (const KeyValue& row : *pRows) {
      CollectRefs(row.key, names);
      CollectRefs(row.value, names);
    }
  }
  CollectRefs(request.body, names);
  const Auth& auth =
      request.auth.mode == AuthMode::inherit && pCollection != nullptr ? pCollection->collection.auth : request.auth;
  for (std::string_view field :
       {std::string_view(auth.username), std::string_view(auth.password), std::string_view(auth.token),
        std::string_view(auth.apiKeyName), std::string_view(auth.apiKeyValue)}) {
    CollectRefs(field, names);
  }
  if (names.empty()) {
    TextDim("This request uses no {{variable}}.");
  } else if (ImGui::BeginTable("##variables", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH)) {
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.35F);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.45F);
    ImGui::TableSetupColumn("From", ImGuiTableColumnFlags_WidthStretch, 0.2F);
    ImGui::TableHeadersRow();
    const Collection* pColl = pCollection == nullptr ? nullptr : &pCollection->collection;
    const int envIndex = pCollection == nullptr ? -1 : pCollection->activeEnvironment;
    const Environment* pEnv = pColl != nullptr && envIndex >= 0 && std::cmp_less(envIndex, pColl->environments.size())
                                  ? &pColl->environments[static_cast<std::uint32_t>(envIndex)]
                                  : nullptr;
    for (std::uint32_t i = 0; i < names.size(); ++i) {
      const std::string_view name = names[i];
      ImGui::PushID(static_cast<int>(i));
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      TextColored(SyntaxColor(TokenKind::variable), name);
      std::string_view value;
      std::string_view source;
      if (const KeyValue* pVar = pEnv == nullptr ? nullptr : FindEnabled(pEnv->variables, name); pVar != nullptr) {
        value = pVar->value;
        source = pEnv->name;
      } else if (const KeyValue* pCollVar = pColl == nullptr ? nullptr : FindEnabled(pColl->variables, name);
                 pCollVar != nullptr) {
        value = pCollVar->value;
        source = "collection";
      } else if (IsDynamicVariable(name)) {
        source = "dynamic";
      }
      ImGui::TableSetColumnIndex(1);
      if (source.empty()) {
        TextColored(pal.warning, "undefined");
      } else if (source == "dynamic") {
        TextDim("(computed at send time)");
      } else if (LooksSecret(name) && !ImGui::IsItemHovered() &&
                 ImGui::TableGetHoveredRow() != ImGui::TableGetRowIndex()) {
        TextDim("\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2");
      } else {
        ImGui::PushTextWrapPos(0);
        Text(value);
        ImGui::PopTextWrapPos();
      }
      ImGui::TableSetColumnIndex(2);
      TextDim(source);
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  if (pCollection != nullptr && ImGui::SmallButton("Edit variables...")) {
    for (std::size_t i = 0; i < _workspace.size(); ++i) {
      if (&_workspace[i] == pCollection) {
        _dialogCollection = i;
      }
    }
    _collectionSettingsEnv = std::max(0, pCollection->activeEnvironment);
    _pendingDialog = Dialog::collectionSettings;
  }
}

void App::drawResponseHeadersCard(RequestTab& tab) {
  if (!tab.exchange || tab.exchange->response.headers.empty()) {
    TextDim("No response yet.");
    return;
  }
  for (std::uint32_t i = 0; i < tab.exchange->response.headers.size(); ++i) {
    const HeaderView& header = tab.exchange->response.headers[i];
    ImGui::PushID(static_cast<int>(i));
    TextColored(SyntaxColor(TokenKind::key), header.name);
    ImGui::SameLine(0, 4.0F);
    ImGui::PushTextWrapPos(0);
    Text(header.value);
    ImGui::PopTextWrapPos();
    if (ImGui::IsItemClicked()) {
      CopyToClipboard(header.value);
      toast("Header value copied");
    }
    ImGui::PopID();
  }
}

void App::DrawNotesCard(Request& request) {
  if (request.docs.empty()) {
    TextDim("No notes. Write some in the Docs tab of the request.");
    return;
  }
  ImGui::PushTextWrapPos(0);
  Text(request.docs);
  ImGui::PopTextWrapPos();
}

}  // namespace zalie::ui
