#include "zalie/snippets.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <optional>
#include <string_view>

#include "zalie/method.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/request.hpp"
#include "zalie/resolved-request.hpp"
#include "zalie/string-utils.hpp"
#include "zalie/url.hpp"

namespace zalie {

namespace {

constexpr std::string_view kContentType = "Content-Type";
constexpr std::string_view kContinuation = " \\\n  ";

// Headers managed by the HTTP stack itself: never set by hand in the generated code.
bool IsTransportHeader(std::string_view name) noexcept {
  return EqualsIgnoreCase(name, "Host") || EqualsIgnoreCase(name, "Content-Length") ||
         EqualsIgnoreCase(name, "Transfer-Encoding") || EqualsIgnoreCase(name, "TE") ||
         EqualsIgnoreCase(name, "Trailer") || EqualsIgnoreCase(name, "Upgrade") || EqualsIgnoreCase(name, "Expect") ||
         EqualsIgnoreCase(name, "Connection");
}

bool HasNonAscii(std::string_view str) noexcept {
  return std::ranges::any_of(str, [](char ch) { return static_cast<unsigned char>(ch) >= 0x80U; });
}

bool IsDefaultTimeout(const RequestSettings& settings) noexcept {
  return settings.timeoutMs == RequestSettings::kDefaultTimeoutMs;
}

// "30" or "2.5": a timeout in seconds without useless decimals.
void AppendSeconds(RawChars32& out, std::uint32_t milliseconds) {
  AppendInt(out, milliseconds / 1000U);
  std::uint32_t fraction = milliseconds % 1000U;
  if (fraction == 0) {
    return;
  }
  out.push_back('.');
  for (std::uint32_t divisor = 100; fraction != 0; divisor /= 10) {
    out.push_back(static_cast<char>('0' + (fraction / divisor)));
    fraction %= divisor;
  }
}

void AppendShellQuotedHeader(const HeaderView& header, std::string_view sep, RawChars32& out) {
  const RawChars32 line = Concat({header.name, sep, header.value});
  AppendShellQuoted(line, out);
}

// Single-quoted string for PowerShell: quotes are doubled.
void AppendPowerShellQuoted(std::string_view str, RawChars32& out) {
  out.ensureAvailableCapacity(str.size() + 2);
  out.unchecked_push_back('\'');
  for (char ch : str) {
    if (ch == '\'') {
      out.push_back('\'');
    }
    out.push_back(ch);
  }
  out.push_back('\'');
}

// C++ string literal. Control characters use octal escapes (hexadecimal ones would swallow following hex digits).
void AppendCppQuoted(std::string_view str, RawChars32& out) {
  out.ensureAvailableCapacity(str.size() + 2);
  out.unchecked_push_back('"');
  for (char ch : str) {
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
          std::format_to(std::back_inserter(out), "\\{:03o}", static_cast<unsigned char>(ch));
        } else {
          out.push_back(ch);
        }
        break;
    }
  }
  out.push_back('"');
}

// C++ literal for a (possibly multi-line) body: a raw string literal when possible, for readability.
void AppendCppBodyLiteral(std::string_view body, RawChars32& out) {
  const bool rawPossible = std::ranges::none_of(
      body, [](char ch) { return static_cast<unsigned char>(ch) < 0x20U && ch != '\n' && ch != '\t'; });
  if (!rawPossible) {
    AppendCppQuoted(body, out);
    return;
  }
  RawChars32 delimiter(std::string_view{"zalie"});
  for (int suffix = 0; body.contains(View(Concat({")", delimiter, "\""}))); ++suffix) {
    delimiter.assign(std::string_view{"zalie"});
    AppendInt(delimiter, suffix);
  }
  AppendAll(out, "R\"", delimiter, "(", body, ")", delimiter, "\"");
}

