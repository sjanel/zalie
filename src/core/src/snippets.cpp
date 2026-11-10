#include "zalie/snippets.hpp"

#include <aeronet/decimal-writer.hpp>
#include <aeronet/ndigits.hpp>
#include <aeronet/string-equal-ignore-case.hpp>
#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "zalie/encoding.hpp"
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
  static constexpr std::array<std::string_view, 8> kNames{
      "Host", "Content-Length", "Transfer-Encoding", "TE", "Trailer", "Upgrade", "Expect", "Connection"};
  return std::ranges::any_of(
      kNames, [name](std::string_view transport) { return aeronet::CaseInsensitiveEqual(name, transport); });
}

bool HasNonAscii(std::string_view str) noexcept {
  return std::ranges::any_of(str, [](char ch) { return static_cast<unsigned char>(ch) >= 0x80U; });
}

bool IsDefaultTimeout(const RequestSettings& settings) noexcept {
  return settings.timeoutMs == RequestSettings::kDefaultTimeoutMs;
}

// A duration in seconds without useless decimals ("30", "2.5"): a part of AppendAll.
class Seconds {
 public:
  explicit constexpr Seconds(std::uint32_t milliseconds) noexcept : _milliseconds(milliseconds) {}

  // The seconds, the dot and 3 decimals.
  [[nodiscard]] constexpr std::size_t maxSize() const noexcept { return aeronet::ndigits(_milliseconds / 1000U) + 4U; }

  [[nodiscard]] char* writeTo(char* pData) const noexcept {
    const std::uint32_t seconds = _milliseconds / 1000U;
    pData = aeronet::WriteU32(pData, seconds, aeronet::ndigits(seconds));
    std::uint32_t fraction = _milliseconds % 1000U;
    if (fraction != 0) {
      *pData++ = '.';
      for (std::uint32_t divisor = 100; fraction != 0; divisor /= 10) {
        *pData++ = static_cast<char>('0' + (fraction / divisor));
        fraction %= divisor;
      }
    }
    return pData;
  }

 private:
  std::uint32_t _milliseconds;
};

// A single-quoted word of a POSIX shell.
constexpr Escaped ShellQuoted(std::string_view str) noexcept { return {str, kShellEscapes, '\''}; }

// A single-quoted string of PowerShell.
constexpr Escaped PowerShellQuoted(std::string_view str) noexcept { return {str, kPowerShellEscapes, '\''}; }

// A double-quoted literal with C-like escapes, valid in JSON, JavaScript, Python and Go.
constexpr Escaped DoubleQuoted(std::string_view str) noexcept { return {str, kJsonEscapes, '"'}; }

// A C++ string literal.
constexpr Escaped CppQuoted(std::string_view str) noexcept { return {str, kCppEscapes, '"'}; }

// "<option>'Name<sep>value'": a header as one shell word.
void AppendShellHeader(std::string_view option, const HeaderView& header, std::string_view sep, RawChars32& out) {
  AppendAll(out, kContinuation, option, '\'', Escaped(header.name, kShellEscapes), sep,
            Escaped(header.value, kShellEscapes), '\'');
}

// C++ literal for a (possibly multi-line) body: a raw string literal when possible, for readability.
void AppendCppBodyLiteral(std::string_view body, RawChars32& out) {
  const bool rawPossible = std::ranges::none_of(
      body, [](char ch) { return static_cast<unsigned char>(ch) < 0x20U && ch != '\n' && ch != '\t'; });
  if (!rawPossible) {
    AppendAll(out, CppQuoted(body));
    return;
  }
  // The delimiter must not end the literal before its end: "zalie", else "zalie0", "zalie1"...
  RawChars32 delimiter(std::string_view{"zalie"});
  for (int suffix = 0; body.contains(View(Concat({")", delimiter, "\""}))); ++suffix) {
    delimiter.clear();
    AppendAll(delimiter, "zalie", suffix);
  }
  AppendAll(out, "R\"", delimiter, '(', body, ')', delimiter, '"');
}

