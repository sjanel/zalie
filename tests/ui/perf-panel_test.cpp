// The Performance tab (run settings, parameter rows and their editors, header buttons, dialogs, dashboard), driven
// as the user does.

#include <gtest/gtest.h>
#include <imgui.h>

#include <aeronet/http-method.hpp>
#include <aeronet/http-request-view.hpp>
#include <aeronet/http-server-config.hpp>
#include <aeronet/router.hpp>
#include <aeronet/single-http-server.hpp>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "app-test-fixture.hpp"
#include "zalie/collection.hpp"
#include "zalie/file-io.hpp"
#include "zalie/key-value.hpp"
#include "zalie/load-plan.hpp"
#include "zalie/load-preview.hpp"
#include "zalie/load-runner.hpp"
#include "zalie/load-settings.hpp"
#include "zalie/method.hpp"
#include "zalie/rate-controller.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/request.hpp"
#include "zalie/ui/app.hpp"
#include "zalie/ui/perf-tab.hpp"
#include "zalie/workspace.hpp"

namespace zalie {
namespace {

class PerfPanelTest : public AppTest {
 protected:
  // The Performance tab of an unsaved request to 'url', its plan compiled and drawn.
  ui::RequestTab& openPerf(std::string_view url, Method method = Method::get) {
    ui::RequestTab& tab = perfTabFor(url, method);
    EXPECT_TRUE(waitPlan(tab));
    frame(2);
    return tab;
  }

  // The request edited directly, then its plan compiled again and drawn.
  void replan(ui::RequestTab& tab) {
    refresh();
    EXPECT_TRUE(waitPlan(tab));
    frame(2);
  }

  // Clicks a field, replaces its text and leaves it (Enter).
  [[nodiscard]] bool edit(std::string_view label, std::string_view text, std::size_t index = 0) {
    if (!click(label, index)) {
      return false;
    }
    typeText(text);
    press(ImGuiKey_Enter);
    return true;
  }

  // The index of the row named 'name' of the parameter table.
  static std::uint32_t RowIndex(const ui::RequestTab& tab, std::string_view name) {
    for (std::uint32_t idx = 0; idx < tab.perf->rows.size(); ++idx) {
      if (View(tab.perf->rows[idx].name) == name) {
        return idx;
      }
    }
    ADD_FAILURE() << "no row " << name;
    return 0;
  }

  // Chooses the source of a row of the parameter table, then lets its plan compile.
  [[nodiscard]] bool chooseSource(ui::RequestTab& tab, std::string_view row, std::string_view source) {
    // The combo box of the row, then the entry of its popup.
    if (!click("##source", RowIndex(tab, row)) || !click(source)) {
      return false;
    }
    if (!waitPlan(tab)) {
      return false;
    }
    frame(2);
    return true;
  }

  // Whether the tooltip of the name of a row shows up, the mouse resting on it (on "(query)" after the name of a
  // query parameter: the tooltip is the one of the last text).
  [[nodiscard]] bool rowNameHasTooltip(const ui::RequestTab& tab, std::uint32_t row) {
    const UiItem* pRow = findItem("##row", row);
    if (pRow == nullptr) {
      return false;
    }
    const LoadRow& loadRow = tab.perf->rows[row];
    const std::string_view name = loadRow.name;
    const float nameEnd = pRow->visible.Min.x + ImGui::GetStyle().ItemSpacing.x +
                          ImGui::CalcTextSize(name.data(), name.data() + name.size()).x;
    MouseMove(ImVec2(nameEnd + (loadRow.target == ParameterTarget::queryParameter ? 20.0F : -6.0F),
                     pRow->visible.GetCenter().y));
    frame();
    waitTooltip();
    return tooltipShown();
  }