void GenerateCurl(const ResolvedRequest& req, RawChars32& out) {
  out.append(std::string_view{"curl"});
  if (req.method == Method::head) {
    out.append(std::string_view{" --head"});
  } else if (req.method != Method::get || !req.body.empty()) {
    AppendAll(out, " -X ", MethodToStr(req.method));
  }
  out.push_back(' ');
  AppendShellQuoted(req.url, out);
  for (const HeaderView& header : req.headers) {
    AppendAll(out, kContinuation, "-H ");
    AppendShellQuotedHeader(header, ": ", out);
  }
  if (!req.body.empty()) {
    AppendAll(out, kContinuation, "--data-raw ");
    AppendShellQuoted(req.body, out);
  }
  const RequestSettings& settings = req.settings;
  if (settings.followRedirects) {
    AppendAll(out, kContinuation, "-L");
  }
  if (!settings.verifyTls) {
    AppendAll(out, kContinuation, "-k");
  }
  if (settings.httpVersion == HttpVersion::http1) {
    AppendAll(out, kContinuation, "--http1.1");
  } else if (settings.httpVersion == HttpVersion::http2) {
    AppendAll(out, kContinuation, View(req.url).starts_with("https") ? "--http2" : "--http2-prior-knowledge");
  }
  if (!IsDefaultTimeout(settings)) {
    AppendAll(out, kContinuation, "--max-time ");
    AppendSeconds(out, settings.timeoutMs);
  }
  out.push_back('\n');
}

void GenerateHttpie(const ResolvedRequest& req, RawChars32& out) {
  out.append(std::string_view{"http"});
  const RequestSettings& settings = req.settings;
  if (settings.followRedirects) {
    out.append(std::string_view{" --follow"});
  }
  if (!settings.verifyTls) {
    out.append(std::string_view{" --verify=no"});
  }
  if (!IsDefaultTimeout(settings)) {
    out.append(std::string_view{" --timeout="});
    AppendSeconds(out, settings.timeoutMs);
  }
  AppendAll(out, " ", MethodToStr(req.method), " ");
  AppendShellQuoted(req.url, out);
  for (const HeaderView& header : req.headers) {
    out.append(kContinuation);
    // 'Name;' sends an empty header with HTTPie ('Name:' would remove it).
    if (header.value.empty()) {
      AppendShellQuoted(View(Concat({header.name, ";"})), out);
    } else {
      AppendShellQuotedHeader(header, ":", out);
    }
  }
  if (!req.body.empty()) {
    AppendAll(out, kContinuation, "--raw ");
    AppendShellQuoted(req.body, out);
  }
  out.push_back('\n');
}

void GenerateWget(const ResolvedRequest& req, RawChars32& out) {
  AppendAll(out, "wget --quiet", kContinuation, "--method=", MethodToStr(req.method));
  for (const HeaderView& header : req.headers) {
    AppendAll(out, kContinuation, "--header=");
    AppendShellQuotedHeader(header, ": ", out);
  }
  if (!req.body.empty()) {
    AppendAll(out, kContinuation, "--body-data=");
    AppendShellQuoted(req.body, out);
  }
  const RequestSettings& settings = req.settings;
  if (!settings.followRedirects) {
    AppendAll(out, kContinuation, "--max-redirect=0");
  }
  if (!settings.verifyTls) {
    AppendAll(out, kContinuation, "--no-check-certificate");
  }
  if (!IsDefaultTimeout(settings)) {
    AppendAll(out, kContinuation, "--timeout=");
    AppendSeconds(out, settings.timeoutMs);
  }
  AppendAll(out, kContinuation, "--output-document=-", kContinuation);
  AppendShellQuoted(req.url, out);
  out.push_back('\n');
}

