#include "http-client-setup.hpp"

#include <aeronet/aeronet-client.hpp>
#include <aeronet/client-connection.hpp>
#include <aeronet/client-protocol.hpp>
#include <aeronet/http-method.hpp>
#include <aeronet/lower-ascii-key.hpp>
#include <aeronet/reserved-headers.hpp>
#include <algorithm>
#include <cstddef>
#include <string_view>

#include "zalie/method.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/request.hpp"
#include "zalie/string-utils.hpp"

namespace zalie {

namespace {

constexpr std::string_view kUserAgent = "user-agent: zalie/" ZALIE_VERSION_STR;

}  // namespace

aeronet::http::Method ToAeronet(Method method) noexcept {
  switch (method) {
    case Method::get:
      return aeronet::http::Method::GET;
    case Method::post:
      return aeronet::http::Method::POST;
    case Method::put:
      return aeronet::http::Method::PUT;
    case Method::patch:
      return aeronet::http::Method::PATCH;
    case Method::del:
      return aeronet::http::Method::DELETE;
    case Method::head:
      return aeronet::http::Method::HEAD;
    case Method::options:
      return aeronet::http::Method::OPTIONS;
    case Method::trace:
      return aeronet::http::Method::TRACE;
    case Method::connect:
      return aeronet::http::Method::CONNECT;
  }
  return aeronet::http::Method::GET;
}

ClientKey KeyOf(const RequestSettings& settings) noexcept {
  return {.timeoutMs = std::max(settings.timeoutMs, 1U),
          .httpVersion = settings.httpVersion,
          .followRedirects = settings.followRedirects,
          .verifyTls = settings.verifyTls,
          .decompress = settings.decompress};
}

HeaderUse ClassifyHeader(std::string_view name, std::string_view value, RawChars32& lowerName) {
  if (EqualsIgnoreCase(name, "content-type")) {
    return HeaderUse::contentType;
  }
  if (!IsHttpToken(name) || value.find_first_of("\r\n", 0) != std::string_view::npos || value.contains('\0')) {
    return HeaderUse::invalid;
  }
  lowerName.clear();
  lowerName.ensureAvailableCapacity(name.size());
  for (char ch : name) {
    lowerName.unchecked_push_back(ToLowerAscii(ch));
  }
  if (aeronet::http::IsReservedOrForbiddenRequestHeader(aeronet::LowerAsciiKey{std::string_view(lowerName)}) ||
      EqualsIgnoreCase(name, "connection")) {
    return HeaderUse::managedByClient;
  }
  return HeaderUse::send;
}

aeronet::HttpClientConfig MakeConfig(const ClientKey& key, std::size_t maxBodyBytes) {
  using Duration = aeronet::HttpClientConfig::Duration;
  aeronet::HttpClientConfig config;
  config.connectTimeout = Duration{key.timeoutMs};
  config.requestTimeout = Duration{key.timeoutMs};
  config.inactivityTimeout = Duration{key.timeoutMs};
  config.followRedirects = key.followRedirects;
  config.maxRedirects = 10;
  config.globalHeaders = aeronet::ConcatenatedHeaders{kUserAgent};
  config.withDecompression(key.decompress);
  // A debugging tool must show what the server sends: no "compression bomb" ratio guard (highly repetitive test
  // payloads compress more than 1000x), only the body size limit.
  config.decompression.maxExpansionRatio = 1e12;
  const std::size_t decompressionLimit = std::max<std::size_t>(maxBodyBytes, config.decompression.decoderChunkSize);
  config.decompression.maxDecompressedBytes = decompressionLimit;
  config.decompression.maxCompressedBytes = decompressionLimit;
  switch (key.httpVersion) {
    case HttpVersion::http1:
      config.httpVersion = aeronet::HttpVersionMode::Http1_1;
      break;
    case HttpVersion::http2:
#ifdef AERONET_ENABLE_HTTP2
      config.httpVersion = aeronet::HttpVersionMode::Http2;
#endif
      break;
    case HttpVersion::automatic:
      config.httpVersion = aeronet::HttpVersionMode::Auto;
      break;
  }
#ifdef AERONET_ENABLE_OPENSSL
  config.tlsVerifyPeer = key.verifyTls;
#endif
  return config;
}

}  // namespace zalie
