#pragma once

#include <amc/vector.hpp>
#include <cstddef>
#include <expected>
#include <string_view>

#include "zalie/raw-chars.hpp"

namespace zalie {

// Values selected by a JSONPath query.
struct JsonPathResult {
  // The matched values, as views of the queried document: copied verbatim (key order, number precision and escapes
  // are kept), in document order for each selector.
  amc::vector<std::string_view> matches;
  // The path selects at most one value by construction (names and indexes only: no wildcard, descendant, slice or
  // union).
  bool singular{true};
  // More than the maximum number of matches were found: 'matches' holds the first ones.
  bool truncated{false};
};

// Evaluates a JSONPath query (the RFC 9535 syntax, without filter expressions) on a JSON document, straight on its
// text: no DOM is built. Supported: '$', '.name', "['name']" / '["name"]', '[index]' (negative from the end),
// '[start:end:step]', '.*' / '[*]', '..' (descendants) and unions '[0,2,'a']'. The leading '$' may be omitted
// ('.items[0]', 'items[0]').
// Returns an error message for an invalid path or document.
[[nodiscard]] std::expected<JsonPathResult, RawChars32> EvaluateJsonPath(std::string_view json, std::string_view path,
                                                                         std::size_t maxMatches = 1U << 20U);

// Writes the result as one JSON document into 'out' (overwritten): the value itself for a singular path that matched,
// an array of the matches otherwise. Stops adding matches once 'out' would exceed 'maxBytes'; returns the number of
// matches written.
std::size_t JoinJsonPathMatches(const JsonPathResult& result, RawChars& out, std::size_t maxBytes = 1UL << 30U);

}  // namespace zalie
