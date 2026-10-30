#include "zalie/persistence.hpp"

#include <amc/vector.hpp>
#include <expected>
#include <filesystem>
#include <glaze/glaze.hpp>
#include <string>
#include <string_view>
#include <utility>

#include "model-glaze.hpp"
#include "zalie/collection.hpp"
#include "zalie/file-io.hpp"
#include "zalie/history.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/settings.hpp"

namespace zalie {

namespace {

template <class T>
std::string Serialize(const T& value) {
  std::string buffer;
  if (const auto errc = glz::write<kWriteOpts>(value, buffer); errc) {
    buffer.clear();  // cannot happen for these types (no variant / custom writer that may fail)
  }
  buffer.push_back('\n');
  return buffer;
}

template <class T>
std::expected<T, RawChars32> Parse(std::string_view json) {
  T value{};
  if (const auto errc = glz::read<kReadOpts>(value, json); errc) {
    const std::string message = glz::format_error(errc, json);
    return std::unexpected(RawChars32(std::string_view(message)));
  }
  return value;
}

}  // namespace

std::string SerializeCollection(const Collection& collection) { return Serialize(collection); }

std::expected<Collection, RawChars32> ParseCollection(std::string_view json) {
  auto collection = Parse<Collection>(json);
  if (collection) {
    EnsureUniqueIds(*collection);
  }
  return collection;
}

std::string SerializeSettings(const Settings& settings) { return Serialize(settings); }

std::expected<Settings, RawChars32> ParseSettings(std::string_view json) {
  auto settings = Parse<Settings>(json);
  if (settings) {
    settings->normalizeCards();
  }
  return settings;
}

std::string SerializeWorkspaceState(const WorkspaceState& state) { return Serialize(state); }

std::expected<WorkspaceState, RawChars32> ParseWorkspaceState(std::string_view json) {
  return Parse<WorkspaceState>(json);
}

std::string SerializeHistory(const amc::vector<HistoryEntry>& entries) { return Serialize(entries); }

std::expected<amc::vector<HistoryEntry>, RawChars32> ParseHistory(std::string_view json) {
  return Parse<amc::vector<HistoryEntry>>(json);
}

std::expected<Collection, RawChars32> LoadCollection(const std::filesystem::path& path) {
  auto content = ReadFile(path);
  if (!content) {
    return std::unexpected(std::move(content.error()));
  }
  auto collection = ParseCollection(*content);
  if (!collection) {
    return std::unexpected(Concat({PathToUtf8(path), ": ", collection.error()}));
  }
  return collection;
}

std::expected<void, RawChars32> SaveCollection(const Collection& collection, const std::filesystem::path& path) {
  return WriteFileAtomically(path, SerializeCollection(collection));
}

}  // namespace zalie