void GeneratePowerShell(const ResolvedRequest& req, RawChars32& out) {
  // PowerShell wants the Content-Type through -ContentType, not in the headers dictionary.
  const std::string_view contentType = req.headerValue(kContentType);
  bool hasHeaders = false;
  for (const HeaderView& header : req.headers) {
    if (EqualsIgnoreCase(header.name, kContentType)) {
      continue;
    }
    if (!hasHeaders) {
      out.append(std::string_view{"$headers = @{\n"});
      hasHeaders = true;
    }
    out.append(std::string_view{"    "});
    AppendPowerShellQuoted(header.name, out);
    out.append(std::string_view{" = "});
    AppendPowerShellQuoted(header.value, out);
    out.push_back('\n');
  }
  if (hasHeaders) {
    out.append(std::string_view{"}\n"});
  }
  if (!req.body.empty()) {
    out.append(std::string_view{"$body = "});
    AppendPowerShellQuoted(req.body, out);
    out.push_back('\n');
  }
  if (hasHeaders || !req.body.empty()) {
    out.push_back('\n');
  }
  out.append(std::string_view{"$response = Invoke-WebRequest -Uri "});
  AppendPowerShellQuoted(req.url, out);
  if (req.method == Method::connect) {
    out.append(std::string_view{" -CustomMethod 'CONNECT'"});
  } else {
    AppendAll(out, " -Method '", MethodToStr(req.method), "'");
  }
  if (hasHeaders) {
    out.append(std::string_view{" -Headers $headers"});
  }
  if (!contentType.empty()) {
    out.append(std::string_view{" -ContentType "});
    AppendPowerShellQuoted(contentType, out);
  }
  if (!req.body.empty()) {
    out.append(std::string_view{" -Body $body"});
  }
  const RequestSettings& settings = req.settings;
  if (!settings.followRedirects) {
    out.append(std::string_view{" -MaximumRedirection 0"});
  }
  if (!settings.verifyTls) {
    out.append(std::string_view{" -SkipCertificateCheck"});
  }
  if (!IsDefaultTimeout(settings)) {
    out.append(std::string_view{" -TimeoutSec "});
    AppendInt(out, std::max(1U, (settings.timeoutMs + 999U) / 1000U));
  }
  out.append(std::string_view{"\n$response.StatusCode\n$response.Content\n"});
}

void GeneratePython(const ResolvedRequest& req, RawChars32& out) {
  out.append(std::string_view{"import requests\n\nurl = "});
  AppendDoubleQuoted(req.url, out);
  out.push_back('\n');
  if (!req.headers.empty()) {
    out.append(std::string_view{"headers = {\n"});
    for (const HeaderView& header : req.headers) {
      out.append(std::string_view{"    "});
      AppendDoubleQuoted(header.name, out);
      out.append(std::string_view{": "});
      AppendDoubleQuoted(header.value, out);
      out.append(std::string_view{",\n"});
    }
    out.append(std::string_view{"}\n"});
  }
  if (!req.body.empty()) {
    out.append(std::string_view{"payload = "});
    AppendDoubleQuoted(req.body, out);
    out.push_back('\n');
  }
  out.append(std::string_view{"\nresponse = requests.request("});
  AppendDoubleQuoted(MethodToStr(req.method), out);
  out.append(std::string_view{", url"});
  if (!req.headers.empty()) {
    out.append(std::string_view{", headers=headers"});
  }
  if (!req.body.empty()) {
    // A str body is sent as latin-1 by http.client: encode it explicitly when it is not plain ASCII.
    out.append(std::string_view{HasNonAscii(req.body) ? ", data=payload.encode(\"utf-8\")" : ", data=payload"});
  }
  const RequestSettings& settings = req.settings;
  out.append(std::string_view{", timeout="});
  AppendSeconds(out, settings.timeoutMs);
  if (!settings.followRedirects) {
    out.append(std::string_view{", allow_redirects=False"});
  }
  if (!settings.verifyTls) {
    out.append(std::string_view{", verify=False"});
  }
  out.append(std::string_view{")\n\nprint(response.status_code)\nprint(response.text)\n"});
}

