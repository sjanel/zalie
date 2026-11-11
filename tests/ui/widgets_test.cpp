// Tests of the custom widgets, rendered headless.

#include "zalie/ui/widgets.hpp"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ui-test-fixture.hpp"
#include "zalie/exchange.hpp"
#include "zalie/key-value.hpp"
#include "zalie/method.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/syntax.hpp"
#include "zalie/text-document.hpp"
#include "zalie/ui/assets.hpp"
#include "zalie/ui/chart.hpp"
#include "zalie/ui/embedded.hpp"
#include "zalie/ui/icons.hpp"
#include "zalie/ui/text-view.hpp"
#include "zalie/ui/theme.hpp"

namespace zalie {
namespace {

using namespace std::chrono_literals;

class WidgetsTest : public test::ImGuiContextFixture {
 protected:
  WidgetsTest() {
    assets.load(textures);
    ui::ApplyTheme(ui::ThemeKind::coton, 1.0F);
  }

  ~WidgetsTest() override = default;

  // Runs a frame with 'body' inside a full-screen window.
  template <class Body>
  void windowFrame(Body&& body) {
    runFrame([&] {
      ImGui::SetNextWindowPos(ImVec2(0, 0));
      ImGui::SetNextWindowSize(ImVec2(1000, 800));
      ImGui::Begin("test", nullptr, ImGuiWindowFlags_NoDecoration);
      body();
      ImGui::End();
    });
  }

