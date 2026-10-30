#pragma once

#include <amc/vector.hpp>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>

#include "zalie/raw-chars.hpp"

namespace zalie {

// Base directories of the XDG specification where desktop entries live.
struct XdgDataDirs {
  // $XDG_DATA_HOME (default ~/.local/share): where the user's own entries are, and where Zalie writes its own.
  std::filesystem::path dataHome;
  // $XDG_DATA_DIRS (default /usr/local/share:/usr/share): system entries (packages, 'sudo cmake --install').
  amc::vector<std::filesystem::path> dataDirs;
};

// The XDG data directories of the environment. Relative values are ignored, as the specification requires; an empty
// dataHome means that neither $XDG_DATA_HOME nor $HOME is usable.
[[nodiscard]] XdgDataDirs XdgDataDirsFromEnvironment();

// Content of the desktop entry Zalie generates for 'executable', with its icon at 'iconPath' (absolute paths).
// Same identity as packaging/zalie.desktop.in (StartupWMClass=Zalie), plus markers recognizing it as generated.
[[nodiscard]] RawChars32 MakeDesktopEntry(const std::filesystem::path& executable,
                                          const std::filesystem::path& iconPath);

enum class DesktopEntryStatus : std::uint8_t {
  // An entry Zalie did not generate exists (package, 'cmake --install'): left untouched.
  foreign,
  upToDate,
  created,
  updated,
};

// Makes sure the desktop finds a desktop entry for Zalie: desktops like GNOME do not show the icon of a window, they
// show the one of the desktop entry matching it (StartupWMClass), and a generic icon without one.
// If no zalie.desktop exists in the data directories, writes <dataHome>/applications/zalie.desktop for 'executable'
// and its icon in <dataHome>/icons/hicolor/256x256/apps. A generated entry is kept pointing to the executable it was
// created for while that one exists, and moved to 'executable' otherwise. A generated entry shadowing a system one
// installed since is removed.
[[nodiscard]] std::expected<DesktopEntryStatus, RawChars32> EnsureDesktopEntry(const XdgDataDirs& dirs,
                                                                               const std::filesystem::path& executable,
                                                                               std::span<const unsigned char> iconPng);

}  // namespace zalie
