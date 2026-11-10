#include "zalie/json-format.hpp"

#include <aeronet/cctype.hpp>
#include <aeronet/char-hexadecimal-converter.hpp>
#include <amc/smallvector.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "json-scan.hpp"
#include "zalie/raw-chars.hpp"

namespace zalie {

namespace {

constexpr std::size_t kMaxDepth = 1024;

// Output policy appending the pretty-printed document.
class PrettyWriter {
 public:
  PrettyWriter(RawChars& out, std::uint32_t indent) noexcept : _out(out), _indent(indent) {}

  void put(char ch) { _out.push_back(ch); }
  void put(std::string_view str) { _out.append(str); }

  void newLine(std::size_t depth) {
    const std::size_t nbSpaces = depth * _indent;
    _out.ensureAvailableCapacityExponential(nbSpaces + 1);
    char* pData = _out.end();
    *pData = '\n';
    std::memset(pData + 1, ' ', nbSpaces);
    _out.setEnd(pData + 1 + nbSpaces);
  }

  void keySeparator() { _out.append(std::string_view{": "}); }

 private:
  RawChars& _out;
  std::uint32_t _indent;
};

// Output policy for validation only.
struct NullWriter {
  void put(char /*ch*/) noexcept {}
  void put(std::string_view /*str*/) noexcept {}
  void newLine(std::size_t /*depth*/) noexcept {}
  void keySeparator() noexcept {}
};

template <class Writer>
class JsonFormatter {
 public:
  JsonFormatter(std::string_view json, Writer& writer) noexcept : _json(json), _writer(writer) {}

  bool run() {
    skipSpaces();
    if (!value()) {
      return false;
    }
    while (!_stack.empty()) {
      skipSpaces();
      if (_pos >= _json.size()) {
        return false;
      }
      const char ch = _json[_pos];
      const char open = _stack.back();
      if (ch == ',') {
        ++_pos;
        _writer.put(',');
        _writer.newLine(_stack.size());
        skipSpaces();
        if (open == '{' && !key()) {
          return false;
        }
        if (!value()) {
          return false;
        }
      } else if ((ch == '}' && open == '{') || (ch == ']' && open == '[')) {
        ++_pos;
        _stack.pop_back();
        _writer.newLine(_stack.size());
        _writer.put(ch);
      } else {
        return false;
      }
    }
    skipSpaces();
    return _pos == _json.size();
  }

 private:
  void skipSpaces() noexcept {
    while (_pos < _json.size() && IsJsonSpace(_json[_pos])) {
      ++_pos;
    }
  }

  // Parses "key": (the key, then the separator), followed by spaces.
  bool key() {
    if (_pos >= _json.size() || _json[_pos] != '"' || !string()) {
      return false;
    }
    skipSpaces();
    if (_pos >= _json.size() || _json[_pos] != ':') {
      return false;
    }
    ++_pos;
    _writer.keySeparator();
    skipSpaces();
    return true;
  }

  // Parses a value. For a non-empty container, only its opening and its first element are consumed: the closing is
  // handled by the loop of run().
  bool value() {
    if (_pos >= _json.size()) {
      return false;
    }
    const char ch = _json[_pos];
    switch (ch) {
      case '{':
      case '[': {
        if (_stack.size() >= kMaxDepth) {
          return false;
        }
        const char close = ch == '{' ? '}' : ']';
        ++_pos;
        skipSpaces();
        if (_pos < _json.size() && _json[_pos] == close) {
          ++_pos;
          _writer.put(ch);
          _writer.put(close);
          return true;
        }
        _writer.put(ch);
        _stack.push_back(ch);
        _writer.newLine(_stack.size());
        if (ch == '{' && !key()) {
          return false;
        }
        return value();
      }
      case '"':
        return string();
      case 't':
        return literal("true");
      case 'f':
        return literal("false");
      case 'n':
        return literal("null");
      default:
        return number();
    }
  }

  bool literal(std::string_view lit) {
    if (_json.substr(_pos, lit.size()) != lit) {
      return false;
    }
    _pos += lit.size();
    _writer.put(lit);
    return true;
  }

  bool string() {
    const std::size_t begin = _pos++;
    while (_pos < _json.size()) {
      const char ch = _json[_pos];
      if (ch == '"') {
        ++_pos;
        _writer.put(_json.substr(begin, _pos - begin));
        return true;
      }
      if (static_cast<unsigned char>(ch) < 0x20U) {
        return false;  // raw control characters are not allowed in JSON strings
      }
      if (ch == '\\') {
        if (++_pos >= _json.size()) {
          return false;
        }
        const char esc = _json[_pos];
        if (esc == 'u') {
          for (std::size_t i = 1; i <= 4; ++i) {
            if (_pos + i >= _json.size() || !aeronet::IsHexDigit(_json[_pos + i])) {
              return false;
            }
          }
          _pos += 4;
        } else if (!std::string_view{"\"\\/bfnrt"}.contains(esc)) {
          return false;
        }
      }
      ++_pos;
    }
    return false;
  }

  bool digits() noexcept {
    const std::size_t begin = _pos;
    while (_pos < _json.size() && aeronet::isdigit(_json[_pos])) {
      ++_pos;
    }
    return _pos != begin;
  }

  bool number() {
    const std::size_t begin = _pos;
    if (_json[_pos] == '-') {
      ++_pos;
    }
    if (_pos >= _json.size()) {
      return false;
    }
    if (_json[_pos] == '0') {
      ++_pos;
    } else if (!digits()) {
      return false;
    }
    if (_pos < _json.size() && _json[_pos] == '.') {
      ++_pos;
      if (!digits()) {
        return false;
      }
    }
    if (_pos < _json.size() && (_json[_pos] == 'e' || _json[_pos] == 'E')) {
      ++_pos;
      if (_pos < _json.size() && (_json[_pos] == '+' || _json[_pos] == '-')) {
        ++_pos;
      }
      if (!digits()) {
        return false;
      }
    }
    _writer.put(_json.substr(begin, _pos - begin));
    return true;
  }

  std::string_view _json;
  Writer& _writer;
  std::size_t _pos{0};
  // Open containers: '{' or '['. Inline storage covers any realistic document without allocating.
  amc::SmallVector<char, 64> _stack;
};

}  // namespace

bool PrettyPrintJson(std::string_view json, RawChars& out, std::uint32_t indent) {
  out.clear();
  // Pretty documents are typically 1.2x to 2x their minified size: one allocation in most cases.
  out.reserve(json.size() + (json.size() / 2) + 64);
  PrettyWriter writer(out, indent);
  return JsonFormatter<PrettyWriter>(json, writer).run();
}

bool IsValidJson(std::string_view json) {
  NullWriter writer;
  return JsonFormatter<NullWriter>(json, writer).run();
}

}  // namespace zalie
