#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

#include "zalie/raw-chars.hpp"

namespace zalie {

struct ResolvedRequest;

// Languages / tools for which Zalie generates a ready-to-paste snippet of the current request.
enum class SnippetLanguage : std::uint8_t {
  curl,
  httpie,
  wget,
  powershell,
  python,
  javascript,
  go,
  cppAeronet,
  rawHttp
};

inline constexpr std::array kSnippetLanguages{
    SnippetLanguage::curl,       SnippetLanguage::httpie,     SnippetLanguage::wget,
    SnippetLanguage::powershell, SnippetLanguage::python,     SnippetLanguage::javascript,
    SnippetLanguage::go,         SnippetLanguage::cppAeronet, SnippetLanguage::rawHttp};

// Display name ("cURL", "Python (requests)"...).
[[nodiscard]] std::string_view SnippetLanguageName(SnippetLanguage language) noexcept;

// Stable identifier used in settings files ("curl", "python"...).
[[nodiscard]] std::string_view SnippetLanguageId(SnippetLanguage language) noexcept;

// Parses an identifier returned by SnippetLanguageId.
[[nodiscard]] std::optional<SnippetLanguage> ParseSnippetLanguage(std::string_view id) noexcept;

// Writes (replaces) in 'out' the snippet sending 'request' in 'language'.
void GenerateSnippet(SnippetLanguage language, const ResolvedRequest& request, RawChars32& out);

}  // namespace zalie
