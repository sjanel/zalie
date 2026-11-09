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

// Adds the headers of 'request' to 'req', skipping the ones the client manages or that are invalid:
// onSkipped(reason, name) is called for each of those.
template <class OnSkipped>
void AddHeaders(const ResolvedRequest& request, aeronet::HttpRequest& req, OnSkipped onSkipped) {
  RawChars32 lowerName;
  for (const HeaderView& header : request.headers) {
    if (EqualsIgnoreCase(header.name, "content-type")) {
      continue;  // given with the body
    }
    if (!IsHttpToken(header.name) || header.value.find_first_of("\r\n", 0) != std::string_view::npos ||
        header.value.contains('\0')) {
      onSkipped("Invalid header not sent: ", header.name);
      continue;
    }
    lowerName.clear();
    lowerName.ensureAvailableCapacity(header.name.size());
    for (char ch : header.name) {
      lowerName.unchecked_push_back(ToLowerAscii(ch));
    }
    const aeronet::LowerAsciiKey key{std::string_view(lowerName)};
    if (aeronet::http::IsReservedOrForbiddenRequestHeader(key) || EqualsIgnoreCase(header.name, "connection")) {
      onSkipped("Header managed by the client, not sent: ", header.name);
      continue;
    }
    if (std::string_view(lowerName) == "user-agent") {
      req.header(key, header.value);  // replaces Zalie's default one
    } else {
      req.headerAddLine(key, header.value);  // duplicates are legitimate (several Cookie or X-Forwarded-For lines)
    }
  }
}

}  // namespace zalie
