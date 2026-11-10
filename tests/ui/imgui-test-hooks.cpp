// Definitions of the hooks Dear ImGui calls when it is compiled with IMGUI_ENABLE_TEST_ENGINE. Part of the Dear ImGui
// library of a build with tests, the application included: it has no observer, and never asks for the hooks.

#include "imgui-test-hooks.hpp"

#include <imgui.h>
#include <imgui_internal.h>

namespace zalie::test {

namespace {

ImGuiItemObserver gObserver;

}  // namespace

void SetImGuiItemObserver(const ImGuiItemObserver& observer) noexcept { gObserver = observer; }

}  // namespace zalie::test

// NOLINTBEGIN(readability-identifier-naming): the names Dear ImGui declares in imgui_internal.h

void ImGuiTestEngineHook_ItemAdd(ImGuiContext* ctx, ImGuiID id, const ImRect& bb,
                                 const ImGuiLastItemData* /*item_data*/) {
  if (const auto& observer = zalie::test::gObserver; observer.itemAdded != nullptr) {
    observer.itemAdded(observer.pUserData, ctx, id, bb);
  }
}

void ImGuiTestEngineHook_ItemInfo(ImGuiContext* /*ctx*/, ImGuiID id, const char* label,
                                  ImGuiItemStatusFlags /*flags*/) {
  if (const auto& observer = zalie::test::gObserver; observer.itemLabeled != nullptr) {
    observer.itemLabeled(observer.pUserData, id, label);
  }
}

void ImGuiTestEngineHook_Log(ImGuiContext* /*ctx*/, const char* /*fmt*/, ...) {}

const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext* /*ctx*/, ImGuiID /*id*/) { return nullptr; }

// NOLINTEND(readability-identifier-naming)
