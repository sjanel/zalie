#pragma once

#include <amc/vector.hpp>
#include <cstddef>
#include <cstdint>

#include "zalie/exchange.hpp"
#include "zalie/method.hpp"
#include "zalie/raw-chars.hpp"

namespace zalie {

// Summary of a sent request, kept in the history (the response itself is not kept).
struct HistoryEntry {
  bool operator==(const HistoryEntry&) const = default;

  RawChars32 url;
  // Milliseconds since the epoch.
  std::int64_t timestampMs{0};
  std::uint64_t requestId{0};
  std::uint32_t durationMs{0};
  std::int16_t status{0};
  Method method{Method::get};
  ExchangeState state{ExchangeState::completed};

  using trivially_relocatable = AllTriviallyRelocatable<RawChars32>;
};

// Most recent first, bounded.
class History {
 public:
  static constexpr std::size_t kDefaultCapacity = 500;

  explicit History(std::size_t capacity = kDefaultCapacity) noexcept : _capacity(capacity) {}

  // Records an exchange at the front.
  void add(const Exchange& exchange);

  // Adds an entry at the front (dropping the oldest one when full).
  void add(HistoryEntry entry);

  void clear() noexcept { _entries.clear(); }

  [[nodiscard]] const amc::vector<HistoryEntry>& entries() const noexcept { return _entries; }
  amc::vector<HistoryEntry>& entries() noexcept { return _entries; }

 private:
  amc::vector<HistoryEntry> _entries;
  std::size_t _capacity;
};

}  // namespace zalie