void GenerateCurl(const ResolvedRequest& req, RawChars32& out) {
  AppendAll(out, "curl");
  if (req.method == Method::head) {
    AppendAll(out, " --head");
  } else if (req.method != Method::get || !req.body.empty()) {
    AppendAll(out, " -X ", MethodToStr(req.method));
  }
  AppendAll(out, ' ', ShellQuoted(req.url));
  for (const HeaderView& header : req.headers) {
    AppendShellHeader("-H ", header, ": ", out);
  }
  if (!req.body.empty()) {
    AppendAll(out, kContinuation, "--data-raw ", ShellQuoted(req.body));
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
    AppendAll(out, kContinuation,
              std::string_view{req.url.starts_with("https") ? "--http2" : "--http2-prior-knowledge"});
  }
  if (!IsDefaultTimeout(settings)) {
    AppendAll(out, kContinuation, "--max-time ", Seconds(settings.timeoutMs));
  }
  out.push_back('\n');
}

void GenerateHttpie(const ResolvedRequest& req, RawChars32& out) {
  AppendAll(out, "http");
  const RequestSettings& settings = req.settings;
  if (settings.followRedirects) {
    AppendAll(out, " --follow");
  }
  if (!settings.verifyTls) {
    AppendAll(out, " --verify=no");
  }
  if (!IsDefaultTimeout(settings)) {
    AppendAll(out, " --timeout=", Seconds(settings.timeoutMs));
  }
  AppendAll(out, ' ', MethodToStr(req.method), ' ', ShellQuoted(req.url));
  for (const HeaderView& header : req.headers) {
    // 'Name;' sends an empty header with HTTPie ('Name:' would remove it).
    AppendShellHeader({}, header, header.value.empty() ? ";" : ":", out);
  }
  if (!req.body.empty()) {
    AppendAll(out, kContinuation, "--raw ", ShellQuoted(req.body));
  }
  out.push_back('\n');
}

void GenerateWget(const ResolvedRequest& req, RawChars32& out) {
  AppendAll(out, "wget --quiet", kContinuation, "--method=", MethodToStr(req.method));
  for (const HeaderView& header : req.headers) {
    AppendShellHeader("--header=", header, ": ", out);
  }
  if (!req.body.empty()) {
    AppendAll(out, kContinuation, "--body-data=", ShellQuoted(req.body));
  }
  const RequestSettings& settings = req.settings;
  if (!settings.followRedirects) {
    AppendAll(out, kContinuation, "--max-redirect=0");
  }
  if (!settings.verifyTls) {
    AppendAll(out, kContinuation, "--no-check-certificate");
  }
  if (!IsDefaultTimeout(settings)) {
    AppendAll(out, kContinuation, "--timeout=", Seconds(settings.timeoutMs));
  }
  AppendAll(out, kContinuation, "--output-document=-", kContinuation, ShellQuoted(req.url), '\n');
}

void GeneratePowerShell(const ResolvedRequest& req, RawChars32& out) {
  // PowerShell wants the Content-Type through -ContentType, not in the headers dictionary.
  const std::string_view contentType = req.headerValue(kContentType);
  bool hasHeaders = false;
  for (const HeaderView& header : req.headers) {
    if (aeronet::CaseInsensitiveEqual(header.name, kContentType)) {
      continue;
    }
    if (!hasHeaders) {
      AppendAll(out, "$headers = @{\n");
      hasHeaders = true;
    }
    AppendAll(out, "    ", PowerShellQuoted(header.name), " = ", PowerShellQuoted(header.value), '\n');
  }
  if (hasHeaders) {
    AppendAll(out, "}\n");
  }
  if (!req.body.empty()) {
    AppendAll(out, "$body = ", PowerShellQuoted(req.body), '\n');
  }
  if (hasHeaders || !req.body.empty()) {
    out.push_back('\n');
  }
  AppendAll(out, "$response = Invoke-WebRequest -Uri ", PowerShellQuoted(req.url));
  if (req.method == Method::connect) {
    AppendAll(out, " -CustomMethod 'CONNECT'");
  } else {
    AppendAll(out, " -Method '", MethodToStr(req.method), '\'');
  }
  if (hasHeaders) {
    AppendAll(out, " -Headers $headers");
  }
  if (!contentType.empty()) {
    AppendAll(out, " -ContentType ", PowerShellQuoted(contentType));
  }
  if (!req.body.empty()) {
    AppendAll(out, " -Body $body");
  }
  const RequestSettings& settings = req.settings;
  if (!settings.followRedirects) {
    AppendAll(out, " -MaximumRedirection 0");
  }
  if (!settings.verifyTls) {
    AppendAll(out, " -SkipCertificateCheck");
  }
  if (!IsDefaultTimeout(settings)) {
    AppendAll(out, " -TimeoutSec ", std::max(1U, (settings.timeoutMs + 999U) / 1000U));
  }
  AppendAll(out, "\n$response.StatusCode\n$response.Content\n");
}

