#pragma once

#include <amc/vector.hpp>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "zalie/key-value.hpp"
#include "zalie/raw-chars.hpp"

namespace zalie {

// Names of variables referenced but not defined: views into the interpolated texts (or into variable values).
using UnresolvedNames = amc::vector<std::string_view>;

// Lookup table of variables, by name. Entries added first win (add the highest priority layer first).
// It stores views: the added strings must outlive the scope.
class VariableScope {
 public:
  // Adds every enabled variable of 'variables'.
  void addLayer(const KeyValues& variables);

  // Adds one variable.
  void add(std::string_view name, std::string_view value) { _entries.push_back({name, value}); }

  // Adds every entry of 'other', after the ones of this scope.
  void addScope(const VariableScope& other) {
    _entries.insert(_entries.end(), other._entries.begin(), other._entries.end());
  }

  // Returns the value of the variable, or nullptr if it is not defined.
  [[nodiscard]] const std::string_view* find(std::string_view name) const noexcept;

  [[nodiscard]] std::size_t size() const noexcept { return _entries.size(); }

 private:
  struct Entry {
    std::string_view name;
    std::string_view value;
  };

  // A few dozen variables at most: a linear scan of contiguous views beats any hash map.
  amc::vector<Entry> _entries;
};

// A {{reference}} found in a text: byte offsets of the whole reference and its trimmed name.
struct VariableRef {
  std::size_t begin{};
  std::size_t end{};
  std::string_view name;
};

using VariableRefs = amc::vector<VariableRef>;

// Appends to 'out' every {{variable}} reference of 'text', in order.
void FindVariableRefs(std::string_view text, VariableRefs& out);

// Whether the name is a built-in dynamic variable ($timestamp, $randomUUID, $env.NAME, process.env.NAME...).
[[nodiscard]] bool IsDynamicVariable(std::string_view name) noexcept;

// Appends the value of the dynamic variable 'name' to 'out'. Returns false if it is not a dynamic variable (or an
// undefined environment variable).
bool AppendDynamicVariable(std::string_view name, RawChars32& out);

// Replaces every {{name}} of 'text' by its value and appends the result to 'out'.
// Values are themselves interpolated (up to a small depth, which also stops reference cycles).
// Built-in dynamic variables are supported:
//   {{$timestamp}} (unix seconds), {{$timestampMs}}, {{$isoTimestamp}}, {{$randomInt}} (0-1000),
//   {{$randomUUID}} / {{$guid}}, {{$env.NAME}} / {{process.env.NAME}} (process environment).
// Unknown references are kept verbatim, and their names appended to 'pUnresolved' if not null (without duplicates).
void Interpolate(std::string_view text, const VariableScope& scope, RawChars32& out,
                 UnresolvedNames* pUnresolved = nullptr);

// Same as above, returning a new buffer (a single exact allocation when 'text' has no reference).
[[nodiscard]] RawChars32 Interpolate(std::string_view text, const VariableScope& scope,
                                     UnresolvedNames* pUnresolved = nullptr);

// Generates a random (version 4) UUID, lower-case, with dashes.
[[nodiscard]] RawChars32 RandomUuid();

// Appends the (version 4) UUID made of these 128 bits, lower-case, with dashes.
void AppendUuid(std::uint64_t high, std::uint64_t low, RawChars32& out);

}  // namespace zalie