  ui::TextureStore textures;
  ui::Assets assets;
};

TEST(Formatting, BytesDurationsClock) {
  std::array<char, 48> buffer{};
  EXPECT_EQ(ui::FormatBytes(0, buffer), "0 B");
  EXPECT_EQ(ui::FormatBytes(1023, buffer), "1023 B");
  EXPECT_EQ(ui::FormatBytes(1536, buffer), "1.5 KB");
  EXPECT_EQ(ui::FormatBytes(std::uint64_t{5} * 1024 * 1024, buffer), "5.0 MB");
  EXPECT_EQ(ui::FormatBytes(3ULL << 30U, buffer), "3.00 GB");
  EXPECT_EQ(ui::FormatDuration(850us, buffer), "850 \xC2\xB5s");
  EXPECT_EQ(ui::FormatDuration(std::chrono::microseconds{3200}, buffer), "3.2 ms");
  EXPECT_EQ(ui::FormatDuration(123ms, buffer), "123 ms");
  EXPECT_EQ(ui::FormatDuration(1240ms, buffer), "1.24 s");
  EXPECT_EQ(ui::FormatDuration(125s, buffer), "2 min 5 s");
  const auto nowMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
          .count();
  EXPECT_EQ(ui::FormatAgo(nowMs, buffer), "just now");
  EXPECT_EQ(ui::FormatAgo(nowMs - 30000, buffer), "30 s ago");
  EXPECT_EQ(ui::FormatAgo(nowMs - 300000, buffer), "5 min ago");
  EXPECT_EQ(ui::FormatAgo(nowMs - (std::int64_t{3} * 3600000), buffer), "3 h ago");
  EXPECT_EQ(ui::FormatAgo(nowMs - (std::int64_t{2} * 86400000), buffer), "2 d ago");
  EXPECT_EQ(ui::FormatClock(std::chrono::system_clock::now(), buffer).size(), 12U);  // HH:MM:SS.mmm
  std::array<char, 4> tiny{};
  EXPECT_EQ(ui::FormatBytes(123456, tiny).size(), 4U);  // truncated, never overflowing
}

TEST(Theme, Palettes) {
  EXPECT_EQ(ui::ParseTheme("nuit"), ui::ThemeKind::nuit);
  EXPECT_EQ(ui::ParseTheme("anything else"), ui::ThemeKind::coton);
  EXPECT_EQ(ui::ThemeId(ui::ThemeKind::nuit), "nuit");
  EXPECT_EQ(ui::WithAlpha(IM_COL32(10, 20, 30, 200), 0.5F), IM_COL32(10, 20, 30, 100));
  EXPECT_NE(ui::StatusColor(200, ExchangeState::completed), ui::StatusColor(500, ExchangeState::completed));
  EXPECT_EQ(ui::StatusColor(200, ExchangeState::failed), ui::StatusColor(503, ExchangeState::completed));
  for (Method method : kMethods) {
    EXPECT_NE(ui::MethodColor(method), 0U);
  }
}

TEST_F(WidgetsTest, InputTextGrowsRawChars) {
  RawChars32 text;
  bool edited = false;
  windowFrame([&] {
    ImGui::SetKeyboardFocusHere();
    ui::InputText("##text", text);
  });
  windowFrame([&] { ui::InputText("##text", text); });  // activated
  const std::string longText(300, 'z');
  Type(longText);
  windowFrame([&] { edited = ui::InputText("##text", text); });
  EXPECT_TRUE(edited);
  EXPECT_EQ(View(text), longText);
  // Erasing shrinks it again (the size follows the edit, the capacity stays).
  for (int i = 0; i < 3; ++i) {
    Key(ImGuiKey_Backspace, true);
    windowFrame([&] { ui::InputText("##text", text); });
    Key(ImGuiKey_Backspace, false);
    windowFrame([&] { ui::InputText("##text", text); });
  }
  EXPECT_EQ(text.size(), 297U);
}

TEST_F(WidgetsTest, KeyValueTableAddsRowsFromTheDraft) {
  KeyValues rows;
  bool modified = false;
  const auto draw = [&] { modified = ui::KeyValueTable("##table", rows, {.maskValues = true}); };
  windowFrame(draw);
  EXPECT_FALSE(modified);
  // Focus the key input of the draft row (the first focusable item of an empty table), then type in it.
  windowFrame([&] {
    ImGui::SetKeyboardFocusHere();
    draw();
  });
  windowFrame(draw);
  Type("new-key");
  windowFrame(draw);
  EXPECT_TRUE(modified);
  windowFrame(draw);
  ASSERT_EQ(rows.size(), 1U);
  EXPECT_EQ(View(rows[0].key), "new-key");
  EXPECT_TRUE(rows[0].enabled);
  // Rows render with their checkbox and delete button (masked values included).
  rows.push_back(MakeKeyValue("token", "s3cr3t", false));
  windowFrame(draw);
  windowFrame(draw);
  EXPECT_EQ(rows.size(), 2U);
}

TEST_F(WidgetsTest, TextViewSelectsCopiesAndFinds) {
  const std::string text = "first line\nsecond {{variable}} line\nthird line with Zalie\n";
  LineIndex lines;
  lines.build(text, SyntaxLanguage::plain);
  ui::TextViewState state;
  const auto draw = [&] {
    ui::TextView("##view", text, lines, state, {.language = SyntaxLanguage::plain, .size = ImVec2(600, 300)});
  };
  windowFrame(draw);
  // Click on the second line, shift-click on the third.
  const float lineHeight = std::floor(ui::Fonts::kMonoSize * 1.42F);
  MouseMove(ImVec2(100, 8 + (lineHeight * 1.5F)));
  windowFrame(draw);
  MouseButton(true);
  windowFrame(draw);
  MouseButton(false);
  windowFrame(draw);
  EXPECT_EQ(state.selectionAnchor, 1U);
  EXPECT_EQ(state.selectionCursor, 1U);
  // Ctrl+C copies the selected line.
  Key(ImGuiMod_Ctrl | ImGuiKey_C, true);
  windowFrame(draw);
  Key(ImGuiMod_Ctrl | ImGuiKey_C, false);
  windowFrame(draw);
  EXPECT_STREQ(ImGui::GetClipboardText(), "second {{variable}} line");
  Key(ImGuiMod_Ctrl | ImGuiKey_A, true);
  windowFrame(draw);
  Key(ImGuiMod_Ctrl | ImGuiKey_A, false);
  EXPECT_EQ(state.selectionAnchor, 0U);
  EXPECT_EQ(state.selectionCursor, lines.size() - 1);

  // Find: case-insensitive, cycling.
  state.query.assign(std::string_view{"LINE"});
  EXPECT_EQ(ui::UpdateMatches(text, state), 3U);
  EXPECT_EQ(ui::UpdateMatches(text, state), 3U);  // cached
  ui::GoToMatch(state, true);
  EXPECT_EQ(state.currentMatch, 1U);
  ui::GoToMatch(state, false);
  ui::GoToMatch(state, false);
  EXPECT_EQ(state.currentMatch, 2U);
  windowFrame(draw);
  state.query.clear();
  EXPECT_EQ(ui::UpdateMatches(text, state), 0U);
  state.reset();
  EXPECT_EQ(state.selectionAnchor, ui::TextViewState::kNoLine);
}

TEST_F(WidgetsTest, TextViewDragsShiftClicksAndHasAContextMenu) {
  const std::string text = "zero\none\ntwo\nthree\nfour\n";
  LineIndex lines;
  lines.build(text, SyntaxLanguage::plain);
  ui::TextViewState state;
  const auto draw = [&] {
    ImGui::Dummy(ImVec2(10, 40));  // room above the view, for the mouse to leave it
    ui::TextView("##view", text, lines, state, {.language = SyntaxLanguage::plain, .size = ImVec2(600, 60)});
  };
  const auto frame = [&] { windowFrame(draw); };
  frame();
  const float lineHeight = std::floor(ui::Fonts::kMonoSize * 1.42F);
  const float top = ImGui::GetStyle().WindowPadding.y + 40.0F + ImGui::GetStyle().ItemSpacing.y;
  const auto lineY = [&](float line) { return top + (lineHeight * (line + 0.5F)); };
  // Pressed on the second line, dragged above the view (scrolls up, selects from the first line), then below it.
  MouseMove(ImVec2(100, lineY(1)));
  frame();
  MouseButton(true);
  frame();
  MouseMove(ImVec2(100, 5));
  frame();
  EXPECT_EQ(state.selectionCursor, 0U);
  MouseMove(ImVec2(100, 400));
  frame();
  frame();
  EXPECT_GT(state.selectionCursor, 1U);
  MouseButton(false);
  frame();
  EXPECT_FALSE(state.dragging);
  // Shift extends the selection from its anchor.
  ASSERT_EQ(state.selectionAnchor, 1U);
  MouseMove(ImVec2(100, lineY(0)));
  frame();
  ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, true);
  clickHere(frame);
  ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, false);
  frame();
  EXPECT_EQ(state.selectionAnchor, 1U);

