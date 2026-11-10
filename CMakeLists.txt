cmake_minimum_required(VERSION 3.28)

if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

# Version: single source of truth is the VERSION file at the repository root.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" ZALIE_VERSION_FILE)
string(STRIP "${ZALIE_VERSION_FILE}" ZALIE_VERSION_FILE)
if(NOT ZALIE_VERSION_FILE MATCHES "^([0-9]+\\.[0-9]+\\.[0-9]+)(-[0-9A-Za-z.-]+)?$")
  message(FATAL_ERROR "VERSION file content '${ZALIE_VERSION_FILE}' must match X.Y.Z[-suffix]")
endif()

project(
  zalie
  VERSION ${CMAKE_MATCH_1}
  DESCRIPTION "Zalie - a light, fast and friendly HTTP client"
  HOMEPAGE_URL "https://github.com/sjanel/zalie"
  LANGUAGES C CXX)

set(ZALIE_VERSION_STRING "${ZALIE_VERSION_FILE}")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Zalie does not use C++ named modules: skip the (slow, and fragile with MSVC + Ninja) dependency scanning.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE
      Release
      CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(ZALIE_BUILD_APP "Build the Zalie desktop application (Dear ImGui + GLFW + OpenGL 3)" ON)
option(ZALIE_BUILD_TESTS "Build the unit and integration tests" ${PROJECT_IS_TOP_LEVEL})
option(ZALIE_ENABLE_TLS "Enable https (OpenSSL)" ON)
option(ZALIE_ENABLE_HTTP2 "Enable HTTP/2 (ALPN h2 and h2c)" ON)
option(ZALIE_ENABLE_COMPRESSION "Enable transparent response decompression (gzip, deflate, zstd, brotli)" ON)
option(ZALIE_ENABLE_WARNINGS "Enable a strict set of compiler warnings" ${PROJECT_IS_TOP_LEVEL})
option(ZALIE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(ZALIE_ENABLE_ASAN "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(ZALIE_GLFW_WAYLAND "Build the native Wayland backend of GLFW (Linux only, X11 is always built)" OFF)
option(ZALIE_ENABLE_LTO "Link-time optimization of Release builds (Zalie and its dependencies)" ON)

include(cmake/ProjectSettings.cmake)
include(cmake/Dependencies.cmake)
include(cmake/EmbedFiles.cmake)

add_subdirectory(src/core)
if(ZALIE_BUILD_APP)
  add_subdirectory(src/ui)
  add_subdirectory(src/app)
  # After the dependencies: zlib-ng includes CPack too, and the last include writes CPackConfig.cmake.
  include(cmake/Packaging.cmake)
endif()

if(ZALIE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

message(STATUS "[zalie] version ${ZALIE_VERSION_STRING} - app: ${ZALIE_BUILD_APP}, tests: ${ZALIE_BUILD_TESTS}, "
               "tls: ${ZALIE_ENABLE_TLS}, http2: ${ZALIE_ENABLE_HTTP2}, compression: ${ZALIE_ENABLE_COMPRESSION}, "
               "lto: ${ZALIE_LTO_STATUS}")
