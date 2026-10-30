#pragma once

#include <amc/vector.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <string_view>

#include "zalie/collection.hpp"
#include "zalie/history.hpp"
#include "zalie/persistence.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/request.hpp"
#include "zalie/settings.hpp"
#include "zalie/string-pool.hpp"

namespace zalie {

// A collection open in the workspace, with its file.
struct OpenCollection {
  Collection collection;
  std::filesystem::path path;
  // Time of the first modification not saved yet (meaningful when dirty).
  std::chrono::steady_clock::time_point dirtySince;
  std::int32_t activeEnvironment{-1};
  bool dirty{false};
};

// Where a request lives.
struct RequestLocation {
  [[nodiscard]] bool valid() const noexcept { return pRequest != nullptr; }

  OpenCollection* pCollection{nullptr};
  Request* pRequest{nullptr};
};

// Everything Zalie persists: settings, open collections (each in its own file), open tabs and history.
// Files live in the home directory (see DefaultHomeDirectory): settings.json, workspace.json, history.json and
// collections/*.zalie.json. Collections can also be opened from anywhere (a git repository, for instance).
class Workspace {
 public:
  explicit Workspace(std::filesystem::path home);

  // Loads the settings, the workspace state, the history and the collections. On first launch (no workspace.json),
  // creates the example collection. Problems (unreadable files...) are reported in errors(), never fatal.
  void load();

  // Saves the settings, the workspace state, the history and every modified collection.
  void saveAll();

  // Saves the collections modified for at least 'delay' (debounced autosave). Returns the number saved.
  std::size_t autosave(std::chrono::milliseconds delay);

  // Creates a new empty collection, saved in the collections directory.
  OpenCollection& createCollection(std::string_view name);

  // Adds a collection (e.g. imported) saved to a new file of the collections directory.
  OpenCollection& addCollection(Collection collection);

  // Opens an existing collection file. If it is already open, returns it.
  std::expected<OpenCollection*, RawChars32> openCollection(const std::filesystem::path& path);

  // Removes the collection from the workspace (its file is kept).
  void closeCollection(std::size_t index);

  // Records a modification of the collection, for the autosave.
  static void MarkDirty(OpenCollection& collection) noexcept;

  // Finds a request by id in all the open collections.
  [[nodiscard]] RequestLocation findRequest(std::uint64_t requestId) noexcept;

  [[nodiscard]] std::size_t size() const noexcept { return _collections.size(); }
  [[nodiscard]] OpenCollection& operator[](std::size_t index) noexcept {
    return *_collections[static_cast<std::uint32_t>(index)];
  }
  [[nodiscard]] const OpenCollection& operator[](std::size_t index) const noexcept {
    return *_collections[static_cast<std::uint32_t>(index)];
  }

  [[nodiscard]] Settings& settings() noexcept { return _settings; }
  [[nodiscard]] History& history() noexcept { return _history; }
  [[nodiscard]] WorkspaceState& state() noexcept { return _state; }
  [[nodiscard]] const std::filesystem::path& home() const noexcept { return _home; }
  [[nodiscard]] std::filesystem::path collectionsDirectory() const { return _home / "collections"; }

  // Problems met while loading / saving, for display.
  [[nodiscard]] StringList& errors() noexcept { return _errors; }

 private:
  bool save(OpenCollection& collection);
  [[nodiscard]] std::filesystem::path newCollectionPath(std::string_view name) const;

  std::filesystem::path _home;
  // unique_ptr: references to open collections stay valid when others are added or closed.
  amc::vector<std::unique_ptr<OpenCollection>> _collections;
  Settings _settings;
  History _history;
  WorkspaceState _state;
  StringList _errors;
};

}  // namespace zalie
