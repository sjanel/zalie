#pragma once

#include <amc/vector.hpp>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

#include "zalie/collection.hpp"
#include "zalie/history.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/settings.hpp"

namespace zalie {

// A collection referenced by the workspace.
struct CollectionRef {
  bool operator==(const CollectionRef&) const = default;

  // UTF-8 path of the collection file.
  RawChars32 path;
  // Index of the active environment, -1 for none.
  std::int32_t activeEnvironment{-1};

  using trivially_relocatable = AllTriviallyRelocatable<RawChars32>;
};

// What is open in the workspace (workspace.json).
struct WorkspaceState {
  bool operator==(const WorkspaceState&) const = default;

  amc::vector<CollectionRef> collections;
  // Ids of the requests open in tabs, in tab order.
  amc::vector<std::uint64_t> openTabs;
  std::uint64_t activeTab{0};
};

// JSON (de)serialization of the persisted types. The output buffers are std::string: glaze needs a resizable buffer.
// Pretty-printed with stable key order, so that collection files diff nicely in git.
[[nodiscard]] std::string SerializeCollection(const Collection& collection);
[[nodiscard]] std::expected<Collection, RawChars32> ParseCollection(std::string_view json);

[[nodiscard]] std::string SerializeSettings(const Settings& settings);
[[nodiscard]] std::expected<Settings, RawChars32> ParseSettings(std::string_view json);

[[nodiscard]] std::string SerializeWorkspaceState(const WorkspaceState& state);
[[nodiscard]] std::expected<WorkspaceState, RawChars32> ParseWorkspaceState(std::string_view json);

[[nodiscard]] std::string SerializeHistory(const amc::vector<HistoryEntry>& entries);
[[nodiscard]] std::expected<amc::vector<HistoryEntry>, RawChars32> ParseHistory(std::string_view json);

// File helpers: read + parse (ids are made unique), serialize + atomic write.
[[nodiscard]] std::expected<Collection, RawChars32> LoadCollection(const std::filesystem::path& path);
[[nodiscard]] std::expected<void, RawChars32> SaveCollection(const Collection& collection,
                                                             const std::filesystem::path& path);

}  // namespace zalie
