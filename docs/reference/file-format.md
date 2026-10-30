# Files and formats

## The Zalie home

| Path | Content |
| ---- | ------- |
| `settings.json` | preferences: theme, zoom, panel sizes, visibility and layout, inspector cards, snippet language, autosave, limits |
| `workspace.json` | the open collections (paths and active environments), the open tabs |
| `history.json` | the last 500 sent requests (summaries only, no bodies) |
| `collections/*.zalie.json` | the collections created in Zalie (or imported) |
| `imgui.ini` | column sizes of the tables |

All files are JSON, written atomically (temporary file + rename). Unknown keys are ignored and missing keys take their
default value: files written by another version of Zalie load fine.

## Collection files

```json
{
  "name": "Pet store",
  "docs": "",
  "variables": [
    { "key": "baseUrl", "value": "https://petstore.example.com", "enabled": true }
  ],
  "headers": [],
  "environments": [
    {
      "name": "local",
      "variables": [{ "key": "baseUrl", "value": "http://localhost:8080", "enabled": true }]
    }
  ],
  "root": {
    "name": "",
    "folders": [
      {
        "name": "Pets",
        "folders": [],
        "requests": [
          {
            "name": "Create pet",
            "url": "{{baseUrl}}/pets?notify=true",
            "body": "{\n  \"name\": \"Zalie\"\n}",
            "docs": "",
            "params": [{ "key": "notify", "value": "true", "enabled": true }],
            "headers": [],
            "form": [],
            "auth": {
              "username": "", "password": "", "token": "", "apiKeyName": "", "apiKeyValue": "",
              "mode": "inherit", "apiKeyInQuery": false
            },
            "id": 8412097523376391722,
            "settings": {
              "timeoutMs": 30000, "httpVersion": "auto", "followRedirects": true, "verifyTls": true,
              "decompress": true
            },
            "method": "POST",
            "bodyMode": "json"
          }
        ],
        "id": 1785212345678901234
      }
    ],
    "requests": [],
    "id": 523456789012345678
  },
  "auth": { "mode": "bearer", "token": "{{token}}", "...": "..." },
  "formatVersion": 1
}
```

| Field | Values |
| ----- | ------ |
| `method` | `GET`, `POST`, `PUT`, `PATCH`, `DELETE`, `HEAD`, `OPTIONS`, `TRACE`, `CONNECT` |
| `bodyMode` | `none`, `json`, `text`, `xml`, `form-urlencoded` |
| `auth.mode` | `inherit`, `none`, `basic`, `bearer`, `api-key` |
| `settings.httpVersion` | `auto`, `http1.1`, `http2` |
| `id` | 64-bit identifiers of folders and requests (tabs and history refer to them). Missing or duplicated ids (a hand edit, a merge) are regenerated at load time |

The key order is stable, and the output pretty-printed with one key per line: collections diff and merge well in git.

!!! warning "Secrets"
    Variable values are stored in clear in the collection file. For secrets, prefer the process environment
    (`{{$env.TOKEN}}`) or keep the file out of version control.
