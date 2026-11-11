#pragma once

#include <amc/vector.hpp>
#include <cstddef>
#include <span>
#include <string_view>

#include "zalie/method.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/request.hpp"
#include "zalie/string-pool.hpp"
#include "zalie/variables.hpp"

namespace zalie {

struct Collection;

// A header line, as views (the case of the name is kept for display and snippets).
struct HeaderView {
  bool operator==(const HeaderView&) const = default;

  std::string_view name;
  std::string_view value;
};

using HeaderViews = amc::vector<HeaderView>;

// Returns the value of the first header with this name (ASCII case-insensitive), or an empty view.
[[nodiscard]] std::string_view FindHeader(std::span<const HeaderView> headers, std::string_view name) noexcept;

// Whether a header with this name (ASCII case-insensitive) is present.
[[nodiscard]] bool HasHeader(std::span<const HeaderView> headers, std::string_view name) noexcept;

// A request ready to be sent: variables resolved, authentication and collection headers applied.
// This is what the executor sends and what the code snippets describe. Move-only: its small strings live in 'strings'.
struct ResolvedRequest {
  ResolvedRequest() = default;
  explicit ResolvedRequest(std::size_t stringsCapacity) : strings(stringsCapacity) {}

  [[nodiscard]] std::string_view headerValue(std::string_view name) const noexcept { return FindHeader(headers, name); }

  // Owns the URL, the header names and values and the unresolved names (views of the fields below).
  StringPool strings;
  // Absolute URL with its query string.
  std::string_view url;
  // Final headers, in sending order, Content-Type included when there is a body.
  HeaderViews headers;
  // Variables referenced but not defined (sent verbatim).
  amc::vector<std::string_view> unresolved;
  // Owned separately from the small strings: it may be large, and is handed as-is to the HTTP client (no copy).
  RawChars32 body;
  RequestSettings settings;
  Method method{Method::get};
};

// Resolves 'request' of 'collection' with the variables of 'scope'.
[[nodiscard]] ResolvedRequest ResolveRequest(const Request& request, const Collection& collection,
                                             const VariableScope& scope);

// Same, with 'url' and 'body' in place of the URL and of the text, JSON or XML body of the request.
[[nodiscard]] ResolvedRequest ResolveRequest(const Request& request, std::string_view url, std::string_view body,
                                             const Collection& collection, const VariableScope& scope);

// Builds the variable scope of a collection: active environment (if 'environmentIndex' is valid) then collection
// variables. The scope refers to the collection strings.
[[nodiscard]] VariableScope MakeScope(const Collection& collection, int environmentIndex);

}  // namespace zalie
