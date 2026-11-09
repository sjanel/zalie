#pragma once

// How the requests of Zalie are turned into aeronet client calls: shared by the executor and the load runner.

#include <aeronet/aeronet-client.hpp>
#include <aeronet/http-method.hpp>
#include <aeronet/lower-ascii-key.hpp>
#include <aeronet/reserved-headers.hpp>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "zalie/method.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/request.hpp"
#include "zalie/resolved-request.hpp"
#include "zalie/string-utils.hpp"

namespace zalie {

[[nodiscard]] aeronet::http::Method ToAeronet(Method method) noexcept;

// The client settings an HttpClient is configured with: requests with equal settings share a client (and its pool).
struct ClientKey {
  bool operator==(const ClientKey&) const = default;

  std::uint32_t timeoutMs;
  HttpVersion httpVersion;
  bool followRedirects;
  bool verifyTls;
  bool decompress;
};

[[nodiscard]] ClientKey KeyOf(const RequestSettings& settings) noexcept;

[[nodiscard]] aeronet::HttpClientConfig MakeConfig(const ClientKey& key, std::size_t maxBodyBytes);

// What happens to a header of a request.
enum class HeaderUse : std::uint8_t { send, contentType, invalid, managedByClient };

// Decides the fate of a header. For HeaderUse::send, 'lowerName' receives its name in lower case.
[[nodiscard]] HeaderUse ClassifyHeader(std::string_view name, std::string_view value, RawChars32& lowerName);

// The reason shown for a header that is not sent.
[[nodiscard]] constexpr std::string_view HeaderSkipReason(HeaderUse use) noexcept {
  return use == HeaderUse::invalid ? "Invalid header not sent: " : "Header managed by the client, not sent: ";
}

// Adds the headers of 'request' to 'req', skipping the ones the client manages or that are invalid:
// onSkipped(reason, name) is called for each of those.
template <class OnSkipped>
void AddHeaders(const ResolvedRequest& request, aeronet::HttpRequest& req, OnSkipped onSkipped) {
  RawChars32 lowerName;
  for (const HeaderView& header : request.headers) {
    const HeaderUse use = ClassifyHeader(header.name, header.value, lowerName);
    if (use == HeaderUse::contentType) {
      continue;  // given with the body
    }
    if (use != HeaderUse::send) {
      onSkipped(HeaderSkipReason(use), header.name);
      continue;
    }
    const aeronet::LowerAsciiKey key{std::string_view(lowerName)};
    if (std::string_view(lowerName) == "user-agent") {
      req.header(key, header.value);  // replaces Zalie's default one
    } else {
      req.headerAddLine(key, header.value);  // duplicates are legitimate (several Cookie or X-Forwarded-For lines)
    }
  }
}

}  // namespace zalie
