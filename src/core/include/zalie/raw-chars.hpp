#pragma once

#include <aeronet/raw-chars.hpp>
#include <amc/type_traits.hpp>
#include <charconv>
#include <concepts>
#include <cstddef>
#include <initializer_list>
#include <string_view>
#include <type_traits>

namespace zalie {

// Zalie stores its strings and buffers in aeronet's RawChars: no SSO, no null terminator, trivially relocatable (so
// amc containers move them with memcpy). RawChars32 (16 bytes) for model fields, RawChars (64-bit size) for bodies.
using aeronet::RawChars;
using aeronet::RawChars32;

// std::true_type when all the given types are trivially relocatable, to declare 'trivially_relocatable' on aggregates.
template <class... Ts>
using AllTriviallyRelocatable = std::bool_constant<(amc::is_trivially_relocatable_v<Ts> && ...)>;

// Explicit view of a RawChars (the implicit conversion does not apply in template deduction contexts).
template <class Buffer>
  requires requires(const Buffer& buf) {
    buf.data();
    buf.size();
  }
[[nodiscard]] constexpr std::string_view View(const Buffer& buf) noexcept {
  return {buf.data(), buf.size()};
}

// Concatenates the given parts into a new buffer, with a single allocation.
template <class Buffer = RawChars32>
[[nodiscard]] Buffer Concat(std::initializer_list<std::string_view> parts) {
  std::size_t size = 0;
  for (std::string_view part : parts) {
    size += part.size();
  }
  Buffer ret(size);
  for (std::string_view part : parts) {
    ret.unchecked_append(part);
  }
  return ret;
}

// Appends all the parts (anything convertible to std::string_view) with a single capacity check.
template <class Buffer, class... Parts>
void AppendAll(Buffer& out, const Parts&... parts) {
  out.ensureAvailableCapacity((std::string_view(parts).size() + ... + std::size_t{0}));
  (out.unchecked_append(std::string_view(parts)), ...);
}

// Appends the decimal representation of an integer.
template <class Buffer>
void AppendInt(Buffer& out, std::integral auto value) {
  static constexpr std::size_t kMaxDigits = 24;
  out.ensureAvailableCapacity(kMaxDigits);
  const auto res = std::to_chars(out.data() + out.size(), out.data() + out.size() + kMaxDigits, value);
  out.setEnd(res.ptr);
}

// Replaces the content of 'buf' with 'value'. Unlike RawChars::assign, 'value' may point into 'buf'.
template <class Buffer>
void AssignSafe(Buffer& buf, std::string_view value) {
  Buffer copy(value);
  buf.swap(copy);
}

}  // namespace zalie
