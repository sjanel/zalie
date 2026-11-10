#include "zalie/postman-import.hpp"

#include <expected>
#include <glaze/glaze.hpp>
#include <string>
#include <string_view>
#include <utility>

#include "zalie/collection.hpp"
#include "zalie/key-value.hpp"
#include "zalie/method.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/request.hpp"
#include "zalie/string-pool.hpp"
#include "zalie/string-utils.hpp"
#include "zalie/url.hpp"

namespace zalie {

namespace {

using Json = glz::generic;

const Json* Child(const Json& node, std::string_view key) {
  if (!node.is_object()) {
    return nullptr;
  }
  for (const auto& [name, value] : node.get_object()) {
    if (name == key) {
      return &value;
    }
  }
  return nullptr;
}

std::string_view StringOf(const Json* pNode) {
  if (pNode == nullptr || !pNode->is_string()) {
    return {};
  }
  return pNode->get<std::string>();
}

std::string_view StringField(const Json& node, std::string_view key) { return StringOf(Child(node, key)); }

bool BoolField(const Json& node, std::string_view key) {
  const Json* pNode = Child(node, key);
  return pNode != nullptr && pNode->is_boolean() && pNode->get<bool>();
}

// Postman descriptions are either a string or {"content": "...", "type": "text/markdown"}.
std::string_view Description(const Json& node) {
  const Json* pDesc = Child(node, "description");
  if (pDesc == nullptr) {
    return {};
  }
  return pDesc->is_string() ? StringOf(pDesc) : StringField(*pDesc, "content");
}

// [{"key": "...", "value": "...", "disabled": true}, ...]
void ReadKeyValues(const Json* pArray, KeyValues& out) {
  if (pArray == nullptr || !pArray->is_array()) {
    return;
  }
  for (const Json& item : pArray->get_array()) {
    if (!item.is_object()) {
      continue;
    }
    const Json* pValue = Child(item, "value");
    RawChars32 value;
    if (pValue != nullptr && pValue->is_number()) {
      AppendAll(value, static_cast<std::int64_t>(pValue->get<double>()));
    } else if (pValue != nullptr && pValue->is_boolean()) {
      value.assign(pValue->get<bool>() ? std::string_view{"true"} : std::string_view{"false"});
    } else {
      value.assign(StringOf(pValue));
    }
    out.push_back({.key = RawChars32(StringField(item, "key")),
                   .value = std::move(value),
                   .enabled = !BoolField(item, "disabled")});
  }
}

// Value of the entry 'key' in a Postman auth parameter list: [{"key": "token", "value": "..."}].
std::string_view AuthParam(const Json& auth, std::string_view scheme, std::string_view key) {
  const Json* pParams = Child(auth, scheme);
  if (pParams == nullptr || !pParams->is_array()) {
    return {};
  }
  for (const Json& item : pParams->get_array()) {
    if (StringField(item, "key") == key) {
      return StringField(item, "value");
    }
  }
  return {};
}

void ReadAuth(const Json* pAuth, Auth& auth, StringList& warnings) {
  if (pAuth == nullptr || !pAuth->is_object()) {
    return;
  }
  const std::string_view type = StringField(*pAuth, "type");
  if (type == "noauth") {
    auth.mode = AuthMode::none;
  } else if (type == "basic") {
    auth.mode = AuthMode::basic;
    auth.username.assign(AuthParam(*pAuth, "basic", "username"));
    auth.password.assign(AuthParam(*pAuth, "basic", "password"));
  } else if (type == "bearer") {
    auth.mode = AuthMode::bearer;
    auth.token.assign(AuthParam(*pAuth, "bearer", "token"));
  } else if (type == "apikey") {
    auth.mode = AuthMode::apiKey;
    auth.apiKeyName.assign(AuthParam(*pAuth, "apikey", "key"));
    auth.apiKeyValue.assign(AuthParam(*pAuth, "apikey", "value"));
    auth.apiKeyInQuery = AuthParam(*pAuth, "apikey", "in") == "query";
  } else if (!type.empty()) {
    warnings.push_back({"unsupported authentication type: ", type});
  }
}

RawChars32 ReadUrl(const Json* pUrl) {
  if (pUrl == nullptr) {
    return {};
  }
  if (pUrl->is_string()) {
    return RawChars32(StringOf(pUrl));
  }
  return RawChars32(StringField(*pUrl, "raw"));
}

void ReadBody(const Json* pBody, Request& req, StringList& warnings) {
  if (pBody == nullptr || !pBody->is_object() || BoolField(*pBody, "disabled")) {
    return;
  }
  const std::string_view mode = StringField(*pBody, "mode");
  if (mode == "raw") {
    req.body.assign(StringField(*pBody, "raw"));
    std::string_view language;
    if (const Json* pOptions = Child(*pBody, "options"); pOptions != nullptr) {
      if (const Json* pRaw = Child(*pOptions, "raw"); pRaw != nullptr) {
        language = StringField(*pRaw, "language");
      }
    }
    const KeyValue* pContentType = FindEnabled(req.headers, "Content-Type", true);
    const std::string_view contentType =
        pContentType == nullptr ? std::string_view{} : std::string_view{pContentType->value};
    if (language == "json" || ContainsIgnoreCase(contentType, "json")) {
      req.bodyMode = BodyMode::json;
    } else if (language == "xml" || ContainsIgnoreCase(contentType, "xml")) {
      req.bodyMode = BodyMode::xml;
    } else {
      req.bodyMode = BodyMode::text;
    }
  } else if (mode == "urlencoded" || mode == "formdata") {
    if (mode == "formdata") {
      warnings.push_back({"multipart form data imported as an url-encoded form: ", req.name});
    }
    ReadKeyValues(Child(*pBody, mode), req.form);
    req.bodyMode = BodyMode::formUrlEncoded;
  } else if (!mode.empty()) {
    warnings.push_back({"unsupported body mode '", mode, "' in request ", req.name});
  }
}

Request ReadRequest(std::string_view name, const Json& node, StringList& warnings) {
  Request req;
  req.name.assign(name);
  if (node.is_string()) {
    req.url.assign(StringOf(&node));  // "request": "https://..." is allowed by the schema
    SyncParamsFromUrl(req);
    return req;
  }
  if (const auto method = ParseMethod(StringField(node, "method"))) {
    req.method = *method;
  }
  req.url = ReadUrl(Child(node, "url"));
  ReadKeyValues(Child(node, "header"), req.headers);
  ReadBody(Child(node, "body"), req, warnings);
  if (const Json* pAuth = Child(node, "auth"); pAuth != nullptr) {
    ReadAuth(pAuth, req.auth, warnings);
  }
  req.docs.assign(Description(node));
  SyncParamsFromUrl(req);
  // Disabled query parameters are only in the structured URL.
  if (const Json* pUrl = Child(node, "url"); pUrl != nullptr && pUrl->is_object()) {
    KeyValues query;
    ReadKeyValues(Child(*pUrl, "query"), query);
    for (KeyValue& param : query) {
      if (!param.enabled) {
        req.params.push_back(std::move(param));
      }
    }
  }
  return req;
}

void ReadItems(const Json* pItems, Folder& folder, StringList& warnings) {
  if (pItems == nullptr || !pItems->is_array()) {
    return;
  }
  for (const Json& item : pItems->get_array()) {
    if (!item.is_object()) {
      continue;
    }
    const std::string_view name = StringField(item, "name");
    if (const Json* pRequest = Child(item, "request"); pRequest != nullptr) {
      folder.requests.push_back(ReadRequest(name, *pRequest, warnings));
      if (Child(item, "event") != nullptr) {
        warnings.push_back({"scripts are not supported (request ", name, ")"});
      }
    } else if (const Json* pChildren = Child(item, "item"); pChildren != nullptr) {
      Folder child;
      child.name.assign(name);
      ReadItems(pChildren, child, warnings);
      if (Child(item, "auth") != nullptr) {
        warnings.push_back({"folder level authentication is not supported (folder ", name, ")"});
      }
      folder.folders.push_back(std::move(child));
    }
  }
}

}  // namespace

bool LooksLikePostmanCollection(std::string_view json) noexcept {
  return json.contains("schema.getpostman.com") || (json.contains("\"info\"") && json.contains("\"item\""));
}

std::expected<PostmanImport, RawChars32> ImportPostmanCollection(std::string_view json) {
  Json root;
  // 'json' is a view, not necessarily null terminated (the content of a RawChars).
  if (const auto errc = glz::read<glz::opts{.null_terminated = false}>(root, json); errc) {
    const std::string message = glz::format_error(errc, json);
    return std::unexpected(Concat({"invalid JSON: ", message}));
  }
  const Json* pInfo = Child(root, "info");
  if (pInfo == nullptr || Child(root, "item") == nullptr) {
    return std::unexpected(RawChars32(std::string_view{"not a Postman collection (no 'info' / 'item')"}));
  }
  PostmanImport result;
  Collection& collection = result.collection;
  collection.name.assign(StringField(*pInfo, "name"));
  if (collection.name.empty()) {
    collection.name.assign(std::string_view{"Postman collection"});
  }
  collection.docs.assign(Description(*pInfo));
  ReadKeyValues(Child(root, "variable"), collection.variables);
  ReadAuth(Child(root, "auth"), collection.auth, result.warnings);
  if (collection.auth.mode == AuthMode::inherit) {
    collection.auth.mode = AuthMode::none;
  }
  ReadItems(Child(root, "item"), collection.root, result.warnings);
  if (Child(root, "event") != nullptr) {
    result.warnings.push_back(std::string_view{"collection scripts are not supported"});
  }
  EnsureUniqueIds(collection);
  return result;
}

}  // namespace zalie
