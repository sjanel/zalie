#pragma once

// A headless Dear ImGui context (null platform and renderer backends) for the UI tests.

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_impl_null.h>

#include <string>
#include <string_view>

#include "zalie/ui/imconfig-zalie.hpp"

namespace zalie::test {

class ImGuiContextFixture : public ::testing::Test {
 protected:
  ImGuiContextFixture() {
    // A Dear ImGui assertion is a misuse of its API by the UI code: fail the test (and keep going, so that the context
    // can still be destroyed cleanly).
    ui::SetImGuiAssertHandler([](const char* expression, const char* file, int line) {
      ADD_FAILURE_AT(file, line) << "Dear ImGui assertion failed: " << expression;
    });
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    ImGui_ImplNull_Init();
    // An in-memory clipboard: the default one of Dear ImGui is the system clipboard on macOS and Windows, shared with
    // the user and with the tests running in parallel.
    ImGuiPlatformIO& platformIo = ImGui::GetPlatformIO();
    platformIo.Platform_ClipboardUserData = &_clipboard;
    platformIo.Platform_GetClipboardTextFn = [](ImGuiContext* /*pContext*/) -> const char* {
      return static_cast<const std::string*>(ImGui::GetPlatformIO().Platform_ClipboardUserData)->c_str();
    };
    platformIo.Platform_SetClipboardTextFn = [](ImGuiContext* /*pContext*/, const char* pText) {
      static_cast<std::string*>(ImGui::GetPlatformIO().Platform_ClipboardUserData)->assign(pText);
    };
  }

 public:
  ImGuiContextFixture(const ImGuiContextFixture&) = delete;
  ImGuiContextFixture& operator=(const ImGuiContextFixture&) = delete;

 protected:
  ~ImGuiContextFixture() override {
    ImGui_ImplNull_Shutdown();
    ImGui::DestroyContext();
    ui::SetImGuiAssertHandler(nullptr);
  }

  // Runs a Dear ImGui frame around 'body', then "renders" it (texture requests acknowledged by the null renderer).
  template <class Body>
  void runFrame(Body&& body) {
    ImGui_ImplNull_NewFrame();
    ImGui::NewFrame();
    body();
    ImGui::Render();
    ImGui_ImplNullRender_RenderDrawData(ImGui::GetDrawData());
  }

  // Number of vertices of the last rendered frame.
  [[nodiscard]] static int LastFrameVertices() {
    const ImDrawData* pDrawData = ImGui::GetDrawData();
    return pDrawData == nullptr ? 0 : pDrawData->TotalVtxCount;
  }

  // Input helpers (applied at the next frame).
  // The Ctrl of a chord is pressed as the user does: Cmd on macOS, that Dear ImGui swaps with Ctrl
  // (io.ConfigMacOSXBehaviors) for the shortcuts written with ImGuiMod_Ctrl.
  static void Key(ImGuiKeyChord chord, bool down) {
    ImGuiIO& io = ImGui::GetIO();
    if ((chord & ImGuiMod_Ctrl) != 0) {
      io.AddKeyEvent(io.ConfigMacOSXBehaviors ? ImGuiMod_Super : ImGuiMod_Ctrl, down);
    }
    if ((chord & ImGuiMod_Shift) != 0) {
      io.AddKeyEvent(ImGuiMod_Shift, down);
    }
    io.AddKeyEvent(static_cast<ImGuiKey>(chord & ~ImGuiMod_Mask_), down);
  }

  static void Type(std::string_view text) {
    for (char ch : text) {
      ImGui::GetIO().AddInputCharacter(static_cast<unsigned char>(ch));
    }
  }

  static void MouseMove(ImVec2 pos) { ImGui::GetIO().AddMousePosEvent(pos.x, pos.y); }
  static void MouseButton(bool down) { ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, down); }

 private:
  std::string _clipboard;
};

}  // namespace zalie::test
