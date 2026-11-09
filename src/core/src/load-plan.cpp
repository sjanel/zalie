#include "zalie/load-plan.hpp"

#include <aeronet/base64.hpp>
#include <algorithm>
#include <amc/vector.hpp>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <limits>
#include <random>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>

#include "http-client-setup.hpp"
#include "zalie/collection.hpp"
#include "zalie/data-file.hpp"
#include "zalie/key-value.hpp"
#include "zalie/load-settings.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/request.hpp"
#include "zalie/resolved-request.hpp"
#include "zalie/string-pool.hpp"
#include "zalie/string-utils.hpp"
#include "zalie/url.hpp"
#include "zalie/value-generators.hpp"
#include "zalie/variables.hpp"

namespace zalie {

namespace {

constexpr std::string_view kHeaderForbidden("\r\n\0", 3);
constexpr std::string_view kHttpScheme = "http://";
constexpr std::string_view kBasicPrefix = "Basic ";
constexpr std::string_view kAuthorization = "Authorization";
constexpr int kMaxReferenceDepth = 8;
// ": " and the end of line around each value of a header.
constexpr std::size_t kHeaderLineOverhead = 4;
constexpr char kSentinelSeparator = '-';

constexpr std::string_view kRefusedValue = "a generated value holds a line break or a null byte";
constexpr std::string_view kUnreadableRow = "a row of the data file cannot be read";

[[nodiscard]] std::uint64_t NewSeed() {
  std::random_device device;
  const std::uint64_t seed = (static_cast<std::uint64_t>(device()) << 32U) | device();
  return seed != 0 ? seed : 1;
}

void AppendJsonEscaped(std::string_view value, RawChars32& out) {
  static constexpr char kHex[] = "0123456789abcdef";
  for (char ch : value) {
    switch (ch) {
      case '"':
        out.append(std::string_view{"\\\""});
        break;
      case '\\':
        out.append(std::string_view{"\\\\"});
        break;
      case '\n':
        out.append(std::string_view{"\\n"});
        break;
      case '\r':
        out.append(std::string_view{"\\r"});
        break;
      case '\t':
        out.append(std::string_view{"\\t"});
        break;
      default:
        if (static_cast<unsigned char>(ch) < 0x20U) {
          const auto code = static_cast<unsigned char>(ch);
          out.append(std::string_view{"\\u00"});
          out.push_back(kHex[code >> 4U]);
          out.push_back(kHex[code & 0xFU]);
        } else {
          out.push_back(ch);
        }
        break;
    }
  }
}

void AppendBasicValue(std::string_view credentials, RawChars32& out) {
  const std::size_t encodedLen = aeronet::B64EncodedLen(credentials.size());
  out.ensureAvailableCapacityExponential(kBasicPrefix.size() + encodedLen);
  out.unchecked_append(kBasicPrefix);
  char* pEncoded = out.data() + out.size();
  aeronet::B64Encode(credentials, pEncoded, pEncoded + encodedLen);
  out.setEnd(pEncoded + encodedLen);
}

// A value bound to a sentinel while the request is resolved.
struct SlotBinding {
  // Name of a dynamic variable, each occurrence of which has its own value. Empty for the variable of a parameter.
  std::string_view dynamicName;
  std::uint32_t variable{ValueGenerators::kNoVariable};
};

// Builds the templates of a request resolved with sentinels in place of the generated values.
class TemplateSplitter {
 public:
  TemplateSplitter(std::string_view prefix, amc::vector<SlotBinding>& bindings, ValueGenerators& generators) noexcept
      : _prefix(prefix), _bindings(bindings), _generators(generators) {}

  [[nodiscard]] bool contains(std::string_view text) const noexcept { return text.contains(_prefix); }

