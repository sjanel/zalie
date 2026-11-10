#include "zalie/persistence.hpp"

#include <gtest/gtest.h>

#include <amc/type_traits.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>

#include "test-utils.hpp"
#include "zalie/collection.hpp"
#include "zalie/exchange.hpp"
#include "zalie/file-io.hpp"
#include "zalie/history.hpp"
#include "zalie/key-value.hpp"
#include "zalie/load-settings.hpp"
#include "zalie/method.hpp"
#include "zalie/paths.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/request.hpp"
#include "zalie/settings.hpp"
#include "zalie/workspace.hpp"

namespace zalie {
namespace {

Collection MakeRichCollection() {
  Collection collection = MakeExampleCollection();
  collection.headers.push_back(MakeKeyValue("X-Team", "core", false));
  collection.auth.mode = AuthMode::apiKey;
  collection.auth.apiKeyInQuery = true;
  Request& req = collection.root.folders[0].requests[1];
  req.form.push_back(MakeKeyValue("é\"\n\\", "unicode \u00e9 and \"quotes\""));
  req.settings = {.timeoutMs = 1234,
                  .httpVersion = HttpVersion::http2,
                  .followRedirects = false,
                  .verifyTls = false,
                  .decompress = false};
  req.method = Method::del;
  req.bodyMode = BodyMode::formUrlEncoded;
  req.docs.assign(std::string_view{"# Notes\n- one"});
  return collection;
}

TEST(Persistence, CollectionRoundTrip) {
  const Collection collection = MakeRichCollection();
  const std::string json = SerializeCollection(collection);
  EXPECT_TRUE(json.contains("\"method\": \"DELETE\""));
  EXPECT_TRUE(json.contains("\"bodyMode\": \"form-urlencoded\""));
  EXPECT_TRUE(json.contains("\"httpVersion\": \"http2\""));
  EXPECT_TRUE(json.contains("\"mode\": \"api-key\""));
  auto parsed = ParseCollection(json);
  ASSERT_TRUE(parsed.has_value()) << View(parsed.error());
  EXPECT_EQ(*parsed, collection);
  // Stable output: serializing again gives the same bytes (clean git diffs).
  EXPECT_EQ(SerializeCollection(*parsed), json);
  // Indented by 2 spaces, ends with a newline.
  EXPECT_TRUE(json.starts_with("{\n  \"name\": ")) << json.substr(0, 40);
  EXPECT_TRUE(json.ends_with("}\n"));
}

// Requests are moved with memcpy in their folders: the optional run settings must not change that.
static_assert(amc::is_trivially_relocatable_v<Request>);

TEST(Persistence, LoadSettingsAreLeftOutWhenUnused) {
  const std::string json = SerializeCollection(MakeRichCollection());
  EXPECT_FALSE(json.contains("\"load\""));
}

TEST(Persistence, LoadSettingsRoundTrip) {
  Collection collection = MakeRichCollection();
  LoadSettings settings;
  settings.seed = 42;
  settings.maxRequests = 1000;
  settings.durationMs = 30000;
  settings.maxRequestsPerSecond = 500;
  settings.warmupMs = 2000;
  settings.connections = 8;
  settings.combine = CombineMode::allCombinations;
  settings.onePass = true;
  LoadParameter list;
  list.name.assign(std::string_view{"term"});
  list.source = ValueSource::list;
  list.listValues.assign(std::string_view{"dog\ncat"});
  list.order = ValueOrder::shuffled;
  LoadParameter file;
  file.name.assign(std::string_view{"user"});
  file.source = ValueSource::dataFile;
  file.filePath.assign(std::string_view{"users.csv"});
  file.fileColumn.assign(std::string_view{"id"});
  file.fileFormat = DataFileFormat::jsonLines;
  file.queryParameter = true;
  file.timestampFormat = TimestampFormat::iso8601;
  LoadParameter omitted;
  omitted.name.assign(std::string_view{"debug"});
  omitted.source = ValueSource::omitted;
  omitted.queryParameter = true;
  settings.parameters.push_back(std::move(list));
  settings.parameters.push_back(std::move(file));
  settings.parameters.push_back(std::move(omitted));
  collection.root.folders[0].requests[0].load = std::move(settings);

  const std::string json = SerializeCollection(collection);
  EXPECT_TRUE(json.contains("\"source\": \"data-file\""));
  EXPECT_TRUE(json.contains("\"source\": \"omitted\""));
  EXPECT_TRUE(json.contains("\"order\": \"shuffled\""));
  EXPECT_TRUE(json.contains("\"combine\": \"all-combinations\""));
  EXPECT_TRUE(json.contains("\"fileFormat\": \"json-lines\""));
  EXPECT_TRUE(json.contains("\"timestampFormat\": \"iso8601\""));
  auto parsed = ParseCollection(json);
  ASSERT_TRUE(parsed.has_value()) << View(parsed.error());
  EXPECT_EQ(*parsed, collection);
  EXPECT_EQ(SerializeCollection(*parsed), json);
}

TEST(Persistence, TruncatedInputIsNotReadPastItsEnd) {
  // The content of a file is a RawChars: no null terminator after the last byte. Under AddressSanitizer, reading past
  // the end of the buffer (glaze assumes null-terminated input by default) fails this test.
  const std::string json = SerializeCollection(MakeRichCollection());
  for (std::size_t size : {std::size_t{1}, std::size_t{2}, json.size() / 2, json.size() - 3}) {
    RawChars content(size);
    content.unchecked_append(std::string_view(json).substr(0, size));
    EXPECT_FALSE(ParseCollection(content).has_value()) << size;
    EXPECT_FALSE(ParseSettings(content).has_value()) << size;
    EXPECT_FALSE(ParseHistory(content).has_value()) << size;
  }
}

TEST(Persistence, LenientReading) {
  // Unknown keys (from a newer version) are ignored, missing keys keep their defaults, ids are generated.
  auto parsed = ParseCollection(
      R"({"name": "x", "future": {"a": [1, 2]}, "root": {"requests": [{"name": "r", "url": "http://h"}]}})");
  ASSERT_TRUE(parsed.has_value()) << View(parsed.error());
  ASSERT_EQ(parsed->root.requests.size(), 1U);
  const Request& req = parsed->root.requests[0];
  EXPECT_EQ(req.method, Method::get);
  EXPECT_EQ(req.settings.timeoutMs, RequestSettings::kDefaultTimeoutMs);
  EXPECT_NE(req.id, 0U);
  EXPECT_NE(parsed->root.id, 0U);

  EXPECT_FALSE(ParseCollection("{").has_value());
  EXPECT_FALSE(ParseCollection(R"({"root": {"requests": [{"method": "BREW"}]}})").has_value());
}

TEST(Persistence, SettingsRoundTripAndCardNormalization) {
  Settings settings;
  settings.theme.assign(std::string_view{"nuit"});
  settings.inspectorWidth = 512.5F;
  settings.maxLoadConnections = 256;
  settings.inspectorCards.push_back({.id = RawChars32(kCardVariables), .visible = false});
  settings.inspectorCards.push_back({.id = RawChars32(std::string_view{"unknown"}), .visible = true});
  settings.inspectorCards.push_back({.id = RawChars32(kCardVariables), .visible = true});  // duplicate
  settings.normalizeCards();
  ASSERT_EQ(settings.inspectorCards.size(), 5U);
  EXPECT_EQ(View(settings.inspectorCards[0].id), kCardVariables);  // user order first
  EXPECT_FALSE(settings.inspectorCards[0].visible);
  EXPECT_EQ(View(settings.inspectorCards[1].id), kCardSnippet);

  auto parsed = ParseSettings(SerializeSettings(settings));
  ASSERT_TRUE(parsed.has_value()) << View(parsed.error());
  EXPECT_EQ(*parsed, settings);

  auto defaults = ParseSettings("{}");
  ASSERT_TRUE(defaults.has_value());
  EXPECT_EQ(View(defaults->theme), "coton");
  EXPECT_EQ(defaults->inspectorCards.size(), 5U);
}

TEST(Persistence, WorkspaceStateAndHistory) {
  WorkspaceState state;
  state.collections.push_back({.path = RawChars32(std::string_view{"/a/b.zalie.json"}), .activeEnvironment = 2});
  state.openTabs.push_back(18446744073709551557ULL);  // ids keep their full 64 bits
  state.activeTab = 42;
  auto parsedState = ParseWorkspaceState(SerializeWorkspaceState(state));
  ASSERT_TRUE(parsedState.has_value());
  EXPECT_EQ(*parsedState, state);

  amc::vector<HistoryEntry> entries;
  entries.push_back({.url = RawChars32(std::string_view{"https://h/x"}),
                     .timestampMs = 1761739200123,
                     .requestId = 7,
                     .durationMs = 12,
                     .status = 404,
                     .method = Method::patch,
                     .state = ExchangeState::failed});
  auto parsedHistory = ParseHistory(SerializeHistory(entries));
  ASSERT_TRUE(parsedHistory.has_value());
  EXPECT_EQ(*parsedHistory, entries);
}

TEST(Persistence, Files) {
  const test::TempDir dir;
  const std::filesystem::path path = dir.path() / "sub" / "c.zalie.json";
  const Collection collection = MakeRichCollection();
  ASSERT_TRUE(SaveCollection(collection, path).has_value());
  EXPECT_FALSE(std::filesystem::exists(path.string() + ".tmp"));
  auto loaded = LoadCollection(path);
  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ(*loaded, collection);

  EXPECT_FALSE(LoadCollection(dir.path() / "missing.json").has_value());
  ASSERT_TRUE(WriteFileAtomically(dir.path() / "bad.json", "[1,").has_value());
  auto bad = LoadCollection(dir.path() / "bad.json");
  ASSERT_FALSE(bad.has_value());
  EXPECT_TRUE(View(bad.error()).contains("bad.json"));
}

TEST(Paths, CollectionFileName) {
  EXPECT_EQ(View(CollectionFileName("My API!")), "my-api.zalie.json");
  EXPECT_EQ(View(CollectionFileName("  Coton de Tulear -- v2 ")), "coton-de-tulear-v2.zalie.json");
  EXPECT_EQ(View(CollectionFileName("???")), "collection.zalie.json");
  EXPECT_FALSE(DefaultHomeDirectory().empty());
}

TEST(History, BoundedMostRecentFirst) {
  History history(3);
  for (int i = 0; i < 5; ++i) {
    history.add(HistoryEntry{.url = {},
                             .timestampMs = i,
                             .requestId = 0,
                             .durationMs = 0,
                             .status = 200,
                             .method = Method::get,
                             .state = ExchangeState::completed});
  }
  ASSERT_EQ(history.entries().size(), 3U);
  EXPECT_EQ(history.entries()[0].timestampMs, 4);
  EXPECT_EQ(history.entries()[2].timestampMs, 2);
  history.clear();
  EXPECT_TRUE(history.entries().empty());
  History disabled(0);
  disabled.add(HistoryEntry{});
  EXPECT_TRUE(disabled.entries().empty());
}

TEST(Workspace, FirstLaunchSaveAndReload) {
  const test::TempDir dir;
  std::uint64_t exampleRequestId = 0;
  {
    Workspace workspace(dir.path());
    workspace.load();
    ASSERT_EQ(workspace.size(), 1U);  // the example collection
    EXPECT_TRUE(workspace.errors().empty());
    OpenCollection& example = workspace[0];
    EXPECT_TRUE(std::filesystem::exists(example.path));
    EXPECT_EQ(example.path.parent_path(), workspace.collectionsDirectory());
    exampleRequestId = example.collection.root.requests[0].id;
    example.activeEnvironment = 0;
    workspace.settings().theme.assign(std::string_view{"nuit"});
    workspace.state().openTabs.push_back(exampleRequestId);

    OpenCollection& created = workspace.createCollection("My API");
    EXPECT_EQ(created.path.filename(), "my-api.zalie.json");
    OpenCollection& again = workspace.createCollection("My API");
    EXPECT_EQ(again.path.filename(), "my-api-2.zalie.json");
    workspace.closeCollection(2);
    EXPECT_EQ(workspace.size(), 2U);

    const RequestLocation location = workspace.findRequest(exampleRequestId);
    ASSERT_TRUE(location.valid());
    EXPECT_EQ(location.pCollection, &workspace[0]);
    EXPECT_FALSE(workspace.findRequest(1).valid());
    workspace.saveAll();
  }
  Workspace reloaded(dir.path());
  reloaded.load();
  EXPECT_TRUE(reloaded.errors().empty()) << reloaded.errors().front();
  ASSERT_EQ(reloaded.size(), 2U);
  EXPECT_EQ(reloaded[0].activeEnvironment, 0);
  EXPECT_EQ(View(reloaded[1].collection.name), "My API");
  EXPECT_EQ(View(reloaded.settings().theme), "nuit");
  ASSERT_EQ(reloaded.state().openTabs.size(), 1U);
  EXPECT_TRUE(reloaded.findRequest(exampleRequestId).valid());
}

TEST(Workspace, FirstLaunchIsSavedAtOnce) {
  // Quitting right after the first launch (no explicit save) must not create a second example collection next time.
  const test::TempDir dir;
  {
    Workspace workspace(dir.path());
    workspace.load();
    ASSERT_EQ(workspace.size(), 1U);
  }
  Workspace reloaded(dir.path());
  reloaded.load();
  EXPECT_TRUE(reloaded.errors().empty());
  EXPECT_EQ(reloaded.size(), 1U);
  EXPECT_EQ(std::distance(std::filesystem::directory_iterator(reloaded.collectionsDirectory()),
                          std::filesystem::directory_iterator{}),
            1);
}

TEST(Workspace, AutosaveAndOpen) {
  const test::TempDir dir;
  Workspace workspace(dir.path());
  workspace.load();
  OpenCollection& example = workspace[0];
  example.collection.name.assign(std::string_view{"Renamed"});
  Workspace::MarkDirty(example);
  EXPECT_EQ(workspace.autosave(std::chrono::hours{1}), 0U);  // not old enough
  std::this_thread::sleep_for(std::chrono::milliseconds{5});
  EXPECT_EQ(workspace.autosave(std::chrono::milliseconds{1}), 1U);
  EXPECT_FALSE(example.dirty);
  auto loaded = LoadCollection(example.path);
  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ(View(loaded->name), "Renamed");

  // Opening an already open file returns it; opening a missing one fails.
  auto same = workspace.openCollection(example.path);
  ASSERT_TRUE(same.has_value());
  EXPECT_EQ(*same, &example);
  EXPECT_FALSE(workspace.openCollection(dir.path() / "nope.zalie.json").has_value());

  // A collection referenced by workspace.json but deleted is reported, not fatal.
  workspace.saveAll();
  std::filesystem::remove(example.path);
  Workspace reloaded(dir.path());
  reloaded.load();
  EXPECT_EQ(reloaded.size(), 0U);
  EXPECT_EQ(reloaded.errors().size(), 1U);
}

TEST(Workspace, CorruptedFilesAreReported) {
  const test::TempDir dir;
  ASSERT_TRUE(WriteFileAtomically(dir.path() / "settings.json", "{oops").has_value());
  ASSERT_TRUE(WriteFileAtomically(dir.path() / "history.json", "[{").has_value());
  ASSERT_TRUE(WriteFileAtomically(dir.path() / "workspace.json", "nope").has_value());
  Workspace workspace(dir.path());
  workspace.load();
  EXPECT_EQ(workspace.errors().size(), 3U);
  EXPECT_EQ(View(workspace.settings().theme), "coton");
  EXPECT_EQ(workspace.size(), 0U);
}

}  // namespace
}  // namespace zalie
