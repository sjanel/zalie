#pragma once

#include <imgui.h>

#include <amc/vector.hpp>
#include <cstdint>
#include <memory>
#include <span>

namespace zalie::ui {

// A texture uploaded through Dear ImGui's texture protocol (ImGuiBackendFlags_RendererHasTextures): the renderer
// backend creates / destroys the GPU object, so the UI code never touches the graphics API (and runs headless in
// tests with the null backend).
class TextureStore {
 public:
  TextureStore() = default;
  TextureStore(const TextureStore&) = delete;
  TextureStore& operator=(const TextureStore&) = delete;
  ~TextureStore();

  // Decodes a PNG / JPEG / GIF / BMP image into a new texture. Returns nullptr if it cannot be decoded.
  ImTextureData* createFromImage(std::span<const unsigned char> encoded);

  // Asks the backend to destroy the texture; it is freed once the backend is done with it.
  void release(ImTextureData* pTexture);

  // Frees the released textures the backend has destroyed. Call once per frame.
  void collect();

 private:
  struct Released {
    std::unique_ptr<ImTextureData> texture;
    int frames{0};
  };

  amc::vector<std::unique_ptr<ImTextureData>> _textures;
  amc::vector<Released> _released;
};

// Animations of the sprite sheet (assets/sprites/zalie-sprites.png, see tools/generate-assets.py).
enum class Animation : std::uint8_t { run, sleep, eat };

// Fonts and images of the app, embedded in the executable.
class Assets {
 public:
  // Loads the fonts into the atlas and the images into textures. Requires a Dear ImGui context.
  void load(TextureStore& textures);

  // Draws the frame of 'animation' at time 'seconds', with its top left corner at 'pos', 'height' pixels high, mirrored
  // horizontally if 'flipped' (running to the left). Returns the size drawn.
  ImVec2 drawSprite(ImDrawList* pDrawList, Animation animation, double seconds, ImVec2 pos, float height,
                    bool flipped = false) const;

  // Draws Zalie eating her piece of meat at time 'seconds', 'eaten' (in [0, 1]) of it gone, like drawSprite.
  ImVec2 drawMeal(ImDrawList* pDrawList, double seconds, float eaten, ImVec2 pos, float height) const;

  [[nodiscard]] ImTextureRef logo() const noexcept { return _pLogo == nullptr ? ImTextureRef{} : _pLogo->GetTexRef(); }
  [[nodiscard]] ImTextureRef logoSmall() const noexcept {
    return _pLogoSmall == nullptr ? ImTextureRef{} : _pLogoSmall->GetTexRef();
  }
  [[nodiscard]] ImTextureRef sprites() const noexcept {
    return _pSprites == nullptr ? ImTextureRef{} : _pSprites->GetTexRef();
  }
  [[nodiscard]] bool loaded() const noexcept { return _pSprites != nullptr; }

 private:
  void drawFrame(ImDrawList* pDrawList, int column, int row, ImVec2 pos, float height, bool flipped) const;

  ImTextureData* _pLogo{nullptr};
  ImTextureData* _pLogoSmall{nullptr};
  ImTextureData* _pSprites{nullptr};
};

}  // namespace zalie::ui