void GenerateJavaScript(const ResolvedRequest& req, RawChars32& out) {
  const RequestSettings& settings = req.settings;
  if (!settings.verifyTls) {
    out.append(std::string_view{
        "// TLS verification is disabled for this request: with Node.js, run with NODE_TLS_REJECT_UNAUTHORIZED=0\n"});
  }
  out.append(std::string_view{"const response = await fetch("});
  AppendDoubleQuoted(req.url, out);
  const bool hasOptions = req.method != Method::get || !req.headers.empty() || !req.body.empty() ||
                          !settings.followRedirects || !IsDefaultTimeout(settings);
  if (hasOptions) {
    out.append(std::string_view{", {\n"});
    if (req.method != Method::get) {
      out.append(std::string_view{"  method: "});
      AppendDoubleQuoted(MethodToStr(req.method), out);
      out.append(std::string_view{",\n"});
    }
    if (!req.headers.empty()) {
      out.append(std::string_view{"  headers: {\n"});
      for (const HeaderView& header : req.headers) {
        out.append(std::string_view{"    "});
        AppendDoubleQuoted(header.name, out);
        out.append(std::string_view{": "});
        AppendDoubleQuoted(header.value, out);
        out.append(std::string_view{",\n"});
      }
      out.append(std::string_view{"  },\n"});
    }
    if (!req.body.empty()) {
      out.append(std::string_view{"  body: "});
      AppendDoubleQuoted(req.body, out);
      out.append(std::string_view{",\n"});
    }
    if (!settings.followRedirects) {
      out.append(std::string_view{"  redirect: \"manual\",\n"});
    }
    if (!IsDefaultTimeout(settings)) {
      out.append(std::string_view{"  signal: AbortSignal.timeout("});
      AppendInt(out, settings.timeoutMs);
      out.append(std::string_view{"),\n"});
    }
    out.push_back('}');
  }
  out.append(std::string_view{");\n\nconsole.log(response.status);\nconsole.log(await response.text());\n"});
}

void GenerateGo(const ResolvedRequest& req, RawChars32& out) {
  const RequestSettings& settings = req.settings;
  const bool hasBody = !req.body.empty();
  out.append(std::string_view{"package main\n\nimport (\n"});
  if (!settings.verifyTls) {
    out.append(std::string_view{"\t\"crypto/tls\"\n"});
  }
  out.append(std::string_view{"\t\"fmt\"\n\t\"io\"\n\t\"net/http\"\n"});
  if (hasBody) {
    out.append(std::string_view{"\t\"strings\"\n"});
  }
  out.append(std::string_view{"\t\"time\"\n)\n\nfunc main() {\n"});
  if (hasBody) {
    out.append(std::string_view{"\tpayload := strings.NewReader("});
    AppendDoubleQuoted(req.body, out);
    out.append(std::string_view{")\n\n"});
  }
  out.append(std::string_view{"\treq, err := http.NewRequest("});
  AppendDoubleQuoted(MethodToStr(req.method), out);
  out.append(std::string_view{", "});
  AppendDoubleQuoted(req.url, out);
  out.append(std::string_view{hasBody ? ", payload)\n" : ", nil)\n"});
  out.append(std::string_view{"\tif err != nil {\n\t\tpanic(err)\n\t}\n"});
  for (const HeaderView& header : req.headers) {
    out.append(std::string_view{"\treq.Header.Add("});
    AppendDoubleQuoted(header.name, out);
    out.append(std::string_view{", "});
    AppendDoubleQuoted(header.value, out);
    out.append(std::string_view{")\n"});
  }
  out.append(std::string_view{"\n\tclient := &http.Client{\n\t\tTimeout: "});
  if (settings.timeoutMs % 1000U == 0) {
    AppendInt(out, settings.timeoutMs / 1000U);
    out.append(std::string_view{" * time.Second,\n"});
  } else {
    AppendInt(out, settings.timeoutMs);
    out.append(std::string_view{" * time.Millisecond,\n"});
  }
  if (!settings.followRedirects) {
    out.append(
        std::string_view{"\t\tCheckRedirect: func(req *http.Request, via []*http.Request) error {\n"
                         "\t\t\treturn http.ErrUseLastResponse\n\t\t},\n"});
  }
  if (!settings.verifyTls) {
    out.append(
        std::string_view{"\t\tTransport: &http.Transport{TLSClientConfig: &tls.Config{InsecureSkipVerify: true}},\n"});
  }
  out.append(std::string_view{
      "\t}\n\tres, err := client.Do(req)\n\tif err != nil {\n\t\tpanic(err)\n\t}\n\tdefer res.Body.Close()\n\n"
      "\tbody, err := io.ReadAll(res.Body)\n\tif err != nil {\n\t\tpanic(err)\n\t}\n"
      "\tfmt.Println(res.Status)\n\tfmt.Println(string(body))\n}\n"});
}

