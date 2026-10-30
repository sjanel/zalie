#include "zalie/ui/widgets.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <format>
#include <span>
#include <string_view>
#include <utility>

#include "zalie/exchange.hpp"
#include "zalie/key-value.hpp"
#include "zalie/method.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/ui/icons.hpp"
#include "zalie/ui/theme.hpp"

namespace zalie::ui {

namespace {

// Keeps a null terminator right after the content (inside the capacity), as ImGui expects.
char* NullTerminated(RawChars32& str) {
  str.ensureAvailableCapacity(1U);
  str.data()[str.size()] = '\0';
  return str.data();
}

int ResizeCallback(ImGuiInputTextCallbackData* pData) {
  if (pData->EventFlag == ImGuiInputTextFlags_CallbackResize) {
    auto* pStr = static_cast<RawChars32*>(pData->UserData);
    pStr->reserve(static_cast<std::uint64_t>(pData->BufSize));
    pStr->setSize(static_cast<RawChars32::size_type>(pData->BufTextLen));
    pData->Buf = pStr->data();
    pData->BufSize = static_cast<int>(pStr->capacity());
  }
  return 0;
}

template <class... Args>
std::string_view Format(std::span<char> buffer, std::format_string<Args...> fmt, Args&&... args) {
  const auto result =
      std::format_to_n(buffer.data(), static_cast<std::ptrdiff_t>(buffer.size()), fmt, std::forward<Args>(args)...);
  return {buffer.data(),
          static_cast<std::size_t>(std::min<std::ptrdiff_t>(result.size, static_cast<std::ptrdiff_t>(buffer.size())))};
}

}  // namespace

Fonts& GetFonts() noexcept {
  static Fonts fonts;
  return fonts;
}

bool InputText(const char* label, RawChars32& str, ImGuiInputTextFlags flags, const char* hint) {
  char* pBuf = NullTerminated(str);
  flags |= ImGuiInputTextFlags_CallbackResize;
  const auto capacity = static_cast<std::size_t>(str.capacity());
  if (hint != nullptr) {
    return ImGui::InputTextWithHint(label, hint, pBuf, capacity, flags, ResizeCallback, &str);
  }
  return ImGui::InputText(label, pBuf, capacity, flags, ResizeCallback, &str);
}

bool InputTextMultiline(const char* label, RawChars32& str, ImVec2 size, ImGuiInputTextFlags flags) {
  char* pBuf = NullTerminated(str);
  flags |= ImGuiInputTextFlags_CallbackResize;
  return ImGui::InputTextMultiline(label, pBuf, static_cast<std::size_t>(str.capacity()), size, flags, ResizeCallback,
                                   &str);
}

void TextColored(ImU32 color, std::string_view text) {
  ImGui::PushStyleColor(ImGuiCol_Text, color);
  Text(text);
  ImGui::PopStyleColor();
}

void TextDim(std::string_view text) { TextColored(CurrentPalette().textDim, text); }

std::string_view MethodShortName(Method method) noexcept {
  switch (method) {
    case Method::del:
      return "DEL";
    case Method::options:
      return "OPT";
    case Method::connect:
      return "CONN";
    default:
      return MethodToStr(method);
  }
}

void MethodLabel(Method method, bool fixedWidth) {
  const std::string_view name = MethodToStr(method);
  {
    ScopedFont font(GetFonts().pMono, Fonts::kMonoSize * 0.86F);
    const float width = ImGui::CalcTextSize("PATCH").x;
    const ImVec2 pos = ImGui::GetCursorPos();
    TextColored(MethodColor(method), fixedWidth ? MethodShortName(method) : name);
    if (fixedWidth) {
      ImGui::SameLine(0, 0);
      ImGui::SetCursorPosX(pos.x + width);
      ImGui::Dummy(ImVec2(0, 0));
    }
  }
}

void PushId(std::uint64_t id) {
  const char* pBegin = reinterpret_cast<const char*>(&id);
  ImGui::PushID(pBegin, pBegin + sizeof(id));
}

bool MethodCombo(const char* strId, Method& method, float width) {
  bool changed = false;
  ImGui::SetNextItemWidth(width);
  ImGui::PushStyleColor(ImGuiCol_Text, MethodColor(method));
  // MethodToStr returns views of string literals: null-terminated, no copy needed.
  // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
  const bool open = ImGui::BeginCombo(strId, MethodToStr(method).data(), ImGuiComboFlags_HeightLarge);
  ImGui::PopStyleColor();
  if (open) {
    for (Method candidate : kMethods) {
      ImGui::PushStyleColor(ImGuiCol_Text, MethodColor(candidate));
      // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
      if (ImGui::Selectable(MethodToStr(candidate).data(), candidate == method)) {
        method = candidate;
        changed = true;
      }
      ImGui::PopStyleColor();
    }
    ImGui::EndCombo();
  }
  return changed;
}

void StatusPill(std::int16_t status, std::string_view reason, ExchangeState state) {
  const ImU32 color = StatusColor(status, state);
  std::array<char, 64> buffer{};
  std::string_view label;
  if (state == ExchangeState::failed) {
    label = "Error";
  } else if (state == ExchangeState::cancelled) {
    label = "Cancelled";
  } else {
    label = Format(buffer, "{} {}", status, reason);
  }
  const ImVec2 padding(ImGui::GetStyle().FramePadding.x, ImGui::GetStyle().FramePadding.y * 0.6F);
  const ImVec2 textSize = ImGui::CalcTextSize(label.data(), label.data() + label.size());
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  const ImVec2 size(textSize.x + (padding.x * 2), textSize.y + (padding.y * 2));
  ImDrawList* pDrawList = ImGui::GetWindowDrawList();
  pDrawList->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), WithAlpha(color, 0.16F), size.y * 0.5F);
  pDrawList->AddText(ImVec2(pos.x + padding.x, pos.y + padding.y), color, label.data(), label.data() + label.size());
  ImGui::Dummy(size);
}

