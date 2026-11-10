#include "zalie/workspace.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <ranges>
#include <string_view>
#include <system_error>
#include <utility>

#include "zalie/collection.hpp"
#include "zalie/file-io.hpp"
#include "zalie/history.hpp"
#include "zalie/paths.hpp"
#include "zalie/persistence.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/request.hpp"

namespace zalie {

namespace {

namespace fs = std::filesystem;

constexpr std::string_view kSettingsFile = "settings.json";
constexpr std::string_view kWorkspaceFile = "workspace.json";
constexpr std::string_view kHistoryFile = "history.json";

bool SamePath(const fs::path& lhs, const fs::path& rhs) {
  std::error_code errc;
  return fs::equivalent(lhs, rhs, errc) || lhs.lexically_normal() == rhs.lexically_normal();
}

}  // namespace

Workspace::Workspace(fs::path home) : _home(std::move(home)) { _settings.normalizeCards(); }

void Workspace::load() {
  if (auto content = ReadFile(_home / kSettingsFile)) {
    if (auto settings = ParseSettings(*content)) {
      _settings = std::move(*settings);
    } else {
      _errors.push_back({"settings.json: ", settings.error()});
    }
  }
  _settings.normalizeCards();

  if (auto content = ReadFile(_home / kHistoryFile)) {
    if (auto entries = ParseHistory(*content)) {
      for (HistoryEntry& entry : std::views::reverse(*entries)) {
        _history.add(std::move(entry));  // oldest first, as add() inserts at the front
      }
    } else {
      _errors.push_back({"history.json: ", entries.error()});
    }
  }

  auto content = ReadFile(_home / kWorkspaceFile);
  if (!content) {
    // First launch: start with the example collection, and record it right away (a crash before the first regular
    // save must not create the example again at the next launch).
    addCollection(MakeExampleCollection());
    saveAll();
    return;
  }
  auto state = ParseWorkspaceState(*content);
  if (!state) {
    _errors.push_back({"workspace.json: ", state.error()});
    return;
  }
  _state = std::move(*state);
  for (const CollectionRef& ref : _state.collections) {
    auto opened = openCollection(PathFromUtf8(ref.path));
    if (!opened) {
      _errors.push_back(View(opened.error()));
      continue;
    }
    OpenCollection& collection = **opened;
    if (ref.activeEnvironment >= 0 && std::cmp_less(ref.activeEnvironment, collection.collection.environments.size())) {
      collection.activeEnvironment = ref.activeEnvironment;
    }
  }
}

bool Workspace::save(OpenCollection& collection) {
  auto saved = SaveCollection(collection.collection, collection.path);
  if (!saved) {
    _errors.push_back(View(saved.error()));
    return false;
  }
  collection.dirty = false;
  return true;
}

void Workspace::saveAll() {
  for (auto& pCollection : _collections) {
    if (pCollection->dirty) {
      save(*pCollection);
    }
  }
  _state.collections.clear();
  for (const auto& pCollection : _collections) {
    _state.collections.push_back(
        {.path = PathToUtf8(pCollection->path), .activeEnvironment = pCollection->activeEnvironment});
  }
  const auto write = [this](std::string_view file, std::string_view content) {
    if (auto written = WriteFileAtomically(_home / file, content); !written) {
      _errors.push_back(View(written.error()));
    }
  };
  write(kSettingsFile, SerializeSettings(_settings));
  write(kWorkspaceFile, SerializeWorkspaceState(_state));
  write(kHistoryFile, SerializeHistory(_history.entries()));
}

std::size_t Workspace::autosave(std::chrono::milliseconds delay) {
  const auto now = std::chrono::steady_clock::now();
  std::size_t nbSaved = 0;
  for (auto& pCollection : _collections) {
    if (pCollection->dirty && now - pCollection->dirtySince >= delay && save(*pCollection)) {
      ++nbSaved;
    }
  }
  return nbSaved;
}

fs::path Workspace::newCollectionPath(std::string_view name) const {
  const fs::path dir = collectionsDirectory();
  const RawChars32 fileName = CollectionFileName(name);
  fs::path path = dir / PathFromUtf8(fileName);
  std::error_code errc;
  for (int suffix = 2; fs::exists(path, errc); ++suffix) {
    std::string_view stem = fileName;
    stem.remove_suffix(std::string_view{".zalie.json"}.size());
    RawChars32 numbered;
    AppendAll(numbered, stem, '-', suffix, ".zalie.json");
    path = dir / PathFromUtf8(numbered);
  }
  return path;
}

OpenCollection& Workspace::createCollection(std::string_view name) {
  Collection collection;
  collection.name.assign(name);
  return addCollection(std::move(collection));
}

OpenCollection& Workspace::addCollection(Collection collection) {
  EnsureUniqueIds(collection);
  auto pOpen = std::make_unique<OpenCollection>();
  pOpen->path = newCollectionPath(collection.name);
  pOpen->collection = std::move(collection);
  OpenCollection& ret = *_collections.emplace_back(std::move(pOpen));
  save(ret);
  return ret;
}

std::expected<OpenCollection*, RawChars32> Workspace::openCollection(const fs::path& path) {
  for (auto& pCollection : _collections) {
    if (SamePath(pCollection->path, path)) {
      return pCollection.get();
    }
  }
  auto collection = LoadCollection(path);
  if (!collection) {
    return std::unexpected(std::move(collection.error()));
  }
  auto pOpen = std::make_unique<OpenCollection>();
  pOpen->collection = std::move(*collection);
  pOpen->path = path;
  return _collections.emplace_back(std::move(pOpen)).get();
}

void Workspace::closeCollection(std::size_t index) {
  if (index < _collections.size()) {
    if (OpenCollection& collection = *_collections[static_cast<std::uint32_t>(index)]; collection.dirty) {
      save(collection);
    }
    _collections.erase(_collections.begin() + static_cast<std::ptrdiff_t>(index));
  }
}

void Workspace::MarkDirty(OpenCollection& collection) noexcept {
  if (!collection.dirty) {
    collection.dirty = true;
    collection.dirtySince = std::chrono::steady_clock::now();
  }
}

RequestLocation Workspace::findRequest(std::uint64_t requestId) noexcept {
  for (auto& pCollection : _collections) {
    if (Request* pRequest = FindRequest(pCollection->collection.root, requestId); pRequest != nullptr) {
      return {.pCollection = pCollection.get(), .pRequest = pRequest};
    }
  }
  return {};
}

}  // namespace zalie
