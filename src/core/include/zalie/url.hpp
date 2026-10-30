#pragma once

#include <string_view>

#include "zalie/key-value.hpp"
#include "zalie/raw-chars.hpp"

namespace zalie {

struct Request;

// Views on the parts of a URL around its query string. 'query' excludes the '?', 'fragment' excludes the '#'.
struct UrlQuerySplit {
  std::string_view base;
  std::string_view query;
  std::string_view fragment;
  bool hasQuery{false};
};

// Splits a URL on its first '?' and '#'. Never fails, works on partially typed URLs and on {{variables}}.
[[nodiscard]] UrlQuerySplit SplitUrlQuery(std::string_view url) noexcept;

// Appends the parameters of a raw query string ("a=1&b=2", not decoded) to 'out', all enabled.
void ParseQuery(std::string_view query, KeyValues& out);

// Rebuilds 'url' with a query made of the enabled 'params' (raw, not encoded). The fragment is kept.
[[nodiscard]] RawChars32 ComposeUrl(std::string_view url, const KeyValues& params);

// After an edit of the URL: re-reads its query into req.params. Disabled params are kept, at the end.
void SyncParamsFromUrl(Request& req);

// After an edit of the params table: rewrites the query of req.url from the enabled params.
void SyncUrlFromParams(Request& req);

// Percent-encodes 'in' (RFC 3986 unreserved characters are kept) and appends it to 'out'.
// With 'spaceAsPlus', spaces become '+' (application/x-www-form-urlencoded).
void AppendUrlEncoded(std::string_view in, RawChars32& out, bool spaceAsPlus = false);

// Decodes %XX sequences (and '+' as space if 'plusAsSpace'). Invalid sequences are kept verbatim.
[[nodiscard]] RawChars32 UrlDecode(std::string_view in, bool plusAsSpace = false);

// Whether the URL starts with a scheme ("http://", "https://", ...).
[[nodiscard]] bool HasScheme(std::string_view url) noexcept;

// The parts of an absolute http(s) URL needed to display a raw HTTP request. Views into the URL.
struct UrlTarget {
  std::string_view scheme;
  std::string_view authority;
  // Path + query, never empty ("/" by default). Excludes the fragment. Starts with '?' for a URL with a query but no
  // path ("host?x=1"): the wire target is then '/' followed by it.
  std::string_view target;
};

// Splits an absolute URL into scheme, authority and request target. Without scheme, the URL is taken as authority.
[[nodiscard]] UrlTarget SplitUrlTarget(std::string_view url) noexcept;

}  // namespace zalie
