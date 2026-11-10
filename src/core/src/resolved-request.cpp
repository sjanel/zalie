#include "zalie/resolved-request.hpp"

#include <aeronet/string-equal-ignore-case.hpp>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

#include "zalie/collection.hpp"
#include "zalie/encoding.hpp"
#include "zalie/key-value.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/request.hpp"
#include "zalie/string-utils.hpp"
#include "zalie/url.hpp"
#include "zalie/variables.hpp"

namespace zalie {

namespace {

constexpr std::string_view kAuthorization = "Authorization";
constexpr std::string_view kContentType = "Content-Type";

// Resolves the fields of a request into the string pool of the output, through one reused scratch buffer.
class Resolver {
 public:
  Resolver(const VariableScope& scope, ResolvedRequest& out) noexcept : _scope(scope), _out(out) {}

  // Interpolates 'text' and returns a view of the result, stored in the pool.
  std::string_view resolve(std::string_view text) {
    if (!text.contains("{{")) {
      return _out.strings.store(text);
    }
    _scratch.clear();
    Interpolate(text, _scope, _scratch, &_unresolved);
    return _out.strings.store(_scratch);
  }

  // Interpolates 'text' at the end of 'buffer'.
  void resolveInto(std::string_view text, RawChars32& buffer) { Interpolate(text, _scope, buffer, &_unresolved); }

  // Interpolates the concatenation "<prefix><text>".
  std::string_view resolveWithPrefix(std::string_view prefix, std::string_view text) {
    _scratch.assign(prefix);
    Interpolate(text, _scope, _scratch, &_unresolved);
    return _out.strings.store(_scratch);
  }

  RawChars32& scratch() noexcept { return _scratch; }

  // Copies the names of the unresolved variables (views into the request model) into the pool.
  void finish() {
    for (std::string_view name : _unresolved) {
      _out.unresolved.push_back(_out.strings.store(name));
    }
  }

