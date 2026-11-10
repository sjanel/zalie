#include "zalie/json-path.hpp"

#include <aeronet/cctype.hpp>
#include <algorithm>
#include <amc/vector.hpp>
#include <cassert>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string_view>
#include <system_error>
#include <utility>

#include "json-scan.hpp"
#include "zalie/encoding.hpp"
#include "zalie/json-format.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/string-utils.hpp"

namespace zalie {

namespace {

// Characters of a shorthand member name ('.name'): RFC 9535 allows letters, digits, '_' and non-ASCII; '-' and '$' are
// accepted too, as most JSONPath implementations do ('.content-type').
constexpr bool IsNameFirst(char ch) noexcept {
  return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_' || static_cast<unsigned char>(ch) >= 0x80;
}
constexpr bool IsNameChar(char ch) noexcept {
  return IsNameFirst(ch) || aeronet::isdigit(ch) || ch == '-' || ch == '$';
}

// Decodes the escapes of a string body (between the quotes) into 'out'. Returns false on an invalid escape.
bool Unescape(std::string_view raw, RawChars32& out) {
  out.clear();
  return AppendJsonUnescaped(raw, out);
}

// Calls 'func(rawKey, value)' for the members of 'object' (a view starting with '{'), in order, until it returns false.
// 'rawKey' is the key without its quotes, escapes untouched. Stops silently at the first malformed member.
template <class Func>
void ForEachMember(std::string_view object, Func&& func) {
  static_cast<void>(
      ScanJsonObject(object, [&func](std::string_view rawKey, std::string_view value) { return func(rawKey, value); }));
}

// Calls 'func(value)' for the elements of 'array' (a view of a valid JSON array), in order, until it returns false.
template <class Func>
void ForEachElement(std::string_view array, Func&& func) {
  std::size_t pos = SkipSpaces(array, 1);
  if (array[pos] == ']') {
    return;
  }
  while (true) {
    const std::size_t valueEnd = ValueEnd(array, pos);
    assert(valueEnd != std::string_view::npos);
    if (!func(array.substr(pos, valueEnd - pos))) {
      return;
    }
    pos = SkipSpaces(array, valueEnd);
    if (array[pos] == ']') {
      return;
    }
    assert(array[pos] == ',');
    pos = SkipSpaces(array, pos + 1);
  }
}

// --- Path ---

enum class SelectorKind : std::uint8_t { name, index, slice, wildcard };

struct Selector {
  // Member name (unescaped) for SelectorKind::name.
  RawChars32 name;
  // Index, or start of a slice.
  std::int64_t index{0};
  std::int64_t end{0};
  std::int64_t step{1};
  SelectorKind kind{SelectorKind::name};
  bool hasStart{false};
  bool hasEnd{false};

  using trivially_relocatable = AllTriviallyRelocatable<RawChars32>;
};

struct Segment {
  amc::vector<Selector> selectors;
  // '..': applies to the node and all its descendants.
  bool descendant{false};

  using trivially_relocatable = AllTriviallyRelocatable<amc::vector<Selector>>;
};

using Segments = amc::vector<Segment>;

class PathParser {
 public:
  explicit PathParser(std::string_view path) noexcept : _path(path) {}

  std::expected<Segments, RawChars32> parse() {
    if (_path.empty()) {
      return std::unexpected(RawChars32(std::string_view{"empty path"}));
    }
    if (_path.front() == '$') {
      ++_pos;
    } else if (IsNameFirst(_path.front())) {
      // 'items[0]' is read as '$.items[0]'.
      readName(_segments.emplace_back());
    } else if (_path.front() != '.' && _path.front() != '[') {
      error("a path starts with '$'");
      return std::unexpected(std::move(_error));
    }
    while (true) {
      skipSpaces();
      if (_pos >= _path.size()) {
        break;
      }
      if (!parseSegment()) {
        return std::unexpected(std::move(_error));
      }
    }
    return std::move(_segments);
  }

