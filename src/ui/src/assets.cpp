#include "zalie/ui/assets.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <stb_image.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <utility>

#include "zalie/ui/embedded.hpp"
#include "zalie/ui/widgets.hpp"

namespace zalie::ui {

namespace {

// Sprite sheet layout: frames of 320x240 (160x120 at 2x). Row 0 = 8 run frames, row 1 = 4 sleep frames, row 2 = 4 eat
// frames then the 4 stages of the meat, drawn behind them.
constexpr float kFrameWidth = 320.0F;
constexpr float kFrameHeight = 240.0F;

struct SpriteAnimation {
  double fps;
  int row;
  int column;
  int nbFrames;
};

// Indexed by Animation. The eating dog chomps at the pace of the snapshots of a run (about 10 per second), which are
// the only redraws during a run.
constexpr std::array<SpriteAnimation, 3> kAnimations{{{14.0, 0, 0, 8}, {2.5, 1, 0, 4}, {10.0, 2, 0, 4}}};
// The stages of the meat, from whole to a bare bone: picked by how much is eaten, not by time.
constexpr SpriteAnimation kMeat{0.0, 2, 4, 4};
// The frame of the eat animation where the jaws close on the meat.
constexpr int kBiteFrame = 1;

int FrameAt(const SpriteAnimation& animation, double seconds) {
  return static_cast<int>(std::floor(seconds * animation.fps)) % animation.nbFrames;
}

ImFont* AddEmbeddedFont(std::span<const unsigned char> ttf, float size) {
  ImFontConfig config;
  config.FontDataOwnedByAtlas = false;  // static storage
  config.OversampleH = 2;
  config.OversampleV = 2;
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast) - the data is only read when FontDataOwnedByAtlas is false
  void* pData = const_cast<unsigned char*>(ttf.data());
  return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(pData, static_cast<int>(ttf.size()), size, &config);
}

}  // namespace

TextureStore::~TextureStore() {
  if (ImGui::GetCurrentContext() == nullptr) {
    return;
  }
  for (auto& pTexture : _textures) {
    ImGui::UnregisterUserTexture(pTexture.get());
  }
  for (Released& released : _released) {
    ImGui::UnregisterUserTexture(released.texture.get());
  }
}

ImTextureData* TextureStore::createFromImage(std::span<const unsigned char> encoded) {
  int width = 0;
  int height = 0;
  int channels = 0;
  unsigned char* pPixels =
      stbi_load_from_memory(encoded.data(), static_cast<int>(encoded.size()), &width, &height, &channels, 4);
  if (pPixels == nullptr) {
    return nullptr;
  }
  auto pTexture = std::make_unique<ImTextureData>();
  pTexture->Create(ImTextureFormat_RGBA32, width, height);
  std::memcpy(pTexture->GetPixels(), pPixels, static_cast<std::size_t>(pTexture->GetSizeInBytes()));
  stbi_image_free(pPixels);
  pTexture->UseColors = true;
  pTexture->SetStatus(ImTextureStatus_WantCreate);
  ImGui::RegisterUserTexture(pTexture.get());
  return _textures.emplace_back(std::move(pTexture)).get();
}

void TextureStore::release(ImTextureData* pTexture) {
  for (auto it = _textures.begin(); it != _textures.end(); ++it) {
    if (it->get() == pTexture) {
      pTexture->WantDestroyNextFrame = true;
      pTexture->SetStatus(ImTextureStatus_WantDestroy);
      _released.push_back({std::move(*it), 0});
      _textures.erase(it);
      return;
    }
  }
}

void TextureStore::collect() {
  static constexpr int kMaxFrames = 8;  // some backends (null) never acknowledge the destruction
  for (std::uint32_t i = 0; i < _released.size();) {
    Released& released = _released[i];
    ++released.frames;
    if (released.texture->Status == ImTextureStatus_Destroyed || released.frames > kMaxFrames) {
      ImGui::UnregisterUserTexture(released.texture.get());
      _released.erase(_released.begin() + static_cast<std::ptrdiff_t>(i));
    } else {
      ++i;
    }
  }
}

void Assets::load(TextureStore& textures) {
  Fonts& fonts = GetFonts();
  fonts.pUi = AddEmbeddedFont(embedded::FontUi(), Fonts::kUiSize);
  fonts.pMono = AddEmbeddedFont(embedded::FontMono(), Fonts::kMonoSize);
  ImGui::GetIO().FontDefault = fonts.pUi;
  ImGui::GetStyle().FontSizeBase = Fonts::kUiSize;

  _pLogo = textures.createFromImage(embedded::LogoPng());
  _pLogoSmall = textures.createFromImage(embedded::LogoSmallPng());
  _pSprites = textures.createFromImage(embedded::SpritesPng());
}

void Assets::drawFrame(ImDrawList* pDrawList, int column, int row, ImVec2 pos, float height, bool flipped) const {
  const float width = height * kFrameWidth / kFrameHeight;
  const float sheetWidth = static_cast<float>(_pSprites->Width);
  const float sheetHeight = static_cast<float>(_pSprites->Height);
  ImVec2 uv0(static_cast<float>(column) * kFrameWidth / sheetWidth,
             static_cast<float>(row) * kFrameHeight / sheetHeight);
  ImVec2 uv1(uv0.x + (kFrameWidth / sheetWidth), uv0.y + (kFrameHeight / sheetHeight));
  if (flipped) {
    std::swap(uv0.x, uv1.x);
  }
  pDrawList->AddImage(_pSprites->GetTexRef(), pos, ImVec2(pos.x + width, pos.y + height), uv0, uv1);
}

ImVec2 Assets::drawSprite(ImDrawList* pDrawList, Animation animation, double seconds, ImVec2 pos, float height,
                          bool flipped) const {
  const ImVec2 size(height * kFrameWidth / kFrameHeight, height);
  if (_pSprites == nullptr) {
    return size;
  }
  const SpriteAnimation& layout = kAnimations[static_cast<std::size_t>(animation)];
  drawFrame(pDrawList, layout.column + FrameAt(layout, seconds), layout.row, pos, height, flipped);
  return size;
}

ImVec2 Assets::drawMeal(ImDrawList* pDrawList, double seconds, float eaten, ImVec2 pos, float height) const {
  const ImVec2 size(height * kFrameWidth / kFrameHeight, height);
  if (_pSprites == nullptr) {
    return size;
  }
  const SpriteAnimation& eat = kAnimations[static_cast<std::size_t>(Animation::eat)];
  const int frame = FrameAt(eat, seconds);
  const int stage = std::clamp(static_cast<int>(eaten * static_cast<float>(kMeat.nbFrames)), 0, kMeat.nbFrames - 1);
  // The meat gives a little under the bite.
  const float squash = frame == kBiteFrame ? std::max(1.0F, std::round(height / 120.0F)) : 0.0F;
  drawFrame(pDrawList, kMeat.column + stage, kMeat.row, ImVec2(pos.x, pos.y + squash), height, false);
  drawFrame(pDrawList, eat.column + frame, eat.row, pos, height, false);
  return size;
}

}  // namespace zalie::ui