bool KeyValueTable(const char* strId, KeyValues& rows, const KeyValueTableOptions& options) {
  bool modified = false;
  ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX;
  if (options.height > 0) {
    flags |= ImGuiTableFlags_ScrollY;
  }
  if (!ImGui::BeginTable(strId, 4, flags, ImVec2(0, options.height))) {
    return false;
  }
  const float square = ImGui::GetFrameHeight();
  ImGui::TableSetupColumn("##enabled", ImGuiTableColumnFlags_WidthFixed, square);
  ImGui::TableSetupColumn(options.keyHint, ImGuiTableColumnFlags_WidthStretch, 0.4F);
  ImGui::TableSetupColumn(options.valueHint, ImGuiTableColumnFlags_WidthStretch, 0.6F);
  ImGui::TableSetupColumn("##delete", ImGuiTableColumnFlags_WidthFixed, square);

  // The trailing empty row edits a draft: typing in it appends a new entry (same widget id: editing goes on).
  static KeyValue draft;
  static constexpr std::size_t kNone = static_cast<std::size_t>(-1);
  std::size_t toDelete = kNone;
  const Palette& pal = CurrentPalette();
  for (std::uint32_t i = 0; i <= rows.size(); ++i) {
    const bool isDraft = i == rows.size();
    KeyValue& row = isDraft ? draft : rows[i];
    ImGui::PushID(static_cast<int>(i));
    ImGui::TableNextRow();
    // Keys and values in the code font (the whole row, so that its frames keep the same height).
    ScopedFont mono(GetFonts().pMono, Fonts::kMonoSize);

    ImGui::TableSetColumnIndex(0);
    if (!isDraft) {
      modified |= ImGui::Checkbox("##on", &row.enabled);
    }
    const bool dim = !isDraft && !row.enabled;
    if (dim) {
      ImGui::PushStyleColor(ImGuiCol_Text, pal.textDim);
    }
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-FLT_MIN);
    bool edited = InputText("##key", row.key, 0, options.keyHint);
    ImGui::TableSetColumnIndex(2);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGuiInputTextFlags valueFlags = 0;
    const bool rowHovered = ImGui::TableGetHoveredRow() == ImGui::TableGetRowIndex();
    if (options.maskValues && !rowHovered && ImGui::GetActiveID() != ImGui::GetID("##value")) {
      valueFlags |= ImGuiInputTextFlags_Password;
    }
    edited |= InputText("##value", row.value, valueFlags, options.valueHint);
    if (dim) {
      ImGui::PopStyleColor();
    }
    ImGui::TableSetColumnIndex(3);
    if (!isDraft && IconButton("##del", Icon::trash, "Remove")) {
      toDelete = i;
    }
    ImGui::PopID();

    if (edited) {
      modified = true;
      if (isDraft) {
        rows.push_back(std::move(draft));
        draft = KeyValue{};
        break;  // 'rows' may have been reallocated: stop iterating this frame
      }
    }
  }
  if (toDelete != kNone) {
    rows.erase(rows.begin() + static_cast<std::ptrdiff_t>(toDelete));
    modified = true;
  }
  ImGui::EndTable();
  return modified;
}

