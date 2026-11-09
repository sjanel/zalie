#pragma once

#include <imgui.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string_view>
#include <utility>

#include "zalie/exchange.hpp"
#include "zalie/key-value.hpp"
#include "zalie/method.hpp"
#include "zalie/raw-chars.hpp"

namespace zalie::ui {

// Fonts of the UI, loaded once (see Assets).
struct Fonts {
  static constexpr float kUiSize = 15.0F;
  static constexpr float kMonoSize = 14.0F;
  static constexpr float kTitleSize = 19.0F;

  ImFont* pUi{nullptr};
  ImFont* pMono{nullptr};
};

[[nodiscard]] Fonts& GetFonts() noexcept;

// RAII font switch.
class ScopedFont {
 public:
  ScopedFont(ImFont* pFont, float size) { ImGui::PushFont(pFont, size); }
  ScopedFont(const ScopedFont&) = delete;
  ScopedFont& operator=(const ScopedFont&) = delete;
  ~ScopedFont() { ImGui::PopFont(); }
};

// Formats into 'buffer' (truncated to its size) and returns the text written.
template <class... Args>
std::string_view FormatTo(std::span<char> buffer, std::format_string<Args...> fmt, Args&&... args) {
  const auto result =
      std::format_to_n(buffer.data(), static_cast<std::ptrdiff_t>(buffer.size()), fmt, std::forward<Args>(args)...);
  return {buffer.data(),
          static_cast<std::size_t>(std::min<std::ptrdiff_t>(result.size, static_cast<std::ptrdiff_t>(buffer.size())))};
}

// Formats into 'buffer' and returns a null-terminated C string (labels for Dear ImGui), truncated if needed.
template <std::size_t N, class... Args>
const char* Label(std::array<char, N>& buffer, std::format_string<Args...> fmt, Args&&... args) {
  const auto result = std::format_to_n(buffer.data(), N - 1, fmt, std::forward<Args>(args)...);
  *result.out = '\0';
  return buffer.data();
}

// InputText editing a RawChars in place (grown on demand by the resize callback: no intermediate std::string).
bool InputText(const char* label, RawChars32& str, ImGuiInputTextFlags flags = 0, const char* hint = nullptr);
bool InputTextMultiline(const char* label, RawChars32& str, ImVec2 size, ImGuiInputTextFlags flags = 0);

// Unformatted text from a view (no copy, no formatting).
inline void Text(std::string_view text) { ImGui::TextUnformatted(text.data(), text.data() + text.size()); }
void TextColored(ImU32 color, std::string_view text);
void TextDim(std::string_view text);

// Up to 4 letters for narrow columns ("DEL", "OPT", "CONN", others unchanged). A view of a string literal.
[[nodiscard]] std::string_view MethodShortName(Method method) noexcept;

// The colored, fixed width label of an HTTP method.
void MethodLabel(Method method, bool fixedWidth = true);

// Pushes a 64-bit model id (request, folder...) on Dear ImGui's id stack (all its bytes are hashed).
void PushId(std::uint64_t id);

// A combo box choosing a method, colored. Returns true when changed.
bool MethodCombo(const char* strId, Method& method, float width);

// A rounded pill "200 OK" colored by status class.
void StatusPill(std::int16_t status, std::string_view reason, ExchangeState state);

// Editable table of key / value rows with enable checkboxes and a delete button. A last empty row adds entries.
// Returns true when the table was modified.
struct KeyValueTableOptions {
  const char* keyHint{"Key"};
  const char* valueHint{"Value"};
  // Values are hidden (secrets) unless hovered.
  bool maskValues{false};
  // Height of the table (0: auto).
  float height{0.0F};
};
bool KeyValueTable(const char* strId, KeyValues& rows, const KeyValueTableOptions& options = {});

// A draggable splitter. Vertical: a vertical bar resizing a width. Returns true while dragged.
bool Splitter(const char* strId, bool vertical, float thickness, float& size, float minSize, float maxSize,
              float length);

// "1.2 KB", "3.4 MB"...
[[nodiscard]] std::string_view FormatBytes(std::uint64_t bytes, std::span<char> buffer);

// "12 ms", "1.24 s", "850 µs"...
[[nodiscard]] std::string_view FormatDuration(std::chrono::nanoseconds duration, std::span<char> buffer);

// "just now", "5 min ago", "yesterday"...
[[nodiscard]] std::string_view FormatAgo(std::int64_t timestampMs, std::span<char> buffer);

// "14:03:27.123" in local time.
[[nodiscard]] std::string_view FormatClock(std::chrono::system_clock::time_point time, std::span<char> buffer);

// Copies 'text' to the clipboard (ImGui wants a null-terminated string: one temporary copy).
void CopyToClipboard(std::string_view text);

// A help marker '(?)' with a tooltip.
void HelpMarker(const char* text);

// A small caps section title.
void SectionTitle(std::string_view title);

// A primary (accent colored) button.
bool PrimaryButton(const char* label, ImVec2 size = ImVec2(0, 0));

// Joined toggle buttons ("Pretty | Raw"), one of them selected. Returns true when the selection changed.
bool SegmentedControl(const char* strId, std::span<const char* const> labels, std::size_t& selected);

}  // namespace zalie::ui
