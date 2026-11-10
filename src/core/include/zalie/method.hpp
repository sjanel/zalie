#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace zalie {

// HTTP request methods Zalie can send.
enum class Method : std::uint8_t { get, post, put, patch, del, head, options, trace, connect };

inline constexpr std::array kMethods{Method::get,  Method::post,    Method::put,   Method::patch,  Method::del,
                                     Method::head, Method::options, Method::trace, Method::connect};

// Upper-case wire names of the methods, in the order of the enumeration.
inline constexpr std::array<std::string_view, kMethods.size()> kMethodNames{
    "GET", "POST", "PUT", "PATCH", "DELETE", "HEAD", "OPTIONS", "TRACE", "CONNECT"};

// Upper-case wire name of the method ("GET", "POST", ...).
[[nodiscard]] constexpr std::string_view MethodToStr(Method method) noexcept {
  assert(static_cast<std::size_t>(method) < kMethodNames.size());
  return kMethodNames[static_cast<std::size_t>(method)];
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
