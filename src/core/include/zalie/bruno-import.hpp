#pragma once

#include <expected>
#include <filesystem>
#include <string_view>

#include "zalie/collection.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/request.hpp"
#include "zalie/string-pool.hpp"

namespace zalie {

// One parsed .bru request file.
struct BruRequest {
  Request request;
  // 'seq' of the meta block (ordering inside its folder).
  int sequence{0};
};

// Parses the content of a Bruno request file (.bru). Scripts, tests, assertions and request variables are skipped.
[[nodiscard]] std::expected<BruRequest, RawChars32> ParseBruRequest(std::string_view content);

// Parses the content of a Bruno environment file (environments/<name>.bru). Secret variables are imported empty.
[[nodiscard]] Environment ParseBruEnvironment(std::string_view name, std::string_view content);

// Result of the import of a Bruno collection directory.
struct BrunoImport {
  Collection collection;
  // Files or features that could not be imported, with the reason.
  StringList warnings;
};

// Imports a single Bruno request file (.bru), with a new id. Its name defaults to the file name. The {{variables}} it
// uses are not imported (they live in the environments and collection.bru of its collection).
[[nodiscard]] std::expected<Request, RawChars32> ImportBruRequestFile(const std::filesystem::path& path);

// Imports a Bruno collection directory (the one containing bruno.json): requests, folders, environments, collection
// headers / auth / variables.
[[nodiscard]] std::expected<BrunoImport, RawChars32> ImportBrunoCollection(const std::filesystem::path& directory);

}  // namespace zalie
