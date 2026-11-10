#include "zalie/variables.hpp"

#include <aeronet/char-hexadecimal-converter.hpp>
#include <aeronet/timestring.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <random>
#include <span>
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

void AppendWordBytes(std::uint64_t word, std::span<std::uint8_t, 8> bytes) noexcept {
  for (std::size_t idx = 0; idx < bytes.size(); ++idx) {
    bytes[idx] = static_cast<std::uint8_t>(word >> (8U * idx));
  }
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
    } else if (name.empty() || !AppendDynamicVariable(name, out)) {
      out.append(reference);
      AddUnresolved(name, pUnresolved);
    }
  }
}

}  // namespace

bool AppendDynamicVariable(std::string_view name, RawChars32& out) {
  using namespace std::chrono;
  if (name == "$timestamp") {
    AppendAll(out, duration_cast<seconds>(system_clock::now().time_since_epoch()).count());
    return true;
  }
  if (name == "$timestampMs") {
    AppendAll(out, duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
    return true;
  }
  if (name == "$isoTimestamp") {
    static constexpr std::size_t kIsoTimestampSize = 24;  // YYYY-MM-DDTHH:MM:SS.sssZ
    out.ensureAvailableCapacityExponential(kIsoTimestampSize);
    out.setEnd(aeronet::TimeToStringISO8601UTCWithMs(system_clock::now(), out.end()));
    return true;
  }
  if (name == "$randomInt") {
    AppendAll(out, RandomU64() % 1001U);
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
  static constexpr std::size_t kUuidLength = 36;
  RawChars32 ret(kUuidLength);
  AppendUuid(RandomU64(), RandomU64(), ret);
  return ret;
}

void AppendUuid(std::uint64_t high, std::uint64_t low, RawChars32& out) {
  static constexpr std::size_t kUuidLength = 36;
  std::array<std::uint8_t, 16> bytes{};
  AppendWordBytes(high, std::span<std::uint8_t, 8>(bytes.data(), 8));
  AppendWordBytes(low, std::span<std::uint8_t, 8>(bytes.data() + 8, 8));
  bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0FU) | 0x40U);  // version 4
  bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3FU) | 0x80U);  // RFC 4122 variant
  out.ensureAvailableCapacityExponential(kUuidLength);
  char* pData = out.end();
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) {
      *pData++ = '-';
    }
    pData = aeronet::ToLowerHex(static_cast<unsigned char>(bytes[i]), pData);
  }
  out.setEnd(pData);
}

}  // namespace zalie