void GenerateCppAeronet(const ResolvedRequest& req, RawChars32& out) {
  const RequestSettings& settings = req.settings;
  out.append(std::string_view{"#include <aeronet/aeronet-client.hpp>\n\n"});
  const bool customConfig = !settings.followRedirects || !settings.verifyTls || !IsDefaultTimeout(settings) ||
                            settings.httpVersion != HttpVersion::automatic || !settings.decompress;
  if (!IsDefaultTimeout(settings)) {
    out.append(std::string_view{"#include <chrono>\n"});
  }
  out.append(std::string_view{"#include <iostream>\n#include <utility>\n\nint main() {\n"});
  if (customConfig) {
    out.append(std::string_view{"  aeronet::HttpClientConfig config;\n"});
    if (!IsDefaultTimeout(settings)) {
      for (std::string_view field : {std::string_view{"connectTimeout"}, std::string_view{"requestTimeout"}}) {
        AppendAll(out, "  config.", field, " = std::chrono::milliseconds{");
        AppendInt(out, settings.timeoutMs);
        out.append(std::string_view{"};\n"});
      }
    }
    if (!settings.followRedirects) {
      out.append(std::string_view{"  config.followRedirects = false;\n"});
    }
    if (!settings.verifyTls) {
      out.append(std::string_view{"  config.tlsVerifyPeer = false;\n"});
    }
    if (settings.httpVersion == HttpVersion::http1) {
      out.append(std::string_view{"  config.httpVersion = aeronet::HttpVersionMode::Http1_1;\n"});
    } else if (settings.httpVersion == HttpVersion::http2) {
      out.append(std::string_view{"  config.httpVersion = aeronet::HttpVersionMode::Http2;\n"});
    }
    if (!settings.decompress) {
      out.append(std::string_view{"  config.withDecompression(false);\n"});
    }
    out.append(std::string_view{"  aeronet::HttpClient client(std::move(config));\n\n"});
  } else {
    out.append(std::string_view{"  aeronet::HttpClient client;\n\n"});
  }
  AppendAll(out, "  aeronet::HttpRequest request = client.makeRequest(aeronet::http::Method::", MethodToStr(req.method),
            ", ");
  AppendCppQuoted(req.url, out);
  out.append(std::string_view{");\n"});
  for (const HeaderView& header : req.headers) {
    if (EqualsIgnoreCase(header.name, kContentType)) {
      continue;  // given with the body
    }
    if (IsTransportHeader(header.name)) {
      AppendAll(out, "  // '", header.name, "' is managed by the client\n");
      continue;
    }
    // aeronet header names are lower-case (checked at compile time for literals).
    out.append(std::string_view{"  request.headerAddLine("});
    AppendCppQuoted(ToLower(header.name), out);
    out.append(std::string_view{", "});
    AppendCppQuoted(header.value, out);
    out.append(std::string_view{");\n"});
  }
  if (!req.body.empty()) {
    out.append(std::string_view{"  request.body("});
    AppendCppBodyLiteral(req.body, out);
    const std::string_view contentType = req.headerValue(kContentType);
    if (!contentType.empty()) {
      out.append(std::string_view{", "});
      AppendCppQuoted(contentType, out);
    }
    out.append(std::string_view{");\n"});
  }
  out.append(std::string_view{
      "\n  auto result = client.request(std::move(request));\n"
      "  if (!result) {\n"
      "    std::cerr << \"request failed: \" << aeronet::ErrcToStr(result.error()) << '\\n';\n"
      "    return 1;\n"
      "  }\n"
      "  std::cout << result->status() << ' ' << result->reason() << '\\n' << result->bodyInMemory() << '\\n';\n"
      "}\n"});
}

void GenerateRawHttp(const ResolvedRequest& req, RawChars32& out) {
  const UrlTarget target = SplitUrlTarget(req.url);
  AppendAll(out, MethodToStr(req.method), target.target.starts_with('?') ? " /" : " ", target.target,
            " HTTP/1.1\r\nHost: ", target.authority, "\r\n");
  for (const HeaderView& header : req.headers) {
    if (EqualsIgnoreCase(header.name, "Host") || EqualsIgnoreCase(header.name, "Content-Length")) {
      continue;
    }
    AppendAll(out, header.name, ": ", header.value, "\r\n");
  }
  if (!req.body.empty() || MethodUsuallyHasBody(req.method)) {
    out.append(std::string_view{"Content-Length: "});
    AppendInt(out, req.body.size());
    out.append(std::string_view{"\r\n"});
  }
  AppendAll(out, "\r\n", req.body);
}

}  // namespace