  // The context menu of the view, opened over its first line.
  const auto contextMenu = [&](std::string_view entry) {
    MouseMove(ImVec2(100, lineY(0)));
    frame();
    clickHere(frame, ImGuiMouseButton_Right);
    return clickItem(entry, frame);
  };
  ASSERT_TRUE(contextMenu("Select all"));
  EXPECT_EQ(state.selectionAnchor, 0U);
  EXPECT_EQ(state.selectionCursor, lines.size() - 1);
  ASSERT_TRUE(contextMenu("Copy selected lines"));
  EXPECT_EQ(std::string_view(ImGui::GetClipboardText()), text);  // every line, the empty last one included
  ImGui::SetClipboardText("");
  ASSERT_TRUE(contextMenu("Copy all"));
  EXPECT_EQ(std::string_view(ImGui::GetClipboardText()), text);
}

TEST_F(WidgetsTest, TextViewScrollsToAMatchFarRight) {
  const std::string text = std::string(400, '.') + "needle\nshort\n";
  LineIndex lines;
  lines.build(text, SyntaxLanguage::plain);
  ui::TextViewState state;
  ui::GoToMatch(state, true);  // no match: nothing to go to
  EXPECT_FALSE(state.scrollToCurrentMatch);
  state.query.assign(std::string_view{"needle"});
  ASSERT_EQ(ui::UpdateMatches(text, state), 1U);
  ui::GoToMatch(state, true);
  const auto draw = [&] {
    ui::TextView("##long", text, lines, state, {.language = SyntaxLanguage::plain, .size = ImVec2(300, 100)});
  };
  windowFrame(draw);
  windowFrame(draw);
  float scrollX = 0;
  for (const ImGuiWindow* pWindow : ImGui::GetCurrentContext()->Windows) {
    if (std::string_view(pWindow->Name).contains("##long")) {
      scrollX = pWindow->Scroll.x;
    }
  }
  EXPECT_GT(scrollX, 0.0F);
  EXPECT_FALSE(state.scrollToCurrentMatch);
}

TEST_F(WidgetsTest, ATextViewOutOfSightDrawsNothing) {
  const std::string text = "line\n";
  LineIndex lines;
  lines.build(text, SyntaxLanguage::plain);
  ui::TextViewState state;
  const std::string empty;
  LineIndex noLines;
  noLines.build(empty, SyntaxLanguage::plain);
  for (int idx = 0; idx < 3; ++idx) {
    windowFrame([&] {
      ui::TextView("##empty", empty, noLines, state, {.language = SyntaxLanguage::plain, .size = ImVec2(300, 50)});
      ImGui::SetCursorPosY(5000.0F);
      ui::TextView("##below", text, lines, state, {.language = SyntaxLanguage::plain, .size = ImVec2(300, 50)});
    });
  }
  EXPECT_GT(LastFrameVertices(), 0);
}

