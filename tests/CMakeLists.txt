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
  desktop-entry
  bruno-import
  json-format
  json-path
  persistence
  postman-import
  resolved-request
  snippets
  string-pool
  syntax
  text-document
  url
  variables)
  zalie_add_test(core ${test} zalie::core)
endforeach()

# Integration tests: real HTTP exchanges against a local aeronet server.
zalie_add_test(core executor zalie::core aeronet::aeronet_server)

# UI tests: the real user interface, driven headless through Dear ImGui's null backend (no window, no GPU).
if(ZALIE_BUILD_APP)
  add_library(zalie_ui_test_support STATIC ${imgui_SOURCE_DIR}/backends/imgui_impl_null.cpp)
  target_include_directories(zalie_ui_test_support SYSTEM PUBLIC ${imgui_SOURCE_DIR}/backends)
  target_link_libraries(zalie_ui_test_support PUBLIC zalie_imgui)
  zalie_add_test(ui app zalie::ui zalie_ui_test_support aeronet::aeronet_server)
  zalie_add_test(ui widgets zalie::ui zalie_ui_test_support)
endif()
