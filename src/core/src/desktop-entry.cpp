#include "zalie/desktop-entry.hpp"

#include <expected>
#include <filesystem>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>

#include "zalie/encoding.hpp"
#include "zalie/file-io.hpp"
#include "zalie/paths.hpp"
#include "zalie/raw-chars.hpp"

namespace zalie {

namespace {

namespace fs = std::filesystem;

constexpr std::string_view kGeneratedMarker = "X-Zalie-Generated=true";
constexpr std::string_view kExecutableKey = "X-Zalie-Executable=";

fs::path EntryPath(const fs::path& dataDir) { return dataDir / "applications" / "zalie.desktop"; }

// The Exec value running 'executable' with the dropped files: the path is a quoted argument (kDesktopExecEscapes),
// and the whole value is then escaped as a string ("The Exec key").
RawChars32 ExecValue(std::string_view executable) {
  RawChars32 quoted;
  AppendAll(quoted, Escaped(executable, kDesktopExecEscapes, '"'), " %F");
  RawChars32 escaped;
  AppendAll(escaped, Escaped(quoted, kDesktopStringEscapes));
  return escaped;
}

// The executable recorded in a generated entry, empty if absent.
std::string_view RecordedExecutable(std::string_view content) {
  const std::size_t pos = content.find(kExecutableKey);
  if (pos == std::string_view::npos) {
    return {};
  }
  const std::string_view rest = content.substr(pos + kExecutableKey.size());
  return rest.substr(0, rest.find('\n'));
}

// Inverse of the escaping of a string value (kDesktopStringEscapes), for the recorded executable.
RawChars32 UnescapeString(std::string_view value) {
  static constexpr std::string_view kControlEscapes = "ntr";
  RawChars32 out(value.size());  // unescaping only shrinks
  char* pOut = out.data();
  for (std::size_t pos = 0; pos < value.size(); ++pos) {
    char ch = value[pos];
    if (ch == '\\' && pos + 1 < value.size()) {
      ch = value[++pos];
      if (kControlEscapes.contains(ch)) {
        ch = ControlEscape(ch);
      }
    }
    *pOut++ = ch;
  }
  out.setEnd(pOut);
  return out;
}

bool IsGenerated(std::string_view content) { return content.contains(kGeneratedMarker); }

std::expected<void, RawChars32> WriteIfDifferent(const fs::path& path, std::string_view content, bool& written) {
  if (auto existing = ReadFile(path); existing && std::string_view(*existing) == content) {
    return {};
  }
  written = true;
  return WriteFileAtomically(path, content);
}

}  // namespace

XdgDataDirs XdgDataDirsFromEnvironment() {
  XdgDataDirs dirs;
  if (const fs::path dataHome = PathFromUtf8(EnvironmentVariable("XDG_DATA_HOME")); dataHome.is_absolute()) {
    dirs.dataHome = dataHome;
  } else if (const fs::path home = PathFromUtf8(EnvironmentVariable("HOME")); home.is_absolute()) {
    dirs.dataHome = home / ".local" / "share";
  }
  std::string_view dataDirs = EnvironmentVariable("XDG_DATA_DIRS");
  if (dataDirs.empty()) {
    dataDirs = "/usr/local/share:/usr/share";
  }
  while (!dataDirs.empty()) {
    const std::size_t colonPos = dataDirs.find(':');
    const fs::path dir = PathFromUtf8(dataDirs.substr(0, colonPos));
    if (dir.is_absolute() && dir != dirs.dataHome) {
      dirs.dataDirs.push_back(dir);
    }
    dataDirs = colonPos == std::string_view::npos ? std::string_view{} : dataDirs.substr(colonPos + 1);
  }
  return dirs;
}

RawChars32 MakeDesktopEntry(const fs::path& executable, const fs::path& iconPath) {
  const RawChars32 exe = PathToUtf8(executable);
  const RawChars32 icon = PathToUtf8(iconPath);
  RawChars32 content;
  AppendAll(content,
            "[Desktop Entry]\n"
            "# Written by Zalie at its launch, for its name and icon in the task bar. Delete it to remove them;\n"
            "# 'zalie --no-desktop-entry' does not create it.\n"
            "Type=Application\n"
            "Name=Zalie\n"
            "GenericName=HTTP Client\n"
            "Comment=A light, fast and friendly HTTP client\n"
            "Exec=",
            ExecValue(exe), "\nIcon=", Escaped(icon, kDesktopStringEscapes),
            "\nTerminal=false\n"
            "Categories=Development;WebDevelopment;\n"
            "Keywords=http;rest;api;curl;postman;bruno;\n"
            "StartupWMClass=Zalie\n",
            kGeneratedMarker, '\n', kExecutableKey, Escaped(exe, kDesktopStringEscapes), '\n');
  return content;
}

std::expected<DesktopEntryStatus, RawChars32> EnsureDesktopEntry(const XdgDataDirs& dirs, const fs::path& executable,
                                                                 std::span<const unsigned char> iconPng) {
  if (dirs.dataHome.empty()) {
    return std::unexpected(RawChars32(std::string_view{"no user data directory ($XDG_DATA_HOME, $HOME)"}));
  }
  std::error_code errc;
  const fs::path userEntry = EntryPath(dirs.dataHome);
  const fs::path icon = dirs.dataHome / "icons" / "hicolor" / "256x256" / "apps" / "zalie.png";
  auto userContent = ReadFile(userEntry);
  const bool userGenerated = userContent && IsGenerated(*userContent);
  if (userContent && !userGenerated) {
    return DesktopEntryStatus::foreign;
  }
  for (const fs::path& dir : dirs.dataDirs) {
    if (fs::is_regular_file(EntryPath(dir), errc)) {
      if (userGenerated) {
        // Installed since: the generated entry would shadow it (the user directory comes first).
        fs::remove(userEntry, errc);
        fs::remove(icon, errc);
      }
      return DesktopEntryStatus::foreign;
    }
  }

  // Keep the executable a generated entry was made for while it exists (two builds launched alternately do not fight
  // over the entry), else point it to this one.
  fs::path target = executable;
  if (userGenerated) {
    const RawChars32 recorded = UnescapeString(RecordedExecutable(*userContent));
    if (const fs::path recordedPath = PathFromUtf8(recorded); !recorded.empty() && fs::exists(recordedPath, errc)) {
      target = recordedPath;
    }
  }
  bool written = false;
  if (auto result = WriteIfDifferent(
          icon, std::string_view(reinterpret_cast<const char*>(iconPng.data()), iconPng.size()), written);
      !result) {
    return std::unexpected(std::move(result.error()));
  }
  const RawChars32 content = MakeDesktopEntry(target, icon);
  bool entryWritten = false;
  if (auto result = WriteIfDifferent(userEntry, content, entryWritten); !result) {
    return std::unexpected(std::move(result.error()));
  }
  if (!userContent) {
    return DesktopEntryStatus::created;
  }
  return written || entryWritten ? DesktopEntryStatus::updated : DesktopEntryStatus::upToDate;
}

}  // namespace zalie
