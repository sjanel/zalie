// The inspector (code snippet, timeline, variables, response headers, notes, the menu of its cards), driven as the
// user does.

#include <gtest/gtest.h>
#include <imgui.h>

#include <cstdint>
#include <string_view>

#include "app-test-fixture.hpp"
#include "zalie/collection.hpp"
#include "zalie/key-value.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/request.hpp"
#include "zalie/settings.hpp"
#include "zalie/ui/app.hpp"
#include "zalie/workspace.hpp"

namespace zalie {
namespace {

class InspectorTest : public AppTest {
 protected:
  InspectorTest() {
    app->openRequest(exampleRequestId(0));  // {{baseUrl}}/json
    frame(3);
  }

  Request& request() { return *app->requestOf(*app->activeTab()); }

  [[nodiscard]] std::uint32_t cardIndex(std::string_view id) {
    const auto& cards = workspace.settings().inspectorCards;
    for (std::uint32_t idx = 0; idx < cards.size(); ++idx) {
      if (View(cards[idx].id) == id) {
        return idx;
      }
    }
    ADD_FAILURE() << "no card " << id;
    return 0;
  }
};

TEST_F(InspectorTest, TheMenuShowsAndOrdersTheCards) {
  auto& cards = workspace.settings().inspectorCards;
  EXPECT_FALSE(cards[cardIndex(kCardNotes)].visible);
  ASSERT_TRUE(click("##inspectorMenu"));
  ASSERT_TRUE(click("Notes"));
  EXPECT_TRUE(cards[cardIndex(kCardNotes)].visible);

  const RawChars32 first = cards[0].id;
  ASSERT_TRUE(click("##down", 0));
  EXPECT_EQ(View(cards[1].id), View(first));
  ASSERT_TRUE(click("##up", 1));
  EXPECT_EQ(View(cards[0].id), View(first));
  press(ImGuiKey_Escape);

  // The notes of the request, or how to write some.
  frame(2);
  request().docs.assign(std::string_view{"Returns a sample JSON document."});
  frame(2);
  EXPECT_TRUE(hasItem("Notes"));  // the header of the card
}

TEST_F(InspectorTest, TheSnippetChangesLanguageIsEditedAndCopied) {
  ASSERT_TRUE(choose("##language", "Raw HTTP"));
  EXPECT_EQ(View(workspace.settings().snippetLanguage), "http");
  ASSERT_TRUE(choose("##language", "Python (requests)"));
  EXPECT_EQ(View(workspace.settings().snippetLanguage), "python");

  ASSERT_TRUE(click("##editSnippet"));
  ASSERT_TRUE(click("##snippetEdit"));
  typeText("print('edited')");
  ASSERT_TRUE(click("##copySnippet"));
  EXPECT_EQ(lastToast(), "Python (requests) snippet copied");
  EXPECT_EQ(ImGui::GetClipboardText(), std::string_view{"print('edited')"});
  // Back to the generated snippet.
  ASSERT_TRUE(click("##editSnippet"));
  ASSERT_TRUE(click("##copySnippet"));
  EXPECT_TRUE(std::string_view(ImGui::GetClipboardText()).contains("import requests"));
}

TEST_F(InspectorTest, TheVariablesTellWhereTheirValueComesFrom) {
  Collection& collection = exampleCollection();
  collection.variables.push_back(MakeKeyValue("token", "s3cr3t"));
  Request& req = request();
  req.url.assign(std::string_view{"{{baseUrl}}/json?id={{$randomUUID}}&missing={{nowhere}}"});
  req.headers.push_back(MakeKeyValue("Authorization", "Bearer {{token}}"));
  frame(3);
  // The collection settings, to edit them.
  ASSERT_TRUE(click("Edit variables..."));
  frame(2);
  EXPECT_TRUE(ModalOpen());
}

TEST_F(InspectorTest, AClickOnAResponseHeaderCopiesItsValue) {
  showBody("application/json", R"({"ok":true})");
  frame(3);
  const UiItem* pHeader = findItem("Response headers");
  ASSERT_NE(pHeader, nullptr);
  const ImGuiStyle& style = ImGui::GetStyle();
  const float x = pHeader->visible.Min.x + style.FramePadding.x + ImGui::CalcTextSize("content-type").x + 30.0F;
  const float y = pHeader->visible.Max.y + style.ItemSpacing.y + (ImGui::GetTextLineHeight() * 0.5F) + 1.0F;
  clickAt(ImVec2(x, y));
  EXPECT_EQ(lastToast(), "Header value copied");
  EXPECT_EQ(ImGui::GetClipboardText(), std::string_view{"application/json"});
}

}  // namespace
}  // namespace zalie
