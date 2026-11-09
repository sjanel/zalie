#pragma once

#include <amc/vector.hpp>
#include <cstddef>
#include <cstdint>

#include "zalie/key-value.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/request.hpp"

namespace zalie {

// A folder of a collection. Sub-folders are listed before requests.
struct Folder {
  bool operator==(const Folder&) const = default;

  using trivially_relocatable = AllTriviallyRelocatable<RawChars32, amc::vector<Folder>, amc::vector<Request>>;

  RawChars32 name;
  amc::vector<Folder> folders;
  amc::vector<Request> requests;
  std::uint64_t id{0};
};

// A named set of variables (e.g. "local", "staging", "prod").
struct Environment {
  bool operator==(const Environment&) const = default;

  using trivially_relocatable = AllTriviallyRelocatable<RawChars32, KeyValues>;

  RawChars32 name;
  KeyValues variables;
};

// A collection of requests, stored in its own file.
struct Collection {
  static constexpr std::uint32_t kFormatVersion = 1;

  bool operator==(const Collection&) const = default;

  RawChars32 name;
  RawChars32 docs;
  // Variables available to every request, overridden by the active environment.
  KeyValues variables;
  // Headers sent with every request (a request header with the same name wins).
  KeyValues headers;
  amc::vector<Environment> environments;
  Folder root;
  // Authentication used by requests in AuthMode::inherit.
  Auth auth{.mode = AuthMode::none};
  std::uint32_t formatVersion{kFormatVersion};
};

// Returns a new random non-zero identifier.
[[nodiscard]] std::uint64_t NewId() noexcept;

// Gives a fresh identifier to every folder / request whose id is 0 or already used in the collection.
void EnsureUniqueIds(Collection& collection);

// Finds a request by id in the folder tree, nullptr if absent.
[[nodiscard]] Request* FindRequest(Folder& root, std::uint64_t id) noexcept;
[[nodiscard]] const Request* FindRequest(const Folder& root, std::uint64_t id) noexcept;

// Finds a folder by id (the root itself matches its own id), nullptr if absent.
[[nodiscard]] Folder* FindFolder(Folder& root, std::uint64_t id) noexcept;

// Finds the folder directly containing the request / folder with this id, nullptr if absent.
[[nodiscard]] Folder* FindParentOfRequest(Folder& root, std::uint64_t requestId) noexcept;
[[nodiscard]] Folder* FindParentOfFolder(Folder& root, std::uint64_t folderId) noexcept;

// Appends to 'path' the folders leading to the request, from a child of 'root' to the folder directly containing it
// (nothing for a request of the root). Returns false, 'path' unchanged, if the request is absent.
bool FindRequestPath(const Folder& root, std::uint64_t requestId, amc::vector<const Folder*>& path);

// Removes the request / folder (with its content) with this id. Returns false if absent.
bool RemoveRequest(Folder& root, std::uint64_t requestId);
bool RemoveFolder(Folder& root, std::uint64_t folderId);

// Inserts a copy of the request with this id right after it, with a new id and a " copy" suffix.
// Returns the copy, or nullptr if the request is absent. Invalidates pointers on requests of the same folder.
Request* DuplicateRequest(Folder& root, std::uint64_t requestId);

// Moves a request into the folder 'targetFolderId', at position 'index' (clamped). Returns false if either is absent.
bool MoveRequest(Folder& root, std::uint64_t requestId, std::uint64_t targetFolderId, std::size_t index);

// Number of requests in the folder tree.
[[nodiscard]] std::size_t CountRequests(const Folder& root) noexcept;

// Calls 'func(Request&)' (or const) for every request of the folder tree, depth first, folders before requests.
template <class FolderT, class Func>
void ForEachRequest(FolderT& folder, Func&& func) {
  for (auto& child : folder.folders) {
    ForEachRequest(child, func);
  }
  for (auto& request : folder.requests) {
    func(request);
  }
}

// A new collection with a few example requests, used on first launch.
[[nodiscard]] Collection MakeExampleCollection();

}  // namespace zalie
