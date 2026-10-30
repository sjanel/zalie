#include "zalie/settings.hpp"

#include <algorithm>
#include <amc/vector.hpp>
#include <array>
#include <cstdint>
#include <string_view>
#include <utility>

#include "zalie/raw-chars.hpp"

namespace zalie {

void Settings::normalizeCards() {
  static constexpr std::array kAllCards{kCardSnippet, kCardTimings, kCardVariables, kCardResponseHeaders, kCardNotes};
  amc::vector<InspectorCard> cards;
  cards.reserve(static_cast<std::uint32_t>(kAllCards.size()));
  // Known cards, in the user's order, without duplicates.
  for (InspectorCard& card : inspectorCards) {
    const bool known = std::ranges::contains(kAllCards, View(card.id));
    const bool duplicate =
        std::ranges::any_of(cards, [&card](const InspectorCard& kept) { return kept.id == card.id; });
    if (known && !duplicate) {
      cards.push_back(std::move(card));
    }
  }
  // Then the missing ones (new cards of a newer version), visible.
  for (std::string_view id : kAllCards) {
    if (std::ranges::none_of(cards, [id](const InspectorCard& card) { return View(card.id) == id; })) {
      cards.push_back({.id = RawChars32(id), .visible = id != kCardNotes});
    }
  }
  inspectorCards = std::move(cards);
}

}  // namespace zalie