void GeneratePython(const ResolvedRequest& req, RawChars32& out) {
  AppendAll(out, "import requests\n\nurl = ", DoubleQuoted(req.url), '\n');
  if (!req.headers.empty()) {
    AppendAll(out, "headers = {\n");
    for (const HeaderView& header : req.headers) {
      AppendAll(out, "    ", DoubleQuoted(header.name), ": ", DoubleQuoted(header.value), ",\n");
    }
    AppendAll(out, "}\n");
  }
  if (!req.body.empty()) {
    AppendAll(out, "payload = ", DoubleQuoted(req.body), '\n');
  }
  AppendAll(out, "\nresponse = requests.request(", DoubleQuoted(MethodToStr(req.method)), ", url");
  if (!req.headers.empty()) {
    AppendAll(out, ", headers=headers");
  }
  if (!req.body.empty()) {
    // A str body is sent as latin-1 by http.client: encode it explicitly when it is not plain ASCII.
    AppendAll(out, std::string_view{HasNonAscii(req.body) ? ", data=payload.encode(\"utf-8\")" : ", data=payload"});
  }
  const RequestSettings& settings = req.settings;
  AppendAll(out, ", timeout=", Seconds(settings.timeoutMs));
  if (!settings.followRedirects) {
    AppendAll(out, ", allow_redirects=False");
  }
  if (!settings.verifyTls) {
    AppendAll(out, ", verify=False");
  }
  AppendAll(out, ")\n\nprint(response.status_code)\nprint(response.text)\n");
}

void GenerateJavaScript(const ResolvedRequest& req, RawChars32& out) {
  const RequestSettings& settings = req.settings;
  if (!settings.verifyTls) {
    AppendAll(
        out,
        "// TLS verification is disabled for this request: with Node.js, run with NODE_TLS_REJECT_UNAUTHORIZED=0\n");
  }
  AppendAll(out, "const response = await fetch(", DoubleQuoted(req.url));
  const bool hasOptions = req.method != Method::get || !req.headers.empty() || !req.body.empty() ||
                          !settings.followRedirects || !IsDefaultTimeout(settings);
  if (hasOptions) {
    AppendAll(out, ", {\n");
    if (req.method != Method::get) {
      AppendAll(out, "  method: ", DoubleQuoted(MethodToStr(req.method)), ",\n");
    }
    if (!req.headers.empty()) {
      AppendAll(out, "  headers: {\n");
      for (const HeaderView& header : req.headers) {
        AppendAll(out, "    ", DoubleQuoted(header.name), ": ", DoubleQuoted(header.value), ",\n");
      }
      AppendAll(out, "  },\n");
    }
    if (!req.body.empty()) {
      AppendAll(out, "  body: ", DoubleQuoted(req.body), ",\n");
    }
    if (!settings.followRedirects) {
      AppendAll(out, "  redirect: \"manual\",\n");
    }
    if (!IsDefaultTimeout(settings)) {
      AppendAll(out, "  signal: AbortSignal.timeout(", settings.timeoutMs, "),\n");
    }
    out.push_back('}');
  }
  AppendAll(out, ");\n\nconsole.log(response.status);\nconsole.log(await response.text());\n");
}