 private:
  const VariableScope& _scope;
  ResolvedRequest& _out;
  RawChars32 _scratch;
  UnresolvedNames _unresolved;
};

std::size_t EstimateStringsSize(const Request& request, const Collection& collection) noexcept {
  std::size_t size = request.url.size();
  for (const KeyValues* pHeaders : {&collection.headers, &request.headers}) {
    for (const KeyValue& header : *pHeaders) {
      size += header.key.size() + header.value.size();
    }
  }
  const Auth& auth = request.auth.mode == AuthMode::inherit ? collection.auth : request.auth;
  size += auth.username.size() + auth.password.size() + auth.token.size() + auth.apiKeyName.size() +
          auth.apiKeyValue.size();
  // Room for the expanded variables, the default headers and the base64 growth of the credentials.
  static constexpr std::size_t kSlack = 128;
  return size + (size / 4) + kSlack;
}

// Appends "name=value" (percent-encoded) to the query string of the URL.
void AppendQueryParam(ResolvedRequest& out, std::string_view name, std::string_view value) {
  const UrlQuerySplit split = SplitUrlQuery(out.url);
  RawChars32 url(out.url.size() + name.size() + value.size() + 2);
  AppendAll(url, split.base, '?');
  if (!split.query.empty()) {
    AppendAll(url, split.query, '&');
  }
  AppendUrlEncoded(name, url);
  url.push_back('=');
  AppendUrlEncoded(value, url);
  if (!split.fragment.empty()) {
    AppendAll(url, '#', split.fragment);
  }
  out.url = out.strings.store(url);
}

// Applies the authentication 'auth' to the request being resolved.
void ApplyAuth(const Auth& auth, Resolver& resolver, ResolvedRequest& out) {
  if (auth.mode != AuthMode::apiKey && HasHeader(out.headers, kAuthorization)) {
    return;  // an explicit Authorization header wins
  }
  switch (auth.mode) {
    case AuthMode::basic: {
      RawChars32 credentials;
      resolver.resolveInto(auth.username, credentials);
      credentials.push_back(':');
      resolver.resolveInto(auth.password, credentials);
      RawChars32& value = resolver.scratch();
      value.clear();
      AppendAll(value, "Basic ", Base64(credentials));
      out.headers.push_back({kAuthorization, out.strings.store(value)});
      break;
    }
    case AuthMode::bearer:
      out.headers.push_back({kAuthorization, resolver.resolveWithPrefix("Bearer ", auth.token)});
      break;
    case AuthMode::apiKey: {
      const std::string_view name = resolver.resolve(auth.apiKeyName);
      if (name.empty()) {
        break;
      }
      const std::string_view value = resolver.resolve(auth.apiKeyValue);
      if (auth.apiKeyInQuery) {
        AppendQueryParam(out, name, value);
      } else if (!HasHeader(out.headers, name)) {
        out.headers.push_back({name, value});
      }
      break;
    }
    case AuthMode::inherit:
    case AuthMode::none:
      break;
  }
}

void AddHeaders(const KeyValues& headers, Resolver& resolver, ResolvedRequest& out) {
  for (const KeyValue& header : headers) {
    const std::string_view key = Trim(header.key);
    if (!header.enabled || key.empty()) {
      continue;
    }
    const std::string_view name = resolver.resolve(key);
    const std::string_view value = resolver.resolve(header.value);
    // A request header replaces a collection header of the same name.
    const auto it = std::ranges::find_if(
        out.headers, [name](const HeaderView& line) { return aeronet::CaseInsensitiveEqual(line.name, name); });
    if (it != out.headers.end()) {
      it->value = value;
    } else {
      out.headers.push_back({name, value});
    }
  }
}

}  // namespace

std::string_view FindHeader(std::span<const HeaderView> headers, std::string_view name) noexcept {
  const auto it = std::ranges::find_if(
      headers, [name](const HeaderView& line) { return aeronet::CaseInsensitiveEqual(line.name, name); });
  return it == headers.end() ? std::string_view{} : it->value;
}

bool HasHeader(std::span<const HeaderView> headers, std::string_view name) noexcept {
  return std::ranges::any_of(headers,
                             [name](const HeaderView& line) { return aeronet::CaseInsensitiveEqual(line.name, name); });
}

VariableScope MakeScope(const Collection& collection, int environmentIndex) {
  VariableScope scope;
  if (environmentIndex >= 0 && std::cmp_less(environmentIndex, collection.environments.size())) {
    scope.addLayer(collection.environments[static_cast<std::uint32_t>(environmentIndex)].variables);
  }
  scope.addLayer(collection.variables);
  return scope;
}

ResolvedRequest ResolveRequest(const Request& request, const Collection& collection, const VariableScope& scope) {
  return ResolveRequest(request, request.url, collection, scope);
}

ResolvedRequest ResolveRequest(const Request& request, std::string_view url, const Collection& collection,
                               const VariableScope& scope) {
  ResolvedRequest out(EstimateStringsSize(request, collection));
  out.method = request.method;
  out.settings = request.settings;
  Resolver resolver(scope, out);

  RawChars32& resolvedUrl = resolver.scratch();
  resolvedUrl.clear();
  resolver.resolveInto(Trim(url), resolvedUrl);
  if (!resolvedUrl.empty() && !HasScheme(resolvedUrl)) {
    out.url = out.strings.concat({"http://", resolvedUrl});
  } else {
    out.url = out.strings.store(resolvedUrl);
  }

  AddHeaders(collection.headers, resolver, out);
  AddHeaders(request.headers, resolver, out);

  // Body (and its default Content-Type, unless one was given explicitly).
  switch (request.bodyMode) {
    case BodyMode::json:
    case BodyMode::text:
    case BodyMode::xml:
      out.body.reserve(request.body.size());
      resolver.resolveInto(request.body, out.body);
      break;
    case BodyMode::formUrlEncoded: {
      RawChars32& field = resolver.scratch();
      for (const KeyValue& entry : request.form) {
        if (!entry.enabled || entry.key.empty()) {
          continue;
        }
        if (!out.body.empty()) {
          out.body.push_back('&');
        }
        field.clear();
        resolver.resolveInto(entry.key, field);
        AppendUrlEncoded(field, out.body, true);
        out.body.push_back('=');
        field.clear();
        resolver.resolveInto(entry.value, field);
        AppendUrlEncoded(field, out.body, true);
      }
      break;
    }
    case BodyMode::none:
      break;
  }
  if (request.bodyMode != BodyMode::none && !HasHeader(out.headers, kContentType)) {
    out.headers.push_back({kContentType, DefaultContentType(request.bodyMode)});
  }

  ApplyAuth(request.auth.mode == AuthMode::inherit ? collection.auth : request.auth, resolver, out);
  resolver.finish();
  return out;
}

}  // namespace zalie
