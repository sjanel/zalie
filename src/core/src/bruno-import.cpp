#include "zalie/bruno-import.hpp"

#include <algorithm>
#include <amc/vector.hpp>
#include <charconv>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "json-scan.hpp"
#include "zalie/collection.hpp"
#include "zalie/encoding.hpp"
#include "zalie/file-io.hpp"
#include "zalie/key-value.hpp"
#include "zalie/method.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/request.hpp"
#include "zalie/string-utils.hpp"
#include "zalie/url.hpp"

namespace zalie {

namespace {

namespace fs = std::filesystem;

// A top-level block of a .bru file: "name {" ... "}" (dictionary or text) or "name [" ... "]" (list).
// 'body' is the raw text between the opening and the closing lines, still indented.
struct BruBlock {
  std::string_view name;
  std::string_view body;
  bool isList{false};
};

using BruBlocks = amc::vector<BruBlock>;

// Calls func(line) for each line of 'text', without the end of line characters.
template <class Func>
void ForEachLine(std::string_view text, Func&& func) {
  while (!text.empty()) {
    const std::size_t eolPos = text.find('\n');
    std::string_view line = text.substr(0, eolPos);
    if (line.ends_with('\r')) {
      line.remove_suffix(1);
    }
    func(line);
    if (eolPos == std::string_view::npos) {
      break;
    }
    text.remove_prefix(eolPos + 1);
  }
}

BruBlocks ParseBlocks(std::string_view content) {
  BruBlocks blocks;
  std::size_t pos = 0;
  while (pos < content.size()) {
    std::size_t eolPos = content.find('\n', pos);
    if (eolPos == std::string_view::npos) {
      eolPos = content.size();
    }
    const std::string_view line = Trim(content.substr(pos, eolPos - pos));
    pos = eolPos + 1;
    if (line.empty() || (!line.ends_with('{') && !line.ends_with('['))) {
      continue;
    }
    const bool isList = line.ends_with('[');
    const char closing = isList ? ']' : '}';
    BruBlock block{.name = Trim(line.substr(0, line.size() - 1)), .body = {}, .isList = isList};
    // The block ends at the first line starting (column 0) with the closing character.
    const std::size_t bodyBegin = std::min(pos, content.size());
    std::size_t scan = bodyBegin;
    std::size_t bodyEnd = content.size();
    while (scan < content.size()) {
      std::size_t lineEnd = content.find('\n', scan);
      if (lineEnd == std::string_view::npos) {
        lineEnd = content.size();
      }
      if (content[scan] == closing && Trim(content.substr(scan, lineEnd - scan)).size() == 1) {
        bodyEnd = scan;
        scan = lineEnd + 1;
        break;
      }
      scan = lineEnd + 1;
    }
    block.body = content.substr(bodyBegin, bodyEnd - bodyBegin);
    blocks.push_back(block);
    pos = scan;
  }
  return blocks;
}

// Calls func(key, value, enabled) for each "key: value" line of a dictionary block. '~' prefixes disabled entries.
template <class Func>
void ForEachPair(std::string_view body, Func&& func) {
  ForEachLine(body, [&func](std::string_view line) {
    line = Trim(line);
    if (line.empty()) {
      return;
    }
    const bool enabled = !line.starts_with('~');
    if (!enabled) {
      line.remove_prefix(1);
    }
    const std::size_t colonPos = line.find(':');
    if (colonPos == std::string_view::npos) {
      func(line, std::string_view{}, enabled);
    } else {
      func(Trim(line.substr(0, colonPos)), Trim(line.substr(colonPos + 1)), enabled);
    }
  });
}

void ParsePairs(std::string_view body, KeyValues& out) {
  ForEachPair(body, [&out](std::string_view key, std::string_view value, bool enabled) {
    out.push_back(MakeKeyValue(key, value, enabled));
  });
}

std::string_view FindPair(std::string_view body, std::string_view wantedKey) {
  std::string_view ret;
  ForEachPair(body, [&](std::string_view key, std::string_view value, bool enabled) {
    if (enabled && key == wantedKey && ret.empty()) {
      ret = value;
    }
  });
  return ret;
}

// Content of a text block, un-indented by the 2 spaces Bruno adds.
RawChars32 Dedent(std::string_view body) {
  RawChars32 ret(body.size());
  ForEachLine(body, [&ret](std::string_view line) {
    if (line.starts_with("  ")) {
      line.remove_prefix(2);
    }
    ret.unchecked_append(line);
    ret.unchecked_push_back('\n');
  });
  while (!ret.empty() && ret[ret.size() - 1] == '\n') {
    ret.setSize(ret.size() - 1);
  }
  return ret;
}

int ParseInt(std::string_view str, int defaultValue) noexcept {
  int value = defaultValue;
  const auto [ptr, errc] = std::from_chars(str.data(), str.data() + str.size(), value);
  return errc == std::errc{} ? value : defaultValue;
}

AuthMode ParseAuthMode(std::string_view mode) noexcept {
  if (mode == "basic") {
    return AuthMode::basic;
  }
  if (mode == "bearer") {
    return AuthMode::bearer;
  }
  if (mode == "apikey") {
    return AuthMode::apiKey;
  }
  if (mode == "inherit") {
    return AuthMode::inherit;
  }
  return AuthMode::none;  // none, and the schemes Zalie does not support (digest, oauth2, awsv4, ntlm...)
}

// Applies an "auth:<scheme>" block to 'auth'. Returns false if the block is not an auth block.
bool ApplyAuthBlock(const BruBlock& block, Auth& auth) {
  if (block.name == "auth:basic") {
    auth.username.assign(FindPair(block.body, "username"));
    auth.password.assign(FindPair(block.body, "password"));
  } else if (block.name == "auth:bearer") {
    auth.token.assign(FindPair(block.body, "token"));
  } else if (block.name == "auth:apikey") {
    auth.apiKeyName.assign(FindPair(block.body, "key"));
    auth.apiKeyValue.assign(FindPair(block.body, "value"));
    auth.apiKeyInQuery = FindPair(block.body, "placement") == "queryparams";
  } else {
    return false;
  }
  return true;
}

BodyMode ParseBodyMode(std::string_view mode) noexcept {
  if (mode == "json") {
    return BodyMode::json;
  }
  if (mode == "text") {
    return BodyMode::text;
  }
  if (mode == "xml") {
    return BodyMode::xml;
  }
  if (mode == "formUrlEncoded" || mode == "multipartForm") {
    return BodyMode::formUrlEncoded;
  }
  return BodyMode::none;
}

// The value of a top-level string member of a small JSON document (bruno.json), empty if it has none.
RawChars32 ExtractJsonString(std::string_view json, std::string_view key) {
  RawChars32 ret;
  json = Trim(json);
  if (json.starts_with('{')) {
    // The scan stops at the member, or at the first malformed one: 'ret' is then left empty.
    static_cast<void>(ScanJsonObject(json, [&ret, key](std::string_view rawKey, std::string_view value) {
      if (rawKey != key) {
        return true;
      }
      if (value.starts_with('"') && !AppendJsonUnescaped(value.substr(1, value.size() - 2), ret)) {
        ret.clear();
      }
      return false;
    }));
  }
  return ret;
}

RawChars32 FileStem(const fs::path& path) { return PathToUtf8(path.stem()); }

// Whether a block of a folder.bru changes what its requests send. Zalie has no folder level headers / auth: the
// requests use the ones of the collection, which is what "auth { mode: inherit }" (Bruno's default) means.
bool IsFolderLevelOverride(const BruBlock& block) {
  if (block.name == "auth") {
    const std::string_view mode = FindPair(block.body, "mode");
    return !mode.empty() && mode != "inherit";
  }
  if (block.name == "headers") {
    bool any = false;
    ForEachPair(block.body,
                [&any](std::string_view key, std::string_view, bool enabled) { any |= enabled && !key.empty(); });
    return any;
  }
  return block.name.starts_with("auth:");
}

struct SortedFolder {
  Folder folder;
  int sequence{INT_MAX};
};

void ImportFolder(const fs::path& directory, Folder& folder, BrunoImport& result, bool isRoot);

void ImportSubFolder(const fs::path& directory, amc::vector<SortedFolder>& folders, BrunoImport& result) {
  SortedFolder sorted;
  sorted.folder.name = PathToUtf8(directory.filename());
  const fs::path folderMeta = directory / "folder.bru";
  std::error_code errc;
  if (fs::is_regular_file(folderMeta, errc)) {
    if (auto content = ReadFile(folderMeta)) {
      for (const BruBlock& block : ParseBlocks(*content)) {
        if (block.name == "meta") {
          if (const std::string_view name = FindPair(block.body, "name"); !name.empty()) {
            sorted.folder.name.assign(name);
          }
          sorted.sequence = ParseInt(FindPair(block.body, "seq"), INT_MAX);
        } else if (IsFolderLevelOverride(block)) {
          result.warnings.push_back({PathToUtf8(folderMeta), ": folder level headers / auth are not supported"});
        }
      }
    } else {
      result.warnings.push_back(View(content.error()));
    }
  }
  ImportFolder(directory, sorted.folder, result, false);
  folders.push_back(std::move(sorted));
}

void ImportFolder(const fs::path& directory, Folder& folder, BrunoImport& result, bool isRoot) {
  amc::vector<SortedFolder> folders;
  amc::vector<BruRequest> requests;
  std::error_code errc;
  // Sorted iteration, for a deterministic import whatever the file system order.
  std::vector<fs::directory_entry> entries;
  for (const fs::directory_entry& entry : fs::directory_iterator(directory, errc)) {
    entries.push_back(entry);
  }
  std::ranges::sort(entries, {}, [](const fs::directory_entry& entry) { return entry.path().filename(); });

  for (const fs::directory_entry& entry : entries) {
    const fs::path& path = entry.path();
    const RawChars32 filename = PathToUtf8(path.filename());
    const std::string_view name = filename;
    if (name.starts_with('.') || name == "node_modules") {
      continue;
    }
    if (entry.is_directory(errc)) {
      if (!(isRoot && name == "environments")) {
        ImportSubFolder(path, folders, result);
      }
      continue;
    }
    if (!name.ends_with(".bru") || name == "folder.bru" || name == "collection.bru") {
      continue;
    }
    auto content = ReadFile(path);
    if (!content) {
      result.warnings.push_back(View(content.error()));
      continue;
    }
    auto parsed = ParseBruRequest(*content);
    if (!parsed) {
      result.warnings.push_back({PathToUtf8(path), ": ", parsed.error()});
      continue;
    }
    if (parsed->request.name.empty()) {
      parsed->request.name = FileStem(path);
    }
    requests.push_back(std::move(*parsed));
  }

  std::ranges::stable_sort(folders, [](const SortedFolder& lhs, const SortedFolder& rhs) {
    return lhs.sequence != rhs.sequence ? lhs.sequence < rhs.sequence : View(lhs.folder.name) < View(rhs.folder.name);
  });
  std::ranges::stable_sort(requests, {}, &BruRequest::sequence);
  for (SortedFolder& sorted : folders) {
    folder.folders.push_back(std::move(sorted.folder));
  }
  for (BruRequest& request : requests) {
    folder.requests.push_back(std::move(request.request));
  }
}

void ImportCollectionFile(const fs::path& path, Collection& collection, BrunoImport& result) {
  auto content = ReadFile(path);
  if (!content) {
    result.warnings.push_back(View(content.error()));
    return;
  }
  for (const BruBlock& block : ParseBlocks(*content)) {
    if (block.name == "headers") {
      ParsePairs(block.body, collection.headers);
    } else if (block.name == "auth") {
      collection.auth.mode = ParseAuthMode(FindPair(block.body, "mode"));
    } else if (ApplyAuthBlock(block, collection.auth)) {
      // done
    } else if (block.name == "vars:pre-request") {
      ParsePairs(block.body, collection.variables);
    } else if (block.name == "docs") {
      collection.docs = Dedent(block.body);
    } else if (block.name.starts_with("script") || block.name == "tests") {
      result.warnings.push_back({PathToUtf8(path), ": collection scripts and tests are not supported"});
    }
  }
  if (collection.auth.mode == AuthMode::inherit) {
    collection.auth.mode = AuthMode::none;
  }
}

}  // namespace

std::expected<BruRequest, RawChars32> ParseBruRequest(std::string_view content) {
  BruRequest result;
  Request& req = result.request;
  RawChars32 jsonBody;
  RawChars32 textBody;
  RawChars32 xmlBody;
  KeyValues urlEncodedForm;
  KeyValues multipartForm;
  KeyValues queryParams;
  bool hasMethod = false;
  for (const BruBlock& block : ParseBlocks(content)) {
    if (block.name == "meta") {
      req.name.assign(FindPair(block.body, "name"));
      result.sequence = ParseInt(FindPair(block.body, "seq"), 0);
    } else if (auto method = ParseMethod(block.name); method && !block.name.contains(':')) {
      hasMethod = true;
      req.method = *method;
      req.url.assign(FindPair(block.body, "url"));
      req.bodyMode = ParseBodyMode(FindPair(block.body, "body"));
      req.auth.mode = ParseAuthMode(FindPair(block.body, "auth"));
    } else if (block.name == "headers") {
      ParsePairs(block.body, req.headers);
    } else if (block.name == "params:query" || block.name == "query") {
      ParsePairs(block.body, queryParams);
    } else if (ApplyAuthBlock(block, req.auth)) {
      // done
    } else if (block.name == "body:json") {
      jsonBody = Dedent(block.body);
    } else if (block.name == "body:text") {
      textBody = Dedent(block.body);
    } else if (block.name == "body:xml") {
      xmlBody = Dedent(block.body);
    } else if (block.name == "body:form-urlencoded") {
      ParsePairs(block.body, urlEncodedForm);
    } else if (block.name == "body:multipart-form") {
      ParsePairs(block.body, multipartForm);
    } else if (block.name == "docs") {
      req.docs = Dedent(block.body);
    } else if (block.name == "settings") {
      // "timeout: 0" means the default of Bruno: Zalie's default is kept.
      if (const int timeoutMs = ParseInt(FindPair(block.body, "timeout"), 0); timeoutMs > 0) {
        req.settings.timeoutMs = static_cast<std::uint32_t>(timeoutMs);
      }
    }
  }
  if (!hasMethod) {
    return std::unexpected(RawChars32(std::string_view{"no HTTP method block (get, post...) found"}));
  }
  switch (req.bodyMode) {
    case BodyMode::json:
      req.body = std::move(jsonBody);
      break;
    case BodyMode::text:
      req.body = std::move(textBody);
      break;
    case BodyMode::xml:
      req.body = std::move(xmlBody);
      break;
    case BodyMode::formUrlEncoded:
      req.form = urlEncodedForm.empty() ? std::move(multipartForm) : std::move(urlEncodedForm);
      break;
    case BodyMode::none:
      break;
  }
  // Enabled query parameters are in the URL already; keep the disabled ones of the table.
  SyncParamsFromUrl(req);
  for (KeyValue& param : queryParams) {
    if (!param.enabled) {
      req.params.push_back(std::move(param));
    }
  }
  return result;
}

Environment ParseBruEnvironment(std::string_view name, std::string_view content) {
  Environment env;
  env.name.assign(name);
  for (const BruBlock& block : ParseBlocks(content)) {
    if (block.name == "vars") {
      ParsePairs(block.body, env.variables);
    } else if (block.name == "vars:secret" && block.isList) {
      // Secret values are kept out of the collection by Bruno: import the names, with empty values.
      ForEachLine(block.body, [&env](std::string_view line) {
        while (!line.empty()) {
          const std::size_t commaPos = line.find(',');
          std::string_view item = Trim(line.substr(0, commaPos));
          line = commaPos == std::string_view::npos ? std::string_view{} : line.substr(commaPos + 1);
          const bool enabled = !item.starts_with('~');
          if (!enabled) {
            item.remove_prefix(1);
          }
          if (!item.empty()) {
            env.variables.push_back(MakeKeyValue(item, {}, enabled));
          }
        }
      });
    }
  }
  return env;
}

std::expected<Request, RawChars32> ImportBruRequestFile(const fs::path& path) {
  const RawChars32 filename = PathToUtf8(path.filename());
  if (View(filename) == "folder.bru" || View(filename) == "collection.bru" || View(filename) == "bruno.json") {
    return std::unexpected(Concat({View(filename),
                                   " describes a Bruno collection or folder, not a request: import the "
                                   "collection folder (the one containing bruno.json) instead"}));
  }
  auto content = ReadFile(path);
  if (!content) {
    return std::unexpected(std::move(content.error()));
  }
  auto parsed = ParseBruRequest(*content);
  if (!parsed) {
    return std::unexpected(Concat({View(filename), ": ", parsed.error()}));
  }
  Request& request = parsed->request;
  if (request.name.empty()) {
    request.name = FileStem(path);
  }
  request.id = NewId();
  return std::move(request);
}

std::expected<BrunoImport, RawChars32> ImportBrunoCollection(const fs::path& directory) {
  const fs::path brunoJson = directory / "bruno.json";
  auto config = ReadFile(brunoJson);
  if (!config) {
    return std::unexpected(Concat({"not a Bruno collection (no readable bruno.json): ", config.error()}));
  }
  BrunoImport result;
  Collection& collection = result.collection;
  collection.name = ExtractJsonString(*config, "name");
  if (collection.name.empty()) {
    collection.name = PathToUtf8(directory.filename());
  }

  std::error_code errc;
  if (const fs::path collectionBru = directory / "collection.bru"; fs::is_regular_file(collectionBru, errc)) {
    ImportCollectionFile(collectionBru, collection, result);
  }

  const fs::path envDir = directory / "environments";
  if (fs::is_directory(envDir, errc)) {
    std::vector<fs::path> envFiles;
    for (const fs::directory_entry& entry : fs::directory_iterator(envDir, errc)) {
      if (entry.path().extension() == ".bru") {
        envFiles.push_back(entry.path());
      }
    }
    std::ranges::sort(envFiles);
    for (const fs::path& envFile : envFiles) {
      if (auto content = ReadFile(envFile)) {
        collection.environments.push_back(ParseBruEnvironment(FileStem(envFile), *content));
      } else {
        result.warnings.push_back(View(content.error()));
      }
    }
  }

  ImportFolder(directory, collection.root, result, true);
  EnsureUniqueIds(collection);
  return result;
}

}  // namespace zalie
