# Packages of the application, made by cpack from the install rules of src/app/CMakeLists.txt:
#   - Linux: a Debian package (cpack -G DEB), its library dependencies computed by dpkg-shlibdeps;
#   - Windows: a portable zip (cpack -G ZIP -C Release), zalie.exe next to the DLLs it needs.
# The macOS disk image is made by .github/workflows/package.yml, which also bundles the Homebrew libraries into the
# application. Packages land in <build>/packages, each with its .sha256.

set(CPACK_PACKAGE_NAME zalie)
set(CPACK_PACKAGE_VENDOR "Zalie contributors")
set(CPACK_PACKAGE_CONTACT "Stephane Janel <67502353+sjanel@users.noreply.github.com>")
set(CPACK_PACKAGE_VERSION "${ZALIE_VERSION_STRING}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Light, fast and friendly HTTP client")
string(
  CONCAT CPACK_PACKAGE_DESCRIPTION
         "Zalie keeps the good ideas of Bruno and Postman (collections, environments, code snippets) in a native "
    "application of a few megabytes. Local only: no account, no cloud.")
set(CPACK_PACKAGE_HOMEPAGE_URL "${PROJECT_HOMEPAGE_URL}")
set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}/packages")
set(CPACK_PACKAGE_CHECKSUM SHA256)
set(CPACK_STRIP_FILES ON)

if(WIN32)
  set(CPACK_GENERATOR ZIP)
  set(CPACK_PACKAGE_FILE_NAME "zalie-${ZALIE_VERSION_STRING}-windows-x64")
elseif(UNIX AND NOT APPLE)
  set(CPACK_GENERATOR DEB)
  # Install with DESTDIR: the install script then sees the final prefix, written in the Exec line of the desktop
  # entry (instead of the staging directory of cpack).
  set(CPACK_SET_DESTDIR ON)
  set(CPACK_INSTALL_PREFIX /usr)
  # A suffix sorts before the release in Debian versions with '~' (0.2.0~dev < 0.2.0), not with '-': the package of a
  # development version is upgraded by its release. The file name keeps the '-': GitHub renames release assets with a
  # '~' in their name (and their .sha256 would no longer match).
  string(REPLACE "-" "~" CPACK_DEBIAN_PACKAGE_VERSION "${ZALIE_VERSION_STRING}")
  execute_process(
    COMMAND dpkg --print-architecture
    OUTPUT_VARIABLE ZALIE_DEB_ARCHITECTURE
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(ZALIE_DEB_ARCHITECTURE)
    set(CPACK_DEBIAN_FILE_NAME "zalie_${ZALIE_VERSION_STRING}_${ZALIE_DEB_ARCHITECTURE}.deb")
  else()
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
  endif()
  set(CPACK_DEBIAN_PACKAGE_SECTION web)
  set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
  # Loaded at run time by GLFW (dlopen), hence invisible to dpkg-shlibdeps.
  set(CPACK_DEBIAN_PACKAGE_DEPENDS "libgl1, libx11-6, libxcursor1, libxext6, libxi6, libxinerama1, libxrandr2")
else()
  set(CPACK_GENERATOR TGZ)
endif()

include(CPack)
