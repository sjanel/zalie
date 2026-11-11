#include "zalie/load-preview.hpp"

#include <algorithm>
#include <amc/vector.hpp>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>

#include "zalie/collection.hpp"
#include "zalie/data-file.hpp"
#include "zalie/file-io.hpp"
#include "zalie/key-value.hpp"
#include "zalie/load-plan.hpp"
#include "zalie/load-settings.hpp"
#include "zalie/method.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/request.hpp"
#include "zalie/string-utils.hpp"
#include "zalie/value-generators.hpp"
#include "zalie/variables.hpp"

namespace zalie {

namespace {

constexpr int kMaxReferenceDepth = 8;
constexpr std::string_view kSampleSeparator = ", ";

// Adds to 'names' the variables referenced by 'text', and by the values of these variables. Dynamic ones are left out:
// they are evaluated again for every request.
void CollectNames(std::string_view text, const VariableScope& scope, amc::vector<std::string_view>& names,
                  int depth = 0) {
  VariableRefs refs;
  FindVariableRefs(text, refs);
  for (const VariableRef& ref : refs) {
    if (ref.name.empty() || IsDynamicVariable(ref.name) || std::ranges::contains(names, ref.name)) {
      continue;
    }
    names.push_back(ref.name);
    if (const std::string_view* pValue = scope.find(ref.name); pValue != nullptr && depth < kMaxReferenceDepth) {
      CollectNames(*pValue, scope, names, depth + 1);
    }
  }
}

void CollectNames(const KeyValues& entries, const VariableScope& scope, amc::vector<std::string_view>& names) {
  for (const KeyValue& entry : entries) {
    if (entry.enabled) {
      CollectNames(entry.key, scope, names);
      CollectNames(entry.value, scope, names);
    }
  }
}

// The beginning of 'value', cut on a character boundary.
[[nodiscard]] std::string_view Shorten(std::string_view value, std::size_t maxBytes) noexcept {
  if (value.size() <= maxBytes) {
    return value;
  }
  std::size_t size = maxBytes;
  while (size > 0 && (static_cast<unsigned char>(value[size]) & 0xC0U) == 0x80U) {
    --size;
  }
  return value.substr(0, size);
}

void AppendSample(std::string_view value, RawChars32& out) {
  const std::string_view shown = Shorten(value, LoadPreview::kMaxSampleBytes);
  const std::string_view ellipsis = shown.size() < value.size() ? std::string_view{"..."} : std::string_view{};
  if (out.empty()) {
    AppendAll(out, shown, ellipsis);
  } else {
    AppendAll(out, kSampleSeparator, shown, ellipsis);
  }
}

}  // namespace

void ListLoadRows(const Request& request, const Collection& collection, const VariableScope& scope,
                  amc::vector<LoadRow>& rows) {
  rows.clear();
  amc::vector<std::string_view> names;
  CollectNames(request.url, scope, names);
  CollectNames(collection.headers, scope, names);
  CollectNames(request.headers, scope, names);
  if (request.bodyMode == BodyMode::formUrlEncoded) {
    CollectNames(request.form, scope, names);
  } else if (request.bodyMode != BodyMode::none) {
    CollectNames(request.body, scope, names);
  }
  const Auth& auth = request.auth.mode == AuthMode::inherit ? collection.auth : request.auth;
  switch (auth.mode) {
    case AuthMode::basic:
      CollectNames(auth.username, scope, names);
      CollectNames(auth.password, scope, names);
      break;
    case AuthMode::bearer:
      CollectNames(auth.token, scope, names);
      break;
    case AuthMode::apiKey:
      CollectNames(auth.apiKeyName, scope, names);
      CollectNames(auth.apiKeyValue, scope, names);
      break;
    case AuthMode::inherit:
    case AuthMode::none:
      break;
  }
  rows.reserve(names.size() + request.params.size());
  for (std::string_view name : names) {
    const std::string_view* pValue = scope.find(name);
    rows.push_back({.name = RawChars32(name),
                    .defaultValue = pValue == nullptr ? RawChars32{} : RawChars32(*pValue),
                    .undefined = pValue == nullptr});
  }
  for (const KeyValue& param : request.params) {
    const std::string_view key = Trim(param.key);
    if (!param.enabled || key.empty() || View(param.key).contains("{{") || View(param.value).contains("{{") ||
        std::ranges::any_of(rows, [key](const LoadRow& row) {
          return row.target == ParameterTarget::queryParameter && View(row.name) == key;
        })) {
      continue;
    }
    rows.push_back({.name = RawChars32(key), .defaultValue = param.value, .target = ParameterTarget::queryParameter});
  }

  if (!request.load) {
    return;
  }
  const amc::vector<LoadParameter>& parameters = request.load->parameters;
  for (std::uint32_t idx = 0; idx < parameters.size(); ++idx) {
    const LoadParameter& parameter = parameters[idx];
    const std::string_view name = Trim(parameter.name);
    const bool ofTheRequest =
        parameter.target == ParameterTarget::variable || parameter.target == ParameterTarget::queryParameter;
    if (ofTheRequest) {
      auto it = std::ranges::find_if(rows, [&](const LoadRow& row) {
        return row.parameter == LoadRow::kNoParameter && row.target == parameter.target && View(row.name) == name;
      });
      if (it != rows.end()) {
        it->parameter = idx;
        continue;
      }
    }
    rows.push_back({.name = RawChars32(name),
                    .parameter = idx,
                    .target = parameter.target,
                    .unused = parameter.target == ParameterTarget::variable,
                    .added = parameter.target != ParameterTarget::variable});
    LoadRow& row = rows.back();
    if (parameter.target == ParameterTarget::body) {
      row.defaultValue.assign(Shorten(request.body, LoadPreview::kMaxBodyBytes));
    } else if (parameter.target == ParameterTarget::jsonField && request.bodyMode == BodyMode::json && !name.empty()) {
      if (const auto spans = JsonFieldSpans(request.body, name); spans) {
        const TextSpan& first = spans->front();
        row.defaultValue.assign(View(request.body).substr(first.begin, first.end - first.begin));
      }
    }
  }
}

LoadPreview MakeLoadPreview(const Request& request, const Collection& collection, const VariableScope& scope,
                            const LoadSettings& settings, const std::filesystem::path& baseDirectory,
                            DataFileIndexCache& dataFiles, std::uint32_t nbRequests) {
  LoadPreview ret;
  ret.plan = std::make_shared<const LoadPlan>(request, collection, scope, settings, baseDirectory, dataFiles);
  const LoadPlan& plan = *ret.plan;
  const std::span<const LoadParameter> parameters(settings.parameters.data(), settings.parameters.size());
  ret.samples.resize(static_cast<std::uint32_t>(parameters.size()));
  ret.dataFiles.resize(static_cast<std::uint32_t>(parameters.size()));
  for (std::uint32_t idx = 0; idx < parameters.size(); ++idx) {
    const LoadParameter& parameter = parameters[idx];
    if (parameter.source == ValueSource::fixed) {
      AppendSample(parameter.fixedValue, ret.samples[idx]);
    } else if (parameter.source == ValueSource::dataFile && !Trim(parameter.filePath).empty()) {
      ret.dataFiles[idx] =
          dataFiles.get(ResolveDataFilePath(Trim(parameter.filePath), baseDirectory), parameter.fileFormat);
    }
  }
  if (!plan.valid()) {
    return ret;
  }

  ValueGenerators::Cursor cursor(plan.generators());
  if (!cursor.error().empty()) {
    ret.error.assign(cursor.error());
    return ret;
  }
  for (std::uint64_t number = 0; number < LoadPreview::kNbSamples; ++number) {
    if (!cursor.fill(number)) {
      break;
    }
    const std::span<const std::string_view> values = cursor.values();
    for (std::uint32_t idx = 0; idx < parameters.size(); ++idx) {
      const std::uint32_t variable = plan.parameterVariable(idx);
      const std::uint64_t count = plan.valueCount(idx);
      if (variable != ValueGenerators::kNoVariable && (count == 0 || number < count)) {
        AppendSample(values[variable], ret.samples[idx]);
      }
    }
  }

  LoadRequestFiller filler(plan);
  ret.requests.reserve(nbRequests);
  for (std::uint64_t number = 0; number < nbRequests; ++number) {
    LoadPreviewRequest& preview = ret.requests.emplace_back();
    if (!filler.fill(number)) {
      preview.line.assign(filler.error());
      preview.refused = true;
      continue;
    }
    AppendAll(preview.line, MethodToStr(plan.method()), std::string_view{" "}, filler.url());
    const std::string_view body = Shorten(filler.body(), LoadPreview::kMaxBodyBytes);
    AppendAll(preview.body, body, body.size() < filler.body().size() ? std::string_view{"..."} : std::string_view{});
  }
  return ret;
}

RawChars32 DataFilePathFor(const std::filesystem::path& file, const std::filesystem::path& baseDirectory) {
  if (!baseDirectory.empty() && file.is_absolute()) {
    const std::filesystem::path relative = file.lexically_normal().lexically_relative(baseDirectory.lexically_normal());
    if (!relative.empty() && *relative.begin() != "..") {
      const std::u8string utf8 = relative.generic_u8string();
      return RawChars32(std::string_view(reinterpret_cast<const char*>(utf8.data()), utf8.size()));
    }
  }
  return PathToUtf8(file);
}

}  // namespace zalie