 private:
  // Records the error (with its 1-based position) and returns false.
  bool error(std::string_view message) {
    _error.clear();
    AppendAll(_error, message, " (at position ", _pos + 1, ')');
    return false;
  }

  void skipSpaces() noexcept {
    while (_pos < _path.size() && IsJsonSpace(_path[_pos])) {
      ++_pos;
    }
  }

  bool parseSegment() {
    Segment& segment = _segments.emplace_back();
    if (_path[_pos] == '[') {
      return parseBracketed(segment);
    }
    if (_path[_pos] != '.') {
      return error("expected '.' or '['");
    }
    ++_pos;
    if (_pos < _path.size() && _path[_pos] == '.') {
      segment.descendant = true;
      ++_pos;
      if (_pos < _path.size() && _path[_pos] == '[') {
        return parseBracketed(segment);
      }
    }
    if (_pos < _path.size() && _path[_pos] == '*') {
      ++_pos;
      segment.selectors.push_back(Selector{.kind = SelectorKind::wildcard});
      return true;
    }
    return parseShorthandName(segment);
  }

  bool parseShorthandName(Segment& segment) {
    if (_pos >= _path.size() || !IsNameFirst(_path[_pos])) {
      return error("expected a member name");
    }
    readName(segment);
    return true;
  }

  // Reads the shorthand member name starting at '_pos', on its first character.
  void readName(Segment& segment) {
    const std::size_t begin = _pos++;
    while (_pos < _path.size() && IsNameChar(_path[_pos])) {
      ++_pos;
    }
    segment.selectors.push_back(Selector{.name = RawChars32(_path.substr(begin, _pos - begin))});
  }

  bool parseBracketed(Segment& segment) {
    ++_pos;  // '['
    while (true) {
      skipSpaces();
      if (_pos >= _path.size()) {
        return error("missing ']'");
      }
      if (!parseSelector(segment)) {
        return false;
      }
      skipSpaces();
      if (_pos >= _path.size()) {
        return error("missing ']'");
      }
      if (_path[_pos] == ']') {
        ++_pos;
        return true;
      }
      if (_path[_pos] != ',') {
        return error("expected ',' or ']'");
      }
      ++_pos;
    }
  }

  bool parseSelector(Segment& segment) {
    const char ch = _path[_pos];
    if (ch == '\'' || ch == '"') {
      return parseQuotedName(segment, ch);
    }
    if (ch == '*') {
      ++_pos;
      segment.selectors.push_back(Selector{.kind = SelectorKind::wildcard});
      return true;
    }
    if (ch == '?') {
      return error("filter expressions are not supported");
    }
    if (ch != '-' && ch != ':' && !aeronet::isdigit(ch)) {
      return error("expected a name, an index, a slice or '*'");
    }
    Selector selector{.kind = SelectorKind::index};
    if (ch != ':') {
      if (!parseInt(selector.index)) {
        return false;
      }
      selector.hasStart = true;
      skipSpaces();
      if (_pos >= _path.size() || _path[_pos] != ':') {
        segment.selectors.push_back(std::move(selector));
        return true;
      }
    }
    // Slice: [start]:[end][:[step]]
    selector.kind = SelectorKind::slice;
    ++_pos;  // ':'
    skipSpaces();
    if (_pos < _path.size() && (_path[_pos] == '-' || aeronet::isdigit(_path[_pos]))) {
      if (!parseInt(selector.end)) {
        return false;
      }
      selector.hasEnd = true;
      skipSpaces();
    }
    if (_pos < _path.size() && _path[_pos] == ':') {
      ++_pos;
      skipSpaces();
      if (_pos < _path.size() && (_path[_pos] == '-' || aeronet::isdigit(_path[_pos])) && !parseInt(selector.step)) {
        return false;
      }
    }
    segment.selectors.push_back(std::move(selector));
    return true;
  }

