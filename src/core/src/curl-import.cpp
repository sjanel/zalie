#include "zalie/curl-import.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string_view>
#include <system_error>
#include <utility>

#include "zalie/key-value.hpp"
#include "zalie/method.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/request.hpp"
#include "zalie/string-pool.hpp"
#include "zalie/string-utils.hpp"
#include "zalie/url.hpp"

namespace zalie {

namespace {

constexpr int HexDigit(char ch) noexcept {
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

// Appends the UTF-8 encoding of a code point.
void AppendUtf8(char32_t codePoint, RawChars32& out) {
  if (codePoint < 0x80) {
    out.push_back(static_cast<char>(codePoint));
  } else if (codePoint < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
    out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
  } else if (codePoint < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
    out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
    out.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
  }
}

// Reads up to 'maxDigits' hexadecimal digits at 'pos'. Returns the number of digits read (0 if none).
std::size_t ReadHex(std::string_view str, std::size_t pos, std::size_t maxDigits, char32_t& value) noexcept {
  std::size_t nbDigits = 0;
  value = 0;
  while (nbDigits < maxDigits && pos + nbDigits < str.size()) {
    const int digit = HexDigit(str[pos + nbDigits]);
    if (digit < 0) {
      break;
    }
    value = (value << 4U) | static_cast<char32_t>(digit);
    ++nbDigits;
  }
  return nbDigits;
}

// Decodes the content of an ANSI-C quoted string $'...' starting after the opening quote. Returns the position after
// the closing quote, or npos if unterminated.
std::size_t ReadAnsiCQuoted(std::string_view cmd, std::size_t pos, RawChars32& word) {
  while (pos < cmd.size()) {
    const char ch = cmd[pos++];
    if (ch == '\'') {
      return pos;
    }
    if (ch != '\\' || pos >= cmd.size()) {
      word.push_back(ch);
      continue;
    }
    const char esc = cmd[pos++];
    switch (esc) {
      case 'n':
        word.push_back('\n');
        break;
      case 't':
        word.push_back('\t');
        break;
      case 'r':
        word.push_back('\r');
        break;
      case 'a':
        word.push_back('\a');
        break;
      case 'b':
        word.push_back('\b');
        break;
      case 'e':
      case 'E':
        word.push_back('\x1B');
        break;
      case 'f':
        word.push_back('\f');
        break;
      case 'v':
        word.push_back('\v');
        break;
      case 'x': {
        char32_t value = 0;
        const std::size_t nbDigits = ReadHex(cmd, pos, 2, value);
        if (nbDigits == 0) {
          word.append(std::string_view{"\\x"});
        } else {
          word.push_back(static_cast<char>(value));
          pos += nbDigits;
        }
        break;
      }
      case 'u':
      case 'U': {
        char32_t value = 0;
        const std::size_t nbDigits = ReadHex(cmd, pos, esc == 'u' ? 4 : 8, value);
        if (nbDigits == 0) {
          word.push_back('\\');
          word.push_back(esc);
        } else {
          AppendUtf8(value, word);
          pos += nbDigits;
        }
        break;
      }
      case '0':
      case '1':
      case '2':
      case '3':
      case '4':
      case '5':
      case '6':
      case '7': {
        unsigned value = static_cast<unsigned>(esc - '0');
        for (int i = 0; i < 2 && pos < cmd.size() && cmd[pos] >= '0' && cmd[pos] <= '7'; ++i, ++pos) {
          value = (value * 8U) + static_cast<unsigned>(cmd[pos] - '0');
        }
        word.push_back(static_cast<char>(value));
        break;
      }
      case '\\':
      case '\'':
      case '"':
      case '?':
        word.push_back(esc);
        break;
      default:
        word.push_back('\\');
        word.push_back(esc);
        break;
    }
  }
  return std::string_view::npos;
}

bool IsLineContinuation(std::string_view cmd, std::size_t pos) noexcept {
  // pos is on the continuation character ('\\', '^' or '`'), followed by an optional '\r' and a '\n'.
  if (pos + 1 < cmd.size() && cmd[pos + 1] == '\n') {
    return true;
  }
  return pos + 2 < cmd.size() && cmd[pos + 1] == '\r' && cmd[pos + 2] == '\n';
}

std::size_t SkipLineContinuation(std::string_view cmd, std::size_t pos) noexcept {
  return cmd[pos + 1] == '\n' ? pos + 2 : pos + 3;
}

struct ParseState {
  Request& request;
  StringList& warnings;
  RawChars32 data;
  bool hasData{false};
  bool getMode{false};
  bool headMode{false};
  bool jsonMode{false};
  bool explicitMethod{false};
};

void AddWarning(ParseState& state, std::string_view message, std::string_view detail) {
  state.warnings.push_back({message, detail});
}

void AppendData(ParseState& state, std::string_view data) {
  if (state.hasData) {
    state.data.push_back('&');
  }
  state.data.append(data);
  state.hasData = true;
}

void AppendUrlEncodedData(ParseState& state, std::string_view arg, bool warnOnFile = true) {
  // --data-urlencode: "content", "=content", "name=content" (the "@file" forms are not supported).
  RawChars32 encoded;
  const std::size_t eqPos = arg.find('=');
  if (warnOnFile &&
      (arg.starts_with('@') || (eqPos != std::string_view::npos && arg.substr(eqPos + 1).starts_with('@')))) {
    AddWarning(state, "--data-urlencode from a file is not supported: ", arg);
  }
  if (eqPos == std::string_view::npos) {
    AppendUrlEncoded(arg, encoded, true);
  } else {
    encoded.append(arg.substr(0, eqPos));
    if (eqPos != 0) {
      encoded.push_back('=');
    }
    AppendUrlEncoded(arg.substr(eqPos + 1), encoded, true);
  }
  AppendData(state, encoded);
}

void AddHeader(ParseState& state, std::string_view line) {
  const std::size_t colonPos = line.find(':');
  if (colonPos == std::string_view::npos) {
    if (line.ends_with(';')) {
      // "Name;" sends the header with an empty value
      state.request.headers.push_back(MakeKeyValue(Trim(line.substr(0, line.size() - 1)), {}));
    } else {
      AddWarning(state, "ignored malformed header: ", line);
    }
    return;
  }
  const std::string_view name = Trim(line.substr(0, colonPos));
  const std::string_view value = Trim(line.substr(colonPos + 1));
  if (value.empty()) {
    return;  // "Name:" removes an internal header in curl: nothing to send
  }
  state.request.headers.push_back(MakeKeyValue(name, value));
}

void SetBasicAuth(ParseState& state, std::string_view userPass) {
  Auth& auth = state.request.auth;
  auth.mode = AuthMode::basic;
  const std::size_t colonPos = userPass.find(':');
  auth.username.assign(userPass.substr(0, colonPos));
  if (colonPos == std::string_view::npos) {
    AddWarning(state, "no password given for user ", userPass);
  } else {
    auth.password.assign(userPass.substr(colonPos + 1));
  }
}

bool SetTimeout(ParseState& state, std::string_view seconds) {
  double value = 0;
  const auto [ptr, errc] = std::from_chars(seconds.data(), seconds.data() + seconds.size(), value);
  if (errc != std::errc{} || ptr != seconds.data() + seconds.size() || value <= 0) {
    AddWarning(state, "ignored invalid --max-time: ", seconds);
    return false;
  }
  static constexpr double kMaxTimeoutSeconds = 24.0 * 3600.0;
  state.request.settings.timeoutMs = static_cast<std::uint32_t>(std::min(value, kMaxTimeoutSeconds) * 1000.0);
  return true;
}

enum class OptionKind : std::uint8_t { flag, withArg };

struct OptionSpec {
  std::string_view longName;
  char shortName;
  OptionKind kind;
};

// Every curl option Zalie knows. Unknown long options are reported and assumed to take no argument.
constexpr std::array kOptions{
    OptionSpec{"request", 'X', OptionKind::withArg},
    OptionSpec{"header", 'H', OptionKind::withArg},
    OptionSpec{"data", 'd', OptionKind::withArg},
    OptionSpec{"data-ascii", '\0', OptionKind::withArg},
    OptionSpec{"data-binary", '\0', OptionKind::withArg},
    OptionSpec{"data-raw", '\0', OptionKind::withArg},
    OptionSpec{"data-urlencode", '\0', OptionKind::withArg},
    OptionSpec{"json", '\0', OptionKind::withArg},
    OptionSpec{"form", 'F', OptionKind::withArg},
    OptionSpec{"form-string", '\0', OptionKind::withArg},
    OptionSpec{"user", 'u', OptionKind::withArg},
    OptionSpec{"user-agent", 'A', OptionKind::withArg},
    OptionSpec{"referer", 'e', OptionKind::withArg},
    OptionSpec{"cookie", 'b', OptionKind::withArg},
    OptionSpec{"max-time", 'm', OptionKind::withArg},
    OptionSpec{"url", '\0', OptionKind::withArg},
    OptionSpec{"oauth2-bearer", '\0', OptionKind::withArg},
    OptionSpec{"range", 'r', OptionKind::withArg},
    OptionSpec{"output", 'o', OptionKind::withArg},
    OptionSpec{"write-out", 'w', OptionKind::withArg},
    OptionSpec{"connect-timeout", '\0', OptionKind::withArg},
    OptionSpec{"retry", '\0', OptionKind::withArg},
    OptionSpec{"retry-delay", '\0', OptionKind::withArg},
    OptionSpec{"retry-max-time", '\0', OptionKind::withArg},
    OptionSpec{"cookie-jar", 'c', OptionKind::withArg},
    OptionSpec{"cacert", '\0', OptionKind::withArg},
    OptionSpec{"capath", '\0', OptionKind::withArg},
    OptionSpec{"cert", 'E', OptionKind::withArg},
    OptionSpec{"key", '\0', OptionKind::withArg},
    OptionSpec{"proxy", 'x', OptionKind::withArg},
    OptionSpec{"proxy-user", 'U', OptionKind::withArg},
    OptionSpec{"resolve", '\0', OptionKind::withArg},
    OptionSpec{"interface", '\0', OptionKind::withArg},
    OptionSpec{"limit-rate", '\0', OptionKind::withArg},
    OptionSpec{"upload-file", 'T', OptionKind::withArg},
    OptionSpec{"max-redirs", '\0', OptionKind::withArg},
    OptionSpec{"dump-header", 'D', OptionKind::withArg},
    OptionSpec{"unix-socket", '\0', OptionKind::withArg},
    OptionSpec{"config", 'K', OptionKind::withArg},
    OptionSpec{"trace", '\0', OptionKind::withArg},
    OptionSpec{"trace-ascii", '\0', OptionKind::withArg},
    OptionSpec{"stderr", '\0', OptionKind::withArg},
    OptionSpec{"aws-sigv4", '\0', OptionKind::withArg},
    OptionSpec{"proto", '\0', OptionKind::withArg},
    OptionSpec{"location", 'L', OptionKind::flag},
    OptionSpec{"insecure", 'k', OptionKind::flag},
    OptionSpec{"get", 'G', OptionKind::flag},
    OptionSpec{"head", 'I', OptionKind::flag},
    OptionSpec{"compressed", '\0', OptionKind::flag},
    OptionSpec{"raw", '\0', OptionKind::flag},
    OptionSpec{"http1.0", '0', OptionKind::flag},
    OptionSpec{"http1.1", '\0', OptionKind::flag},
    OptionSpec{"http2", '\0', OptionKind::flag},
    OptionSpec{"http2-prior-knowledge", '\0', OptionKind::flag},
    OptionSpec{"http3", '\0', OptionKind::flag},
    OptionSpec{"silent", 's', OptionKind::flag},
    OptionSpec{"show-error", 'S', OptionKind::flag},
    OptionSpec{"verbose", 'v', OptionKind::flag},
    OptionSpec{"include", 'i', OptionKind::flag},
    OptionSpec{"fail", 'f', OptionKind::flag},
    OptionSpec{"fail-with-body", '\0', OptionKind::flag},
    OptionSpec{"progress-bar", '#', OptionKind::flag},
    OptionSpec{"no-progress-meter", '\0', OptionKind::flag},
    OptionSpec{"no-buffer", 'N', OptionKind::flag},
    OptionSpec{"globoff", 'g', OptionKind::flag},
    OptionSpec{"location-trusted", '\0', OptionKind::flag},
    OptionSpec{"ipv4", '4', OptionKind::flag},
    OptionSpec{"ipv6", '6', OptionKind::flag},
    OptionSpec{"remote-name", 'O', OptionKind::flag},
    OptionSpec{"remote-header-name", 'J', OptionKind::flag},
    OptionSpec{"netrc", 'n', OptionKind::flag},
    OptionSpec{"path-as-is", '\0', OptionKind::flag},
    OptionSpec{"ssl-no-revoke", '\0', OptionKind::flag},
    OptionSpec{"tlsv1.2", '\0', OptionKind::flag},
    OptionSpec{"tlsv1.3", '\0', OptionKind::flag},
    OptionSpec{"disable", 'q', OptionKind::flag},
};

const OptionSpec* FindLongOption(std::string_view name) noexcept {
  // std::array iterators are not pointers everywhere (MSVC): dereference before taking the address.
  const auto it = std::ranges::find(kOptions, name, &OptionSpec::longName);
  return it == kOptions.end() ? nullptr : &*it;
}

const OptionSpec* FindShortOption(char name) noexcept {
  const auto it = std::ranges::find(kOptions, name, &OptionSpec::shortName);
  return it == kOptions.end() || name == '\0' ? nullptr : &*it;
}

// Applies one option (identified by its long name) to the request.
void ApplyOption(ParseState& state, std::string_view name, std::string_view arg) {
  Request& req = state.request;
  if (name == "request") {
    if (auto method = ParseMethod(arg)) {
      req.method = *method;
      state.explicitMethod = true;
    } else {
      AddWarning(state, "unsupported method: ", arg);
    }
  } else if (name == "header") {
    AddHeader(state, arg);
  } else if (name == "data" || name == "data-ascii" || name == "data-binary" || name == "data-raw") {
    if (name != "data-raw" && arg.starts_with('@')) {
      AddWarning(state, "reading the body from a file is not supported: ", arg);
    }
    AppendData(state, arg);
  } else if (name == "data-urlencode") {
    AppendUrlEncodedData(state, arg);
  } else if (name == "json") {
    AppendData(state, arg);
    state.jsonMode = true;
  } else if (name == "form" || name == "form-string") {
    AddWarning(state, "multipart forms are not supported, imported as an url-encoded field: ", arg);
    AppendUrlEncodedData(state, arg, false);
  } else if (name == "user") {
    SetBasicAuth(state, arg);
  } else if (name == "user-agent") {
    req.headers.push_back(MakeKeyValue("User-Agent", arg));
  } else if (name == "referer") {
    req.headers.push_back(MakeKeyValue("Referer", arg));
  } else if (name == "cookie") {
    if (arg.contains('=')) {
      req.headers.push_back(MakeKeyValue("Cookie", arg));
    } else {
      AddWarning(state, "cookie files are not supported: ", arg);
    }
  } else if (name == "range") {
    req.headers.push_back(MakeKeyValue("Range", View(Concat({"bytes=", arg}))));
  } else if (name == "max-time") {
    SetTimeout(state, arg);
  } else if (name == "url") {
    req.url.assign(arg);
  } else if (name == "oauth2-bearer") {
    req.auth.mode = AuthMode::bearer;
    req.auth.token.assign(arg);
  } else if (name == "location") {
    req.settings.followRedirects = true;
  } else if (name == "insecure") {
    req.settings.verifyTls = false;
  } else if (name == "get") {
    state.getMode = true;
  } else if (name == "head") {
    state.headMode = true;
  } else if (name == "compressed") {
    req.settings.decompress = true;
  } else if (name == "raw") {
    req.settings.decompress = false;
  } else if (name == "http1.0" || name == "http1.1") {
    req.settings.httpVersion = HttpVersion::http1;
  } else if (name == "http2" || name == "http2-prior-knowledge") {
    req.settings.httpVersion = HttpVersion::http2;
  } else if (name == "http3" || name == "proxy" || name == "proxy-user" || name == "upload-file" ||
             name == "unix-socket" || name == "config" || name == "aws-sigv4" || name == "cert" || name == "key" ||
             name == "cacert" || name == "capath" || name == "resolve") {
    AddWarning(state, "unsupported option ignored: --", name);
  }
  // Every other known option only affects curl's own output: silently ignored.
}

// Derives a readable request name from its method and URL: "GET users/42".
RawChars32 NameFromUrl(Method method, std::string_view url) {
  const UrlTarget target = SplitUrlTarget(SplitUrlQuery(url).base);
  std::string_view path = target.target;
  while (path.starts_with('/')) {
    path.remove_prefix(1);
  }
  while (path.ends_with('/')) {
    path.remove_suffix(1);
  }
  return Concat({MethodToStr(method), " ", path.empty() ? target.authority : path});
}

// Fills the body of the request from the collected data, choosing the body mode from the Content-Type.
void ApplyData(ParseState& state) {
  Request& req = state.request;
  if (state.getMode) {
    // -G: the data goes to the query string.
    RawChars32 url(View(req.url));
    url.push_back(SplitUrlQuery(url).hasQuery ? '&' : '?');
    url.append(state.data);
    req.url.swap(url);
    return;
  }
  const KeyValue* pContentType = FindEnabled(req.headers, "Content-Type", true);
  std::string_view contentType = pContentType == nullptr ? std::string_view{} : std::string_view{pContentType->value};
  const std::string_view trimmed = Trim(state.data);
  if (state.jsonMode && pContentType == nullptr) {
    contentType = "application/json";
    if (FindEnabled(req.headers, "Accept", true) == nullptr) {
      req.headers.push_back(MakeKeyValue("Accept", "application/json"));
    }
  } else if (contentType.empty() && (trimmed.starts_with('{') || trimmed.starts_with('['))) {
    contentType = "application/json";  // what the author meant, even if curl would label it as a form
  }

  if (FindIgnoreCase(contentType, "json") != std::string_view::npos) {
    req.bodyMode = BodyMode::json;
  } else if (FindIgnoreCase(contentType, "xml") != std::string_view::npos) {
    req.bodyMode = BodyMode::xml;
  } else if (contentType.empty() || FindIgnoreCase(contentType, "x-www-form-urlencoded") != std::string_view::npos) {
    // curl's default for -d. Decoded into editable fields; re-encoded identically at send time.
    KeyValues fields;
    ParseQuery(state.data, fields);
    for (KeyValue& field : fields) {
      field.key = UrlDecode(field.key, true);
      field.value = UrlDecode(field.value, true);
    }
    req.bodyMode = BodyMode::formUrlEncoded;
    req.form = std::move(fields);
    return;
  } else {
    req.bodyMode = BodyMode::text;
  }
  req.body = std::move(state.data);
}

// Splits combined short options ("-sSL", "-XPOST", "-HName: v") into (option, attached argument) pairs.
// Returns false on an unknown short option.
template <class Func>
bool ForEachShortOption(std::string_view word, Func&& func) {
  for (std::size_t i = 1; i < word.size(); ++i) {
    const OptionSpec* pSpec = FindShortOption(word[i]);
    if (pSpec == nullptr) {
      return false;
    }
    if (pSpec->kind == OptionKind::withArg) {
      func(*pSpec, word.substr(i + 1));  // attached argument (possibly empty: then it is the next word)
      return true;
    }
    func(*pSpec, std::string_view{});
  }
  return true;
}

}  // namespace

bool LooksLikeCurlCommand(std::string_view text) noexcept {
  text = Trim(text);
  return text.starts_with("curl ") || text.starts_with("curl\t") || text.starts_with("curl.exe ") ||
         text.starts_with("curl\\\n");
}

std::expected<StringList, RawChars32> SplitShellWords(std::string_view cmd) {
  StringList words(cmd.size());
  RawChars32 word;  // one scratch buffer for every word, stored in the arena of 'words' once complete
  bool inWord = false;
  const auto endWord = [&] {
    if (inWord) {
      words.push_back(View(word));
      word.clear();
      inWord = false;
    }
  };
  std::size_t pos = 0;
  while (pos < cmd.size()) {
    const char ch = cmd[pos];
    if ((ch == '\\' || ch == '^' || ch == '`') && IsLineContinuation(cmd, pos)) {
      pos = SkipLineContinuation(cmd, pos);
      continue;
    }
    if (IsSpace(ch)) {
      endWord();
      ++pos;
      continue;
    }
    inWord = true;
    if (ch == '\\') {
      if (pos + 1 < cmd.size()) {
        word.push_back(cmd[pos + 1]);
      }
      pos += 2;
    } else if (ch == '\'') {
      const std::size_t closePos = cmd.find('\'', pos + 1);
      if (closePos == std::string_view::npos) {
        return std::unexpected(RawChars32(std::string_view{"unterminated single quote"}));
      }
      word.append(cmd.substr(pos + 1, closePos - pos - 1));
      pos = closePos + 1;
    } else if (ch == '$' && pos + 1 < cmd.size() && cmd[pos + 1] == '\'') {
      pos = ReadAnsiCQuoted(cmd, pos + 2, word);
      if (pos == std::string_view::npos) {
        return std::unexpected(RawChars32(std::string_view{"unterminated $'...' quote"}));
      }
    } else if (ch == '"') {
      ++pos;
      bool closed = false;
      while (pos < cmd.size()) {
        const char inner = cmd[pos];
        if (inner == '"') {
          closed = true;
          ++pos;
          break;
        }
        if (inner == '\\' && pos + 1 < cmd.size()) {
          const char next = cmd[pos + 1];
          if (next == '"' || next == '\\' || next == '$' || next == '`') {
            word.push_back(next);
            pos += 2;
            continue;
          }
          if (next == '\n') {
            pos += 2;  // line continuation inside double quotes
            continue;
          }
        }
        word.push_back(inner);
        ++pos;
      }
      if (!closed) {
        return std::unexpected(RawChars32(std::string_view{"unterminated double quote"}));
      }
    } else {
      word.push_back(ch);
      ++pos;
    }
  }
  endWord();
  return words;
}

std::expected<CurlImport, RawChars32> ParseCurlCommand(std::string_view command) {
  auto words = SplitShellWords(Trim(command));
  if (!words) {
    return std::unexpected(std::move(words.error()));
  }
  const std::string_view program = words->empty() ? std::string_view{} : words->front();
  if (program != "curl" && !program.ends_with("/curl") && program != "curl.exe") {
    return std::unexpected(RawChars32(std::string_view{"not a curl command"}));
  }

  CurlImport result;
  ParseState state{.request = result.request, .warnings = result.warnings};
  bool urlFound = false;
  bool endOfOptions = false;
  for (std::size_t idx = 1; idx < words->size(); ++idx) {
    const std::string_view word = (*words)[idx];
    const auto nextArg = [&](std::string_view optionName) -> std::expected<std::string_view, RawChars32> {
      if (idx + 1 >= words->size()) {
        return std::unexpected(Concat({"missing argument for option ", optionName}));
      }
      return (*words)[++idx];
    };

    if (endOfOptions || !word.starts_with('-') || word == "-") {
      if (!urlFound) {
        result.request.url.assign(word);
        urlFound = true;
      } else {
        AddWarning(state, "ignored extra argument: ", word);
      }
      continue;
    }
    if (word == "--") {
      endOfOptions = true;
      continue;
    }
    if (word.starts_with("--")) {
      std::string_view name = word.substr(2);
      std::string_view attached;
      bool hasAttached = false;
      if (const std::size_t eqPos = name.find('='); eqPos != std::string_view::npos) {
        attached = name.substr(eqPos + 1);
        name = name.substr(0, eqPos);
        hasAttached = true;
      }
      const OptionSpec* pSpec = FindLongOption(name);
      if (pSpec == nullptr) {
        AddWarning(state, "unknown option ignored: ", word);
        continue;
      }
      if (pSpec->kind == OptionKind::flag) {
        ApplyOption(state, pSpec->longName, {});
        continue;
      }
      if (hasAttached) {
        ApplyOption(state, pSpec->longName, attached);
        continue;
      }
      auto arg = nextArg(word);
      if (!arg) {
        return std::unexpected(std::move(arg.error()));
      }
      ApplyOption(state, pSpec->longName, *arg);
      continue;
    }
    // Short option(s)
    RawChars32 error;
    const bool known = ForEachShortOption(word, [&](const OptionSpec& spec, std::string_view attached) {
      if (spec.kind == OptionKind::flag || !attached.empty()) {
        ApplyOption(state, spec.longName, attached);
        return;
      }
      auto arg = nextArg(word);
      if (!arg) {
        error = std::move(arg.error());
        return;
      }
      ApplyOption(state, spec.longName, *arg);
    });
    if (!error.empty()) {
      return std::unexpected(std::move(error));
    }
    if (!known) {
      AddWarning(state, "unknown option ignored: ", word);
    }
  }

  if (result.request.url.empty()) {
    return std::unexpected(RawChars32(std::string_view{"no URL in the curl command"}));
  }
  if (!state.explicitMethod) {
    if (state.headMode) {
      result.request.method = Method::head;
    } else if (state.hasData && !state.getMode) {
      result.request.method = Method::post;
    }
  }
  if (state.hasData) {
    ApplyData(state);
  }
  SyncParamsFromUrl(result.request);
  result.request.name = NameFromUrl(result.request.method, result.request.url);
  return result;
}

}  // namespace zalie
