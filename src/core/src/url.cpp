#include "zalie/url.hpp"

#include <aeronet/cctype.hpp>
#include <aeronet/memory-utils-sv.hpp>
#include <aeronet/string-equal-ignore-case.hpp>
#include <aeronet/toupperlower.hpp>
#include <aeronet/url-char-class.hpp>
#include <aeronet/url-decode.hpp>
#include <aeronet/url-encode.hpp>
#include <algorithm>
#include <cstddef>
#include <string_view>
#include <utility>

#include "zalie/key-value.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/request.hpp"

namespace zalie {

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
  char* pData = aeronet::Append(split.base, ret.data());
  char sep = '?';
  for (const KeyValue& param : params) {
    if (!param.enabled) {
      continue;
    }
    *pData++ = sep;
    sep = '&';
    pData = aeronet::Append(param.key, pData);
    if (!param.value.empty()) {
      *pData++ = '=';
      pData = aeronet::Append(param.value, pData);
    }
  }
  if (keepFragment) {
    *pData++ = '#';
    pData = aeronet::Append(split.fragment, pData);
  }
  ret.setEnd(pData);
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
  if (!spaceAsPlus) {
    out.ensureAvailableCapacityExponential(aeronet::URLEncodedSize(in, aeronet::url::IsUnreserved));
    out.setEnd(aeronet::URLEncode(in, aeronet::url::IsUnreserved, out.end()));
    return;
  }
  // The spaces are written as they are, then replaced: no other space can be in the encoded text.
  static constexpr auto kIsKept = [](char ch) noexcept { return aeronet::url::IsUnreserved(ch) || ch == ' '; };
  out.ensureAvailableCapacityExponential(aeronet::URLEncodedSize(in, kIsKept));
  char* pBegin = out.end();
  char* pEnd = aeronet::URLEncode(in, kIsKept, pBegin);
  std::replace(pBegin, pEnd, ' ', '+');
  out.setEnd(pEnd);
}

RawChars32 UrlDecode(std::string_view in, bool plusAsSpace) {
  RawChars32 ret(in);  // decoding only shrinks
  ret.setEnd(aeronet::url::DecodeInPlace(ret.data(), ret.end(), plusAsSpace ? ' ' : '+', false));
  return ret;
}

bool HasScheme(std::string_view url) noexcept {
  const std::size_t sepPos = url.find("://");
  if (sepPos == 0 || sepPos == std::string_view::npos) {
    return false;
  }
  const std::string_view scheme = url.substr(0, sepPos);
  return aeronet::islower(aeronet::tolower(scheme.front())) && std::ranges::all_of(scheme, aeronet::url::IsSchemeChar);
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

std::string_view UrlHost(std::string_view url) noexcept {
  std::string_view authority = SplitUrlTarget(url).authority;
  if (const std::size_t atPos = authority.rfind('@'); atPos != std::string_view::npos) {
    authority.remove_prefix(atPos + 1);
  }
  if (authority.starts_with('[')) {
    return authority.substr(1, authority.find(']') - 1);
  }
  return authority.substr(0, authority.find(':'));
}

bool IsLocalHost(std::string_view host) noexcept {
  static constexpr std::string_view kLocalhost = "localhost";
  static constexpr std::string_view kLoopbackPrefix = "127.";
  if (aeronet::CaseInsensitiveEqual(host, kLocalhost) ||
      (host.size() > kLocalhost.size() + 1 && host[host.size() - kLocalhost.size() - 1] == '.' &&
       aeronet::CaseInsensitiveEqual(host.substr(host.size() - kLocalhost.size()), kLocalhost))) {
    return true;
  }
  if (host == "::1") {
    return true;
  }
  return host.starts_with(kLoopbackPrefix) && std::ranges::count(host, '.') == 3 && host.back() != '.' &&
         std::ranges::all_of(host, [](char ch) { return ch == '.' || aeronet::isdigit(ch); });
}

}  // namespace zalie
