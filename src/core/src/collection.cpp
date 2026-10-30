#include "zalie/collection.hpp"

#include <algorithm>
#include <amc/flatset.hpp>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <mutex>
#include <random>
#include <utility>

#include "zalie/key-value.hpp"
#include "zalie/method.hpp"
#include "zalie/request.hpp"

namespace zalie {

std::uint64_t NewId() noexcept {
  // One generator for the process, seeded once from the OS entropy source.
  static std::mutex mutex;
  static std::mt19937_64 engine{std::random_device{}()};
  std::uint64_t id = 0;
  std::scoped_lock lock(mutex);
  while (id == 0) {
    id = engine();
  }
  return id;
}

namespace {

using IdSet = amc::FlatSet<std::uint64_t>;

void MakeUnique(std::uint64_t& id, IdSet& seen) {
  if (id == 0 || !seen.insert(id).second) {
    do {
      id = NewId();
    } while (!seen.insert(id).second);
  }
}

void EnsureUniqueIds(Folder& folder, IdSet& seen) {
  MakeUnique(folder.id, seen);
  for (Folder& child : folder.folders) {
    EnsureUniqueIds(child, seen);
  }
  for (Request& request : folder.requests) {
    MakeUnique(request.id, seen);
  }
}

template <class FolderT>
auto* FindRequestImpl(FolderT& folder, std::uint64_t id) noexcept {
  using RequestPtr = decltype(folder.requests.data());
  for (auto& request : folder.requests) {
    if (request.id == id) {
      return &request;
    }
  }
  for (auto& child : folder.folders) {
    if (RequestPtr pRequest = FindRequestImpl(child, id); pRequest != nullptr) {
      return pRequest;
    }
  }
  return static_cast<RequestPtr>(nullptr);
}

auto FindRequestIt(Folder& folder, std::uint64_t requestId) noexcept {
  return std::ranges::find(folder.requests, requestId, &Request::id);
}

}  // namespace

void EnsureUniqueIds(Collection& collection) {
  IdSet seen;
  EnsureUniqueIds(collection.root, seen);
}

Request* FindRequest(Folder& root, std::uint64_t id) noexcept { return FindRequestImpl(root, id); }

const Request* FindRequest(const Folder& root, std::uint64_t id) noexcept { return FindRequestImpl(root, id); }

Folder* FindFolder(Folder& root, std::uint64_t id) noexcept {
  if (root.id == id) {
    return &root;
  }
  for (Folder& child : root.folders) {
    if (Folder* pFolder = FindFolder(child, id); pFolder != nullptr) {
      return pFolder;
    }
  }
  return nullptr;
}

Folder* FindParentOfRequest(Folder& root, std::uint64_t requestId) noexcept {
  if (FindRequestIt(root, requestId) != root.requests.end()) {
    return &root;
  }
  for (Folder& child : root.folders) {
    if (Folder* pFolder = FindParentOfRequest(child, requestId); pFolder != nullptr) {
      return pFolder;
    }
  }
  return nullptr;
}

Folder* FindParentOfFolder(Folder& root, std::uint64_t folderId) noexcept {
  for (Folder& child : root.folders) {
    if (child.id == folderId) {
      return &root;
    }
    if (Folder* pFolder = FindParentOfFolder(child, folderId); pFolder != nullptr) {
      return pFolder;
    }
  }
  return nullptr;
}

bool FindRequestPath(const Folder& root, std::uint64_t requestId, amc::vector<const Folder*>& path) {
  if (std::ranges::any_of(root.requests, [requestId](const Request& request) { return request.id == requestId; })) {
    return true;
  }
  for (const Folder& child : root.folders) {
    path.push_back(&child);
    if (FindRequestPath(child, requestId, path)) {
      return true;
    }
    path.pop_back();
  }
  return false;
}

bool RemoveRequest(Folder& root, std::uint64_t requestId) {
  Folder* pParent = FindParentOfRequest(root, requestId);
  if (pParent == nullptr) {
    return false;
  }
  pParent->requests.erase(FindRequestIt(*pParent, requestId));
  return true;
}

bool RemoveFolder(Folder& root, std::uint64_t folderId) {
  Folder* pParent = FindParentOfFolder(root, folderId);
  if (pParent == nullptr) {
    return false;
  }
  std::erase_if(pParent->folders, [folderId](const Folder& folder) { return folder.id == folderId; });
  return true;
}

Request* DuplicateRequest(Folder& root, std::uint64_t requestId) {
  Folder* pParent = FindParentOfRequest(root, requestId);
  if (pParent == nullptr) {
    return nullptr;
  }
  auto it = FindRequestIt(*pParent, requestId);
  Request copy = *it;
  copy.id = NewId();
  copy.name.append(" copy");
  it = pParent->requests.insert(std::next(it), std::move(copy));
  return &*it;
}

bool MoveRequest(Folder& root, std::uint64_t requestId, std::uint64_t targetFolderId, std::size_t index) {
  Folder* pSource = FindParentOfRequest(root, requestId);
  Folder* pTarget = FindFolder(root, targetFolderId);
  if (pSource == nullptr || pTarget == nullptr) {
    return false;
  }
  auto it = FindRequestIt(*pSource, requestId);
  const auto sourceIndex = static_cast<std::size_t>(std::distance(pSource->requests.begin(), it));
  Request moved = std::move(*it);
  pSource->requests.erase(it);
  if (pSource == pTarget && index > sourceIndex) {
    --index;  // the removal shifted the requests after it
  }
  index = std::min(index, static_cast<std::size_t>(pTarget->requests.size()));
  pTarget->requests.insert(pTarget->requests.begin() + static_cast<std::ptrdiff_t>(index), std::move(moved));
  return true;
}

std::size_t CountRequests(const Folder& root) noexcept {
  std::size_t count = 0;
  ForEachRequest(root, [&count](const Request&) { ++count; });
  return count;
}

Collection MakeExampleCollection() {
  Collection collection;
  collection.name.assign("Zalie examples");
  collection.docs.assign("A few requests to get started. Feel free to edit or delete them.");
  collection.variables.push_back(MakeKeyValue("baseUrl", "https://httpbin.org"));
  Environment local;
  local.name.assign("local");
  local.variables.push_back(MakeKeyValue("baseUrl", "http://localhost:8080"));
  collection.environments.push_back(std::move(local));

  Folder basics;
  basics.name.assign("Basics");

  Request get;
  get.name.assign("Get JSON");
  get.url.assign("{{baseUrl}}/json");
  get.headers.push_back(MakeKeyValue("Accept", "application/json"));
  basics.requests.push_back(std::move(get));

  Request post;
  post.name.assign("Post JSON");
  post.method = Method::post;
  post.url.assign("{{baseUrl}}/anything?source=zalie");
  post.params.push_back(MakeKeyValue("source", "zalie"));
  post.bodyMode = BodyMode::json;
  post.body.assign("{\n  \"name\": \"Zalie\",\n  \"breed\": \"Coton de Tulear\",\n  \"goodGirl\": true\n}");
  basics.requests.push_back(std::move(post));

  Request headers;
  headers.name.assign("Inspect my headers");
  headers.url.assign("{{baseUrl}}/headers");
  basics.requests.push_back(std::move(headers));

  collection.root.folders.push_back(std::move(basics));

  Request status;
  status.name.assign("I'm a teapot");
  status.url.assign("{{baseUrl}}/status/418");
  collection.root.requests.push_back(std::move(status));

  EnsureUniqueIds(collection);
  return collection;
}

}  // namespace zalie