  bool parseInt(std::int64_t& value) {
    // Integers of RFC 9535: within +/- (2^53 - 1).
    static constexpr std::int64_t kMaxInt = (std::int64_t{1} << 53) - 1;
    const char* pBegin = _path.data() + _pos;
    const auto [ptr, errc] = std::from_chars(pBegin, _path.data() + _path.size(), value);
    if (errc != std::errc{} || value > kMaxInt || value < -kMaxInt) {
      return error("invalid integer");
    }
    _pos += static_cast<std::size_t>(ptr - pBegin);
    return true;
  }

  bool parseQuotedName(Segment& segment, char quote) {
    const std::size_t begin = ++_pos;
    while (_pos < _path.size() && _path[_pos] != quote) {
      _pos += _path[_pos] == '\\' ? 2 : 1;
    }
    if (_pos >= _path.size()) {
      _pos = begin - 1;
      return error("unterminated string");
    }
    Selector selector;
    if (!Unescape(_path.substr(begin, _pos - begin), selector.name)) {
      return error("invalid escape in a name");
    }
    ++_pos;  // closing quote
    segment.selectors.push_back(std::move(selector));
    return true;
  }

  std::string_view _path;
  std::size_t _pos{0};
  Segments _segments;
  RawChars32 _error;
};

// --- Evaluation ---

class Evaluator {
 public:
  explicit Evaluator(std::size_t maxMatches) noexcept : _maxMatches(maxMatches) {}

  JsonPathResult run(std::string_view root, const Segments& segments) {
    JsonPathResult result;
    amc::vector<std::string_view> nodes;
    nodes.push_back(root);
    for (const Segment& segment : segments) {
      result.singular =
          result.singular && !segment.descendant && segment.selectors.size() == 1 &&
          (segment.selectors[0].kind == SelectorKind::name || segment.selectors[0].kind == SelectorKind::index);
      _out.clear();
      for (std::string_view node : nodes) {
        if (segment.descendant) {
          descend(node, segment);
        } else {
          select(node, segment);
        }
        if (_stopped) {
          break;
        }
      }
      nodes.swap(_out);
      if (_stopped) {
        result.truncated = true;
        break;
      }
    }
    result.matches = std::move(nodes);
    return result;
  }

 private:
  bool push(std::string_view value) {
    if (_out.size() >= _maxMatches) {
      _stopped = true;
      return false;
    }
    _out.push_back(value);
    return true;
  }

  void select(std::string_view node, const Segment& segment) {
    for (const Selector& selector : segment.selectors) {
      if (_stopped) {
        return;
      }
      apply(node, selector);
    }
  }

  // The node and its descendants, each before its own descendants (RFC 9535 '..').
  void descend(std::string_view node, const Segment& segment) {
    select(node, segment);
    if (_stopped) {
      return;
    }
    if (node.front() == '{') {
      ForEachMember(node, [&](std::string_view /*rawKey*/, std::string_view value) {
        descend(value, segment);
        return !_stopped;
      });
    } else if (node.front() == '[') {
      ForEachElement(node, [&](std::string_view value) {
        descend(value, segment);
        return !_stopped;
      });
    }
  }

  bool keyEquals(std::string_view rawKey, std::string_view name) {
    if (!rawKey.contains('\\')) {
      return rawKey == name;
    }
    return Unescape(rawKey, _keyScratch) && View(_keyScratch) == name;
  }

