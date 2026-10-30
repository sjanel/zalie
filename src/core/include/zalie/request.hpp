#pragma once

#include <cstdint>
#include <string_view>

#include "zalie/key-value.hpp"
#include "zalie/method.hpp"
#include "zalie/raw-chars.hpp"

namespace zalie {

// How the body of a request is edited and encoded.
enum class BodyMode : std::uint8_t { none, json, text, xml, formUrlEncoded };

// Authentication scheme of a request. 'inherit' uses the authentication of the collection.
enum class AuthMode : std::uint8_t { inherit, none, basic, bearer, apiKey };

// HTTP version preference. 'automatic' negotiates HTTP/2 with ALPN over https and uses HTTP/1.1 otherwise.
enum class HttpVersion : std::uint8_t { automatic, http1, http2 };

// Authentication parameters. Every scheme keeps its own fields so switching scheme in the UI loses nothing.
struct Auth {
  bool operator==(const Auth&) const = default;

  RawChars32 username;
  RawChars32 password;
  RawChars32 token;
  RawChars32 apiKeyName;
  RawChars32 apiKeyValue;
  AuthMode mode{AuthMode::inherit};
  // For AuthMode::apiKey: send the key as a query parameter instead of a header.
  bool apiKeyInQuery{false};

  using trivially_relocatable = AllTriviallyRelocatable<RawChars32>;
};

// Transport settings of a request.
struct RequestSettings {
  static constexpr std::uint32_t kDefaultTimeoutMs = 30000;

  bool operator==(const RequestSettings&) const = default;

  std::uint32_t timeoutMs{kDefaultTimeoutMs};
  HttpVersion httpVersion{HttpVersion::automatic};
  bool followRedirects{true};
  bool verifyTls{true};
  bool decompress{true};
};

// A saved request of a collection. Values may contain {{variables}}, resolved at send time.
struct Request {
  bool operator==(const Request&) const = default;

  RawChars32 name;
  // Full URL, query string included. Kept in sync with 'params' (see url.hpp).
  RawChars32 url;
  RawChars32 body;
  // Free markdown notes.
  RawChars32 docs;
  KeyValues params;
  KeyValues headers;
  // Fields of a BodyMode::formUrlEncoded body.
  KeyValues form;
  Auth auth;
  std::uint64_t id{0};
  RequestSettings settings;
  Method method{Method::get};
  BodyMode bodyMode{BodyMode::none};

  using trivially_relocatable = AllTriviallyRelocatable<RawChars32, KeyValues, Auth>;
};

// Default Content-Type of a body mode (empty for BodyMode::none).
[[nodiscard]] constexpr std::string_view DefaultContentType(BodyMode mode) noexcept {
  switch (mode) {
    case BodyMode::json:
      return "application/json";
    case BodyMode::text:
      return "text/plain";
    case BodyMode::xml:
      return "application/xml";
    case BodyMode::formUrlEncoded:
      return "application/x-www-form-urlencoded";
    case BodyMode::none:
      break;
  }
  return {};
}

}  // namespace zalie
