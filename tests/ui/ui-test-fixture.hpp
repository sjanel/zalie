#pragma once

// A headless Dear ImGui context (null platform and renderer backends) for the UI tests.

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_impl_null.h>
#include <imgui_internal.h>

#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "imgui-test-hooks.hpp"
#include "zalie/ui/imconfig-zalie.hpp"

namespace zalie::test {

class ImGuiContextFixture : public ::testing::Test {
 protected:
  // An item (button, field, menu entry, tab...) laid out by the last frame.
  struct UiItem {
    // Whether a part of it is on screen, where the mouse can reach it.
    [[nodiscard]] bool isVisible() const noexcept {
      return visible.Min.x < visible.Max.x && visible.Min.y < visible.Max.y;
    }

    ImGuiID id;
    // Its rectangle, clipped by its window.
    ImRect visible;
    // The label given to Dear ImGui, with its "##identifier" part. Empty for the items without label.
    std::string label;
  };

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
    // The items of each frame are recorded, to find them by their label (imgui-test-hooks.hpp).
    ImGui::GetCurrentContext()->TestEngineHookItems = true;
    SetImGuiItemObserver({.itemAdded =
                              [](void* pUserData, ImGuiContext* pContext, ImGuiID id, const ImRect& rect) {
                                static_cast<ImGuiContextFixture*>(pUserData)->onItemAdded(*pContext, id, rect);
                              },
                          .itemLabeled =
                              [](void* pUserData, ImGuiID id, const char* label) {
                                static_cast<ImGuiContextFixture*>(pUserData)->onItemLabeled(id, label);
                              },
                          .pUserData = this});
  }

 public:
  ImGuiContextFixture(const ImGuiContextFixture&) = delete;
  ImGuiContextFixture& operator=(const ImGuiContextFixture&) = delete;

 protected:
  ~ImGuiContextFixture() override {
    SetImGuiItemObserver({});
    ImGui_ImplNull_Shutdown();
    ImGui::DestroyContext();
    ui::SetImGuiAssertHandler(nullptr);
  }

