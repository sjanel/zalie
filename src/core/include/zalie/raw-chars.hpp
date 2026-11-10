#pragma once

#include <aeronet/decimal-writer.hpp>
#include <aeronet/memory-utils-sv.hpp>
#include <aeronet/memory-utils.hpp>
#include <aeronet/nchars.hpp>
#include <aeronet/ndigits.hpp>
#include <aeronet/raw-chars.hpp>
#include <amc/type_traits.hpp>
#include <concepts>
#include <cstddef>
#include <initializer_list>
#include <string_view>
#include <type_traits>

namespace zalie {

// Zalie stores its strings and buffers in aeronet's RawChars: no SSO, no null terminator, trivially relocatable (so
// amc containers move them with memcpy). RawChars32 (16 bytes) for model fields, RawChars (64-bit size) for bodies.
//
// They are filled with the append idiom of aeronet: one capacity check for the whole write, then raw pointer writes
// (aeronet::Append returns the pointer after the bytes it copied) and a final setEnd():
//
//   buf.ensureAvailableCapacity(a.size() + b.size());
//   char* pData = buf.end();
//   pData = aeronet::Append(a, pData);
//   pData = aeronet::Append(b, pData);
//   buf.setEnd(pData);
//
// AppendAll() below is that idiom for a list of parts.
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

// A part of AppendAll written in decimal: an integer that is not a char (written as it is) nor a bool.
template <class T>
concept DecimalPart = std::integral<T> && !std::same_as<T, char> && !std::same_as<T, bool>;

// A part of AppendAll written by its own code (an escaped or encoded string, a formatted number...): writeTo() writes
// at most maxSize() bytes at the given pointer, and returns the end of the bytes to keep.
template <class T>
concept CustomPart = requires(const T& part, char* pData) {
  { part.maxSize() } -> std::convertible_to<std::size_t>;
  { part.writeTo(pData) } -> std::same_as<char*>;
};

namespace details {

[[nodiscard]] constexpr std::size_t PartSize(std::string_view part) noexcept { return part.size(); }
[[nodiscard]] constexpr std::size_t PartSize(std::same_as<char> auto /*part*/) noexcept { return 1; }
[[nodiscard]] constexpr std::size_t PartSize(DecimalPart auto part) noexcept { return aeronet::nchars(part); }
[[nodiscard]] constexpr std::size_t PartSize(const CustomPart auto& part) noexcept { return part.maxSize(); }
// A string literal: its size is a constant, the copy becomes direct stores.
template <std::size_t N>
[[nodiscard]] constexpr std::size_t PartSize(const char (& /*part*/)[N]) noexcept {
  return N - 1;
}

[[nodiscard]] inline char* AppendPart(std::string_view part, char* pData) noexcept {
  return aeronet::Append(part, pData);
}
[[nodiscard]] inline char* AppendPart(std::same_as<char> auto part, char* pData) noexcept {
  *pData = part;
  return pData + 1;
}
[[nodiscard]] inline char* AppendPart(DecimalPart auto part, char* pData) noexcept {
  return aeronet::WriteInt(pData, part, aeronet::ndigits(part));
}
[[nodiscard]] inline char* AppendPart(const CustomPart auto& part, char* pData) noexcept { return part.writeTo(pData); }
template <std::size_t N>
[[nodiscard]] inline char* AppendPart(const char (&part)[N], char* pData) noexcept {
  return aeronet::Append(part, N - 1, pData);
}

}  // namespace details

// Concatenates the given parts into a new buffer, with a single allocation of the exact size.
template <class Buffer = RawChars32>
[[nodiscard]] Buffer Concat(std::initializer_list<std::string_view> parts) {
  std::size_t size = 0;
  for (std::string_view part : parts) {
    size += part.size();
  }
  Buffer ret(size);
  char* pData = ret.data();
  for (std::string_view part : parts) {
    pData = aeronet::Append(part, pData);
  }
  ret.setEnd(pData);
  return ret;
}

// Appends all the parts to 'out' with a single capacity check. A part is anything convertible to std::string_view, a
// string literal, a char, an integer (written in decimal) or a CustomPart. The parts must not point into 'out'.
// The capacity grows exponentially: an output is usually built by several calls.
template <class Buffer, class... Parts>
void AppendAll(Buffer& out, const Parts&... parts) {
  out.ensureAvailableCapacityExponential((details::PartSize(parts) + ... + std::size_t{0}));
  char* pData = out.end();
  ((pData = details::AppendPart(parts, pData)), ...);
  out.setEnd(pData);
}

// Replaces the content of 'buf' with 'value'. Unlike RawChars::assign, 'value' may point into 'buf'.
template <class Buffer>
void AssignSafe(Buffer& buf, std::string_view value) {
  Buffer copy(value);
  buf.swap(copy);
}

}  // namespace zalie