  // Splits 'text' on its sentinels. encodingOf(literal, position) gives the encoding of the value that follows the
  // literal, 'position' being its offset in 'text'.
  template <class EncodingOf>
  [[nodiscard]] TextTemplate split(std::string_view text, EncodingOf encodingOf) {
    TextTemplate ret;
    std::size_t begin = 0;
    while (true) {
      const std::size_t sentinel = text.find(_prefix, begin);
      if (sentinel == std::string_view::npos) {
        ret.addPart({.literal = text.substr(begin)});
        return ret;
      }
      const std::string_view literal = text.substr(begin, sentinel - begin);
      const char* pDigits = text.data() + sentinel + _prefix.size();
      std::uint32_t bindingIndex = 0;
      const auto res = std::from_chars(pDigits, text.data() + text.size(), bindingIndex);
      const SlotEncoding encoding = encodingOf(literal, sentinel);
      ret.addPart({.literal = literal, .variable = variableOf(bindingIndex), .encoding = encoding});
      begin = static_cast<std::size_t>(res.ptr - text.data()) + 1;  // skips the final separator
    }
  }

 private:
  [[nodiscard]] std::uint32_t variableOf(std::uint32_t bindingIndex) {
    const SlotBinding& binding = _bindings[bindingIndex];
    return binding.dynamicName.empty() ? binding.variable : _generators.addDynamic(binding.dynamicName);
  }