std::string_view SnippetLanguageName(SnippetLanguage language) noexcept {
  switch (language) {
    case SnippetLanguage::curl:
      return "cURL";
    case SnippetLanguage::httpie:
      return "HTTPie";
    case SnippetLanguage::wget:
      return "Wget";
    case SnippetLanguage::powershell:
      return "PowerShell";
    case SnippetLanguage::python:
      return "Python (requests)";
    case SnippetLanguage::javascript:
      return "JavaScript (fetch)";
    case SnippetLanguage::go:
      return "Go (net/http)";
    case SnippetLanguage::cppAeronet:
      return "C++ (aeronet)";
    case SnippetLanguage::rawHttp:
      return "Raw HTTP";
  }
  return "cURL";
}

std::string_view SnippetLanguageId(SnippetLanguage language) noexcept {
  switch (language) {
    case SnippetLanguage::curl:
      return "curl";
    case SnippetLanguage::httpie:
      return "httpie";
    case SnippetLanguage::wget:
      return "wget";
    case SnippetLanguage::powershell:
      return "powershell";
    case SnippetLanguage::python:
      return "python";
    case SnippetLanguage::javascript:
      return "javascript";
    case SnippetLanguage::go:
      return "go";
    case SnippetLanguage::cppAeronet:
      return "cpp-aeronet";
    case SnippetLanguage::rawHttp:
      return "http";
  }
  return "curl";
}

std::optional<SnippetLanguage> ParseSnippetLanguage(std::string_view id) noexcept {
  for (SnippetLanguage language : kSnippetLanguages) {
    if (SnippetLanguageId(language) == id) {
      return language;
    }
  }
  return std::nullopt;
}

void AppendShellQuoted(std::string_view str, RawChars32& out) {
  const auto nbQuotes = static_cast<std::size_t>(std::ranges::count(str, '\''));
  static constexpr std::string_view kEscapedQuote = "'\\''";
  out.ensureAvailableCapacity(str.size() + 2 + (nbQuotes * (kEscapedQuote.size() - 1)));
  out.unchecked_push_back('\'');
  for (char ch : str) {
    if (ch == '\'') {
      out.unchecked_append(kEscapedQuote);
    } else {
      out.unchecked_push_back(ch);
    }
  }
  out.unchecked_push_back('\'');
}

void AppendDoubleQuoted(std::string_view str, RawChars32& out) {
  out.ensureAvailableCapacity(str.size() + 2);
  out.unchecked_push_back('"');
  for (char ch : str) {
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
      case '\b':
        out.append(std::string_view{"\\b"});
        break;
      case '\f':
        out.append(std::string_view{"\\f"});
        break;
      default:
        if (static_cast<unsigned char>(ch) < 0x20U) {
          std::format_to(std::back_inserter(out), "\\u{:04x}", static_cast<unsigned char>(ch));
        } else {
          out.push_back(ch);
        }
        break;
    }
  }
  out.push_back('"');
}

void GenerateSnippet(SnippetLanguage language, const ResolvedRequest& request, RawChars32& out) {
  out.clear();
  switch (language) {
    case SnippetLanguage::curl:
      GenerateCurl(request, out);
      break;
    case SnippetLanguage::httpie:
      GenerateHttpie(request, out);
      break;
    case SnippetLanguage::wget:
      GenerateWget(request, out);
      break;
    case SnippetLanguage::powershell:
      GeneratePowerShell(request, out);
      break;
    case SnippetLanguage::python:
      GeneratePython(request, out);
      break;
    case SnippetLanguage::javascript:
      GenerateJavaScript(request, out);
      break;
    case SnippetLanguage::go:
      GenerateGo(request, out);
      break;
    case SnippetLanguage::cppAeronet:
      GenerateCppAeronet(request, out);
      break;
    case SnippetLanguage::rawHttp:
      GenerateRawHttp(request, out);
      break;
  }
}

}  // namespace zalie
