#pragma once

// The hooks of the test engine of Dear ImGui (IMGUI_ENABLE_TEST_ENGINE, defined when Zalie is built with its tests):
// Dear ImGui reports every item it lays out, with its identifier, its rectangle and, for the widgets having one, its
// label. The headless tests find a widget by its label with them, and click it.

#include <imgui.h>
#include <imgui_internal.h>

namespace zalie::test {

// Receives the items laid out by the contexts whose 'TestEngineHookItems' is set.
struct ImGuiItemObserver {
  void (*itemAdded)(void* pUserData, ImGuiContext* pContext, ImGuiID id, const ImRect& rect) = nullptr;
  void (*itemLabeled)(void* pUserData, ImGuiID id, const char* label) = nullptr;
  void* pUserData = nullptr;
};

// Installs the observer of the items (a default constructed one removes it).
void SetImGuiItemObserver(const ImGuiItemObserver& observer) noexcept;

}  // namespace zalie::test
