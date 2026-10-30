# Third-party dependencies, all pinned to an exact archive + SHA-256 and fetched with FetchContent.
#
# Any of them can be redirected to a local checkout with the standard FetchContent override, for instance
#   -D FETCHCONTENT_SOURCE_DIR_AERONET=/path/to/aeronet
# which is handy to develop Zalie and aeronet side by side.

include(FetchContent)

# Zalie ships as a single self-contained executable: every fetched dependency is a static library (zlib-ng and brotli
# would default to shared ones).
set(BUILD_SHARED_LIBS
    OFF
    CACHE BOOL "Build shared libraries (Zalie links its dependencies statically)")
# Third-party code is not ours to lint: keep their warnings out of our builds.
set(FETCHCONTENT_QUIET ON)

# ---------------------------------------------------------------------------
# aeronet - HTTP/1.1 + HTTP/2 client (and the server, for the integration tests only)
# ---------------------------------------------------------------------------
set(ZALIE_AERONET_URL
    "https://github.com/sjanel/aeronet/archive/9ce19dec4e8e310a2e478fd0d41e26ccb33013d9.tar.gz"
    CACHE STRING "aeronet source archive")
set(ZALIE_AERONET_SHA256
    "0f745ea9f100c9c5c50764ce7bcb276b88816dafad06f9ad2ef45423b67298f6"
    CACHE STRING "aeronet source archive SHA-256")

set(AERONET_ENABLE_HTTP_CLIENT ON)
# The server is only needed by the integration tests (they spin up a local aeronet server).
set(AERONET_ENABLE_HTTP_SERVER ${ZALIE_BUILD_TESTS})
set(AERONET_ENABLE_ASYNC_HANDLERS OFF)
set(AERONET_ENABLE_WEBSOCKET OFF)
set(AERONET_ENABLE_HTTP2 ${ZALIE_ENABLE_HTTP2})
set(AERONET_ENABLE_OPENSSL ${ZALIE_ENABLE_TLS})
set(AERONET_ENABLE_ZLIB ${ZALIE_ENABLE_COMPRESSION})
set(AERONET_ENABLE_ZSTD ${ZALIE_ENABLE_COMPRESSION})
set(AERONET_ENABLE_BROTLI ${ZALIE_ENABLE_COMPRESSION})
set(AERONET_ENABLE_SPDLOG OFF)
set(AERONET_ENABLE_OPENTELEMETRY OFF)
set(AERONET_ENABLE_PROTOBUF OFF)
set(AERONET_ENABLE_GLAZE OFF)
set(AERONET_ENABLE_JWT OFF)
set(AERONET_BUILD_TESTS OFF)
set(AERONET_BUILD_EXAMPLES OFF)
set(AERONET_BUILD_BENCHMARKS OFF)
set(AERONET_INSTALL OFF)
set(AERONET_UNITY_BUILD ON)

FetchContent_Declare(
  aeronet
  URL "${ZALIE_AERONET_URL}"
  URL_HASH SHA256=${ZALIE_AERONET_SHA256}
  EXCLUDE_FROM_ALL SYSTEM)

# ---------------------------------------------------------------------------
# glaze - JSON reflection used for the collection / settings files
# ---------------------------------------------------------------------------
set(glaze_DEVELOPER_MODE OFF)
set(glaze_ENABLE_FUZZING OFF)
FetchContent_Declare(
  glaze
  URL "https://github.com/stephenberry/glaze/archive/refs/tags/v9.0.0.tar.gz"
  URL_HASH SHA256=dd7b033c5bf6e4308615bb7834c541291d5b20f64121d9a68222a39414635517
  EXCLUDE_FROM_ALL SYSTEM)

# zstd (fetched by aeronet) enables the ASM language: CMake 4.1+ asks whether MSVC is an assembler for it (policy
# CMP0194). Keep the answer of the older versions, without their developer warning: zstd builds no assembly with MSVC.
set(CMAKE_POLICY_DEFAULT_CMP0194 OLD)

FetchContent_MakeAvailable(aeronet glaze)

if(NOT TARGET aeronet::aeronet_client)
  add_library(aeronet::aeronet_client ALIAS aeronet_client)
endif()
if(ZALIE_BUILD_TESTS AND NOT TARGET aeronet::aeronet_server)
  add_library(aeronet::aeronet_server ALIAS aeronet_server)
endif()

# ---------------------------------------------------------------------------
# GUI stack: Dear ImGui + GLFW + stb (image decoding / encoding)
# ---------------------------------------------------------------------------
if(ZALIE_BUILD_APP)
  FetchContent_Declare(
    imgui
    URL "https://github.com/ocornut/imgui/archive/refs/tags/v1.93.0.tar.gz"
    URL_HASH SHA256=f7bf6bc76e5d79d40d751096663e6c5d18daf08129dd52e7f7b09e4251c8eeee)

  set(GLFW_BUILD_DOCS OFF)
  set(GLFW_BUILD_TESTS OFF)
  set(GLFW_BUILD_EXAMPLES OFF)
  set(GLFW_INSTALL OFF)
  set(GLFW_BUILD_WAYLAND ${ZALIE_GLFW_WAYLAND})
  FetchContent_Declare(
    glfw
    URL "https://github.com/glfw/glfw/archive/refs/tags/3.5.1.tar.gz"
    URL_HASH SHA256=5234f4f29473e9a06bc7847d8371858dd135d38466eeeaa652fdc9f8f9ff0c20
    EXCLUDE_FROM_ALL SYSTEM)

  FetchContent_Declare(
    stb
    URL "https://github.com/nothings/stb/archive/003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5.tar.gz"
    URL_HASH SHA256=a6c31950b3bdf05eb92258768ba2a11bd44449490574b1aac67028452288c40d)

  FetchContent_MakeAvailable(imgui glfw stb)

  # Dear ImGui core (no platform / renderer backend: those live in the app, so the UI can run headless in tests).
  add_library(
    zalie_imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/src/imgui-assert.cpp)
  target_include_directories(zalie_imgui SYSTEM PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/misc/cpp)
  target_include_directories(zalie_imgui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/include)
  target_compile_definitions(zalie_imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS
                                                IMGUI_USER_CONFIG="zalie/ui/imconfig-zalie.hpp")
  set_target_properties(zalie_imgui PROPERTIES POSITION_INDEPENDENT_CODE ON)

  # stb_image (PNG / JPEG / GIF / BMP decoding) and stb_image_write (PNG screenshots).
  add_library(zalie_stb STATIC ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/src/stb-impl.cpp)
  target_include_directories(zalie_stb SYSTEM PUBLIC ${stb_SOURCE_DIR})
endif()

# ---------------------------------------------------------------------------
# GoogleTest
# ---------------------------------------------------------------------------
if(ZALIE_BUILD_TESTS)
  set(gtest_force_shared_crt ON)
  set(INSTALL_GTEST OFF)
  set(BUILD_GMOCK OFF)
  FetchContent_Declare(
    googletest
    URL "https://github.com/google/googletest/archive/refs/tags/v1.18.0.tar.gz"
    URL_HASH SHA256=6e3191c1455468b3fc35a417fb565c1c5071aee1b7e7f85e30cf48a98d37d8b5
    EXCLUDE_FROM_ALL SYSTEM)
  FetchContent_MakeAvailable(googletest)
endif()
