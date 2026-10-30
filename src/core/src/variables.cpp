#include "zalie/variables.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <iterator>
#include <mutex>
#include <random>
#include <string>
#include <string_view>

#include "zalie/key-value.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/string-utils.hpp"

namespace zalie {

namespace {

constexpr int kMaxDepth = 8;

constexpr std::string_view kEnvPrefix = "$env.";
constexpr std::string_view kProcessEnvPrefix = "process.env.";

std::uint64_t RandomU64() {
  static std::mutex mutex;
  static std::mt19937_64 engine{std::random_device{}()};
  std::scoped_lock lock(mutex);
  return engine();
}

// Appends the value of a dynamic variable to 'out'. Returns false if 'name' is not a dynamic variable (or an undefined
// environment variable).
bool AppendDynamic(std::string_view name, RawChars32& out) {
  using namespace std::chrono;
  if (name == "$timestamp") {
    AppendInt(out, duration_cast<seconds>(system_clock::now().time_since_epoch()).count());
    return true;
  }
  if (name == "$timestampMs") {
    AppendInt(out, duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
    return true;
  }
  if (name == "$isoTimestamp") {
    std::format_to(std::back_inserter(out), "{:%FT%TZ}", floor<milliseconds>(system_clock::now()));
    return true;
  }
  if (name == "$randomInt") {
    AppendInt(out, RandomU64() % 1001U);
    return true;
  }
  if (name == "$randomUUID" || name == "$guid") {
    out.append(RandomUuid());
    return true;
  }
  std::string_view envName;
  if (name.starts_with(kEnvPrefix)) {
    envName = name.substr(kEnvPrefix.size());
  } else if (name.starts_with(kProcessEnvPrefix)) {
    envName = name.substr(kProcessEnvPrefix.size());
  } else {
    return false;
  }
  const std::string envNameStr(envName);  // getenv needs a null-terminated name
  // NOLINTNEXTLINE(concurrency-mt-unsafe) - Zalie never modifies its environment
  const char* pValue = std::getenv(envNameStr.c_str());
  if (pValue == nullptr) {
    return false;
  }
  out.append(std::string_view(pValue));
  return true;
}

void AddUnresolved(std::string_view name, UnresolvedNames* pUnresolved) {
  if (pUnresolved != nullptr && !std::ranges::contains(*pUnresolved, name)) {
    pUnresolved->push_back(name);
  }
}

void InterpolateImpl(std::string_view text, const VariableScope& scope, RawChars32& out, UnresolvedNames* pUnresolved,
                     int depth) {
  while (!text.empty()) {
    const std::size_t openPos = text.find("{{");
    if (openPos == std::string_view::npos) {
      out.append(text);
      return;
    }
    const std::size_t closePos = text.find("}}", openPos + 2);
    if (closePos == std::string_view::npos) {
      out.append(text);
      return;
    }
    out.append(text.substr(0, openPos));
    const std::string_view reference = text.substr(openPos, closePos + 2 - openPos);
    const std::string_view name = Trim(text.substr(openPos + 2, closePos - openPos - 2));
    text.remove_prefix(closePos + 2);

    const std::string_view* pValue = name.empty() ? nullptr : scope.find(name);
    if (pValue != nullptr) {
      if (depth < kMaxDepth && pValue->contains("{{")) {
        InterpolateImpl(*pValue, scope, out, pUnresolved, depth + 1);
      } else {
        out.append(*pValue);
      }
    } else if (name.empty() || !AppendDynamic(name, out)) {
      out.append(reference);
      AddUnresolved(name, pUnresolved);
    }
  }
}

}  // namespace

void VariableScope::addLayer(const KeyValues& variables) {
  for (const KeyValue& variable : variables) {
    if (variable.enabled && !variable.key.empty()) {
      _entries.push_back({variable.key, variable.value});
    }
  }
}

const std::string_view* VariableScope::find(std::string_view name) const noexcept {
  const auto it = std::ranges::find(_entries, name, &Entry::name);
  return it == _entries.end() ? nullptr : &it->value;
}

void FindVariableRefs(std::string_view text, VariableRefs& out) {
  std::size_t pos = 0;
  while (true) {
    const std::size_t openPos = text.find("{{", pos);
    if (openPos == std::string_view::npos) {
      return;
    }
    const std::size_t closePos = text.find("}}", openPos + 2);
    if (closePos == std::string_view::npos) {
      return;
    }
    out.push_back(
        {.begin = openPos, .end = closePos + 2, .name = Trim(text.substr(openPos + 2, closePos - openPos - 2))});
    pos = closePos + 2;
  }
}

bool IsDynamicVariable(std::string_view name) noexcept {
  static constexpr std::array kNames{std::string_view{"$timestamp"},    std::string_view{"$timestampMs"},
                                     std::string_view{"$isoTimestamp"}, std::string_view{"$randomInt"},
                                     std::string_view{"$randomUUID"},   std::string_view{"$guid"}};
  return std::ranges::contains(kNames, name) || name.starts_with(kEnvPrefix) || name.starts_with(kProcessEnvPrefix);
}

void Interpolate(std::string_view text, const VariableScope& scope, RawChars32& out, UnresolvedNames* pUnresolved) {
  InterpolateImpl(text, scope, out, pUnresolved, 0);
}

RawChars32 Interpolate(std::string_view text, const VariableScope& scope, UnresolvedNames* pUnresolved) {
  if (!text.contains("{{")) {
    return RawChars32(text);  // fast path: one exact allocation, no scanning
  }
  static constexpr std::size_t kExpectedGrowth = 32;
  RawChars32 out(text.size() + kExpectedGrowth);
  InterpolateImpl(text, scope, out, pUnresolved, 0);
  return out;
}

RawChars32 RandomUuid() {
  std::array<std::uint8_t, 16> bytes{};
  for (std::size_t i = 0; i < bytes.size(); i += 8) {
    const std::uint64_t value = RandomU64();
    for (std::size_t j = 0; j < 8; ++j) {
      bytes[i + j] = static_cast<std::uint8_t>(value >> (8U * j));
    }
  }
  bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0FU) | 0x40U);  // version 4
  bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3FU) | 0x80U);  // RFC 4122 variant
  static constexpr char kHex[] = "0123456789abcdef";
  static constexpr std::size_t kUuidLength = 36;
  RawChars32 ret(kUuidLength);
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) {
      ret.unchecked_push_back('-');
    }
    ret.unchecked_push_back(kHex[bytes[i] >> 4U]);
    ret.unchecked_push_back(kHex[bytes[i] & 0xFU]);
  }
  return ret;
}

}  // namespace zalie
