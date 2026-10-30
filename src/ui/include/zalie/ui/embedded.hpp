#pragma once

#include <span>

// Binary assets compiled into the executable (see cmake/EmbedFiles.cmake).
namespace zalie::embedded {

std::span<const unsigned char> FontUi() noexcept;
std::span<const unsigned char> FontMono() noexcept;
std::span<const unsigned char> LogoPng() noexcept;
std::span<const unsigned char> LogoSmallPng() noexcept;
std::span<const unsigned char> SpritesPng() noexcept;

}  // namespace zalie::embedded