  std::string_view _prefix;
  amc::vector<SlotBinding>& _bindings;
  ValueGenerators& _generators;
};

// End of the part of 'url' that designates the server: a value before it is written as it is, so that a variable can
// hold a host, a port or a whole URL.
[[nodiscard]] std::size_t AuthorityEnd(std::string_view url) noexcept {
  const std::size_t schemeEnd = url.find("://");
  const std::size_t start = schemeEnd == std::string_view::npos ? 0 : schemeEnd + 3;
  return std::min(url.find_first_of("/?#", start), url.size());
}

// Replaces in 'url' the value of the first query parameter 'key' by 'reference'. Returns false if there is none.
[[nodiscard]] bool ReplaceQueryValue(RawChars32& url, std::string_view key, std::string_view reference) {
  const UrlQuerySplit split = SplitUrlQuery(url);
  std::string_view rest = split.query;
  while (!rest.empty()) {
    const std::size_t ampPos = rest.find('&');
    const std::string_view pair = rest.substr(0, ampPos);
    const std::size_t eqPos = pair.find('=');
    if (pair.substr(0, eqPos) == key) {
      const std::string_view before(url.data(), static_cast<std::size_t>(pair.data() - url.data()));
      const std::string_view after(pair.data() + pair.size(),
                                   static_cast<std::size_t>(url.data() + url.size() - (pair.data() + pair.size())));
      RawChars32 replaced = Concat({before, key, "=", reference, after});
      url.swap(replaced);
      return true;
    }
    rest = ampPos == std::string_view::npos ? std::string_view{} : rest.substr(ampPos + 1);
  }
  return false;
}

// Whether 'name' changes from a request to the next. The environment variables are constant during a run.
[[nodiscard]] bool VariesPerRequest(std::string_view name) noexcept {
  return IsDynamicVariable(name) && !name.starts_with("$env.") && !name.starts_with("process.env.");
}

// Adds to 'names' the varying dynamic variables referenced by 'text', through the values of the other variables too.
void CollectDynamicNames(std::string_view text, const VariableScope& scope, amc::vector<std::string_view>& names,
                         int depth = 0) {
  VariableRefs refs;
  FindVariableRefs(text, refs);
  for (const VariableRef& ref : refs) {
    const std::string_view* pValue = scope.find(ref.name);
    if (pValue != nullptr) {
      if (depth < kMaxReferenceDepth) {
        CollectDynamicNames(*pValue, scope, names, depth + 1);
      }
    } else if (VariesPerRequest(ref.name) && !std::ranges::contains(names, ref.name)) {
      names.push_back(ref.name);
    }
  }
}

void CollectDynamicNames(const KeyValues& entries, const VariableScope& scope, amc::vector<std::string_view>& names) {
  for (const KeyValue& entry : entries) {
    if (entry.enabled) {
      CollectDynamicNames(entry.key, scope, names);
      CollectDynamicNames(entry.value, scope, names);
    }
  }
}

}  // namespace

bool TextTemplate::appendTo(std::span<const std::string_view> values, RawChars32& out) const {
  for (const TemplatePart& part : _parts) {
    out.append(part.literal);
    if (part.variable == ValueGenerators::kNoVariable) {
      continue;
    }
    const std::string_view value = values[part.variable];
    switch (part.encoding) {
      case SlotEncoding::checked:
        if (value.find_first_of(kHeaderForbidden) != std::string_view::npos) {
          return false;
        }
        out.append(value);
        break;
      case SlotEncoding::raw:
        out.append(value);
        break;
      case SlotEncoding::url:
        AppendUrlEncoded(value, out);
        break;
      case SlotEncoding::form:
        AppendUrlEncoded(value, out, true);
        break;
      case SlotEncoding::jsonString:
        AppendJsonEscaped(value, out);
        break;
    }
  }
  return true;
}

LoadPlan::LoadPlan(ResolvedRequest request) : _resolved(std::move(request)), _generators(CombineMode::independent, 1) {
  _url.addPart({.literal = _resolved.url});
  _body.addPart({.literal = View(_resolved.body)});
  _host = UrlHost(_resolved.url);
  compileHeaders({}, {});
}

LoadPlan::LoadPlan(const Request& request, const Collection& collection, const VariableScope& scope,
                   const LoadSettings& settings, const std::filesystem::path& baseDirectory,
                   DataFileIndexCache& dataFiles)
    : _generators(settings.combine, settings.seed != 0 ? settings.seed : NewSeed()) {
  const RawChars32 prefix = Concat({"zalie-", RandomUuid(), "-"});
  StringPool scopeStrings;
  amc::vector<SlotBinding> bindings;
  const auto newSentinel = [&](SlotBinding binding) {
    RawChars32 sentinel(prefix);
    AppendInt(sentinel, bindings.size());
    sentinel.push_back(kSentinelSeparator);
    bindings.push_back(binding);
    return scopeStrings.store(sentinel);
  };

  // The parameters come first: they take precedence over the variables of the environment and of the collection.
  VariableScope runScope;
  RawChars32 url(Trim(request.url));
  _parameterVariables.resize(settings.parameters.size(), ValueGenerators::kNoVariable);
  for (std::uint32_t idx = 0; idx < settings.parameters.size(); ++idx) {
    const LoadParameter& parameter = settings.parameters[idx];
    const std::string_view key = Trim(parameter.name);
    std::string_view name = key;
    if (name.empty()) {
      _issues.push_back({.message = RawChars32(std::string_view{"The parameter has no name"}), .parameter = idx});
      continue;
    }
    if (parameter.queryParameter) {
      RawChars32 varyName(std::string_view{"$vary."});
      AppendInt(varyName, idx);
      name = scopeStrings.store(varyName);
      if (!ReplaceQueryValue(url, key, scopeStrings.concat({"{{", name, "}}"}))) {
        _issues.push_back({.message = Concat({"No query parameter '", key, "'"}), .parameter = idx});
        continue;
      }
    }
    if (parameter.source == ValueSource::fixed) {
      runScope.add(name, parameter.fixedValue);
      continue;
    }
    RawChars32 error;
    const std::uint32_t variable = _generators.add(parameter, baseDirectory, dataFiles, error);
    if (!error.empty()) {
      _issues.push_back({.message = std::move(error), .parameter = idx});
      runScope.add(name, {});  // it has a value: the error above is enough
      continue;
    }
    _parameterVariables[idx] = variable;
    runScope.add(name, newSentinel({.variable = variable}));
  }
  runScope.addScope(scope);

  // The dynamic variables are evaluated again for every request.
  const Auth& auth = request.auth.mode == AuthMode::inherit ? collection.auth : request.auth;
  amc::vector<std::string_view> dynamicNames;
  CollectDynamicNames(url, runScope, dynamicNames);
  CollectDynamicNames(collection.headers, runScope, dynamicNames);
  CollectDynamicNames(request.headers, runScope, dynamicNames);
  if (request.bodyMode == BodyMode::formUrlEncoded) {
    CollectDynamicNames(request.form, runScope, dynamicNames);
  } else {
    CollectDynamicNames(request.body, runScope, dynamicNames);
  }
  for (const RawChars32* pField : {&auth.username, &auth.password, &auth.token, &auth.apiKeyName, &auth.apiKeyValue}) {
    CollectDynamicNames(*pField, runScope, dynamicNames);
  }
  for (std::string_view name : dynamicNames) {
    runScope.add(name, newSentinel({.dynamicName = name}));
  }

  _resolved = ResolveRequest(request, url, collection, runScope);
  for (std::string_view name : _resolved.unresolved) {
    _issues.push_back({.message = Concat({"The variable '", name, "' has no value"})});
  }
  if (_resolved.url.empty()) {
    _issues.push_back({.message = RawChars32(std::string_view{"The URL is empty"})});
  }
  if (!_issues.empty()) {
    return;
  }

  TemplateSplitter splitter(prefix, bindings, _generators);

  // A URL starting with a generated value can receive a whole URL: its scheme is only known once filled.
  std::string_view resolvedUrl = _resolved.url;
  if (resolvedUrl.starts_with(kHttpScheme) && resolvedUrl.substr(kHttpScheme.size()).starts_with(View(prefix)) &&
      !HasScheme(Interpolate(Trim(url), runScope))) {
    resolvedUrl.remove_prefix(kHttpScheme.size());
    _urlNeedsScheme = true;
  }
  const std::size_t authorityEnd = AuthorityEnd(resolvedUrl);
  if (!_urlNeedsScheme && !resolvedUrl.substr(0, authorityEnd).contains(View(prefix))) {
    _host = UrlHost(resolvedUrl);
  }
  _url = splitter.split(resolvedUrl, [authorityEnd](std::string_view /*literal*/, std::size_t position) {
    return position < authorityEnd ? SlotEncoding::checked : SlotEncoding::url;
  });

  bool inJsonString = false;
  _body = splitter.split(_resolved.body, [&](std::string_view literal, std::size_t /*position*/) {
    switch (request.bodyMode) {
      case BodyMode::formUrlEncoded:
        return SlotEncoding::form;
      case BodyMode::json:
        for (std::size_t idx = 0; idx < literal.size(); ++idx) {
          if (inJsonString && literal[idx] == '\\') {
            ++idx;
          } else if (literal[idx] == '"') {
            inJsonString = !inJsonString;
          }
        }
        return inJsonString ? SlotEncoding::jsonString : SlotEncoding::raw;
      default:
        return SlotEncoding::raw;
    }
  });

  // Base64 hides the sentinels: Basic credentials with generated values are compiled from their source.
  RawChars32 credentials;
  std::string_view basicCredentials;
  if (auth.mode == AuthMode::basic) {
    Interpolate(auth.username, runScope, credentials);
    credentials.push_back(':');
    Interpolate(auth.password, runScope, credentials);
    if (splitter.contains(credentials)) {
      basicCredentials = _resolved.strings.store(credentials);
    }
  }
  compileHeaders(prefix, basicCredentials);
  if (!_issues.empty()) {
    return;
  }

  for (LoadHeader& header : _headers) {
    if (header.isBasicAuth) {
      header.basicCredentials =
          splitter.split(basicCredentials, [](std::string_view, std::size_t) { return SlotEncoding::raw; });
    } else {
      header.value =
          splitter.split(header.value.literal(), [](std::string_view, std::size_t) { return SlotEncoding::checked; });
    }
  }

  _isConstant = _url.isConstant() && _body.isConstant() && std::ranges::all_of(_headers, [](const LoadHeader& header) {
                  return !header.isBasicAuth && header.value.isConstant();
                });
  _generators.finalize();
}

void LoadPlan::compileHeaders(std::string_view sentinelPrefix, std::string_view basicCredentials) {
  RawChars32 lowerName;
  RawChars32 expectedBasic;
  if (!basicCredentials.empty()) {
    AppendBasicValue(basicCredentials, expectedBasic);
  }
  for (const HeaderView& header : _resolved.headers) {
    if (!sentinelPrefix.empty() && header.name.contains(sentinelPrefix)) {
      // The name of a header is validated once, when the request is compiled.
      _issues.push_back({.message = RawChars32(std::string_view{"A generated value cannot be the name of a header"})});
      continue;
    }
    const bool isBasicAuth = !basicCredentials.empty() && EqualsIgnoreCase(header.name, kAuthorization) &&
                             header.value == View(expectedBasic);
    const HeaderUse use = ClassifyHeader(header.name, header.value, lowerName);
    if (use == HeaderUse::invalid || use == HeaderUse::managedByClient) {
      _warnings.push_back(Concat({HeaderSkipReason(use), header.name}));
      continue;
    }
    LoadHeader compiled;
    compiled.isContentType = use == HeaderUse::contentType;
    compiled.isBasicAuth = isBasicAuth;
    if (!compiled.isContentType) {
      compiled.lowerName = _resolved.strings.store(lowerName);
      compiled.replaces = compiled.lowerName == "user-agent";
    }
    compiled.value.addPart({.literal = header.value});
    _headerLinesBytes += header.name.size() + kHeaderLineOverhead;
    _headers.push_back(std::move(compiled));
  }
}

std::uint64_t LoadPlan::valueCount(std::uint32_t parameter) const noexcept {
  const std::uint32_t variable = _parameterVariables[parameter];
  return variable == ValueGenerators::kNoVariable ? 1 : _generators.valueCount(variable);
}

std::expected<ResolvedRequest, RawChars32> LoadPlan::preview(std::uint64_t number) const {
  LoadRequestFiller filler(*this);
  if (!filler.setupError().empty()) {
    return std::unexpected(RawChars32(filler.setupError()));
  }
  if (!filler.fill(number)) {
    return std::unexpected(RawChars32(filler.error()));
  }
  ResolvedRequest out;
  out.method = _resolved.method;
  out.settings = _resolved.settings;
  out.url = out.strings.store(filler.url());
  for (std::uint32_t idx = 0; idx < _headers.size(); ++idx) {
    const std::string_view name =
        _headers[idx].isContentType ? std::string_view{"content-type"} : _headers[idx].lowerName;
    out.headers.push_back({out.strings.store(name), out.strings.store(filler.headerValue(idx))});
  }
  out.body.append(filler.body());
  return out;
}

LoadRequestFiller::LoadRequestFiller(const LoadPlan& plan) : _pPlan(&plan), _cursor(plan.generators()) {
  _headerEnds.resize(static_cast<std::uint32_t>(plan.headers().size()) + 1U, 0U);
}

bool LoadRequestFiller::fill(std::uint64_t number) {
  const LoadPlan& plan = *_pPlan;
  if (!_cursor.fill(number)) {
    _error = kUnreadableRow;
    return false;
  }
  const std::span<const std::string_view> values = _cursor.values();
  _url.clear();
  if (!plan.url().appendTo(values, _url)) {
    _error = kRefusedValue;
    return false;
  }
  if (plan.urlNeedsScheme() && !HasScheme(_url)) {
    RawChars32 withScheme = Concat({kHttpScheme, _url});
    _url.swap(withScheme);
  }
  _headerValues.clear();
  const std::span<const LoadHeader> headers = plan.headers();
  for (std::uint32_t idx = 0; idx < headers.size(); ++idx) {
    if (headers[idx].isBasicAuth) {
      _scratch.clear();
      if (!headers[idx].basicCredentials.appendTo(values, _scratch)) {
        _error = kRefusedValue;
        return false;
      }
      AppendBasicValue(_scratch, _headerValues);
    } else if (!headers[idx].value.appendTo(values, _headerValues)) {
      _error = kRefusedValue;
      return false;
    }
    _headerEnds[idx + 1U] = _headerValues.size();
  }
  _body.clear();
  if (!plan.body().appendTo(values, _body)) {
    _error = kRefusedValue;
    return false;
  }
  return true;
}

}  // namespace zalie
