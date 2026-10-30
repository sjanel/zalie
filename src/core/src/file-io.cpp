#include "zalie/file-io.hpp"

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <system_error>

#include "zalie/raw-chars.hpp"

namespace zalie {

namespace {

RawChars32 ErrorMessage(std::string_view action, const std::filesystem::path& path, std::string_view reason) {
  return Concat({action, " '", PathToUtf8(path), "': ", reason});
}

}  // namespace

RawChars32 PathToUtf8(const std::filesystem::path& path) {
  const std::u8string utf8 = path.u8string();
  return RawChars32(std::string_view(reinterpret_cast<const char*>(utf8.data()), utf8.size()));
}

std::filesystem::path PathFromUtf8(std::string_view utf8) {
  return {std::u8string_view(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size())};
}

std::expected<RawChars, RawChars32> ReadFile(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) {
    return std::unexpected(ErrorMessage("cannot open", path, std::generic_category().message(errno)));
  }
  const std::streamoff size = file.tellg();
  if (size < 0) {
    return std::unexpected(ErrorMessage("cannot read", path, "unknown size"));
  }
  RawChars content(static_cast<std::size_t>(size));
  file.seekg(0);
  if (size > 0 && !file.read(content.data(), static_cast<std::streamsize>(size))) {
    return std::unexpected(ErrorMessage("cannot read", path, std::generic_category().message(errno)));
  }
  content.setSize(static_cast<RawChars::size_type>(size));
  return content;
}

std::expected<void, RawChars32> WriteFileAtomically(const std::filesystem::path& path, std::string_view content) {
  std::error_code errc;
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path(), errc);
    if (errc) {
      return std::unexpected(ErrorMessage("cannot create the directory of", path, errc.message()));
    }
  }
  std::filesystem::path tmpPath = path;
  tmpPath += ".tmp";
  {
    std::ofstream file(tmpPath, std::ios::binary | std::ios::trunc);
    if (!file) {
      return std::unexpected(ErrorMessage("cannot create", tmpPath, std::generic_category().message(errno)));
    }
    file.write(content.data(), static_cast<std::streamsize>(content.size()));
    file.flush();
    if (!file) {
      return std::unexpected(ErrorMessage("cannot write", tmpPath, std::generic_category().message(errno)));
    }
  }
  std::filesystem::rename(tmpPath, path, errc);  // atomic replacement on POSIX and on Windows (MoveFileEx)
  if (errc) {
    std::filesystem::remove(tmpPath, errc);
    return std::unexpected(ErrorMessage("cannot replace", path, errc.message()));
  }
  return {};
}

}  // namespace zalie
