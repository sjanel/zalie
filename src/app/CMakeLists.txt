# The Zalie executable: GLFW window + OpenGL 3 renderer around zalie_ui.

# Dear ImGui platform / renderer backends (third-party code: built without Zalie's warnings).
add_library(zalie_imgui_backends STATIC ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
                                        ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(zalie_imgui_backends SYSTEM PUBLIC ${imgui_SOURCE_DIR}/backends)
target_link_libraries(zalie_imgui_backends PUBLIC zalie_imgui glfw ${CMAKE_DL_LIBS})
# The OpenGL declarations come from the loader bundled with Dear ImGui: no system GL headers needed.
target_compile_definitions(zalie_imgui_backends PUBLIC GLFW_INCLUDE_NONE)
if(APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  # GCC cannot parse the Apple framework headers GLFW's glfw3native.h includes for Cocoa: a stand-in declaring what
  # the backend needs takes its place (searched before GLFW's own headers, included as SYSTEM).
  target_include_directories(zalie_imgui_backends BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/macos-gcc)
endif()

add_executable(zalie WIN32 MACOSX_BUNDLE main.cpp gl-screenshot.cpp)
target_link_libraries(zalie PRIVATE zalie::ui zalie_imgui_backends zalie_stb)
zalie_set_target_options(zalie)
# The executable goes to <build>/bin (<build>/bin/<Config> with multi-config generators), unless the whole build
# redirects its executables with CMAKE_RUNTIME_OUTPUT_DIRECTORY.
if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
  set_target_properties(zalie PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()
zalie_embed_file(zalie "${PROJECT_SOURCE_DIR}/assets/logo/zalie-logo-32.png" Icon32Png)
zalie_embed_file(zalie "${PROJECT_SOURCE_DIR}/assets/logo/zalie-logo-48.png" Icon48Png)
zalie_embed_file(zalie "${PROJECT_SOURCE_DIR}/assets/logo/zalie-logo-128.png" Icon128Png)

if(WIN32)
  # A GUI application (no console window) keeping the standard main() entry point (MinGW does it by itself).
  if(MSVC)
    target_link_options(zalie PRIVATE /ENTRY:mainCRTStartup)
  endif()
endif()

if(APPLE)
  # Bundle versions are numeric (X.Y.Z): without the suffix of a development version.
  set_target_properties(
    zalie
    PROPERTIES MACOSX_BUNDLE_BUNDLE_NAME "Zalie"
               MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.sjanel.zalie"
               MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
               MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}")
endif()

include(GNUInstallDirs)
if(WIN32)
  # Portable layout: zalie.exe at the top of the install directory (and of the zip package), next to the DLLs it
  # loads, so that it runs on a machine without OpenSSL or the Visual C++ runtime.
  install(TARGETS zalie RUNTIME DESTINATION .)
  if(ZALIE_ENABLE_TLS)
    cmake_path(GET OPENSSL_INCLUDE_DIR PARENT_PATH ZALIE_OPENSSL_ROOT)
    file(GLOB ZALIE_OPENSSL_DLLS "${ZALIE_OPENSSL_ROOT}/bin/libssl-*.dll" "${ZALIE_OPENSSL_ROOT}/bin/libcrypto-*.dll")
    if(ZALIE_OPENSSL_DLLS)
      install(FILES ${ZALIE_OPENSSL_DLLS} DESTINATION .)
    else()
      message(STATUS "[zalie] no OpenSSL DLL in ${ZALIE_OPENSSL_ROOT}/bin (static OpenSSL?): none installed")
    endif()
  endif()
  set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION .)
  include(InstallRequiredSystemLibraries)
else()
  install(TARGETS zalie RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} BUNDLE DESTINATION .)
endif()
if(UNIX AND NOT APPLE)
  # Desktop integration (freedesktop): the desktop entry, matched by the desktop to the window through its WM_CLASS /
  # Wayland app id, gives Zalie its name and icon in the task bar and the applications menu. Its Exec line is the
  # absolute path of the installed executable, computed at install time (it honors 'cmake --install --prefix').
  set(ZALIE_DESKTOP_FILE "${CMAKE_CURRENT_BINARY_DIR}/zalie.desktop")
  install(
    CODE "
    if(IS_ABSOLUTE \"${CMAKE_INSTALL_BINDIR}\")
      set(ZALIE_DESKTOP_EXEC \"${CMAKE_INSTALL_BINDIR}/zalie\")
    else()
      set(ZALIE_DESKTOP_EXEC \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}/zalie\")
    endif()
    configure_file(\"${PROJECT_SOURCE_DIR}/packaging/zalie.desktop.in\" \"${ZALIE_DESKTOP_FILE}\" @ONLY)")
  install(FILES "${ZALIE_DESKTOP_FILE}" DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
  foreach(size IN ITEMS 16 32 48 64 128 256 512)
    install(FILES "${PROJECT_SOURCE_DIR}/assets/logo/zalie-logo-${size}.png"
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps RENAME zalie.png)
  endforeach()
  install(FILES "${PROJECT_SOURCE_DIR}/assets/logo/zalie-logo.svg"
          DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps RENAME zalie.svg)
endif()
