#include "zalie/paths.hpp"

#include <cstdlib>
#include <filesystem>
#include <string_view>

#include "zalie/file-io.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/string-utils.hpp"

namespace zalie {

namespace {

std::string_view Env(const char* name) {
  // Zalie never modifies its environment.
  const char* pValue = std::getenv(name);  // NOLINT(concurrency-mt-unsafe)
  return pValue == nullptr ? std::string_view{} : std::string_view{pValue};
}

}  // namespace

std::filesystem::path DefaultHomeDirectory() {
  if (const std::string_view home = Env("ZALIE_HOME"); !home.empty()) {
    return PathFromUtf8(home);
  }
#ifdef _WIN32
  if (const std::string_view appData = Env("APPDATA"); !appData.empty()) {
    return PathFromUtf8(appData) / "Zalie";
  }
  return std::filesystem::current_path() / "Zalie";
#elif defined(__APPLE__)  // NOLINT(readability-use-concise-preprocessor-directives): no C++23 #elifdef for MSVC
  return PathFromUtf8(Env("HOME")) / "Library" / "Application Support" / "Zalie";
#else
  if (const std::string_view xdg = Env("XDG_CONFIG_HOME"); !xdg.empty()) {
    return PathFromUtf8(xdg) / "zalie";
  }
  return PathFromUtf8(Env("HOME")) / ".config" / "zalie";
#endif
}

RawChars32 CollectionFileName(std::string_view collectionName) {
  RawChars32 slug(collectionName.size() + 16);
  bool pendingDash = false;
  for (char ch : collectionName) {
    const char lower = ToLowerAscii(ch);
    if ((lower >= 'a' && lower <= 'z') || (lower >= '0' && lower <= '9')) {
      if (pendingDash && !slug.empty()) {
        slug.push_back('-');
      }
      slug.push_back(lower);
      pendingDash = false;
    } else {
      pendingDash = true;
    }
  }
  if (slug.empty()) {
    slug.append(std::string_view{"collection"});
  }
  slug.append(std::string_view{".zalie.json"});
  return slug;
}

}  // namespace zalie
