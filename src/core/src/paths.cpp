#include "zalie/paths.hpp"

#include <aeronet/cctype.hpp>
#include <aeronet/memory-utils-sv.hpp>
#include <aeronet/toupperlower.hpp>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string_view>

#include "zalie/file-io.hpp"
#include "zalie/raw-chars.hpp"

namespace zalie {

std::string_view EnvironmentVariable(const char* name) noexcept {
  const char* pValue = std::getenv(name);  // NOLINT(concurrency-mt-unsafe)
  return pValue == nullptr ? std::string_view{} : std::string_view{pValue};
}

std::filesystem::path DefaultHomeDirectory() {
  if (const std::string_view home = EnvironmentVariable("ZALIE_HOME"); !home.empty()) {
    return PathFromUtf8(home);
  }
#ifdef _WIN32
  if (const std::string_view appData = EnvironmentVariable("APPDATA"); !appData.empty()) {
    return PathFromUtf8(appData) / "Zalie";
  }
  return std::filesystem::current_path() / "Zalie";
#elif defined(__APPLE__)  // NOLINT(readability-use-concise-preprocessor-directives): no C++23 #elifdef for MSVC
  return PathFromUtf8(EnvironmentVariable("HOME")) / "Library" / "Application Support" / "Zalie";
#else
  if (const std::string_view xdg = EnvironmentVariable("XDG_CONFIG_HOME"); !xdg.empty()) {
    return PathFromUtf8(xdg) / "zalie";
  }
  return PathFromUtf8(EnvironmentVariable("HOME")) / ".config" / "zalie";
#endif
}

RawChars32 CollectionFileName(std::string_view collectionName) {
  static constexpr std::string_view kDefaultName = "collection";
  static constexpr std::string_view kExtension = ".zalie.json";
  // A slug is not longer than the name it is made of.
  RawChars32 slug(std::max(collectionName.size(), kDefaultName.size()) + kExtension.size());
  char* pData = slug.data();
  bool pendingDash = false;
  for (char ch : collectionName) {
    const char lower = aeronet::tolower(ch);
    if (aeronet::islower(lower) || aeronet::isdigit(lower)) {
      if (pendingDash && pData != slug.data()) {
        *pData++ = '-';
      }
      *pData++ = lower;
      pendingDash = false;
    } else {
      pendingDash = true;
    }
  }
  if (pData == slug.data()) {
    pData = aeronet::Append(kDefaultName, pData);
  }
  slug.setEnd(aeronet::Append(kExtension, pData));
  return slug;
}

}  // namespace zalie