bool Splitter(const char* strId, bool vertical, float thickness, float& size, float minSize, float maxSize,
              float length) {
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  const ImVec2 buttonSize = vertical ? ImVec2(thickness, length) : ImVec2(length, thickness);
  ImGui::InvisibleButton(strId, buttonSize);
  const bool active = ImGui::IsItemActive();
  const bool hovered = ImGui::IsItemHovered();
  if (hovered || active) {
    ImGui::SetMouseCursor(vertical ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
  }
  if (active) {
    const ImVec2 delta = ImGui::GetIO().MouseDelta;
    size = std::clamp(size + (vertical ? delta.x : delta.y), minSize, std::max(minSize, maxSize));
  }
  const Palette& pal = CurrentPalette();
  ImDrawList* pDrawList = ImGui::GetWindowDrawList();
  ImU32 color = pal.border;
  if (active) {
    color = pal.accent;
  } else if (hovered) {
    color = WithAlpha(pal.accent, 0.6F);
  }
  if (vertical) {
    const float x = std::floor(pos.x + (thickness * 0.5F));
    pDrawList->AddLineV(x, pos.y, pos.y + length, color, active || hovered ? 2.0F : 1.0F);
  } else {
    const float y = std::floor(pos.y + (thickness * 0.5F));
    pDrawList->AddLineH(pos.x, pos.x + length, y, color, active || hovered ? 2.0F : 1.0F);
  }
  return active;
}

std::string_view FormatBytes(std::uint64_t bytes, std::span<char> buffer) {
  static constexpr double kUnit = 1024.0;
  if (bytes < 1024) {
    return Format(buffer, "{} B", bytes);
  }
  const auto value = static_cast<double>(bytes);
  if (value < kUnit * kUnit) {
    return Format(buffer, "{:.1f} KB", value / kUnit);
  }
  if (value < kUnit * kUnit * kUnit) {
    return Format(buffer, "{:.1f} MB", value / (kUnit * kUnit));
  }
  return Format(buffer, "{:.2f} GB", value / (kUnit * kUnit * kUnit));
}

std::string_view FormatDuration(std::chrono::nanoseconds duration, std::span<char> buffer) {
  const auto ns = static_cast<double>(duration.count());
  if (ns < 1e6) {
    return Format(buffer, "{:.0f} \xC2\xB5s", ns / 1e3);
  }
  if (ns < 1e7) {
    return Format(buffer, "{:.1f} ms", ns / 1e6);
  }
  if (ns < 1e9) {
    return Format(buffer, "{:.0f} ms", ns / 1e6);
  }
  if (ns < 6e10) {
    return Format(buffer, "{:.2f} s", ns / 1e9);
  }
  return Format(buffer, "{:.0f} min {:.0f} s", std::floor(ns / 6e10), std::fmod(ns / 1e9, 60.0));
}

std::string_view FormatAgo(std::int64_t timestampMs, std::span<char> buffer) {
  using namespace std::chrono;
  const std::int64_t nowMs = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
  const std::int64_t seconds = std::max<std::int64_t>(0, (nowMs - timestampMs) / 1000);
  if (seconds < 10) {
    return Format(buffer, "just now");
  }
  if (seconds < 60) {
    return Format(buffer, "{} s ago", seconds);
  }
  if (seconds < 3600) {
    return Format(buffer, "{} min ago", seconds / 60);
  }
  if (seconds < 86400) {
    return Format(buffer, "{} h ago", seconds / 3600);
  }
  return Format(buffer, "{} d ago", seconds / 86400);
}

std::string_view FormatClock(std::chrono::system_clock::time_point time, std::span<char> buffer) {
  using namespace std::chrono;
  const std::time_t seconds = system_clock::to_time_t(time);
  std::tm local{};
#ifdef _WIN32
  localtime_s(&local, &seconds);
#else
  localtime_r(&seconds, &local);
#endif
  const auto millis = duration_cast<milliseconds>(time.time_since_epoch()).count() % 1000;
  return Format(buffer, "{:02}:{:02}:{:02}.{:03}", local.tm_hour, local.tm_min, local.tm_sec, millis);
}

void CopyToClipboard(std::string_view text) {
  RawChars copy(text.size() + 1);
  copy.unchecked_append(text);
  copy.unchecked_push_back('\0');
  ImGui::SetClipboardText(copy.data());
}

void HelpMarker(const char* text) {
  ImGui::SameLine();
  TextDim("(?)");
  if (ImGui::BeginItemTooltip()) {
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0F);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
}

void SectionTitle(std::string_view title) {
  ScopedFont font(GetFonts().pUi, Fonts::kUiSize * 0.8F);
  TextDim(title);
}

bool PrimaryButton(const char* label, ImVec2 size) {
  const Palette& pal = CurrentPalette();
  ImGui::PushStyleColor(ImGuiCol_Button, pal.accent);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, pal.accentHover);
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, pal.accentActive);
  ImGui::PushStyleColor(ImGuiCol_Text, pal.onAccent);
  ImGui::PushStyleColor(ImGuiCol_Border, pal.accent);
  const bool pressed = ImGui::Button(label, size);
  ImGui::PopStyleColor(5);
  return pressed;
}

bool SegmentedControl(const char* strId, std::span<const char* const> labels, std::size_t& selected) {
  const Palette& pal = CurrentPalette();
  bool changed = false;
  ImGui::PushID(strId);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(1.0F, ImGui::GetStyle().ItemSpacing.y));
  for (std::size_t i = 0; i < labels.size(); ++i) {
    if (i != 0) {
      ImGui::SameLine();
    }
    const bool active = i == selected;
    ImGui::PushStyleColor(ImGuiCol_Button, active ? pal.accentSoft : pal.surface);
    ImGui::PushStyleColor(ImGuiCol_Text, active ? pal.accent : pal.textDim);
    ImGui::PushID(static_cast<int>(i));
    if (ImGui::Button(labels[i]) && !active) {
      selected = i;
      changed = true;
    }
    ImGui::PopID();
    ImGui::PopStyleColor(2);
  }
  ImGui::PopStyleVar();
  ImGui::PopID();
  return changed;
}

}  // namespace zalie::ui
