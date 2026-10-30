# zalie_core: everything but the UI - model, variables, snippets, importers, persistence and the HTTP executor.
# It has no dependency on the GUI stack, so it is fully testable headless.

add_library(
  zalie_core STATIC
  src/basics.cpp
  src/bruno-import.cpp
  src/collection.cpp
  src/curl-import.cpp
  src/desktop-entry.cpp
  src/exchange.cpp
  src/executor.cpp
  src/file-io.cpp
  src/history.cpp
  src/json-format.cpp
  src/json-path.cpp
  src/paths.cpp
  src/persistence.cpp
  src/postman-import.cpp
  src/resolved-request.cpp
  src/settings.cpp
  src/snippets.cpp
  src/string-pool.cpp
  src/syntax.cpp
  src/text-document.cpp
  src/url.cpp
  src/variables.cpp
  src/workspace.cpp)
add_library(zalie::core ALIAS zalie_core)

target_include_directories(zalie_core PUBLIC include)
# aeronet's headers (RawChars, ObjectArrayPool) and amc containers are part of the public API.
target_link_libraries(zalie_core PUBLIC aeronet::aeronet_client amc::amc PRIVATE glaze::glaze)
find_package(Threads REQUIRED)
target_link_libraries(zalie_core PUBLIC Threads::Threads)
zalie_set_target_options(zalie_core)
