#include "zalie/load-preview.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>

#include "test-utils.hpp"
#include "zalie/collection.hpp"
#include "zalie/data-file.hpp"
#include "zalie/file-io.hpp"
#include "zalie/key-value.hpp"
#include "zalie/load-settings.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/request.hpp"
#include "zalie/resolved-request.hpp"
#include "zalie/variables.hpp"

namespace zalie {
namespace {

LoadParameter Parameter(std::string_view name, ValueSource source, bool queryParameter = false) {
  LoadParameter parameter;
  parameter.name.assign(name);
  parameter.source = source;
  parameter.queryParameter = queryParameter;
  return parameter;
}

class LoadPreviewTest : public ::testing::Test {
 protected:
  LoadPreviewTest() {
    request.url.assign(std::string_view{"{{base}}/search?q={{term}}&page=1&lang=en&{{flag}}=1"});
    request.params.push_back(MakeKeyValue("q", "{{term}}"));
    request.params.push_back(MakeKeyValue("page", "1"));
    request.params.push_back(MakeKeyValue("lang", "en"));
    request.params.push_back(MakeKeyValue("off", "x", false));
    request.params.push_back(MakeKeyValue("{{flag}}", "1"));
    request.params.push_back(MakeKeyValue("page", "2"));  // the first one is varied
    collection.variables.push_back(MakeKeyValue("base", "http://{{host}}:{{port}}"));
    collection.variables.push_back(MakeKeyValue("host", "localhost"));
    collection.variables.push_back(MakeKeyValue("token", "secret"));
    collection.headers.push_back(MakeKeyValue("X-Trace", "{{$randomUUID}}"));
    collection.headers.push_back(MakeKeyValue("X-Off", "{{off}}", false));
    request.auth.mode = AuthMode::bearer;
    request.auth.token.assign(std::string_view{"{{token}}"});
  }

  void listRows() {
    const VariableScope scope = MakeScope(collection, -1);
    ListLoadRows(request, collection, scope, rows);
  }

  [[nodiscard]] LoadPreview preview(std::uint32_t nbRequests = 3) {
    const VariableScope scope = MakeScope(collection, -1);
    return MakeLoadPreview(request, collection, scope, *request.load, dir.path(), cache, nbRequests);
  }

