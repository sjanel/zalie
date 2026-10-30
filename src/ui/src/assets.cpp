#include "zalie/ui/assets.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <stb_image.h>

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

// Sprite sheet layout: frames of 320x240 (160x120 at 2x), row 0 = 8 run frames, row 1 = 4 sleep frames.
constexpr float kFrameWidth = 320.0F;
constexpr float kFrameHeight = 240.0F;
constexpr int kRunFrames = 8;
constexpr int kSleepFrames = 4;
constexpr double kRunFps = 14.0;
constexpr double kSleepFps = 2.5;

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

ImVec2 Assets::drawSprite(ImDrawList* pDrawList, Animation animation, double seconds, ImVec2 pos, float height,
                          bool flipped) const {
  const float width = height * kFrameWidth / kFrameHeight;
  if (_pSprites == nullptr) {
    return {width, height};
  }
  const bool run = animation == Animation::run;
  const int nbFrames = run ? kRunFrames : kSleepFrames;
  const int frame = static_cast<int>(std::floor(seconds * (run ? kRunFps : kSleepFps))) % nbFrames;
  const float sheetWidth = static_cast<float>(_pSprites->Width);
  const float sheetHeight = static_cast<float>(_pSprites->Height);
  const float row = run ? 0.0F : 1.0F;
  ImVec2 uv0(static_cast<float>(frame) * kFrameWidth / sheetWidth, row * kFrameHeight / sheetHeight);
  ImVec2 uv1(uv0.x + (kFrameWidth / sheetWidth), uv0.y + (kFrameHeight / sheetHeight));
  if (flipped) {
    std::swap(uv0.x, uv1.x);
  }
  pDrawList->AddImage(_pSprites->GetTexRef(), pos, ImVec2(pos.x + width, pos.y + height), uv0, uv1);
  return {width, height};
}

}  // namespace zalie::ui
