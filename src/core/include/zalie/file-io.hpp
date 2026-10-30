#pragma once

#include <expected>
#include <filesystem>
#include <string_view>

#include "zalie/raw-chars.hpp"

namespace zalie {

// Reads a whole file, in a single allocation of its exact size.
[[nodiscard]] std::expected<RawChars, RawChars32> ReadFile(const std::filesystem::path& path);

// Writes a file atomically: the content goes to a temporary file in the same directory, renamed over the target.
// A crash in the middle never leaves a truncated collection behind. Parent directories are created if needed.
[[nodiscard]] std::expected<void, RawChars32> WriteFileAtomically(const std::filesystem::path& path,
                                                                  std::string_view content);

// UTF-8 string of a path (std::filesystem::path::string() is not UTF-8 on Windows).
[[nodiscard]] RawChars32 PathToUtf8(const std::filesystem::path& path);

// Path from a UTF-8 string.
[[nodiscard]] std::filesystem::path PathFromUtf8(std::string_view utf8);

}  // namespace zalie
