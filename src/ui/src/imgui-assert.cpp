#include <atomic>
#include <cstdio>
#include <cstdlib>

#include "zalie/ui/imconfig-zalie.hpp"

namespace zalie::ui {

namespace {

void DefaultHandler(const char* expression, const char* file, int line) {
  std::fprintf(stderr, "Dear ImGui assertion failed: %s (%s:%d)\n", expression, file, line);
#ifndef NDEBUG
  std::abort();
#endif
}

std::atomic<ImGuiAssertHandler> gHandler{DefaultHandler};

}  // namespace

void SetImGuiAssertHandler(ImGuiAssertHandler handler) noexcept {
  gHandler.store(handler == nullptr ? DefaultHandler : handler);
}

void ImGuiAssertFailed(const char* expression, const char* file, int line) { gHandler.load()(expression, file, line); }

}  // namespace zalie::ui