TEST_F(WidgetsTest, AHelpMarkerExplainsOnHover) {
  ImRect marker;
  const auto frame = [&] {
    windowFrame([&] {
      ImGui::TextUnformatted("Option");
      ui::HelpMarker("What the option does");
      marker = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    });
  };
  frame();
  MouseMove(marker.GetCenter());
  for (int idx = 0; idx < 45; ++idx) {
    frame();
  }
  bool tooltip = false;
  for (const ImGuiWindow* pWindow : ImGui::GetCurrentContext()->Windows) {
    tooltip |= pWindow->Active && std::string_view(pWindow->Name).contains("##Tooltip");
  }
  EXPECT_TRUE(tooltip);
}

TEST(Widgets, MethodShortNames) {
  EXPECT_EQ(ui::MethodShortName(Method::get), "GET");
  EXPECT_EQ(ui::MethodShortName(Method::del), "DEL");
  EXPECT_EQ(ui::MethodShortName(Method::options), "OPT");
  EXPECT_EQ(ui::MethodShortName(Method::connect), "CONN");
}

TEST_F(WidgetsTest, MiscWidgetsRender) {
  Method method = Method::get;
  float size = 200;
  windowFrame([&] {
    ui::MethodCombo("##method", method, 100);
    ui::MethodLabel(Method::del);
    ui::StatusPill(201, "Created", ExchangeState::completed);
    ui::StatusPill(0, "", ExchangeState::failed);
    ui::StatusPill(0, "", ExchangeState::cancelled);
    ui::Splitter("##split", true, 6, size, 100, 400, 300);
    ui::SectionTitle("TITLE");
    ui::HelpMarker("help");
    ui::PrimaryButton("Primary");
    ui::TextDim("dim");
    ui::HexView("##hex", std::string_view("\x00\x01hello world, binary\xff", 22), ImVec2(600, 100));
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    for (int icon = 0; icon <= static_cast<int>(ui::Icon::pawPrints); ++icon) {
      ui::DrawIcon(ImGui::GetWindowDrawList(), static_cast<ui::Icon>(icon),
                   ImVec2(pos.x + (20.0F * static_cast<float>(icon)), pos.y), 16, IM_COL32_WHITE);
    }
    ui::IconButton("##icon", ui::Icon::gear, "tooltip", true);
    assets.drawSprite(ImGui::GetWindowDrawList(), ui::Animation::run, 1.23, pos, 60, true);
    assets.drawSprite(ImGui::GetWindowDrawList(), ui::Animation::sleep, 0.5, pos, 60);
  });
  EXPECT_GT(LastFrameVertices(), 500);
  EXPECT_TRUE(assets.loaded());
  ui::ApplyTheme(ui::ThemeKind::nuit, 2.0F);
  EXPECT_EQ(ui::CurrentPalette().kind, ui::ThemeKind::nuit);
  EXPECT_FLOAT_EQ(ImGui::GetStyle().FontScaleDpi, 2.0F);
}

