# Files and formats

## The Zalie home

| Path | Content |
| ---- | ------- |
| `settings.json` | preferences: theme, zoom, panel sizes, visibility and layout, inspector cards, snippet language, autosave, limits (`maxLoadConnections`: most connections of a performance run, 0 for the number of vCPUs) |
| `workspace.json` | the open collections (paths and active environments), the open tabs |
| `history.json` | the last 500 sent requests (summaries only, no bodies) |
| `collections/*.zalie.json` | the collections created in Zalie (or imported) |
| `imgui.ini` | column sizes of the tables |
| `runs/*.json` | the [run reports](#run-reports) exported from the Performance tabs (the default place) |

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

### Run settings

A request can carry the settings of its load test in a `load` object. It is left out of the file until a run is set up.

| Field | Values |
| ----- | ------ |
| `load.durationMs`, `load.maxRequests`, `load.maxRequestsPerSecond` | limits of the run, 0 for none: the first one reached ends it |
| `load.adaptiveRate` | the rate adapts to what the server sustains, from `load.minRequestsPerSecond` (0 for 10) up to `load.maxRequestsPerSecond` |
| `load.maxP99Ms` | adaptive rate: p99 of the response times above which the rate backs off, 0 for none |
| `load.onePass` | also ends the run after one pass over the values |
| `load.warmupMs` | first part of the run, sent but not counted |
| `load.connections` | number of connections, 0 for the number of vCPUs |
| `load.seed` | equal seeds give equal requests, timestamps aside. 0 picks a new one for each run |
| `load.combine` | `independent`, `together`, `all-combinations` |
| `load.parameters[].name` | the `{{variable}}` fed by this row or, with `queryParameter`, the key of a query parameter replaced for the run only (appended to the URL of the run when the request has none) |
| `load.parameters[].source` | `fixed`, `list`, `integer-range`, `decimal-range`, `date-range`, `random-string`, `uuid`, `timestamp`, `data-file`, or `omitted` for a query parameter left out of the run |
| `load.parameters[].order` | `in-order`, `random`, `shuffled` |
| `load.parameters[].timestampFormat` | `seconds`, `milliseconds`, `iso8601` |
| `load.parameters[].fileFormat` | `text`, `csv`, `json-lines` |

Each source keeps its own fields (`listValues`, `integerFrom`, `dateFormat`, `filePath`...), so switching source loses
nothing. A data file is only referenced by its path, absolute or relative to the directory of the collection.

!!! warning "Secrets"
    Variable values are stored in clear in the collection file. For secrets, prefer the process environment
    (`{{$env.TOKEN}}`) or keep the file out of version control.

## Run reports

**Export JSON...** of a [Performance tab](../guide/performance.md) writes the whole run. Durations are in nanoseconds,
sizes in bytes.

```json
{
  "format": "zalie-run",
  "formatVersion": 1,
  "zalieVersion": "0.1.0",
  "startedAt": "2026-11-09T21:30:05Z",
  "request": { "name": "Search", "method": "GET", "url": "{{baseUrl}}/search?q={{term}}&page=1" },
  "settings": { "parameters": ["..."], "seed": 42, "durationMs": 30000, "connections": 32, "...": "..." },
  "seed": 42,
  "connections": 32,
  "setupError": "",
  "warnings": [],
  "dataFiles": [{ "path": "/data/users.csv", "format": "csv", "rows": 120000, "sizeBytes": 4718592 }],
  "totals": {
    "requests": 353450, "bodyBytes": 541421568, "minBodyBytes": 1536, "maxBodyBytes": 1536,
    "elapsedSeconds": 30.0, "meanRequestsPerSecond": 11781.7, "peakRequestsPerSecond": 12851.0,
    "sustainedRequestsPerSecond": 0.0, "bodyBytesPerSecond": 18047385.6, "cpuSeconds": 41.2, "clientLoad": 0.43,
    "clientSaturated": false
  },
  "latency": {
    "count": 353414, "minNs": 331000, "p50Ns": 2160000, "p90Ns": 3870000, "p95Ns": 4590000,
    "p99Ns": 6680000, "p999Ns": 9700000, "maxNs": 23500000, "meanNs": 2440000.0, "stddevNs": 1200000.0
  },
  "headLatency": { "count": 353414, "...": "..." },
  "classes": [
    { "name": "2xx", "count": 352360, "latency": { "...": "..." } },
    { "name": "4xx", "count": 1054, "latency": { "...": "..." } },
    { "name": "error", "count": 36, "latency": { "...": "..." } }
  ],
  "statuses": [{ "count": 352360, "status": 200 }, { "count": 1054, "status": 404 }],
  "errors": [{ "kind": "timeout", "count": 36, "firstMessage": "request timed out" }],
  "histograms": { "total": [[327680, 12], [344064, 31], "..."], "head": ["..."] },
  "series": {
    "classes": ["2xx", "3xx", "4xx", "5xx", "error"],
    "firstSecond": 0,
    "points": [
      { "counts": [12360, 0, 37, 0, 0], "bodyBytes": 19042304, "p50Ns": 2150000, "p90Ns": 3860000,
        "p99Ns": 6650000, "maxNs": 12100000, "seconds": 1.0, "inFlight": 32, "targetRate": 0 }
    ]
  }
}
```

| Field | Content |
| ----- | ------- |
| `request` | the request as written, `{{variables}}` included |
| `settings` | the [run settings](#run-settings), with the seed actually used: sending the same requests again only needs them |
| `latency`, `headLatency` | times to the end of the body and to the response head of the received responses. The requests without response (connection refused, timeout...) only count in the `error` class |
| `classes` | requests per class of outcome (`2xx` with `1xx`, `3xx`, `4xx`, `5xx` with the codes above 599, `error` without response), with their own latencies |
| `errors[].kind` | `connection`, `tls`, `timeout`, `closed`, `malformed`, `too-large`, `other`, `invalid-value` (a generated value refused where it lands: the request was not sent) |
| `histograms` | the non-empty buckets of the latency histograms: `[lower bound, count]`. Buckets are log-linear, 16 per power of two: a bucket starting at `l >= 16` is `2^(floor(log2(l)) - 4)` wide |
| `series.points` | one point per second since the end of the warm-up, the counts in the order of `series.classes`. `seconds` is below 1 for the last point of a run. A long run keeps its last hour: `firstSecond` is the index of the first point kept |
| `totals.clientLoad` | CPU time of Zalie over the measured time, relative to the CPUs its connections can use. `clientSaturated` is set above 0.9 |
| `totals.sustainedRequestsPerSecond`, `series.points[].targetRate` | adaptive rate: the highest rate measured while the server was healthy, and the rate aimed at during each second. 0 for a fixed rate |

