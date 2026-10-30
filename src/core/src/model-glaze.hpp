#pragma once

// glaze reflection of the Zalie model. Private to zalie_core, and only included by the translation units doing JSON
// I/O: glaze costs a lot of compile time per translation unit.

#include <array>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <string>
#include <string_view>

#include "zalie/exchange.hpp"
#include "zalie/method.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/request.hpp"

// RawChars32 is (de)serialized as a JSON string.
namespace glz {

template <>
struct to<JSON, zalie::RawChars32> {
  template <auto Opts>
  static void op(const zalie::RawChars32& value, is_context auto&& ctx, auto&& buffer, auto&& ix) {
    serialize<JSON>::op<Opts>(std::string_view(value), ctx, buffer, ix);
  }
};

template <>
struct from<JSON, zalie::RawChars32> {
  template <auto Opts>
  static void op(zalie::RawChars32& value, is_context auto&& ctx, auto&& it, auto&& end) {
    // glaze unescapes into a resizable string: one scratch per thread, its capacity reused across all the fields.
    thread_local std::string scratch;
    scratch.clear();
    parse<JSON>::op<Opts>(scratch, ctx, it, end);
    value.assign(scratch);
  }
};

template <>
struct meta<zalie::Method> {
  using enum zalie::Method;
  static constexpr std::array keys{
      std::string_view{"GET"},     std::string_view{"POST"},   std::string_view{"PUT"},
      std::string_view{"PATCH"},   std::string_view{"DELETE"}, std::string_view{"HEAD"},
      std::string_view{"OPTIONS"}, std::string_view{"TRACE"},  std::string_view{"CONNECT"}};
  static constexpr std::array value{get, post, put, patch, del, head, options, trace, connect};
};

template <>
struct meta<zalie::BodyMode> {
  using enum zalie::BodyMode;
  static constexpr std::array keys{std::string_view{"none"}, std::string_view{"json"}, std::string_view{"text"},
                                   std::string_view{"xml"}, std::string_view{"form-urlencoded"}};
  static constexpr std::array value{none, json, text, xml, formUrlEncoded};
};

template <>
struct meta<zalie::AuthMode> {
  using enum zalie::AuthMode;
  static constexpr std::array keys{std::string_view{"inherit"}, std::string_view{"none"}, std::string_view{"basic"},
                                   std::string_view{"bearer"}, std::string_view{"api-key"}};
  static constexpr std::array value{inherit, none, basic, bearer, apiKey};
};

template <>
struct meta<zalie::HttpVersion> {
  using enum zalie::HttpVersion;
  static constexpr std::array keys{std::string_view{"auto"}, std::string_view{"http1.1"}, std::string_view{"http2"}};
  static constexpr std::array value{automatic, http1, http2};
};

template <>
struct meta<zalie::ExchangeState> {
  using enum zalie::ExchangeState;
  static constexpr std::array keys{std::string_view{"pending"}, std::string_view{"completed"},
                                   std::string_view{"failed"}, std::string_view{"cancelled"}};
  static constexpr std::array value{pending, completed, failed, cancelled};
};

}  // namespace glz

namespace zalie {

// Reading is lenient: unknown keys are skipped (files written by a newer version), missing keys keep their defaults.
// The input is a std::string_view (typically the content of a RawChars, which has no terminating null): glaze must
// not look for a null character after its end.
inline constexpr glz::opts kReadOpts{.null_terminated = false, .error_on_unknown_keys = false};

// Writing: pretty-printed, indented by 2 spaces (glaze's inheritable option, 3 by default).
struct WriteOpts : glz::opts {
  std::uint8_t indentation_width = 2;
};
inline constexpr WriteOpts kWriteOpts = [] {
  WriteOpts opts{};
  opts.prettify = true;
  return opts;
}();

}  // namespace zalie
