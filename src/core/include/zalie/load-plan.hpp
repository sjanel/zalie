#pragma once

#include <amc/vector.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <limits>
#include <span>
#include <string_view>

#include "zalie/data-file.hpp"
#include "zalie/load-settings.hpp"
#include "zalie/method.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/resolved-request.hpp"
#include "zalie/value-generators.hpp"
#include "zalie/variables.hpp"

namespace zalie {

struct Collection;
struct Request;

// How a generated value is written where it lands in the request.
enum class SlotEncoding : std::uint8_t {
  // As it is: a text or XML body, a JSON body outside of a string ({"page": {{page}}}).
  raw,
  // As it is, refused if it holds a line break or a null byte: header values and the beginning of a URL.
  checked,
  // Percent-encoded: the path and the query of a URL.
  url,
  // Form-encoded: the fields of a form body.
  form,
  // Escaped: inside a JSON string ({"q": "{{term}}"}).
  jsonString,
};

// A piece of text known in advance, followed by a generated value.
struct TemplatePart {
  std::string_view literal;
  // ValueGenerators::kNoVariable for a last part without value.
  std::uint32_t variable{ValueGenerators::kNoVariable};
  SlotEncoding encoding{SlotEncoding::raw};
};

// A text compiled once, then filled for each request.
class TextTemplate {
 public:
  [[nodiscard]] bool isConstant() const noexcept { return _parts.size() <= 1 && !hasValue(); }

  // The text of a constant template.
  [[nodiscard]] std::string_view literal() const noexcept {
    return _parts.empty() ? std::string_view{} : _parts.front().literal;
  }

  // Appends the text to 'out' with the given values of the variables. Returns false if a value is refused.
  [[nodiscard]] bool appendTo(std::span<const std::string_view> values, RawChars32& out) const;

  void addPart(TemplatePart part) { _parts.push_back(part); }

  using trivially_relocatable = AllTriviallyRelocatable<amc::vector<TemplatePart>>;

 private:
  [[nodiscard]] bool hasValue() const noexcept {
    return !_parts.empty() && _parts.back().variable != ValueGenerators::kNoVariable;
  }

  amc::vector<TemplatePart> _parts;
};

// Bytes [begin, end) of a text.
struct TextSpan {
  std::uint32_t begin{0};
  std::uint32_t end{0};
};

// Where a run puts the values of a JSON field parameter: the values that 'path' (a JSONPath) selects in 'body', inside
// their quotes for a string, whole for another value. A {{placeholder}} outside of a string ({"page": {{page}}}) counts
// as a value. Fails on an invalid path or body, or when nothing matches.
[[nodiscard]] std::expected<amc::vector<TextSpan>, RawChars32> JsonFieldSpans(std::string_view body,
                                                                              std::string_view path);

// A header sent by a run.
struct LoadHeader {
  using trivially_relocatable = AllTriviallyRelocatable<TextTemplate>;

  TextTemplate value;
  // Credentials "user:password" of a Basic authentication with generated values: 'value' is then not used.
  TextTemplate basicCredentials;
  std::string_view lowerName;
  // Replaces the header of the same name set by default.
  bool replaces{false};
  // Given with the body.
  bool isContentType{false};
  bool isBasicAuth{false};
};

// What prevents a run: shown before it starts.
struct LoadPlanIssue {
  static constexpr std::uint32_t kNoParameter = std::numeric_limits<std::uint32_t>::max();

  using trivially_relocatable = AllTriviallyRelocatable<RawChars32>;

  RawChars32 message;
  // The parameter concerned, or kNoParameter.
  std::uint32_t parameter{kNoParameter};
};

// A request compiled for a run: literal parts and variable slots, and the generators feeding them. The header and
// authentication rules of a normal Fetch apply. Compiling is done once, filling is done by LoadRequestFiller.
// Immutable once built, so that the load threads can share it.
class LoadPlan {
 public:
  // The same request sent each time.
  explicit LoadPlan(ResolvedRequest request);

  // Compiles 'request' with its run settings. Relative data file paths start at 'baseDirectory'. Data files not
  // indexed yet are scanned here, which can take long: call it off the UI thread. See valid() for the outcome.
  LoadPlan(const Request& request, const Collection& collection, const VariableScope& scope,
           const LoadSettings& settings, const std::filesystem::path& baseDirectory, DataFileIndexCache& dataFiles);