void GenerateGo(const ResolvedRequest& req, RawChars32& out) {
  const RequestSettings& settings = req.settings;
  const bool hasBody = !req.body.empty();
  AppendAll(out, "package main\n\nimport (\n");
  if (!settings.verifyTls) {
    AppendAll(out, "\t\"crypto/tls\"\n");
  }
  AppendAll(out, "\t\"fmt\"\n\t\"io\"\n\t\"net/http\"\n");
  if (hasBody) {
    AppendAll(out, "\t\"strings\"\n");
  }
  AppendAll(out, "\t\"time\"\n)\n\nfunc main() {\n");
  if (hasBody) {
    AppendAll(out, "\tpayload := strings.NewReader(", DoubleQuoted(req.body), ")\n\n");
  }
  AppendAll(out, "\treq, err := http.NewRequest(", DoubleQuoted(MethodToStr(req.method)), ", ", DoubleQuoted(req.url),
            std::string_view{hasBody ? ", payload)\n" : ", nil)\n"}, "\tif err != nil {\n\t\tpanic(err)\n\t}\n");
  for (const HeaderView& header : req.headers) {
    AppendAll(out, "\treq.Header.Add(", DoubleQuoted(header.name), ", ", DoubleQuoted(header.value), ")\n");
  }
  AppendAll(out, "\n\tclient := &http.Client{\n\t\tTimeout: ");
  if (settings.timeoutMs % 1000U == 0) {
    AppendAll(out, settings.timeoutMs / 1000U, " * time.Second,\n");
  } else {
    AppendAll(out, settings.timeoutMs, " * time.Millisecond,\n");
  }
  if (!settings.followRedirects) {
    AppendAll(out,
              "\t\tCheckRedirect: func(req *http.Request, via []*http.Request) error {\n"
              "\t\t\treturn http.ErrUseLastResponse\n\t\t},\n");
  }
  if (!settings.verifyTls) {
    AppendAll(out, "\t\tTransport: &http.Transport{TLSClientConfig: &tls.Config{InsecureSkipVerify: true}},\n");
  }
  AppendAll(out,
            "\t}\n\tres, err := client.Do(req)\n\tif err != nil {\n\t\tpanic(err)\n\t}\n\tdefer res.Body.Close()\n\n"
            "\tbody, err := io.ReadAll(res.Body)\n\tif err != nil {\n\t\tpanic(err)\n\t}\n"
            "\tfmt.Println(res.Status)\n\tfmt.Println(string(body))\n}\n");
}

void GenerateCppAeronet(const ResolvedRequest& req, RawChars32& out) {
  const RequestSettings& settings = req.settings;
  AppendAll(out, "#include <aeronet/aeronet-client.hpp>\n\n");
  const bool customConfig = !settings.followRedirects || !settings.verifyTls || !IsDefaultTimeout(settings) ||
                            settings.httpVersion != HttpVersion::automatic || !settings.decompress;
  if (!IsDefaultTimeout(settings)) {
    AppendAll(out, "#include <chrono>\n");
  }
  AppendAll(out, "#include <iostream>\n#include <utility>\n\nint main() {\n");
  if (customConfig) {
    AppendAll(out, "  aeronet::HttpClientConfig config;\n");
    if (!IsDefaultTimeout(settings)) {
      for (std::string_view field : {std::string_view{"connectTimeout"}, std::string_view{"requestTimeout"}}) {
        AppendAll(out, "  config.", field, " = std::chrono::milliseconds{", settings.timeoutMs, "};\n");
      }
    }
    if (!settings.followRedirects) {
      AppendAll(out, "  config.followRedirects = false;\n");
    }
    if (!settings.verifyTls) {
      AppendAll(out, "  config.tlsVerifyPeer = false;\n");
    }
    if (settings.httpVersion == HttpVersion::http1) {
      AppendAll(out, "  config.httpVersion = aeronet::HttpVersionMode::Http1_1;\n");
    } else if (settings.httpVersion == HttpVersion::http2) {
      AppendAll(out, "  config.httpVersion = aeronet::HttpVersionMode::Http2;\n");
    }
    if (!settings.decompress) {
      AppendAll(out, "  config.withDecompression(false);\n");
    }
    AppendAll(out, "  aeronet::HttpClient client(std::move(config));\n\n");
  } else {
    AppendAll(out, "  aeronet::HttpClient client;\n\n");
  }
  AppendAll(out, "  aeronet::HttpRequest request = client.makeRequest(aeronet::http::Method::", MethodToStr(req.method),
            ", ", CppQuoted(req.url), ");\n");
  RawChars32 lowerName;
  for (const HeaderView& header : req.headers) {
    if (aeronet::CaseInsensitiveEqual(header.name, kContentType)) {
      continue;  // given with the body
    }
    if (IsTransportHeader(header.name)) {
      AppendAll(out, "  // '", header.name, "' is managed by the client\n");
      continue;
    }
    // aeronet header names are lower-case (checked at compile time for literals).
    AssignLower(lowerName, header.name);
    AppendAll(out, "  request.headerAddLine(", CppQuoted(lowerName), ", ", CppQuoted(header.value), ");\n");
  }
  if (!req.body.empty()) {
    AppendAll(out, "  request.body(");
    AppendCppBodyLiteral(req.body, out);
    if (const std::string_view contentType = req.headerValue(kContentType); !contentType.empty()) {
      AppendAll(out, ", ", CppQuoted(contentType));
    }
    AppendAll(out, ");\n");
  }
  AppendAll(out,
            "\n  auto result = client.request(std::move(request));\n"
            "  if (!result) {\n"
            "    std::cerr << \"request failed: \" << aeronet::ErrcToStr(result.error()) << '\\n';\n"
            "    return 1;\n"
            "  }\n"
            "  std::cout << result->status() << ' ' << result->reason() << '\\n' << result->bodyInMemory() << '\\n';\n"
            "}\n");
}

