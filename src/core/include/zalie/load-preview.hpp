#pragma once

#include <amc/vector.hpp>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>

#include "zalie/data-file.hpp"
#include "zalie/load-plan.hpp"
#include "zalie/load-settings.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/variables.hpp"

namespace zalie {

struct Collection;
struct Request;

// A row of the parameter table of a run: a {{variable}} of the request or one of its query parameters, with its saved
// parameter when it has one.
struct LoadRow {
  static constexpr std::uint32_t kNoParameter = LoadPlanIssue::kNoParameter;

  RawChars32 name;
  // What the request sends without a saved parameter: the value of the variable, or the one of the query parameter.
  RawChars32 defaultValue;
  // Its parameter in the run settings, or kNoParameter.
  std::uint32_t parameter{kNoParameter};
  bool queryParameter{false};
  // A variable defined nowhere: without a saved parameter, the run cannot start.
  bool undefined{false};
  // A saved parameter that the request does not use any more.
  bool unused{false};

  using trivially_relocatable = AllTriviallyRelocatable<RawChars32>;
};

// Fills 'rows' with the parameter table of a request: its {{variables}} in order of appearance (with those referenced
// by the values of other variables, dynamic ones excepted), its query parameters whose key and value have no
// {{variable}}, then the saved parameters matching none of them.
void ListLoadRows(const Request& request, const Collection& collection, const VariableScope& scope,
                  amc::vector<LoadRow>& rows);

// One of the first requests of a run, as it will be sent.
struct LoadPreviewRequest {
  // "GET https://...", or why the request cannot be sent.
  RawChars32 line;
  // The beginning of its body.
  RawChars32 body;
  bool refused{false};

  using trivially_relocatable = AllTriviallyRelocatable<RawChars32>;
};

// What the run settings show before a run, computed with its plan: a few values of each parameter and the first
// requests. Immutable once built.
struct LoadPreview {
  static constexpr std::uint32_t kNbSamples = 5;
  static constexpr std::uint32_t kMaxSampleBytes = 40;
  static constexpr std::uint32_t kMaxBodyBytes = 200;

  // Never null.
  std::shared_ptr<const LoadPlan> plan;
  // Per parameter of the settings: its first values joined by ", ".
  amc::vector<RawChars32> samples;
  // Per parameter of the settings: the index of its data file, null for another source.
  amc::vector<std::shared_ptr<const DataFileIndex>> dataFiles;
  amc::vector<LoadPreviewRequest> requests;
  // Why the values cannot be computed at all (a data file changed since it was indexed...).
  RawChars32 error;
};

// Compiles the plan of a run and its preview. Data files not indexed yet are scanned, which can take long: call it off
// the UI thread.
[[nodiscard]] LoadPreview MakeLoadPreview(const Request& request, const Collection& collection,
                                          const VariableScope& scope, const LoadSettings& settings,
                                          const std::filesystem::path& baseDirectory, DataFileIndexCache& dataFiles,
                                          std::uint32_t nbRequests);

// The path to save for a data file: relative to 'baseDirectory' with '/' separators when the file is inside it (git
// friendly), absolute otherwise.
[[nodiscard]] RawChars32 DataFilePathFor(const std::filesystem::path& file, const std::filesystem::path& baseDirectory);

}  // namespace zalie
