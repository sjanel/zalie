#pragma once

#include <expected>
#include <string_view>

#include "zalie/collection.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/string-pool.hpp"

namespace zalie {

// Result of the import of a Postman collection.
struct PostmanImport {
  Collection collection;
  // Features that could not be imported.
  StringList warnings;
};

// Imports a Postman collection (format v2.0 / v2.1, as exported by Postman): folders, requests, headers, query, raw /
// url-encoded bodies, basic / bearer / api key authentication and collection variables. Scripts are skipped.
[[nodiscard]] std::expected<PostmanImport, RawChars32> ImportPostmanCollection(std::string_view json);

// Whether a JSON document looks like a Postman collection (to pick the importer of a dropped file).
[[nodiscard]] bool LooksLikePostmanCollection(std::string_view json) noexcept;

}  // namespace zalie
