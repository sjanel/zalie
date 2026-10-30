#pragma once

#include <amc/vector.hpp>
#include <string_view>

#include "zalie/raw-chars.hpp"

namespace zalie {

// One row of an editable key / value table: query parameter, header, form field or variable.
struct KeyValue {
  bool operator==(const KeyValue&) const = default;

  RawChars32 key;
  RawChars32 value;
  bool enabled{true};

  using trivially_relocatable = AllTriviallyRelocatable<RawChars32>;
};

// Trivially relocatable rows: amc::vector grows / inserts / erases them with memcpy.
using KeyValues = amc::vector<KeyValue>;

// Builds an enabled row.
[[nodiscard]] inline KeyValue MakeKeyValue(std::string_view key, std::string_view value, bool enabled = true) {
  return {.key = RawChars32(key), .value = RawChars32(value), .enabled = enabled};
}

// Returns the first enabled entry whose key equals `key` (ASCII case-insensitive if `caseInsensitive`), or nullptr.
[[nodiscard]] const KeyValue* FindEnabled(const KeyValues& entries, std::string_view key,
                                          bool caseInsensitive = false) noexcept;

}  // namespace zalie
