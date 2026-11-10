#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace zalie {

// HTTP request methods Zalie can send.
enum class Method : std::uint8_t { get, post, put, patch, del, head, options, trace, connect };

inline constexpr std::array kMethods{Method::get,  Method::post,    Method::put,   Method::patch,  Method::del,
                                     Method::head, Method::options, Method::trace, Method::connect};

// Upper-case wire name of the method ("GET", "POST", ...).
[[nodiscard]] constexpr std::string_view MethodToStr(Method method) noexcept {
  switch (method) {
    case Method::get:
      return "GET";
    case Method::post:
      return "POST";
    case Method::put:
      return "PUT";
    case Method::patch:
      return "PATCH";
    case Method::del:
      return "DELETE";
    case Method::head:
      return "HEAD";
    case Method::options:
      return "OPTIONS";
    case Method::trace:
      return "TRACE";
    case Method::connect:
      return "CONNECT";
  }
  return "GET";
}

// Parses a method name, case-insensitively. Returns std::nullopt for an unknown method.
[[nodiscard]] std::optional<Method> ParseMethod(std::string_view name) noexcept;

// Whether the method only reads (RFC 9110 safe methods): sending it many times should not change the server.
[[nodiscard]] constexpr bool MethodIsSafe(Method method) noexcept {
  return method == Method::get || method == Method::head || method == Method::options || method == Method::trace;
}

// Whether a request with this method conventionally carries a body (used by importers and snippets).
[[nodiscard]] constexpr bool MethodUsuallyHasBody(Method method) noexcept {
  return method == Method::post || method == Method::put || method == Method::patch;
}

}  // namespace zalie