TEST_F(WidgetsTest, ZalieEatsHerMeatStageByStage) {
  // Row 2 of the sheet: the 4 frames of the eating dog, then the 4 stages of the meat (drawn first, behind her).
  const auto meal = [&](double seconds, float eaten) {
    windowFrame([&] { assets.drawMeal(ImGui::GetWindowDrawList(), seconds, eaten, ImVec2(10, 10), 72); });
    return LastFrameSprites(assets.sprites());
  };
  using Frames = std::vector<SpriteFrame>;
  EXPECT_EQ(meal(0.0, 0.0F), (Frames{{4, 2}, {0, 2}}));
  EXPECT_EQ(meal(0.1, 0.3F), (Frames{{5, 2}, {1, 2}}));  // 10 frames per second
  EXPECT_EQ(meal(0.25, 0.6F), (Frames{{6, 2}, {2, 2}}));
  EXPECT_EQ(meal(0.35, 0.99F), (Frames{{7, 2}, {3, 2}}));
  EXPECT_EQ(meal(0.4, 1.0F), (Frames{{7, 2}, {0, 2}}));  // a bare bone, and the cycle starts again
  EXPECT_EQ(meal(0.0, -1.0F), (Frames{{4, 2}, {0, 2}}));

  // The meat gives a little under the bite (frame 1 of the dog), not otherwise.
  const auto meatTop = [&](double seconds) {
    windowFrame([&] { assets.drawMeal(ImGui::GetWindowDrawList(), seconds, 0.0F, ImVec2(10, 10), 72); });
    // The first sprite drawn is the meat: the first vertex of its quad is its top left corner.
    for (const ImDrawList* pDrawList : ImGui::GetDrawData()->CmdLists) {
      for (const ImDrawCmd& cmd : pDrawList->CmdBuffer) {
        if (cmd.TexRef._TexData == assets.sprites()._TexData) {
          return pDrawList
              ->VtxBuffer[static_cast<int>(cmd.VtxOffset + pDrawList->IdxBuffer[static_cast<int>(cmd.IdxOffset)])]
              .pos.y;
        }
      }
    }
    return FLT_MAX;
  };
  EXPECT_FLOAT_EQ(meatTop(0.0), 10.0F);
  EXPECT_FLOAT_EQ(meatTop(0.1), 11.0F);

  windowFrame([&] { assets.drawSprite(ImGui::GetWindowDrawList(), ui::Animation::eat, 0.25, ImVec2(10, 10), 72); });
  EXPECT_EQ(LastFrameSprites(assets.sprites()), (Frames{{2, 2}}));
  windowFrame([&] { assets.drawSprite(ImGui::GetWindowDrawList(), ui::Animation::sleep, 0.5, ImVec2(10, 10), 72); });
  EXPECT_EQ(LastFrameSprites(assets.sprites()), (Frames{{1, 1}}));
}

TEST_F(WidgetsTest, TexturesAreReleasedOnceDestroyed) {
  const std::string notAnImage = "definitely not a PNG";
  EXPECT_EQ(textures.createFromImage({reinterpret_cast<const unsigned char*>(notAnImage.data()), notAnImage.size()}),
            nullptr);
  ImTextureData* pTexture = textures.createFromImage(embedded::LogoSmallPng());
  ASSERT_NE(pTexture, nullptr);
  EXPECT_EQ(pTexture->Width, 64);
  EXPECT_EQ(pTexture->Height, 64);
  windowFrame([&] { ImGui::Image(pTexture->GetTexRef(), ImVec2(64, 64)); });
  textures.release(pTexture);
  textures.release(pTexture);  // unknown (already released): ignored
  // Freed once the renderer destroyed it, or after a few frames for renderers that never acknowledge.
  for (int i = 0; i < 12; ++i) {
    windowFrame([] {});
    textures.collect();
  }
  SUCCEED();
}

