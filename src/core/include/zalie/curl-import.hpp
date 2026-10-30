#pragma once

#include <expected>
#include <string_view>

#include "zalie/raw-chars.hpp"
#include "zalie/request.hpp"
#include "zalie/string-pool.hpp"

namespace zalie {

// Result of the conversion of a curl command line into a request.
struct CurlImport {
  Request request;
  // Options that were ignored or approximated.
  StringList warnings;
};

// Whether the text looks like a curl command ("curl ..."), e.g. pasted in the URL bar.
[[nodiscard]] bool LooksLikeCurlCommand(std::string_view text) noexcept;

// Splits a shell command line into arguments: single / double quotes, ANSI-C $'...' quotes, backslash escapes and
// line continuations (backslash, caret or backtick + newline) are supported. Fails on an unterminated quote.
// The words are stored in a single StringList (one arena) instead of one allocation each.
[[nodiscard]] std::expected<StringList, RawChars32> SplitShellWords(std::string_view cmd);

// Converts a curl command (as copied from a browser's devtools "Copy as cURL", or a documentation) into a request.
[[nodiscard]] std::expected<CurlImport, RawChars32> ParseCurlCommand(std::string_view command);

}  // namespace zalie
