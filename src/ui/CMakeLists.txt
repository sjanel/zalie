# zalie_ui: the user interface (Dear ImGui), independent of the platform and renderer backends so that it can be
# driven headless by the tests.

add_library(
  zalie_ui STATIC
  src/app.cpp
  src/assets.cpp
  src/dialogs.cpp
  src/icons.cpp
  src/inspector.cpp
  src/request-panel.cpp
  src/sidebar.cpp
  src/text-view.cpp
  src/theme.cpp
  src/widgets.cpp)
add_library(zalie::ui ALIAS zalie_ui)

target_include_directories(zalie_ui PUBLIC include)
target_link_libraries(zalie_ui PUBLIC zalie::core zalie_imgui PRIVATE zalie_stb)
zalie_set_target_options(zalie_ui)

# Fonts shipped with Dear ImGui (Roboto: Apache 2.0, Cousine: Apache 2.0) and Zalie's own artwork.
zalie_embed_file(zalie_ui "${imgui_SOURCE_DIR}/misc/fonts/Roboto-Medium.ttf" FontUi)
zalie_embed_file(zalie_ui "${imgui_SOURCE_DIR}/misc/fonts/Cousine-Regular.ttf" FontMono)
zalie_embed_file(zalie_ui "${PROJECT_SOURCE_DIR}/assets/logo/zalie-logo-256.png" LogoPng)
zalie_embed_file(zalie_ui "${PROJECT_SOURCE_DIR}/assets/logo/zalie-logo-64.png" LogoSmallPng)
zalie_embed_file(zalie_ui "${PROJECT_SOURCE_DIR}/assets/sprites/zalie-sprites.png" SpritesPng)
