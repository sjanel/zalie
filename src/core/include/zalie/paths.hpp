#pragma once

#include <filesystem>
#include <string_view>

#include "zalie/raw-chars.hpp"

namespace zalie {

// Directory where Zalie keeps its settings, workspace state, history and (by default) its collections:
//   $ZALIE_HOME if set, else
//   Linux:   $XDG_CONFIG_HOME/zalie or ~/.config/zalie
//   macOS:   ~/Library/Application Support/Zalie
//   Windows: %APPDATA%\Zalie
[[nodiscard]] std::filesystem::path DefaultHomeDirectory();

// File name for a new collection: a lower-case slug of its name ("My API!" -> "my-api.zalie.json").
[[nodiscard]] RawChars32 CollectionFileName(std::string_view collectionName);

}  // namespace zalie