  // Runs a Dear ImGui frame around 'body', then "renders" it (texture requests acknowledged by the null renderer).
  template <class Body>
  void runFrame(Body&& body) {
    _items.clear();
    _earlyLabels.clear();
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

  // A frame of the sprite sheet (assets/sprites/zalie-sprites.png): its column and its row.
  struct SpriteFrame {
    bool operator==(const SpriteFrame&) const = default;

    int column;
    int row;
  };

  // The frames of the sprite sheet 'sprites' drawn (not mirrored) by the last rendered frame, in drawing order.
  [[nodiscard]] static std::vector<SpriteFrame> LastFrameSprites(ImTextureRef sprites) {
    static constexpr float kFrameWidth = 320.0F;
    static constexpr float kFrameHeight = 240.0F;
    std::vector<SpriteFrame> frames;
    const ImDrawData* pDrawData = ImGui::GetDrawData();
    if (pDrawData == nullptr || sprites._TexData == nullptr) {
      return frames;
    }
    const float columns = static_cast<float>(sprites._TexData->Width) / kFrameWidth;
    const float rows = static_cast<float>(sprites._TexData->Height) / kFrameHeight;
    for (const ImDrawList* pDrawList : pDrawData->CmdLists) {
      for (const ImDrawCmd& cmd : pDrawList->CmdBuffer) {
        if (cmd.TexRef._TexData != sprites._TexData) {
          continue;
        }
        // An image is a quad of 6 indices, the first one on its top left corner (the first texture coordinates).
        for (unsigned idx = 0; idx < cmd.ElemCount; idx += 6) {
          const ImDrawVert& corner = pDrawList->VtxBuffer[static_cast<int>(
              cmd.VtxOffset + pDrawList->IdxBuffer[static_cast<int>(cmd.IdxOffset + idx)])];
          frames.push_back({static_cast<int>(std::lround(corner.uv.x * columns)),
                            static_cast<int>(std::lround(corner.uv.y * rows))});
        }
      }
    }
    return frames;
  }

  // Number of vertices of the last rendered frame at a NaN or infinite position: a coordinate the UI failed to map.
  [[nodiscard]] static int LastFrameNonFiniteVertices() {
    const ImDrawData* pDrawData = ImGui::GetDrawData();
    int count = 0;
    if (pDrawData != nullptr) {
      for (const ImDrawList* pDrawList : pDrawData->CmdLists) {
        for (const ImDrawVert& vertex : pDrawList->VtxBuffer) {
          count += static_cast<int>(!std::isfinite(vertex.pos.x) || !std::isfinite(vertex.pos.y));
        }
      }
    }
    return count;
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
  static void MouseButton(bool down, ImGuiMouseButton button = ImGuiMouseButton_Left) {
    ImGui::GetIO().AddMouseButtonEvent(button, down);
  }

  // The item of the last frame labeled 'label' (the label given to Dear ImGui, its "##identifier" part included):
  // the 'index'-th one when several items have this label. nullptr if there is none.
  [[nodiscard]] const UiItem* findItem(std::string_view label, std::size_t index = 0) const noexcept {
    for (const UiItem& item : _items) {
      if (item.label == label && index-- == 0) {
        return &item;
      }
    }
    return nullptr;
  }

  // Moves the mouse over an item of the last frame (see findItem) and renders a frame with 'frame': the item is
  // hovered. Returns false, without rendering, when the item does not exist or is not on screen.
  template <class Frame>
  [[nodiscard]] bool hoverItem(std::string_view label, Frame&& frame, std::size_t index = 0) {
    const UiItem* pItem = findItem(label, index);
    if (pItem == nullptr && !isWatched(label)) {
      // Some widgets do not report their label (a combo box). The identifier of an item is the hash of its label:
      // the label is watched from now on, and recognized while the items of the next frames are laid out.
      _watchedLabels.emplace_back(label);
      frame();
      pItem = findItem(label, index);
    }
    if (pItem == nullptr || !pItem->isVisible()) {
      return false;
    }
    MouseMove(pItem->visible.GetCenter());
    frame();
    return true;
  }

  // Clicks where the mouse is, as the user does: the button goes down then up, one frame each.
  template <class Frame>
  void clickHere(Frame&& frame, ImGuiMouseButton button = ImGuiMouseButton_Left) {
    MouseButton(true, button);
    frame();
    MouseButton(false, button);
    frame();
    // What the click opened: a popup is laid out at its second frame, the tabs of a tab bar too.
    frame();
    frame();
  }

  // Clicks an item of the last frame (see hoverItem).
  template <class Frame>
  [[nodiscard]] bool clickItem(std::string_view label, Frame&& frame, ImGuiMouseButton button = ImGuiMouseButton_Left,
                               std::size_t index = 0) {
    if (!hoverItem(label, frame, index)) {
      return false;
    }
    clickHere(frame, button);
    return true;
  }

  // Drags the item 'from' and drops it on the item 'to' (see hoverItem).
  template <class Frame>
  [[nodiscard]] bool dragItem(std::string_view from, std::string_view to, Frame&& frame, std::size_t fromIndex = 0,
                              std::size_t toIndex = 0) {
    if (!hoverItem(from, frame, fromIndex)) {
      return false;
    }
    MouseButton(true);
    frame();
    // The drag starts once the mouse has moved farther than the drag threshold of Dear ImGui.
    const ImVec2 start = ImGui::GetIO().MousePos;
    MouseMove(ImVec2(start.x + 12.0F, start.y + 12.0F));
    frame();
    const bool onTarget = hoverItem(to, frame, toIndex);
    if (onTarget) {
      frame();
    }
    MouseButton(false);
    frame();
    frame();
    return onTarget;
  }

 private:
  struct EarlyLabel {
    ImGuiID id;
    std::string label;
  };

  [[nodiscard]] bool isWatched(std::string_view label) const noexcept {
    for (const std::string& watched : _watchedLabels) {
      if (watched == label) {
        return true;
      }
    }
    return false;
  }

  void onItemAdded(const ImGuiContext& context, ImGuiID id, const ImRect& rect) {
    const ImGuiWindow& window = *context.CurrentWindow;
    ImRect visible = rect;
    visible.ClipWith(window.ClipRect);
    UiItem& item = _items.emplace_back(UiItem{.id = id, .visible = visible, .label = {}});
    if (window.IDStack.empty()) {
      return;  // the window itself
    }
    for (const std::string& watched : _watchedLabels) {
      if (ImHashStr(watched.data(), watched.size(), window.IDStack.back()) == id) {
        item.label = watched;
      }
    }
    for (EarlyLabel& early : _earlyLabels) {
      if (early.id == id) {
        item.label = std::move(early.label);
        early.id = 0;
      }
    }
  }

  void onItemLabeled(ImGuiID id, const char* label) {
    // The label of an item follows it, right away or after its children (a tree node)...
    for (auto it = _items.rbegin(); it != _items.rend(); ++it) {
      if (it->id == id) {
        it->label = label;
        return;
      }
    }
    // ... or comes first (the tabs of a tab bar).
    _earlyLabels.push_back({.id = id, .label = label});
  }

  std::string _clipboard;
  std::vector<UiItem> _items;
  std::vector<EarlyLabel> _earlyLabels;
  std::vector<std::string> _watchedLabels;
};

}  // namespace zalie::test
