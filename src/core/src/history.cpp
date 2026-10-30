#include "zalie/history.hpp"

#include <chrono>
#include <cstdint>
#include <string_view>
#include <utility>

#include "zalie/exchange.hpp"
#include "zalie/raw-chars.hpp"

namespace zalie {

void History::add(const Exchange& exchange) {
  using namespace std::chrono;
  const Response& response = exchange.response;
  add(HistoryEntry{
      .url = RawChars32(exchange.request.url),
      .timestampMs = duration_cast<milliseconds>(response.timings.startedAt.time_since_epoch()).count(),
      .requestId = exchange.requestId,
      .durationMs = static_cast<std::uint32_t>(duration_cast<milliseconds>(response.timings.total).count()),
      .status = response.status,
      .method = exchange.request.method,
      .state = response.state});
}

void History::add(HistoryEntry entry) {
  if (_capacity == 0) {
    return;
  }
  if (_entries.size() >= _capacity) {
    _entries.pop_back();
  }
  // Trivially relocatable entries: the shift is a single memmove.
  _entries.insert(_entries.begin(), std::move(entry));
}

}  // namespace zalie
