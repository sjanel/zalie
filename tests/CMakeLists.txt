include(GoogleTest)

# Adds a GoogleTest executable 'zalie_<name>_test' from tests/<dir>/<name>_test.cpp, discovered by ctest.
function(zalie_add_test dir name)
  set(target zalie_${name}_test)
  add_executable(${target} ${dir}/${name}_test.cpp)
  target_link_libraries(${target} PRIVATE GTest::gtest_main ${ARGN})
  target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  zalie_set_target_options(${target})
  target_compile_definitions(${target} PRIVATE ZALIE_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
  gtest_discover_tests(${target} DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 60)
endfunction()

foreach(
  test
  collection
  curl-import
  data-file
  desktop-entry
  encoding
  file-io
  bruno-import
  json-format
  json-path
  latency-histogram
  load-plan
  load-preview
  load-report
  persistence
  postman-import
  rate-controller
  raw-chars
  resolved-request
  snippets
  string-pool
  string-utils
  syntax
  text-document
  url
  value-generators
  variables)
  zalie_add_test(core ${test} zalie::core)
endforeach()

# A private header of zalie_core.
zalie_add_test(core json-scan zalie::core)
target_include_directories(zalie_json-scan_test PRIVATE ${PROJECT_SOURCE_DIR}/src/core/src)

# Integration tests: real HTTP exchanges against a local aeronet server.
zalie_add_test(core executor zalie::core aeronet::aeronet_server)
zalie_add_test(core load-runner zalie::core aeronet::aeronet_server)

# UI tests: the real user interface, driven headless through Dear ImGui's null backend (no window, no GPU).
if(ZALIE_BUILD_APP)
  add_library(zalie_ui_test_support STATIC ${imgui_SOURCE_DIR}/backends/imgui_impl_null.cpp)
  target_include_directories(zalie_ui_test_support SYSTEM PUBLIC ${imgui_SOURCE_DIR}/backends)
  target_link_libraries(zalie_ui_test_support PUBLIC zalie_imgui)
  zalie_add_test(ui app zalie::ui zalie_ui_test_support aeronet::aeronet_server)
  # The tests of the application share one executable (app-test-fixture.hpp): each one would link the whole interface.
  target_sources(zalie_app_test PRIVATE ui/dialogs_test.cpp ui/inspector_test.cpp ui/perf-panel_test.cpp ui/request-panel_test.cpp ui/sidebar_test.cpp)
  zalie_add_test(ui widgets zalie::ui zalie_ui_test_support)
endif()