void GenerateRawHttp(const ResolvedRequest& req, RawChars32& out) {
  const UrlTarget target = SplitUrlTarget(req.url);
  AppendAll(out, MethodToStr(req.method), std::string_view{target.target.starts_with('?') ? " /" : " "}, target.target,
            " HTTP/1.1\r\nHost: ", target.authority, "\r\n");
  for (const HeaderView& header : req.headers) {
    if (aeronet::CaseInsensitiveEqual(header.name, "Host") ||
        aeronet::CaseInsensitiveEqual(header.name, "Content-Length")) {
      continue;
    }
    AppendAll(out, header.name, ": ", header.value, "\r\n");
  }
  if (!req.body.empty() || MethodUsuallyHasBody(req.method)) {
    AppendAll(out, "Content-Length: ", req.body.size(), "\r\n");
  }
  AppendAll(out, "\r\n", req.body);
}

// What Zalie knows of a language, in the order of SnippetLanguage.
struct LanguageInfo {
  std::string_view id;
  std::string_view name;
  void (*generate)(const ResolvedRequest& request, RawChars32& out);
};

constexpr std::array<LanguageInfo, kSnippetLanguages.size()> kLanguages{{
    {.id = "curl", .name = "cURL", .generate = GenerateCurl},
    {.id = "httpie", .name = "HTTPie", .generate = GenerateHttpie},
    {.id = "wget", .name = "Wget", .generate = GenerateWget},
    {.id = "powershell", .name = "PowerShell", .generate = GeneratePowerShell},
    {.id = "python", .name = "Python (requests)", .generate = GeneratePython},
    {.id = "javascript", .name = "JavaScript (fetch)", .generate = GenerateJavaScript},
    {.id = "go", .name = "Go (net/http)", .generate = GenerateGo},
    {.id = "cpp-aeronet", .name = "C++ (aeronet)", .generate = GenerateCppAeronet},
    {.id = "http", .name = "Raw HTTP", .generate = GenerateRawHttp},
}};

const LanguageInfo& InfoOf(SnippetLanguage language) noexcept {
  assert(static_cast<std::size_t>(language) < kLanguages.size());
  return kLanguages[static_cast<std::size_t>(language)];
}

}  // namespace

std::string_view SnippetLanguageName(SnippetLanguage language) noexcept { return InfoOf(language).name; }

std::string_view SnippetLanguageId(SnippetLanguage language) noexcept { return InfoOf(language).id; }

std::optional<SnippetLanguage> ParseSnippetLanguage(std::string_view id) noexcept {
  const auto it = std::ranges::find(kLanguages, id, &LanguageInfo::id);
  if (it == kLanguages.end()) {
    return std::nullopt;
  }
  return kSnippetLanguages[static_cast<std::size_t>(it - kLanguages.begin())];
}

void GenerateSnippet(SnippetLanguage language, const ResolvedRequest& request, RawChars32& out) {
  out.clear();
  InfoOf(language).generate(request, out);
}

}  // namespace zalie
