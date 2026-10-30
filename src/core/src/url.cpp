#include "zalie/url.hpp"

#include <algorithm>
#include <cstddef>
#include <string_view>
#include <utility>

#include "zalie/key-value.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/request.hpp"

namespace zalie {

namespace {

constexpr bool IsUnreserved(char ch) noexcept {
  return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '.' ||
         ch == '_' || ch == '~';
}

constexpr int HexValue(char ch) noexcept {
  if (ch >= '0' && ch <= '9') {
    return ch - '0';
  }
  if (ch >= 'a' && ch <= 'f') {
    return ch - 'a' + 10;
  }
  if (ch >= 'A' && ch <= 'F') {
    return ch - 'A' + 10;
  }
  return -1;
}

constexpr bool IsSchemeChar(char ch) noexcept {
  return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '+' || ch == '-' ||
         ch == '.';
}

}  // namespace

UrlQuerySplit SplitUrlQuery(std::string_view url) noexcept {
  UrlQuerySplit split;
  const std::size_t hashPos = url.find('#');
  if (hashPos != std::string_view::npos) {
    split.fragment = url.substr(hashPos + 1);
    url = url.substr(0, hashPos);
  }
  const std::size_t queryPos = url.find('?');
  if (queryPos == std::string_view::npos) {
    split.base = url;
  } else {
    split.base = url.substr(0, queryPos);
    split.query = url.substr(queryPos + 1);
    split.hasQuery = true;
  }
  return split;
}

void ParseQuery(std::string_view query, KeyValues& out) {
  while (!query.empty()) {
    const std::size_t ampPos = query.find('&');
    const std::string_view pair = query.substr(0, ampPos);
    query = ampPos == std::string_view::npos ? std::string_view{} : query.substr(ampPos + 1);
    if (pair.empty()) {
      continue;
    }
    const std::size_t eqPos = pair.find('=');
    if (eqPos == std::string_view::npos) {
      out.push_back(MakeKeyValue(pair, {}));
    } else {
      out.push_back(MakeKeyValue(pair.substr(0, eqPos), pair.substr(eqPos + 1)));
    }
  }
}

RawChars32 ComposeUrl(std::string_view url, const KeyValues& params) {
  const UrlQuerySplit split = SplitUrlQuery(url);
  const bool keepFragment = !split.fragment.empty() || url.ends_with('#');
  std::size_t size = split.base.size() + (keepFragment ? split.fragment.size() + 1 : 0);
  for (const KeyValue& param : params) {
    size += param.key.size() + param.value.size() + 2;  // separator + '='
  }
  RawChars32 ret(size);
  ret.unchecked_append(split.base);
  char sep = '?';
  for (const KeyValue& param : params) {
    if (!param.enabled) {
      continue;
    }
    ret.unchecked_push_back(sep);
    sep = '&';
    ret.unchecked_append(param.key);
    if (!param.value.empty()) {
      ret.unchecked_push_back('=');
      ret.unchecked_append(param.value);
    }
  }
  if (keepFragment) {
    ret.unchecked_push_back('#');
    ret.unchecked_append(split.fragment);
  }
  return ret;
}

void SyncParamsFromUrl(Request& req) {
  KeyValues params;
  ParseQuery(SplitUrlQuery(req.url).query, params);
  for (KeyValue& param : req.params) {
    if (!param.enabled) {
      params.push_back(std::move(param));
    }
  }
  req.params = std::move(params);
}

void SyncUrlFromParams(Request& req) { req.url = ComposeUrl(req.url, req.params); }

void AppendUrlEncoded(std::string_view in, RawChars32& out, bool spaceAsPlus) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  const auto nbEncoded = static_cast<std::size_t>(std::ranges::count_if(in, [](char ch) { return !IsUnreserved(ch); }));
  out.ensureAvailableCapacity(in.size() + (2 * nbEncoded));
  for (char ch : in) {
    if (IsUnreserved(ch)) {
      out.unchecked_push_back(ch);
    } else if (ch == ' ' && spaceAsPlus) {
      out.unchecked_push_back('+');
    } else {
      const auto byte = static_cast<unsigned char>(ch);
      out.unchecked_push_back('%');
      out.unchecked_push_back(kHex[byte >> 4U]);
      out.unchecked_push_back(kHex[byte & 0xFU]);
    }
  }
}

RawChars32 UrlDecode(std::string_view in, bool plusAsSpace) {
  RawChars32 ret(in.size());
  for (std::size_t i = 0; i < in.size(); ++i) {
    const char ch = in[i];
    if (ch == '%' && i + 2 < in.size()) {
      const int high = HexValue(in[i + 1]);
      const int low = HexValue(in[i + 2]);
      if (high >= 0 && low >= 0) {
        ret.unchecked_push_back(static_cast<char>((high << 4) | low));
        i += 2;
        continue;
      }
    }
    ret.unchecked_push_back(ch == '+' && plusAsSpace ? ' ' : ch);
  }
  return ret;
}

bool HasScheme(std::string_view url) noexcept {
  const std::size_t sepPos = url.find("://");
  if (sepPos == 0 || sepPos == std::string_view::npos) {
    return false;
  }
  const std::string_view scheme = url.substr(0, sepPos);
  const bool startsWithLetter = (scheme[0] >= 'a' && scheme[0] <= 'z') || (scheme[0] >= 'A' && scheme[0] <= 'Z');
  return startsWithLetter && std::ranges::all_of(scheme, IsSchemeChar);
}

UrlTarget SplitUrlTarget(std::string_view url) noexcept {
  UrlTarget ret;
  if (HasScheme(url)) {
    const std::size_t sepPos = url.find("://");
    ret.scheme = url.substr(0, sepPos);
    url.remove_prefix(sepPos + 3);
  }
  url = url.substr(0, url.find('#'));
  const std::size_t targetPos = url.find_first_of("/?");
  ret.authority = url.substr(0, targetPos);
  ret.target = targetPos == std::string_view::npos ? std::string_view{"/"} : url.substr(targetPos);
  return ret;
}

}  // namespace zalie
