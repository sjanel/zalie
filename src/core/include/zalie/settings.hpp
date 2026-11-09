#pragma once

#include <amc/vector.hpp>
#include <cstdint>
#include <string_view>

#include "zalie/raw-chars.hpp"

namespace zalie {

// One card of the configurable right panel (the inspector): which one, shown or not. Their order is the display order.
struct InspectorCard {
  bool operator==(const InspectorCard&) const = default;

  RawChars32 id;
  bool visible{true};

  using trivially_relocatable = AllTriviallyRelocatable<RawChars32>;
};

// Identifiers of the inspector cards.
inline constexpr std::string_view kCardSnippet = "snippet";
inline constexpr std::string_view kCardTimings = "timings";
inline constexpr std::string_view kCardVariables = "variables";
inline constexpr std::string_view kCardResponseHeaders = "response-headers";
inline constexpr std::string_view kCardNotes = "notes";

// User preferences, saved in settings.json.
struct Settings {
  // Restores the default card list, keeping the known cards of 'cards' in their order and appending the missing ones.
  void normalizeCards();

  bool operator==(const Settings&) const = default;

  // "coton" (light) or "nuit" (dark).
  RawChars32 theme{std::string_view{"coton"}};
  // Identifier of the snippet language (see SnippetLanguageId).
  RawChars32 snippetLanguage{std::string_view{"curl"}};
  amc::vector<InspectorCard> inspectorCards;
  float uiScale{1.0F};
  float sidebarWidth{280.0F};
  float inspectorWidth{400.0F};
  // Fraction of the central area height used by the request editor (the response takes the rest).
  float requestEditorRatio{0.42F};
  // Same, in width, when the response is beside the request.
  float requestEditorWidthRatio{0.5F};
  std::uint32_t maxBodyMiB{512};
  std::uint32_t workers{4};
  // Most connections of a performance run (each one is a thread). 0: the number of vCPUs.
  std::uint32_t maxLoadConnections{0};
  bool inspectorVisible{true};
  bool sidebarVisible{true};
  bool autosave{true};
  bool prettyJson{true};
  bool wrapLongLines{false};
  // The response is shown beside the request editor instead of below it (when the window is wide enough).
  bool responseBeside{false};
};

}  // namespace zalie
