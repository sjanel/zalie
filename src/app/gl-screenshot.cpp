#include "gl-screenshot.hpp"

// clang-format off
#include <GLFW/glfw3.h>  // GLFW_INCLUDE_NONE: the GL declarations come from the Dear ImGui loader
#include <imgui_impl_opengl3_loader.h>
// clang-format on
#include <stb_image_write.h>

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace zalie::app {

namespace {

constexpr GLenum kFramebuffer = 0x8D40;
constexpr GLenum kColorAttachment0 = 0x8CE0;
constexpr GLenum kFramebufferComplete = 0x8CD5;

using GenFramebuffers = void (*)(GLsizei, GLuint*);
using DeleteFramebuffers = void (*)(GLsizei, const GLuint*);
using BindFramebuffer = void (*)(GLenum, GLuint);
using FramebufferTexture2D = void (*)(GLenum, GLenum, GLenum, GLuint, GLint);
using CheckFramebufferStatus = GLenum (*)(GLenum);

template <class Fn>
Fn Load(const char* name) {
  return reinterpret_cast<Fn>(glfwGetProcAddress(name));
}

}  // namespace

OffscreenTarget::~OffscreenTarget() {
  if (_framebuffer != 0) {
    if (auto deleteFramebuffers = Load<DeleteFramebuffers>("glDeleteFramebuffers")) {
      deleteFramebuffers(1, &_framebuffer);
    }
  }
  if (_texture != 0) {
    glDeleteTextures(1, &_texture);
  }
}

bool OffscreenTarget::create(int width, int height) {
  const auto genFramebuffers = Load<GenFramebuffers>("glGenFramebuffers");
  const auto bindFramebuffer = Load<BindFramebuffer>("glBindFramebuffer");
  const auto framebufferTexture2D = Load<FramebufferTexture2D>("glFramebufferTexture2D");
  const auto checkStatus = Load<CheckFramebufferStatus>("glCheckFramebufferStatus");
  if (genFramebuffers == nullptr || bindFramebuffer == nullptr || framebufferTexture2D == nullptr ||
      checkStatus == nullptr) {
    return false;
  }
  _width = width;
  _height = height;
  glGenTextures(1, &_texture);
  glBindTexture(GL_TEXTURE_2D, _texture);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  genFramebuffers(1, &_framebuffer);
  bindFramebuffer(kFramebuffer, _framebuffer);
  framebufferTexture2D(kFramebuffer, kColorAttachment0, GL_TEXTURE_2D, _texture, 0);
  const bool complete = checkStatus(kFramebuffer) == kFramebufferComplete;
  bindFramebuffer(kFramebuffer, 0);
  return complete;
}

void OffscreenTarget::bind() const {
  if (const auto bindFramebuffer = Load<BindFramebuffer>("glBindFramebuffer")) {
    bindFramebuffer(kFramebuffer, _framebuffer);
  }
  glViewport(0, 0, _width, _height);
}

void OffscreenTarget::Unbind() {
  if (const auto bindFramebuffer = Load<BindFramebuffer>("glBindFramebuffer")) {
    bindFramebuffer(kFramebuffer, 0);
  }
}

bool OffscreenTarget::savePng(const std::filesystem::path& path) const {
  const auto stride = static_cast<std::size_t>(_width) * 4U;
  std::vector<unsigned char> pixels(stride * static_cast<std::size_t>(_height));
  bind();
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, _width, _height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
  Unbind();
  // OpenGL rows go bottom-up: write them flipped.
  stbi_flip_vertically_on_write(1);
  const std::string file = path.string();
  return stbi_write_png(file.c_str(), _width, _height, 4, pixels.data(), static_cast<int>(stride)) != 0;
}

}  // namespace zalie::app