TEST(ChartFormatting, Units) {
  std::array<char, 32> buffer{};
  using ui::ChartUnit;
  EXPECT_EQ(ui::FormatChartValue(ChartUnit::count, 0, buffer), "0");
  EXPECT_EQ(ui::FormatChartValue(ChartUnit::count, 1.5, buffer), "1.5");
  EXPECT_EQ(ui::FormatChartValue(ChartUnit::count, 999.7, buffer), "1000");
  EXPECT_EQ(ui::FormatChartValue(ChartUnit::count, 1234, buffer), "1234");
  EXPECT_EQ(ui::FormatChartValue(ChartUnit::count, 12500, buffer), "12.5k");
  EXPECT_EQ(ui::FormatChartValue(ChartUnit::count, 3e6, buffer), "3M");
  EXPECT_EQ(ui::FormatChartValue(ChartUnit::count, 2.25e9, buffer), "2.25G");
  EXPECT_EQ(ui::FormatChartValue(ChartUnit::nanoseconds, 850, buffer), "850 ns");
  EXPECT_EQ(ui::FormatChartValue(ChartUnit::nanoseconds, 500'000, buffer), "500 \xC2\xB5s");
  EXPECT_EQ(ui::FormatChartValue(ChartUnit::nanoseconds, 1'500'000, buffer), "1.5 ms");
  EXPECT_EQ(ui::FormatChartValue(ChartUnit::nanoseconds, 2e9, buffer), "2 s");
  EXPECT_EQ(ui::FormatChartValue(ChartUnit::nanoseconds, 0, buffer), "0");
  EXPECT_EQ(ui::FormatChartValue(ChartUnit::bytes, 512, buffer), "512 B");
  EXPECT_EQ(ui::FormatChartValue(ChartUnit::bytes, 1536, buffer), "1.5 KB");
  EXPECT_EQ(ui::FormatChartValue(ChartUnit::bytes, 3.0 * 1024 * 1024, buffer), "3 MB");
  EXPECT_EQ(ui::FormatChartValue(ChartUnit::bytes, 2.0 * 1024 * 1024 * 1024, buffer), "2 GB");
  EXPECT_EQ(ui::FormatChartValue(ChartUnit::seconds, 30, buffer), "30 s");
  EXPECT_EQ(ui::FormatChartValue(ChartUnit::seconds, 120, buffer), "2 min");
  EXPECT_EQ(ui::FormatChartValue(ChartUnit::seconds, 150, buffer), "2:30");
  EXPECT_EQ(ui::FormatChartValue(ChartUnit::percent, 50, buffer), "50 %");
  EXPECT_EQ(ui::FormatChartValue(ChartUnit::percent, 99.9, buffer), "99.9 %");
  EXPECT_EQ(ui::FormatChartValue(ChartUnit::percent, 99.99, buffer), "99.99 %");
  std::array<char, 3> tiny{};
  EXPECT_LE(ui::FormatChartValue(ChartUnit::nanoseconds, 1'500'000, tiny).size(), 3U);  // never overflowing
}

class ChartTest : public WidgetsTest {
 protected:
  ChartTest() {
    for (int idx = 0; idx < 60; ++idx) {
      x.push_back(static_cast<float>(idx + 1));
      first.push_back(static_cast<float>(100 + (idx % 7)));
      second.push_back(idx == 30 ? std::numeric_limits<float>::quiet_NaN() : static_cast<float>(idx));
    }
  }

  // Draws the chart in a frame, and remembers where it is.
  void draw(const ui::ChartOptions& options) {
    const std::array series{
        ui::ChartSeries{.name = "first", .values = first, .color = IM_COL32(200, 80, 60, 255)},
        ui::ChartSeries{
            .name = "second", .values = second, .color = IM_COL32(60, 80, 200, 255), .reduce = ui::ChartReduce::max}};
    windowFrame([&] {
      ui::Chart("##chart", x, series, state, options);
      rectMin = ImGui::GetItemRectMin();
      rectMax = ImGui::GetItemRectMax();
    });
  }

  // Left click at a point.
  void click(ImVec2 pos, const ui::ChartOptions& options) {
    MouseMove(pos);
    draw(options);
    MouseButton(true);
    draw(options);
    MouseButton(false);
    draw(options);
  }

  [[nodiscard]] static bool TooltipShown() {
    for (const ImGuiWindow* pWindow : ImGui::GetCurrentContext()->Windows) {
      if (pWindow->Active && std::string_view(pWindow->Name).starts_with("##Tooltip")) {
        return true;
      }
    }
    return false;
  }

  std::vector<float> x;
  std::vector<float> first;
  std::vector<float> second;
  ui::ChartState state;
  ImVec2 rectMin;
  ImVec2 rectMax;
};

TEST_F(ChartTest, EveryStyleAndScaleDraws) {
  for (ui::ChartStyle style : {ui::ChartStyle::lines, ui::ChartStyle::stackedAreas}) {
    for (ui::ChartScale scale : {ui::ChartScale::linear, ui::ChartScale::log}) {
      state.invalidate();
      draw({.title = "Title",
            .x = {.unit = ui::ChartUnit::seconds},
            .y = {.unit = ui::ChartUnit::nanoseconds, .scale = scale},
            .height = 200,
            .style = style});
      EXPECT_GT(LastFrameVertices(), 500);
      EXPECT_EQ(LastFrameNonFiniteVertices(), 0);
    }
  }
  EXPECT_EQ(state.nbPoints, 60U);  // fewer points than pixels: all kept
  EXPECT_FLOAT_EQ(rectMax.y - rectMin.y, 200.0F);

  // Bars over the edges of buckets, on a logarithmic axis, with markers; a percentile curve.
  const std::vector<float> edges{1000, 2000, 4000, 8000};
  const std::vector<float> counts{5, 0, 12};
  const std::array bars{ui::ChartSeries{.name = "count", .values = counts, .color = IM_COL32_WHITE}};
  ui::ChartState barsState;
  const auto drawBars = [&](std::span<const ui::ChartMarker> markers) {
    windowFrame([&] {
      ui::Chart("##bars", edges, bars, barsState,
                {.markers = markers,
                 .x = {.unit = ui::ChartUnit::nanoseconds, .scale = ui::ChartScale::log},
                 .style = ui::ChartStyle::bars});
    });
    EXPECT_EQ(LastFrameNonFiniteVertices(), 0);
    return LastFrameVertices();
  };
  const int withoutMarkers = drawBars({});
  const std::array markers{ui::ChartMarker{.label = "p50", .x = 3000, .color = IM_COL32_WHITE}};
  const int withMarker = drawBars(markers);
  EXPECT_GT(withMarker, withoutMarkers);
  // A marker the axis cannot place (0 on a logarithmic axis, a percentile not computed) is left out.
  const std::array unplaced{markers[0], ui::ChartMarker{.label = "p90", .x = 0, .color = IM_COL32_WHITE},
                            ui::ChartMarker{.label = "p99", .x = -5, .color = IM_COL32_WHITE}};
  EXPECT_EQ(drawBars(unplaced), withMarker);
  const std::vector<float> percentiles{0, 90, 99, 99.9F};
  const std::vector<float> latencies{1e6, 2e6, 5e6, 4e7};
  const std::array curve{ui::ChartSeries{.name = "latency", .values = latencies, .color = IM_COL32_WHITE}};
  ui::ChartState curveState;
  windowFrame([&] {
    ui::Chart("##curve", percentiles, curve, curveState,
              {.x = {.unit = ui::ChartUnit::percent, .scale = ui::ChartScale::nines},
               .y = {.unit = ui::ChartUnit::nanoseconds, .scale = ui::ChartScale::log}});
  });
  // No data: the axes and a hint.
  ui::ChartState emptyState;
  windowFrame([&] { ui::Chart("##empty", {}, {}, emptyState, {}); });
  EXPECT_EQ(emptyState.nbPoints, 0U);
}

TEST_F(ChartTest, AnOverlayIsALineOverTheStack) {
  const std::vector<float> target(x.size(), 1000.0F);
  const ui::ChartOptions options{.height = 200, .style = ui::ChartStyle::stackedAreas};
  ui::ChartState overlayState;
  const auto drawWith = [&](bool overlay) {
    const std::array series{
        ui::ChartSeries{.name = "first", .values = first, .color = IM_COL32(200, 80, 60, 255)},
        ui::ChartSeries{.name = "second", .values = second, .color = IM_COL32(60, 80, 200, 255)},
        ui::ChartSeries{.name = "target", .values = target, .color = IM_COL32_WHITE, .overlay = true}};
    windowFrame([&] {
      ui::Chart("##overlay", x, std::span<const ui::ChartSeries>(series).first(overlay ? 3 : 2), overlayState, options);
      rectMin = ImGui::GetItemRectMin();
      rectMax = ImGui::GetItemRectMax();
    });
    EXPECT_EQ(LastFrameNonFiniteVertices(), 0);
    return LastFrameVertices();
  };
  const int stackOnly = drawWith(false);
  overlayState.invalidate();
  const int withOverlay = drawWith(true);
  EXPECT_GT(withOverlay, stackOnly);
  // Hovered: the tooltip lists it, out of the total.
  MouseMove(ImVec2((rectMin.x + rectMax.x) * 0.5F, (rectMin.y + rectMax.y) * 0.5F));
  drawWith(true);
  drawWith(true);
  EXPECT_TRUE(TooltipShown());
}

TEST_F(ChartTest, MorePointsThanPixelsAreReducedOncePerChange) {
  x.clear();
  first.clear();
  second.clear();
  for (int idx = 0; idx < 100'000; ++idx) {
    x.push_back(static_cast<float>(idx));
    first.push_back(1.0F);
    second.push_back(idx == 50'000 ? 1000.0F : 1.0F);  // one spike: kept by the max reduction
  }
  const ui::ChartOptions options{.y = {.unit = ui::ChartUnit::count}};
  draw(options);
  ASSERT_GT(state.nbPoints, 100U);
  EXPECT_LE(state.nbPoints, 1000U);
  const float* pSecond = state.points.data() + (2 * state.stride);
  EXPECT_FLOAT_EQ(*std::max_element(pSecond, pSecond + state.nbPoints), 1000.0F);
  const float* pFirst = state.points.data() + state.stride;
  EXPECT_FLOAT_EQ(*std::max_element(pFirst, pFirst + state.nbPoints), 1.0F);  // the mean of ones

  // Not prepared again while nothing changes.
  state.points[0] = -1.0F;
  draw(options);
  EXPECT_FLOAT_EQ(state.points[0], -1.0F);
  state.invalidate();
  draw(options);
  EXPECT_GT(state.points[0], 0.0F);  // the mean x of the first pixel column
}

TEST_F(ChartTest, TheTooltipFollowsTheStyleAndTheHiddenSeries) {
  // Stacked areas: a total under the values, "-" for the missing value in the middle.
  const ui::ChartOptions stacked{.height = 200, .style = ui::ChartStyle::stackedAreas};
  draw(stacked);
  MouseMove(ImVec2((rectMin.x + rectMax.x) * 0.5F, (rectMin.y + rectMax.y) * 0.5F));
  draw(stacked);
  draw(stacked);
  EXPECT_TRUE(TooltipShown());
  // A hidden series is neither drawn nor listed.
  click(ImVec2(rectMax.x - 4.0F, rectMin.y + 4.0F), stacked);
  ASSERT_TRUE(state.isHidden(1));
  MouseMove(ImVec2((rectMin.x + rectMax.x) * 0.5F, (rectMin.y + rectMax.y) * 0.5F));
  draw(stacked);
  draw(stacked);
  EXPECT_TRUE(TooltipShown());
  EXPECT_EQ(LastFrameNonFiniteVertices(), 0);

  // Bars: the tooltip tells the range of the bar.
  const std::vector<float> edges{0, 10, 20, 30};
  const std::vector<float> counts{5, 0, 12};
  const std::vector<float> others{1, 2, 3};
  const std::array bars{ui::ChartSeries{.name = "count", .values = counts, .color = IM_COL32_WHITE},
                        ui::ChartSeries{.name = "other", .values = others, .color = IM_COL32_BLACK}};
  ui::ChartState barsState;
  ImVec2 min;
  ImVec2 max;
  const auto drawBars = [&] {
    windowFrame([&] {
      ui::Chart("##bars", edges, bars, barsState, {.height = 200, .style = ui::ChartStyle::bars});
      min = ImGui::GetItemRectMin();
      max = ImGui::GetItemRectMax();
    });
  };
  drawBars();
  MouseMove(ImVec2(max.x - 4.0F, min.y + 4.0F));  // the legend of the last series
  drawBars();
  MouseButton(true);
  drawBars();
  MouseButton(false);
  drawBars();
  ASSERT_TRUE(barsState.isHidden(1));
  MouseMove(ImVec2((min.x * 0.3F) + (max.x * 0.7F), (min.y + max.y) * 0.5F));
  drawBars();
  drawBars();
  EXPECT_TRUE(TooltipShown());
  EXPECT_EQ(LastFrameNonFiniteVertices(), 0);
}

TEST_F(ChartTest, LegendHoverAndZoom) {
  const ui::ChartOptions options{.x = {.unit = ui::ChartUnit::seconds}, .height = 200, .zoomable = true};
  draw(options);
  // The legend is on the right of the title line: the last series ends at the right border.
  click(ImVec2(rectMax.x - 4.0F, rectMin.y + 4.0F), options);
  EXPECT_TRUE(state.isHidden(1));
  EXPECT_FALSE(state.isHidden(0));
  click(ImVec2(rectMax.x - 4.0F, rectMin.y + 4.0F), options);
  EXPECT_FALSE(state.isHidden(1));

  // Hovering the plot shows the values of the point.
  const float midY = (rectMin.y + rectMax.y) * 0.5F;
  MouseMove(ImVec2((rectMin.x + rectMax.x) * 0.5F, midY));
  draw(options);
  draw(options);
  EXPECT_TRUE(TooltipShown());

  // Dragging selects a window of x.
  MouseMove(ImVec2(rectMin.x + 200.0F, midY));
  draw(options);
  MouseButton(true);
  draw(options);
  MouseMove(ImVec2(rectMin.x + 400.0F, midY));
  draw(options);
  MouseButton(false);
  draw(options);
  ASSERT_TRUE(state.zoomed());
  EXPECT_GT(state.zoomMin, 1.0);
  EXPECT_LT(state.zoomMax, 60.0);
  draw(options);
  EXPECT_LT(state.nbPoints, 60U);

  // A double click shows everything again.
  for (int idx = 0; idx < 2; ++idx) {
    MouseButton(true);
    draw(options);
    MouseButton(false);
    draw(options);
  }
  EXPECT_FALSE(state.zoomed());
}

}  // namespace
}  // namespace zalie