  std::filesystem::path writeFile(std::string_view name, std::string_view content) {
    const std::filesystem::path path = home.path() / name;
    std::ofstream(path) << content;
    return path;
  }
};

TEST_F(PerfPanelTest, TheRunSettingsAreEditedInTheirFields) {
  workspace.settings().maxLoadConnections = 4;
  ui::RequestTab& tab = openPerf("http://127.0.0.1:1/x");
  const Request& request = *app->requestOf(tab);
  EXPECT_FALSE(request.load.has_value());  // created at the first change only

  ASSERT_TRUE(edit("##connections", "9"));
  ASSERT_TRUE(request.load.has_value());
  EXPECT_EQ(request.load->connections, 4U);  // the limit of the settings
  ASSERT_TRUE(edit("##duration", "2.5"));
  EXPECT_EQ(request.load->durationMs, 2500U);
  ASSERT_TRUE(edit("##warmup", "-3"));
  EXPECT_EQ(request.load->warmupMs, 0U);
  ASSERT_TRUE(edit("##maxRequests", "70"));
  EXPECT_EQ(request.load->maxRequests, 70U);
  ASSERT_TRUE(edit("##maxRate", "15"));
  EXPECT_EQ(request.load->maxRequestsPerSecond, 15U);
  ASSERT_TRUE(edit("##seed", "1234"));
  EXPECT_EQ(request.load->seed, 1234U);
  ASSERT_TRUE(click("One pass"));
  EXPECT_TRUE(request.load->onePass);
  ASSERT_TRUE(choose("##combine", "All combinations"));
  EXPECT_EQ(request.load->combine, CombineMode::allCombinations);

  EXPECT_TRUE(hasTooltip("##seed"));
  EXPECT_TRUE(hasTooltip("##duration"));
  EXPECT_TRUE(hasTooltip("One pass"));
  EXPECT_TRUE(hasTooltip("##combine"));
  EXPECT_TRUE(hasTooltip("##perfRun"));
}

TEST_F(PerfPanelTest, TheSplitterSharesTheHeightWithTheDashboard) {
  openPerf("http://127.0.0.1:1/x");
  const float before = workspace.settings().requestEditorRatio;
  ASSERT_TRUE(hover("##perfSplit"));
  MouseButton(true);
  frame();
  const ImVec2 start = ImGui::GetIO().MousePos;
  MouseMove(ImVec2(start.x, start.y + 60.0F));
  frame(2);
  MouseButton(false);
  frame();
  EXPECT_GT(workspace.settings().requestEditorRatio, before);
}

TEST_F(PerfPanelTest, ARowTakesASourceAndItsEditorFollows) {
  ui::RequestTab& tab = openPerf("http://127.0.0.1:1/items?page=3");
  const Request& request = *app->requestOf(tab);
  ASSERT_EQ(tab.perf->rows.size(), 1U);
  EXPECT_TRUE(rowNameHasTooltip(tab, 0));  // a query parameter

  // Varied from the value of the request.
  ASSERT_TRUE(chooseSource(tab, "page", "Integer range"));
  ASSERT_TRUE(request.load.has_value());
  ASSERT_EQ(request.load->parameters.size(), 1U);
  const LoadParameter& page = request.load->parameters[0];
  EXPECT_EQ(page.source, ValueSource::integerRange);
  EXPECT_EQ(page.target, ParameterTarget::queryParameter);
  EXPECT_EQ(View(page.fixedValue), "3");
  ASSERT_TRUE(edit("##value", "2", 2));  // From, To, Step
  EXPECT_EQ(page.integerStep, 2);
  ASSERT_TRUE(edit("##padding", "5"));
  EXPECT_EQ(page.integerPadding, 5U);
  EXPECT_TRUE(hasTooltip("##padding"));

  ASSERT_TRUE(chooseSource(tab, "page", "Decimal range"));
  ASSERT_TRUE(edit("##decimals", "3"));
  EXPECT_EQ(page.decimals, 3U);

  // The step of a date range is shown in the largest unit dividing it.
  ASSERT_TRUE(chooseSource(tab, "page", "Date range"));
  ASSERT_TRUE(choose("##stepUnit", "hours"));
  EXPECT_EQ(page.dateStepSeconds, 3600);
  ASSERT_TRUE(edit("##step", "90"));
  EXPECT_EQ(page.dateStepSeconds, 90 * 3600);

  ASSERT_TRUE(chooseSource(tab, "page", "Timestamp"));
  ASSERT_TRUE(choose("##timestampFormat", "ISO 8601"));
  EXPECT_EQ(page.timestampFormat, TimestampFormat::iso8601);
  ASSERT_TRUE(chooseSource(tab, "page", "List"));
  ASSERT_TRUE(choose("##order", "Shuffled"));
  EXPECT_EQ(page.order, ValueOrder::shuffled);

  // Back to the value of the request: the parameter goes away.
  ASSERT_TRUE(chooseSource(tab, "page", "Default"));
  EXPECT_TRUE(request.load->parameters.empty());
}

TEST_F(PerfPanelTest, ADateRangeStartsFromTheDateOfTheQueryParameter) {
  ui::RequestTab& tab = openPerf("http://127.0.0.1:1/search?origins=LON&dates=2026-12-18");
  const Request& request = *app->requestOf(tab);
  ASSERT_TRUE(chooseSource(tab, "dates", "Date range"));
  ASSERT_TRUE(request.load.has_value());
  const LoadParameter& dates = request.load->parameters[0];
  EXPECT_EQ(View(dates.dateFrom), "2026-12-18");
  EXPECT_EQ(View(dates.dateTo), "2026-12-24");
  // Valid at once: the run can start.
  const LoadPlan& plan = *tab.perf->preview->plan;
  EXPECT_TRUE(plan.valid());
  EXPECT_EQ(plan.valueCount(0), 7U);
  ASSERT_FALSE(tab.perf->preview->requests.empty());
  EXPECT_EQ(View(tab.perf->preview->requests[0].line), "GET http://127.0.0.1:1/search?origins=LON&dates=2026-12-18");

  // Not a date: from today.
  ASSERT_TRUE(chooseSource(tab, "origins", "Date range"));
  EXPECT_FALSE(request.load->parameters[1].dateFrom.empty());
  EXPECT_TRUE(tab.perf->preview->plan->valid());
}

TEST_F(PerfPanelTest, AQueryParameterIsLeftOutOfTheRunThenSentAgain) {
  ui::RequestTab& tab = openPerf("http://127.0.0.1:1/search?origins=LON&locale=en");
  const Request& request = *app->requestOf(tab);
  ASSERT_EQ(tab.perf->rows.size(), 2U);
  ASSERT_TRUE(click("##remove", RowIndex(tab, "locale")));
  ASSERT_TRUE(waitPlan(tab));
  frame(2);
  ASSERT_TRUE(request.load.has_value());
  ASSERT_EQ(request.load->parameters.size(), 1U);
  EXPECT_EQ(request.load->parameters[0].source, ValueSource::omitted);
  EXPECT_EQ(View(request.url), "http://127.0.0.1:1/search?origins=LON&locale=en");  // the request keeps it
  ASSERT_FALSE(tab.perf->preview->requests.empty());
  EXPECT_EQ(View(tab.perf->preview->requests[0].line), "GET http://127.0.0.1:1/search?origins=LON");
  EXPECT_TRUE(rowNameHasTooltip(tab, RowIndex(tab, "locale")));
  EXPECT_TRUE(hasTooltip("##restore"));

  // "Not sent" is a source of a query parameter of the request, not of a variable.
  ASSERT_TRUE(chooseSource(tab, "locale", "List"));
  EXPECT_EQ(request.load->parameters[0].source, ValueSource::list);
  ASSERT_TRUE(chooseSource(tab, "locale", "Not sent"));
  EXPECT_EQ(request.load->parameters[0].source, ValueSource::omitted);

  // The plus sends it again, as written in the request.
  ASSERT_TRUE(click("##restore"));
  ASSERT_TRUE(waitPlan(tab));
  frame(2);
  EXPECT_TRUE(request.load->parameters.empty());
  EXPECT_EQ(View(tab.perf->preview->requests[0].line), "GET http://127.0.0.1:1/search?origins=LON&locale=en");
}

TEST_F(PerfPanelTest, AQueryParameterIsAddedForTheRunThenRemoved) {
  ui::RequestTab& tab = openPerf("http://127.0.0.1:1/search?origins=LON");
  const Request& request = *app->requestOf(tab);
  EXPECT_TRUE(hasTooltip("##addQuery"));
  ASSERT_TRUE(click("##addQuery"));
  frame(2);
  ASSERT_TRUE(request.load.has_value());
  ASSERT_EQ(request.load->parameters.size(), 1U);
  ASSERT_EQ(tab.perf->rows.size(), 2U);
  EXPECT_TRUE(tab.perf->rows[1].added);
  EXPECT_EQ(tab.perf->selectedRow, 1U);
  // Its name field has the keyboard.
  typeText("page");
  press(ImGuiKey_Enter);
  ASSERT_TRUE(waitPlan(tab));
  frame(2);
  EXPECT_EQ(View(request.load->parameters[0].name), "page");
  // The tooltip of "(added)", after its name field.
  const UiItem* pName = findItem("##name");
  ASSERT_NE(pName, nullptr);
  MouseMove(ImVec2(pName->visible.Max.x + 12.0F, pName->visible.GetCenter().y));
  frame();
  waitTooltip();
  EXPECT_TRUE(tooltipShown());

  // Varied like any other: no "Default" to fall back to, nor "Not sent".
  ASSERT_TRUE(click("##source", 1));
  EXPECT_EQ(findItem("Not sent"), nullptr);
  ASSERT_TRUE(click("Integer range"));
  ASSERT_TRUE(waitPlan(tab));
  frame(2);
  ASSERT_FALSE(tab.perf->preview->requests.empty());
  EXPECT_EQ(View(tab.perf->preview->requests[1].line), "GET http://127.0.0.1:1/search?origins=LON&page=2");
  EXPECT_EQ(View(request.url), "http://127.0.0.1:1/search?origins=LON");  // the request does not change

  // Its cross removes it.
  ASSERT_TRUE(click("##remove", 1));
  ASSERT_TRUE(waitPlan(tab));
  frame(2);
  EXPECT_TRUE(request.load->parameters.empty());
  EXPECT_EQ(tab.perf->rows.size(), 1U);
}

TEST_F(PerfPanelTest, TheBodyAndItsJsonFieldsVaryForTheRunOnly) {
  ui::RequestTab& tab = openPerf("http://127.0.0.1:1/uppercase", Method::post);
  Request& request = *app->requestOf(tab);
  EXPECT_FALSE(hasItem("##addBody"));  // no body
  request.bodyMode = BodyMode::json;
  request.body.assign(std::string_view{R"({"text": "hello", "n": 1})"});
  replan(tab);
  EXPECT_TRUE(hasTooltip("##addJsonField"));
  ASSERT_TRUE(click("##addJsonField"));
  frame(2);
  ASSERT_TRUE(request.load.has_value());
  ASSERT_EQ(request.load->parameters.size(), 1U);
  EXPECT_EQ(request.load->parameters[0].target, ParameterTarget::jsonField);
  // Its JSONPath field has the keyboard.
  typeText("$.text");
  press(ImGuiKey_Enter);
  ASSERT_TRUE(waitPlan(tab));
  frame(2);
  EXPECT_EQ(View(request.load->parameters[0].name), "$.text");
  ASSERT_TRUE(tab.perf->preview->plan->valid());
  ASSERT_FALSE(tab.perf->preview->requests.empty());
  EXPECT_EQ(View(tab.perf->preview->requests[0].body), R"({"text": "", "n": 1})");  // a fixed empty value first

  // The whole body: its one line is the first value of a list. It overlaps the field.
  EXPECT_TRUE(hasTooltip("##addBody"));
  ASSERT_TRUE(click("##addBody"));
  ASSERT_TRUE(waitPlan(tab));
  frame(2);
  ASSERT_EQ(request.load->parameters.size(), 2U);
  EXPECT_EQ(request.load->parameters[1].target, ParameterTarget::body);
  EXPECT_EQ(request.load->parameters[1].source, ValueSource::list);
  EXPECT_EQ(View(request.load->parameters[1].listValues), View(request.body));
  EXPECT_FALSE(tab.perf->preview->plan->valid());
  EXPECT_FALSE(hasItem("##addBody"));  // one whole body at most

  // The crosses remove them: the field first.
  ASSERT_TRUE(click("##remove", 0));
  ASSERT_TRUE(waitPlan(tab));
  frame(2);
  ASSERT_EQ(request.load->parameters.size(), 1U);
  ASSERT_TRUE(tab.perf->preview->plan->valid());
  EXPECT_EQ(View(tab.perf->preview->requests[0].body), View(request.body));
  ASSERT_TRUE(click("##remove", 0));
  ASSERT_TRUE(waitPlan(tab));
  frame(2);
  EXPECT_TRUE(request.load->parameters.empty());
  EXPECT_TRUE(tab.perf->rows.empty());
  EXPECT_EQ(View(request.body), R"({"text": "hello", "n": 1})");  // the request never changed

  // A text body has no JSON field.
  request.bodyMode = BodyMode::text;
  replan(tab);
  EXPECT_TRUE(hasItem("##addBody"));
  EXPECT_FALSE(hasItem("##addJsonField"));
}

TEST_F(PerfPanelTest, AQueryParameterIsAddedToARequestWithoutParameters) {
  ui::RequestTab& tab = openPerf("http://127.0.0.1:1/x");
  ASSERT_TRUE(tab.perf->rows.empty());
  ASSERT_TRUE(click("##addQuery"));
  frame(2);
  typeText("q");
  ASSERT_TRUE(waitPlan(tab));
  frame(2);
  ASSERT_EQ(tab.perf->rows.size(), 1U);
  ASSERT_FALSE(tab.perf->preview->requests.empty());
  EXPECT_EQ(View(tab.perf->preview->requests[0].line), "GET http://127.0.0.1:1/x?q=");
}

TEST_F(PerfPanelTest, ADataFileRowShowsItsColumnsAndItsProblems) {
  const std::filesystem::path csv = writeFile("users.csv", "id,name\n1,zalie\n2\n3,physalie\n");
  ui::RequestTab& tab = openPerf("http://127.0.0.1:1/users/{{user}}");
  const Request& request = *app->requestOf(tab);
  ASSERT_TRUE(chooseSource(tab, "user", "Data file"));
  ASSERT_TRUE(edit("##value", PathToUtf8(csv)));  // the File field
  ASSERT_TRUE(waitPlan(tab));
  frame(2);
  const LoadParameter& user = request.load->parameters[0];
  ASSERT_NE(tab.perf->preview->dataFiles[0], nullptr);
  EXPECT_NE(tab.perf->preview->dataFiles[0]->firstMalformedLine(), 0U);
  ASSERT_TRUE(choose("##column", "name"));
  EXPECT_EQ(View(user.fileColumn), "name");
  ASSERT_TRUE(waitPlan(tab));
  frame(2);

  // A file that does not exist.
  ASSERT_TRUE(edit("##value", PathToUtf8(home.path() / "missing.csv")));
  ASSERT_TRUE(waitPlan(tab));
  frame(2);
  ASSERT_NE(tab.perf->preview->dataFiles[0], nullptr);
  EXPECT_FALSE(tab.perf->preview->dataFiles[0]->valid());
}

TEST_F(PerfPanelTest, AFileDroppedOnARowBecomesItsDataFile) {
  ui::RequestTab& tab = openPerf("http://127.0.0.1:1/users/{{user}}");
  const Request& request = *app->requestOf(tab);
  // On the row under the mouse, which has no source yet.
  const std::filesystem::path lines[] = {writeFile("users.jsonl", "{\"id\":1}\n")};
  ASSERT_TRUE(hover("##row"));
  app->onFilesDropped(lines);
  frame(2);
  ASSERT_TRUE(request.load.has_value());
  ASSERT_EQ(request.load->parameters.size(), 1U);
  EXPECT_EQ(request.load->parameters[0].source, ValueSource::dataFile);
  EXPECT_EQ(request.load->parameters[0].fileFormat, DataFileFormat::jsonLines);
  // On the selected data file row, the mouse elsewhere.
  MouseMove(ImVec2(2.0F, 2.0F));
  frame();
  const std::filesystem::path text[] = {writeFile("users.txt", "zalie\nphysalie\n")};
  app->onFilesDropped(text);
  frame(2);
  EXPECT_EQ(request.load->parameters[0].fileFormat, DataFileFormat::text);
}

TEST_F(PerfPanelTest, AFileDroppedOnATabWithoutRowsIsImported) {
  openPerf("http://127.0.0.1:1/x");
  const std::filesystem::path dropped[] = {writeFile("users.csv", "id\n1\n")};
  app->onFilesDropped(dropped);
  frame(2);
  ASSERT_FALSE(app->toasts().empty());
  EXPECT_TRUE(app->toasts().back().error);  // not a collection
}

TEST_F(PerfPanelTest, TheRowsOfTheTableTellWhereTheirValueComesFrom) {
  ui::RequestTab& tab = openPerf("http://127.0.0.1:1/users/{{user}}");
  Request& request = *app->requestOf(tab);
  EXPECT_TRUE(rowNameHasTooltip(tab, 0));  // a variable
  // A parameter the request does not use any more.
  LoadParameter gone;
  gone.name.assign(std::string_view{"gone"});
  request.load.emplace().parameters.push_back(std::move(gone));
  replan(tab);
  ASSERT_EQ(tab.perf->rows.size(), 2U);
  const std::uint32_t goneRow = RowIndex(tab, "gone");
  EXPECT_TRUE(tab.perf->rows[goneRow].unused);
  EXPECT_TRUE(rowNameHasTooltip(tab, goneRow));

  // An undefined variable cannot run.
  ASSERT_TRUE(click("##perfRun"));
  frame(2);
  EXPECT_TRUE(lastToast().starts_with("The run cannot start: ")) << lastToast();
  EXPECT_FALSE(tab.perf->running());
}

TEST_F(PerfPanelTest, TheFirstRequestsShowTheirBodyOrTheirRefusedValue) {
  ui::RequestTab& tab = perfTabFor("http://127.0.0.1:1/orders", Method::post);
  Request& request = *app->requestOf(tab);
  request.bodyMode = BodyMode::json;
  request.body.assign(std::string_view{R"({"id":{{id}}})"});
  LoadParameter id;
  id.name.assign(std::string_view{"id"});
  id.source = ValueSource::integerRange;
  request.load.emplace().parameters.push_back(std::move(id));
  replan(tab);
  ASSERT_FALSE(tab.perf->preview->requests.empty());
  EXPECT_EQ(View(tab.perf->preview->requests[0].body), R"({"id":1})");

  // A header cannot hold a line break.
  request.headers.push_back(MakeKeyValue("X-Tag", "{{tag}}"));
  LoadParameter tag;
  tag.name.assign(std::string_view{"tag"});
  tag.source = ValueSource::randomString;
  tag.randomCharacters.assign(std::string_view{"\r"});
  request.load->parameters.push_back(std::move(tag));
  replan(tab);
  ASSERT_FALSE(tab.perf->preview->requests.empty());
  EXPECT_TRUE(tab.perf->preview->requests[0].refused) << View(tab.perf->preview->requests[0].line);
}

TEST_F(PerfPanelTest, ThePlanUsesTheActiveEnvironmentOfTheCollection) {
  Environment local{.name = RawChars32(std::string_view{"local"}), .variables = {}};
  local.variables.push_back(MakeKeyValue("baseUrl", "http://127.0.0.1:9"));
  exampleCollection().environments.push_back(std::move(local));
  workspace[0].activeEnvironment = static_cast<std::int32_t>(exampleCollection().environments.size() - 1);
  app->openRequest(exampleRequestId(0));  // {{baseUrl}}/json
  frame(2);
  ui::RequestTab& tab = app->openPerformanceTab(app->activeTabIndex());
  ASSERT_TRUE(waitPlan(tab));
  EXPECT_EQ(tab.perf->preview->plan->host(), "127.0.0.1");
}

TEST_F(PerfPanelTest, TheHeaderRunsStopsAndExportsARun) {
  aeronet::Router router;
  router.setPath(aeronet::http::Method::GET, "/slow", [](const aeronet::HttpRequestView& req) {
    std::this_thread::sleep_for(20ms);
    return req.makeResponse(200, "late", "text/plain");
  });
  aeronet::SingleHttpServer server(aeronet::HttpServerConfig{}.withPort(0).withPollInterval(20ms), std::move(router));
  server.start();
  ui::RequestTab& tab = openPerf(LocalUrl(server, "/slow"));
  EXPECT_FALSE(app->exportRun(tab, home.path() / "none.json"));
  EXPECT_EQ(lastToast(), "No run to export");

  app->requestOf(tab)->load.emplace().durationMs = 0;  // until stopped
  app->requestOf(tab)->load->connections = 1;
  replan(tab);
  ASSERT_TRUE(click("##perfRun"));
  const auto deadline = std::chrono::steady_clock::now() + 20s;
  while (std::chrono::steady_clock::now() < deadline && !tab.perf->running()) {
    frame();
    std::this_thread::sleep_for(5ms);
  }
  ASSERT_TRUE(tab.perf->running());
  EXPECT_TRUE(hasTooltip("##perfStop"));
  ASSERT_TRUE(click("##perfStop"));
  ASSERT_TRUE(waitRun(tab));

  // The dialog proposes a file of the runs directory.
  ASSERT_TRUE(click("Export JSON..."));
  ASSERT_TRUE(ModalOpen());
  EXPECT_TRUE(View(tab.perf->exportPath).ends_with(".json")) << View(tab.perf->exportPath);
  ASSERT_TRUE(click("Cancel"));
  EXPECT_FALSE(ModalOpen());
  ASSERT_TRUE(click("Export JSON..."));
  const std::filesystem::path report = home.path() / "slow.json";
  typeText(PathToUtf8(report));  // the path field has the keyboard focus
  ASSERT_TRUE(click("Export"));
  EXPECT_FALSE(ModalOpen());
  EXPECT_TRUE(std::filesystem::exists(report));
}

TEST_F(PerfPanelTest, ARunWithoutAnyResponseStopsAndSaysWhy) {
  ui::RequestTab& tab = openPerf("http://127.0.0.1:1/x");  // nothing listens there
  app->requestOf(tab)->load.emplace().durationMs = 25000;
  app->requestOf(tab)->load->connections = 2;
  replan(tab);
  ASSERT_TRUE(click("##perfRun"));
  ASSERT_TRUE(waitRun(tab));
  EXPECT_EQ(lastToast(), "No response from the server after 3 requests: connection failed");
  EXPECT_LT(tab.perf->snapshot->elapsedSeconds, 20.0);
  EXPECT_EQ(LastFrameNonFiniteVertices(), 0);
}

TEST_F(PerfPanelTest, TheAdaptiveRateShowsItsFieldsOnceChosen) {
  ui::RequestTab& tab = openPerf("http://127.0.0.1:1/x");
  const Request& request = *app->requestOf(tab);
  EXPECT_FALSE(hasItem("##minRate"));
  EXPECT_FALSE(hasItem("##maxP99"));
  ASSERT_TRUE(click("Adaptive rate"));
  ASSERT_TRUE(request.load.has_value());
  EXPECT_TRUE(request.load->adaptiveRate);
  frame(2);
  ASSERT_TRUE(edit("##minRate", "50"));
  EXPECT_EQ(request.load->minRequestsPerSecond, 50U);
  ASSERT_TRUE(edit("##maxP99", "12.5"));
  EXPECT_DOUBLE_EQ(request.load->maxP99Ms, 12.5);
  ASSERT_TRUE(edit("##maxP99", "-4"));
  EXPECT_DOUBLE_EQ(request.load->maxP99Ms, 0.0);
  ASSERT_TRUE(edit("##maxRate", "400"));
  ASSERT_TRUE(click("Adaptive rate"));
  frame(2);
  EXPECT_FALSE(request.load->adaptiveRate);
  EXPECT_FALSE(hasItem("##minRate"));
  EXPECT_EQ(request.load->minRequestsPerSecond, 50U);  // kept for the next time
}

TEST_F(PerfPanelTest, TheDashboardDrawsTheTargetOfAnAdaptiveRate) {
  ui::RequestTab& tab = openPerf("http://127.0.0.1:1/x");
  const auto show = [&](bool adaptive, bool searching = false) {
    auto snapshot = std::make_shared<LoadSnapshot>();
    snapshot->phase = LoadPhase::running;
    snapshot->connections = 4;
    snapshot->elapsedSeconds = 6.0;
    for (std::uint32_t second = 0; second < 6; ++second) {
      const std::uint32_t target = 100U << std::min(second, 3U);
      snapshot->series.push_back(
          LoadSecond{.counts = {target, 0, 0, 0, 0}, .p50Ns = 1'000'000, .targetRate = adaptive ? target : 0U});
    }
    snapshot->counts = {3000, 0, 0, 0, 0};
    if (adaptive) {
      snapshot->targetRequestsPerSecond = 640.0;
      snapshot->sustainedRequestsPerSecond = 800.0;
      snapshot->baselineP50Ns = 1'000'000;
      snapshot->rateState = RateState::backOff;
    }
    if (searching) {
      snapshot->rateState = RateState::search;
      snapshot->searchLowRequestsPerSecond = 400.0;
      snapshot->searchHighRequestsPerSecond = 10'000.0;
    }
    app->showLoadSnapshot(tab, snapshot);
    frame(2);
    EXPECT_EQ(LastFrameNonFiniteVertices(), 0);
    return LastFrameVertices();
  };
  const int fixed = show(false);
  const int adaptive = show(true);
  EXPECT_GT(adaptive, fixed);             // the target line, its legend and the state of the rate
  EXPECT_GT(show(true, true), adaptive);  // the bounds of the bisection
  const ui::PerfCharts& charts = tab.perf->charts;
  ASSERT_EQ(charts.rates.size(), (kNbLoadClasses + 1) * 6U);
  EXPECT_FLOAT_EQ(charts.rates[(kNbLoadClasses * 6U) + 5], 800.0F);
}

TEST_F(PerfPanelTest, TheDashboardShowsTheErrorsOfAFakeRun) {
  ui::RequestTab& tab = openPerf("http://127.0.0.1:1/x");
  auto snapshot = std::make_shared<LoadSnapshot>();
  snapshot->phase = LoadPhase::finished;
  snapshot->connections = 1;
  snapshot->elapsedSeconds = 2.0;
  snapshot->setupError.assign(std::string_view{"no route to host"});
  for (std::uint32_t second = 0; second < 2; ++second) {
    snapshot->series.push_back(LoadSecond{.counts = {10, 0, 0, 2, 1}, .p50Ns = 1'000'000, .maxNs = 4'000'000});
  }
  snapshot->counts = {20, 0, 0, 4, 2};
  snapshot->statuses.push_back({.count = 20, .status = 200});
  snapshot->statuses.push_back({.count = 4, .status = 503});
  snapshot->errors[static_cast<std::size_t>(LoadErrorKind::timeout)] = {.count = 2, .firstMessage = "timed out"};
  app->showLoadSnapshot(tab, snapshot);
  frame(2);
  const bool logLatency = tab.perf->logLatency;
  ASSERT_TRUE(click("Logarithmic scale##logLatency"));
  EXPECT_NE(tab.perf->logLatency, logLatency);
  EXPECT_EQ(LastFrameNonFiniteVertices(), 0);
}

TEST_F(PerfPanelTest, ZalieEatsHerMeatDuringARunThenNaps) {
  ui::RequestTab& tab = openPerf("http://127.0.0.1:1/x");
  app->requestOf(tab)->load.emplace().durationMs = 10000;
  const auto show = [&](LoadPhase phase, double elapsedSeconds) {
    auto snapshot = std::make_shared<LoadSnapshot>();
    snapshot->phase = phase;
    snapshot->connections = 1;
    snapshot->elapsedSeconds = elapsedSeconds;
    snapshot->counts = {5, 0, 0, 0, 0};
    app->showLoadSnapshot(tab, snapshot);
    frame(2);
    return LastFrameSprites(app->assets().sprites());
  };
  // Row 2 of the sprite sheet: the eating dog (columns 0 to 3) over the meat (4 stages, columns 4 to 7).
  std::vector<SpriteFrame> sprites = show(LoadPhase::running, 6.0);
  ASSERT_EQ(sprites.size(), 2U);
  EXPECT_EQ(sprites[0], (SpriteFrame{.column = 6, .row = 2}));  // 6 s of 10: past half of the meat
  EXPECT_EQ(sprites[1].row, 2);
  EXPECT_LT(sprites[1].column, 4);
  sprites = show(LoadPhase::warmup, 0.0);
  ASSERT_EQ(sprites.size(), 2U);
  EXPECT_EQ(sprites[0], (SpriteFrame{.column = 4, .row = 2}));  // the bites of the warm-up do not count

  // Over: she naps (row 1), the banner keeps its place.
  sprites = show(LoadPhase::finished, 10.0);
  ASSERT_EQ(sprites.size(), 1U);
  EXPECT_EQ(sprites[0].row, 1);

  // Without a limit, a new piece of meat every 8 seconds: 11 s is 3 s into the second piece.
  app->requestOf(tab)->load->durationMs = 0;
  sprites = show(LoadPhase::running, 11.0);
  ASSERT_EQ(sprites.size(), 2U);
  EXPECT_EQ(sprites[0], (SpriteFrame{.column = 5, .row = 2}));
}

}  // namespace
}  // namespace zalie
