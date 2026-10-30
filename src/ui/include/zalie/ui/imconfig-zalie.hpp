#pragma once

// Dear ImGui configuration of Zalie (IMGUI_USER_CONFIG). Included by every Dear ImGui translation unit.

namespace zalie::ui {

// Called on a failed IM_ASSERT. The default handler prints the failure and aborts in debug builds (and ignores it with
// NDEBUG, like assert()); tests install a handler throwing an exception, so that misuses of the Dear ImGui API fail
// the test instead of the process.
using ImGuiAssertHandler = void (*)(const char* expression, const char* file, int line);

void SetImGuiAssertHandler(ImGuiAssertHandler handler) noexcept;

void ImGuiAssertFailed(const char* expression, const char* file, int line);

}  // namespace zalie::ui

#define IM_ASSERT(_EXPR) ((_EXPR) ? (void)0 : ::zalie::ui::ImGuiAssertFailed(#_EXPR, __FILE__, __LINE__))
