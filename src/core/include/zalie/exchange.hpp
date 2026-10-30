#pragma once

#include <amc/vector.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "zalie/raw-chars.hpp"
#include "zalie/resolved-request.hpp"
#include "zalie/string-pool.hpp"
#include "zalie/text-document.hpp"

namespace zalie {

enum class ExchangeState : std::uint8_t { pending, completed, failed, cancelled };

// When the exchange happened and how long it took.
struct Timings {
  std::chrono::system_clock::time_point startedAt;
  // Time to the response head: connection (DNS, TCP, TLS), request upload, server processing and redirects included.
  std::chrono::nanoseconds head{0};
  // Time to the end of the body.
  std::chrono::nanoseconds total{0};
};

// A response body and everything needed to display it smoothly, computed once on a worker thread (never on the UI
// thread): kind, pretty-printed JSON, line indexes for virtualized rendering.
struct ResponseBody {
  // Detects the kind of the body, pretty-prints it if it is JSON and indexes its lines.
  void analyze(std::string_view contentType);

  // Text to display: the pretty-printed JSON if available and wanted, the raw body otherwise.
  [[nodiscard]] std::string_view text(bool wantPretty) const noexcept {
    return hasPretty() && wantPretty ? std::string_view(pretty) : std::string_view(raw);
  }
  [[nodiscard]] const LineIndex& lines(bool wantPretty) const noexcept {
    return hasPretty() && wantPretty ? prettyLines : rawLines;
  }
  [[nodiscard]] bool hasPretty() const noexcept { return !pretty.empty(); }

  // Decoded body bytes, as received (the sink appends the network chunks in place, reserved once from Content-Length).
  RawChars raw;
  RawChars pretty;
  LineIndex rawLines;
  LineIndex prettyLines;
  ContentKind kind{ContentKind::empty};
};

// Result of an exchange. Its small strings (reason, error, headers, warnings) share one StringPool.
struct Response {
  [[nodiscard]] std::string_view headerValue(std::string_view name) const noexcept { return FindHeader(headers, name); }

  [[nodiscard]] bool ok() const noexcept { return state == ExchangeState::completed; }

  StringPool strings;
  std::string_view reason;
  // Why the exchange failed or was cancelled (empty when completed).
  std::string_view error;
  HeaderViews headers;
  // Headers of the request that were not sent (managed by the client, or invalid).
  amc::vector<std::string_view> warnings;
  ResponseBody body;
  Timings timings;
  // Size of the response head as received (status line and headers).
  std::size_t headBytes{0};
  std::int16_t status{0};
  ExchangeState state{ExchangeState::pending};
};

// A request as it was sent, with its response.
struct Exchange {
  ResolvedRequest request;
  Response response;
  // Identifier returned by Executor::submit.
  std::uint64_t ticket{0};
  // Id of the request of the collection it was sent for (0 for an unsaved request).
  std::uint64_t requestId{0};
};

}  // namespace zalie