  void apply(std::string_view node, const Selector& selector) {
    const bool isObject = node.front() == '{';
    const bool isArray = node.front() == '[';
    switch (selector.kind) {
      case SelectorKind::name:
        if (isObject) {
          ForEachMember(node, [&](std::string_view rawKey, std::string_view value) {
            if (keyEquals(rawKey, selector.name)) {
              push(value);
              return false;
            }
            return true;
          });
        }
        break;
      case SelectorKind::wildcard:
        if (isObject) {
          ForEachMember(node, [&](std::string_view /*rawKey*/, std::string_view value) { return push(value); });
        } else if (isArray) {
          ForEachElement(node, [&](std::string_view value) { return push(value); });
        }
        break;
      case SelectorKind::index:
        if (isArray) {
          if (selector.index >= 0) {
            std::int64_t position = 0;
            ForEachElement(node, [&](std::string_view value) {
              if (position++ == selector.index) {
                push(value);
                return false;
              }
              return true;
            });
          } else {
            collectElements(node);
            const auto size = static_cast<std::int64_t>(_elements.size());
            if (size + selector.index >= 0) {
              push(_elements[static_cast<std::uint32_t>(size + selector.index)]);
            }
          }
        }
        break;
      case SelectorKind::slice:
        if (isArray) {
          applySlice(node, selector);
        }
        break;
    }
  }

  void collectElements(std::string_view array) {
    _elements.clear();
    ForEachElement(array, [&](std::string_view value) {
      _elements.push_back(value);
      return true;
    });
  }

  // Slice semantics of RFC 9535 (section 2.3.4.2.2), negative steps included.
  void applySlice(std::string_view array, const Selector& selector) {
    if (selector.step == 0) {
      return;
    }
    collectElements(array);
    const auto len = static_cast<std::int64_t>(_elements.size());
    const auto normalize = [len](std::int64_t idx) { return idx >= 0 ? idx : len + idx; };
    const std::int64_t step = selector.step;
    if (step > 0) {
      const std::int64_t lower = std::clamp(selector.hasStart ? normalize(selector.index) : 0, std::int64_t{0}, len);
      const std::int64_t upper = std::clamp(selector.hasEnd ? normalize(selector.end) : len, std::int64_t{0}, len);
      for (std::int64_t i = lower; i < upper && push(_elements[static_cast<std::uint32_t>(i)]); i += step) {
      }
    } else {
      const std::int64_t upper =
          std::clamp(selector.hasStart ? normalize(selector.index) : len - 1, std::int64_t{-1}, len - 1);
      const std::int64_t lower =
          std::clamp(selector.hasEnd ? normalize(selector.end) : -len - 1, std::int64_t{-1}, len - 1);
      for (std::int64_t i = upper; lower < i && push(_elements[static_cast<std::uint32_t>(i)]); i += step) {
      }
    }
  }

  amc::vector<std::string_view> _out;
  amc::vector<std::string_view> _elements;
  RawChars32 _keyScratch;
  std::size_t _maxMatches;
  bool _stopped{false};
};

}  // namespace

std::expected<JsonPathResult, RawChars32> EvaluateJsonPath(std::string_view json, std::string_view path,
                                                           std::size_t maxMatches) {
  auto segments = PathParser(Trim(path)).parse();
  if (!segments) {
    return std::unexpected(std::move(segments.error()));
  }
  if (!IsValidJson(json)) {
    return std::unexpected(RawChars32(std::string_view{"not a valid JSON document"}));
  }
  const std::size_t begin = SkipSpaces(json, 0);
  const std::size_t end = ValueEnd(json, begin);
  return Evaluator(maxMatches).run(json.substr(begin, end - begin), *segments);
}

std::size_t JoinJsonPathMatches(const JsonPathResult& result, RawChars& out, std::size_t maxBytes) {
  out.clear();
  if (result.singular && result.matches.size() == 1) {
    if (result.matches[0].size() > maxBytes) {
      return 0;
    }
    out.append(result.matches[0]);
    return 1;
  }
  std::size_t total = 2;
  std::size_t count = 0;
  for (std::string_view match : result.matches) {
    if (total + match.size() + 1 > maxBytes) {
      break;
    }
    total += match.size() + 1;
    ++count;
  }
  out.reserve(total);
  out.unchecked_push_back('[');
  for (std::uint32_t i = 0; i < count; ++i) {
    if (i != 0) {
      out.unchecked_push_back(',');
    }
    out.unchecked_append(result.matches[i]);
  }
  out.unchecked_push_back(']');
  return count;
}

}  // namespace zalie