  [[nodiscard]] bool valid() const noexcept { return _issues.empty(); }

  // What prevents the run: an empty range, a variable without value, a missing file...
  [[nodiscard]] std::span<const LoadPlanIssue> issues() const noexcept { return _issues; }

  // The headers of the request that will not be sent, and why.
  [[nodiscard]] std::span<const RawChars32> warnings() const noexcept { return _warnings; }

  // True when every request is identical.
  [[nodiscard]] bool isConstant() const noexcept { return _isConstant; }

  [[nodiscard]] const ValueGenerators& generators() const noexcept { return _generators; }
  [[nodiscard]] std::uint64_t seed() const noexcept { return _generators.seed(); }
  [[nodiscard]] std::uint64_t passLength() const noexcept { return _generators.passLength(); }

  // Number of distinct values of a parameter of the settings: 1 for a fixed one, 0 when it is unbounded.
  [[nodiscard]] std::uint64_t valueCount(std::uint32_t parameter) const noexcept;

  // The generated variable of a parameter of the settings, ValueGenerators::kNoVariable for a fixed or invalid one.
  [[nodiscard]] std::uint32_t parameterVariable(std::uint32_t parameter) const noexcept {
    return _parameterVariables[parameter];
  }

  // The host the requests go to, empty when a generated value is part of it (or when the plan is not valid).
  [[nodiscard]] std::string_view host() const noexcept { return _host; }

  [[nodiscard]] Method method() const noexcept { return _resolved.method; }
  [[nodiscard]] const RequestSettings& settings() const noexcept { return _resolved.settings; }
  [[nodiscard]] const TextTemplate& url() const noexcept { return _url; }
  [[nodiscard]] bool urlNeedsScheme() const noexcept { return _urlNeedsScheme; }
  [[nodiscard]] const TextTemplate& body() const noexcept { return _body; }
  [[nodiscard]] std::span<const LoadHeader> headers() const noexcept { return _headers; }

  // Bytes of the header lines around their values.
  [[nodiscard]] std::size_t headerLinesBytes() const noexcept { return _headerLinesBytes; }

  // The request 'number' as it will be sent. Fails if a generated value is refused.
  [[nodiscard]] std::expected<ResolvedRequest, RawChars32> preview(std::uint64_t number) const;

 private:
  // 'sentinelPrefix' is what generated values look like in the resolved request, empty if there is none.
  void compileHeaders(std::string_view sentinelPrefix, std::string_view basicCredentials);

  ResolvedRequest _resolved;
  ValueGenerators _generators;
  TextTemplate _url;
  TextTemplate _body;
  amc::vector<LoadHeader> _headers;
  amc::vector<LoadPlanIssue> _issues;
  amc::vector<RawChars32> _warnings;
  amc::vector<std::uint32_t> _parameterVariables;
  std::string_view _host;
  std::size_t _headerLinesBytes{0};
  bool _urlNeedsScheme{false};
  bool _isConstant{true};
};

// Fills the requests of a plan: one per thread, its buffers are reused from a request to the next.
class LoadRequestFiller {
 public:
  explicit LoadRequestFiller(const LoadPlan& plan);

  // Why the requests cannot be filled at all: a data file is missing, or changed since it was indexed.
  [[nodiscard]] std::string_view setupError() const noexcept { return _cursor.error(); }

  // Computes request 'number'. Returns false if it must not be sent: see error().
  [[nodiscard]] bool fill(std::uint64_t number);

  // Why the last fill() failed (static storage).
  [[nodiscard]] std::string_view error() const noexcept { return _error; }

  // What the last fill() computed, valid until the next one.
  [[nodiscard]] std::string_view url() const noexcept { return _url; }
  [[nodiscard]] std::string_view body() const noexcept { return _body; }
  [[nodiscard]] std::string_view headerValue(std::uint32_t header) const noexcept {
    return std::string_view(_headerValues).substr(_headerEnds[header], _headerEnds[header + 1] - _headerEnds[header]);
  }
  [[nodiscard]] std::size_t headerValuesBytes() const noexcept { return _headerValues.size(); }

 private:
  const LoadPlan* _pPlan;
  ValueGenerators::Cursor _cursor;
  RawChars32 _url;
  RawChars32 _body;
  RawChars32 _headerValues;
  RawChars32 _scratch;
  amc::vector<std::uint32_t> _headerEnds;
  std::string_view _error;
};

}  // namespace zalie
