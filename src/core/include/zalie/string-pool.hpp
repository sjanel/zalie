#pragma once

#include <aeronet/object-array-pool.hpp>
#include <amc/vector.hpp>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string_view>

namespace zalie {

// Arena of immutable strings sharing one lifetime (the strings of a resolved request, of a response head...).
// Built on aeronet::ObjectArrayPool<char>: strings are packed in a few blocks (one, when the initial capacity is a good
// estimate) instead of one heap allocation each, and the returned views stay valid until the pool is destroyed - moving
// the pool keeps them valid too.
class StringPool {
 public:
  static constexpr std::size_t kDefaultInitialCapacity = 256;

  StringPool() noexcept : StringPool(kDefaultInitialCapacity) {}
  explicit StringPool(std::size_t initialCapacity) noexcept : _pool(initialCapacity) {}

  // Copies 'str' into the pool and returns a stable view on the copy.
  [[nodiscard]] std::string_view store(std::string_view str);

  // Concatenates 'parts' into the pool (one array) and returns a stable view on the result.
  [[nodiscard]] std::string_view concat(std::initializer_list<std::string_view> parts);

  // Takes the strings of 'other' (their views stay valid, owned by this pool from now on).
  void splice(StringPool& other) noexcept { _pool.splice(other._pool); }

 private:
  aeronet::ObjectArrayPool<char> _pool;
};

// A list of immutable strings stored in a StringPool, exposed as views. Typically warnings or error messages.
class StringList {
 public:
  using const_iterator = amc::vector<std::string_view>::const_iterator;

  StringList() noexcept = default;
  explicit StringList(std::size_t initialCapacity) noexcept : _pool(initialCapacity) {}

  // Appends a copy of 'str'.
  void push_back(std::string_view str) { _items.push_back(_pool.store(str)); }

  // Appends the concatenation of 'parts' as a single entry.
  void push_back(std::initializer_list<std::string_view> parts) { _items.push_back(_pool.concat(parts)); }

  // Appends a copy of 'str' unless an equal entry is already present.
  void pushBackUnique(std::string_view str);

  [[nodiscard]] bool contains(std::string_view str) const noexcept;

  [[nodiscard]] std::size_t size() const noexcept { return _items.size(); }
  [[nodiscard]] bool empty() const noexcept { return _items.empty(); }

  [[nodiscard]] std::string_view operator[](std::size_t pos) const noexcept {
    return _items[static_cast<std::uint32_t>(pos)];
  }
  [[nodiscard]] std::string_view front() const noexcept { return _items.front(); }
  [[nodiscard]] std::string_view back() const noexcept { return _items.back(); }

  [[nodiscard]] const_iterator begin() const noexcept { return _items.begin(); }
  [[nodiscard]] const_iterator end() const noexcept { return _items.end(); }

 private:
  StringPool _pool;
  amc::vector<std::string_view> _items;
};

}  // namespace zalie
