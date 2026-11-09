#pragma once

#include <filesystem>

namespace zalie::app {

// An offscreen OpenGL framebuffer, to render a frame of a hidden window and save it as PNG (--screenshot).
// The framebuffer object functions are loaded through GLFW (the Dear ImGui loader only has what its backend uses).
class OffscreenTarget {
 public:
  OffscreenTarget() = default;

  OffscreenTarget(OffscreenTarget&&) noexcept = delete;
  OffscreenTarget(const OffscreenTarget&) = delete;
  OffscreenTarget& operator=(const OffscreenTarget&) = delete;
  OffscreenTarget& operator=(OffscreenTarget&&) noexcept = delete;

  ~OffscreenTarget();

  // Creates the target. Returns false if framebuffer objects are not available.
  bool create(int width, int height);

  // Renders into the target (bind) or into the window again (unbind).
  void bind() const;
  static void Unbind();

  // Reads the target and writes it as a PNG file. Returns false on failure.
  [[nodiscard]] bool savePng(const std::filesystem::path& path) const;

 private:
  unsigned int _framebuffer{0};
  unsigned int _texture{0};
  int _width{0};
  int _height{0};
};

}  // namespace zalie::app
