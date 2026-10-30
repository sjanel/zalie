#pragma once

#include <cstdint>
#include <string_view>

#include "zalie/raw-chars.hpp"

namespace zalie {

// Pretty-prints a JSON document with 'indent' spaces, in a single pass, validating it at the same time.
// Empty objects and arrays stay on one line ("{}", "[]"). Strings are copied verbatim (escapes untouched).
// Returns false if 'json' is not valid JSON (RFC 8259); 'out' is then left in an unspecified state.
// 'out' is overwritten. Nesting deeper than 1024 levels is rejected.
[[nodiscard]] bool PrettyPrintJson(std::string_view json, RawChars& out, std::uint32_t indent = 2);

// Whether 'json' is a valid JSON document.
[[nodiscard]] bool IsValidJson(std::string_view json);

}  // namespace zalie