  test::TempDir dir;
  Collection collection;
  Request request;
  DataFileIndexCache cache;
  amc::vector<LoadRow> rows;
};

TEST_F(LoadPreviewTest, RowsListTheVariablesThenTheQueryParameters) {
  listRows();
  ASSERT_EQ(rows.size(), 8U);
  // In order of appearance, a variable referenced by another one right after it. Dynamic variables and disabled entries
  // are left out.
  const char* const names[] = {"base", "host", "port", "term", "flag", "token", "page", "lang"};
  for (std::uint32_t idx = 0; idx < rows.size(); ++idx) {
    EXPECT_EQ(View(rows[idx].name), names[idx]) << idx;
    EXPECT_EQ(rows[idx].parameter, LoadRow::kNoParameter);
    EXPECT_FALSE(rows[idx].unused);
  }
  EXPECT_EQ(View(rows[0].defaultValue), "http://{{host}}:{{port}}");
  EXPECT_EQ(View(rows[1].defaultValue), "localhost");
  EXPECT_FALSE(rows[1].undefined);
  EXPECT_TRUE(rows[2].undefined);
  EXPECT_TRUE(rows[3].undefined);
  EXPECT_EQ(View(rows[5].defaultValue), "secret");
  EXPECT_TRUE(rows[6].queryParameter);
  EXPECT_EQ(View(rows[6].defaultValue), "1");
  EXPECT_FALSE(rows[5].queryParameter);
}

TEST_F(LoadPreviewTest, SavedParametersMatchTheirRows) {
  request.load.emplace();
  request.load->parameters.push_back(Parameter("lang", ValueSource::list, true));
  request.load->parameters.push_back(Parameter(" term ", ValueSource::list));
  request.load->parameters.push_back(Parameter("lang", ValueSource::fixed));  // a variable, not the query parameter
  request.load->parameters.push_back(Parameter("term", ValueSource::uuid));   // a duplicate
  listRows();
  ASSERT_EQ(rows.size(), 10U);
  EXPECT_EQ(rows[3].parameter, 1U);
  EXPECT_EQ(rows[7].parameter, 0U);
  EXPECT_TRUE(rows[7].queryParameter);
  EXPECT_EQ(View(rows[8].name), "lang");
  EXPECT_EQ(rows[8].parameter, 2U);
  EXPECT_FALSE(rows[8].queryParameter);
  EXPECT_TRUE(rows[8].unused);
  EXPECT_FALSE(rows[8].added);
  EXPECT_EQ(rows[9].parameter, 3U);
  EXPECT_TRUE(rows[9].unused);
}

TEST_F(LoadPreviewTest, AQueryParameterTheRequestDoesNotHaveIsAddedForTheRun) {
  request.load.emplace();
  request.load->parameters.push_back(Parameter("extra", ValueSource::list, true));
  request.load->parameters.push_back(Parameter("", ValueSource::fixed, true));  // just added, no name yet
  listRows();
  ASSERT_GE(rows.size(), 2U);
  for (std::uint32_t idx = rows.size() - 2; idx < rows.size(); ++idx) {
    EXPECT_TRUE(rows[idx].queryParameter);
    EXPECT_TRUE(rows[idx].added);
    EXPECT_FALSE(rows[idx].unused);
  }
  EXPECT_EQ(View(rows[rows.size() - 2].name), "extra");
  EXPECT_EQ(rows.back().parameter, 1U);
}

TEST_F(LoadPreviewTest, BodyFormAndBasicAuthenticationAreSearched) {
  request.url.assign(std::string_view{"http://h/"});
  request.params.clear();
  request.bodyMode = BodyMode::json;
  request.body.assign(std::string_view{R"({"id": {{id}}})"});
  request.form.push_back(MakeKeyValue("field", "{{ignored}}"));
  request.auth.mode = AuthMode::inherit;
  collection.auth.mode = AuthMode::basic;
  collection.auth.username.assign(std::string_view{"{{user}}"});
  listRows();
  ASSERT_EQ(rows.size(), 2U);
  EXPECT_EQ(View(rows[0].name), "id");
  EXPECT_EQ(View(rows[1].name), "user");

  request.bodyMode = BodyMode::formUrlEncoded;
  listRows();
  ASSERT_EQ(rows.size(), 2U);
  EXPECT_EQ(View(rows[0].name), "ignored");
}

TEST_F(LoadPreviewTest, SamplesAndFirstRequests) {
  request.url.assign(std::string_view{"http://localhost/search?q={{term}}&page=1"});
  request.params.clear();
  request.params.push_back(MakeKeyValue("q", "{{term}}"));
  request.params.push_back(MakeKeyValue("page", "1"));
  request.auth.mode = AuthMode::none;
  collection.headers.clear();
  request.bodyMode = BodyMode::text;
  request.body.assign(std::string_view{"term={{term}}"});
  request.load.emplace();
  request.load->seed = 7;
  LoadParameter terms = Parameter("term", ValueSource::list);
  terms.listValues.assign(std::string_view{"dog\ncat\nbird"});
  request.load->parameters.push_back(std::move(terms));
  LoadParameter pages = Parameter("page", ValueSource::integerRange, true);
  pages.integerFrom = 1;
  pages.integerTo = 1000;
  request.load->parameters.push_back(std::move(pages));
  LoadParameter fixed = Parameter("unused", ValueSource::fixed);
  fixed.fixedValue.assign(std::string_view{"constant"});
  request.load->parameters.push_back(std::move(fixed));

  const LoadPreview result = preview();
  ASSERT_TRUE(result.plan->valid()) << View(result.plan->issues()[0].message);
  EXPECT_EQ(result.plan->seed(), 7U);
  ASSERT_EQ(result.samples.size(), 3U);
  EXPECT_EQ(View(result.samples[0]), "dog, cat, bird");  // three values: not repeated
  EXPECT_EQ(View(result.samples[1]), "1, 2, 3, 4, 5");
  EXPECT_EQ(View(result.samples[2]), "constant");
  ASSERT_EQ(result.requests.size(), 3U);
  EXPECT_EQ(View(result.requests[0].line), "GET http://localhost/search?q=dog&page=1");
  EXPECT_EQ(View(result.requests[1].line), "GET http://localhost/search?q=cat&page=2");
  EXPECT_EQ(View(result.requests[2].body), "term=bird");
  EXPECT_FALSE(result.requests[2].refused);
  EXPECT_TRUE(result.error.empty());
}

TEST_F(LoadPreviewTest, LongValuesAreShortened) {
  request.url.assign(std::string_view{"http://localhost/{{id}}"});
  request.params.clear();
  request.auth.mode = AuthMode::none;
  request.bodyMode = BodyMode::text;
  request.body.assign(std::string(500, 'b'));
  request.load.emplace();
  LoadParameter ids = Parameter("id", ValueSource::list);
  // 39 ASCII bytes then a 2-byte character: the cut does not split it.
  ids.listValues.assign(std::string_view{"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\xC3\xA9tail"});
  request.load->parameters.push_back(std::move(ids));
  const LoadPreview result = preview(1);
  ASSERT_TRUE(result.plan->valid());
  EXPECT_EQ(View(result.samples[0]), std::string(39, 'a') + "...");
  EXPECT_EQ(result.requests[0].body.size(), LoadPreview::kMaxBodyBytes + 3);
}

TEST_F(LoadPreviewTest, RefusedValuesAndInvalidPlans) {
  {
    std::ofstream file(dir.path() / "values.jsonl");
    file << "{\"value\": \"ok\"}\n{\"value\": \"a\\nb\"}\n";
  }
  request.url.assign(std::string_view{"http://localhost/"});
  request.params.clear();
  request.auth.mode = AuthMode::none;
  collection.headers.clear();
  request.headers.push_back(MakeKeyValue("X-Value", "{{value}}"));
  request.load.emplace();
  LoadParameter values = Parameter("value", ValueSource::dataFile);
  values.filePath.assign(std::string_view{"values.jsonl"});
  values.fileFormat = DataFileFormat::jsonLines;
  request.load->parameters.push_back(std::move(values));
  const LoadPreview result = preview(2);
  ASSERT_TRUE(result.plan->valid()) << View(result.plan->issues()[0].message);
  ASSERT_EQ(result.requests.size(), 2U);
  EXPECT_FALSE(result.requests[0].refused);
  EXPECT_TRUE(result.requests[1].refused);  // a line break in a header value
  EXPECT_TRUE(View(result.requests[1].line).contains("line break")) << View(result.requests[1].line);

  // An invalid plan has no request, its issues tell why.
  request.load->parameters.push_back(Parameter("", ValueSource::uuid));
  const LoadPreview invalid = preview();
  EXPECT_FALSE(invalid.plan->valid());
  EXPECT_TRUE(invalid.requests.empty());
  EXPECT_EQ(invalid.samples.size(), 2U);
}

TEST_F(LoadPreviewTest, DataFilesAreIndexedForTheirRows) {
  {
    std::ofstream file(dir.path() / "users.csv");
    file << "id,token\n1,a\n2,b\n";
  }
  request.url.assign(std::string_view{"http://localhost/users/{{id}}"});
  request.params.clear();
  request.auth.mode = AuthMode::none;
  request.load.emplace();
  LoadParameter users = Parameter("id", ValueSource::dataFile);
  users.filePath.assign(std::string_view{"users.csv"});
  request.load->parameters.push_back(std::move(users));
  request.load->parameters.push_back(Parameter("missing", ValueSource::dataFile));  // no path yet
  const LoadPreview result = preview(2);
  ASSERT_NE(result.dataFiles[0], nullptr);
  EXPECT_EQ(result.dataFiles[0]->nbRows(), 2U);
  EXPECT_EQ(result.dataFiles[0]->columns().size(), 2U);
  EXPECT_EQ(result.dataFiles[1], nullptr);
}

TEST(DataFilePathFor, RelativeInsideTheBaseDirectory) {
  const std::filesystem::path base = std::filesystem::temp_directory_path() / "collection";
  EXPECT_EQ(View(DataFilePathFor(base / "data" / "users.csv", base)), "data/users.csv");
  EXPECT_EQ(View(DataFilePathFor(base / "." / "users.csv", base)), "users.csv");
  const std::filesystem::path outside = std::filesystem::temp_directory_path() / "other" / "users.csv";
  EXPECT_EQ(View(DataFilePathFor(outside, base)), View(PathToUtf8(outside)));
  EXPECT_EQ(View(DataFilePathFor(outside, {})), View(PathToUtf8(outside)));
}

}  // namespace
}  // namespace zalie
