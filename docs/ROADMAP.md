# Roadmap

Zalie aims to be the official graphical client of the [aeronet](https://github.com/sjanel/aeronet) HTTP client:
every capability of the client reachable from the interface, its speed made visible, and the C++ snippet as an
interactive guide to its API. It stays local only and light: a feature costs nothing while unused (no CPU when idle,
nothing per frame proportional to a body or a run), and heavy dependencies stay behind build options.

Ideas by theme, roughly by priority within each theme.

## Milestones

| Version | Focus |
| ------- | ----- |
| 0.2 | [Performance tab](#performance-tab) (first version), the [aeronet settings available today](#available-in-aeronet-not-exposed-yet), aeronet pin update |
| 0.3 | Values extracted from responses into variables, assertions, collection runner, command line runner (sharing the performance engine) |
| 0.4 | Multipart and file bodies, cookie jar, more authentication schemes, Protocol Buffers bodies (aeronet 1.8.0) |
| With aeronet | What needs [new client APIs](#needs-new-aeronet-client-apis): timings, connection details, cancellation, multiplexing, WebSocket |

## Performance tab

Send a request many times, varying its parameters, and watch latency, throughput, sizes and statuses live: a load
tester (wrk, oha, k6) built into the client, on top of the speed of aeronet. A sketch (made-up numbers):

```text
GET {{base}}/search?q={{term}}&page={{page}}      [Closed: 32 connections, 30 s]  [Run]  [Stop]
  term: list(dog, cat, bird) shuffled    page: int 1..50    seed 42

  12 418 req/s   p50 2.1 ms   p99 7.8 ms   max 41 ms   2xx 99.7 %   errors 0.1 %   18.2 MB/s
  [ throughput over time ]   [ p50 / p99 over time ]   [ latency histogram ]
  Status: 200 371 862 | 404 1 118 | timeout 37        Slowest: page=47 (p99 19 ms)
```

### Run model

- **Closed model**: C connections, each sending its next request as soon as the previous one completes (wrk, hey).
  Measures the highest throughput.
- **Open model**: a target rate in requests per second, scheduled whatever the response times (wrk2, vegeta, k6
  constant arrival rate). Latency is measured from the scheduled send time, which corrects the coordinated omission;
  requests that could not start in time are counted as late.
- Stop after a number of requests, after a duration, or by hand. A warm-up period is excluded from the statistics.
  Later: ramp-up stages (rate or connections changing over time).
- Connection modes: keep-alive (default), or a new connection per request to measure the connection and TLS setup.
  The HTTP version setting compares HTTP/1.1 and HTTP/2 on the same server.
- The aeronet client is synchronous: C connections are C threads, each with its own `HttpClient`, and an HTTP/2
  connection carries one stream at a time. Multiplexing many streams on few connections waits for the asynchronous
  client of aeronet (see below).

### Varying the requests

- **Generators** feed variables of a run scope, so any `{{name}}` works in the URL, query parameters, headers and
  body: fixed value, list (in order, random, or shuffled without repetition), integer or decimal range, random string
  (length, character set), UUID, timestamp, rows of a data file (CSV or JSON lines, in order or random). The existing
  dynamic variables (`{{$randomUUID}}`, `{{$timestamp}}`...) are evaluated again for every request.
- A seed per run makes it reproducible; it is part of the report.
- Each completed request remembers its generated values, so a slow or failed response tells which parameters caused
  it.
- Cost per request: the request is compiled once into a template (literal parts and variable slots), filled on the
  worker thread into a reused buffer. Nothing is resolved on the UI thread per request.

### Metrics

- Throughput: requests per second, decoded MB/s, totals.
- Latency to the response head and to the end of the body: min, mean, standard deviation, p50, p90, p95, p99,
  p99.9, max, from a log-linear (HDR-like) histogram per worker: fixed memory, mergeable, as in the
  `benchmarks/scripted-clients` harness of aeronet.
- Response sizes: min, mean, max.
- Statuses: count per code, grouped by class. Errors per kind (connection, TLS, timeout, closed, malformed, too
  large), with the first message of each.
- Time series of one second buckets (ring buffer): throughput, p50 and p99, errors, requests in flight.
- **Breakdown by generated value**: latency and statuses per value of a chosen variable (bounded to the most frequent
  values), to find the slow `page` or the failing `id`.
- Client saturation: CPU time of the load threads against the wall time, with a warning when the client, not the
  server, is the bottleneck (the check of the aeronet benchmark harness).
- Once aeronet exposes them: DNS, connect, TLS and first byte times, connections opened versus reused.

### Checks

- Bodies are counted, not kept: the response sink only adds up bytes (no copy, no analysis), so that the run measures
  the server rather than Zalie.
- Optional assertions per response, kept cheap: status in a set, header present, JSONPath value or existence (the
  JSONPath engine of the response filter), size bounds, body contains. A failed assertion counts as an error.
- Thresholds on the whole run (p99 under X ms, error rate under Y %, throughput above Z): a pass or fail verdict,
  also the exit code of the command line runner.
- Samples: the slowest and the first failed exchanges (a few each) are kept whole (request as sent, response head,
  truncated body) and open like a normal response.

### Interface

- Opened from the Fetch button menu, in a tab of its own bound to the request: the run settings on top, the
  dashboard below (key numbers, percentile table, status distribution, charts of throughput and latency over time,
  latency histogram).
- Charts drawn with `ImDrawList` from the ring buffers: constant cost per frame, no plotting dependency. ImPlot is the
  alternative if more interaction is needed, to weigh against the binary size.
- While running, the dashboard refreshes about 10 times per second (not at the vsync rate, so that the interface does
  not take CPU from the load threads); once stopped, the loop is idle again.
- Stop drops the queued requests at once; the ones in flight end at their next received bytes (until aeronet has a
  cross-thread cancellation).
- Before a run with a non-idempotent method (`POST`, `DELETE`...), a confirmation tells the target host and the
  planned volume.

### Results

- The run settings are saved in the collection, next to the request (git friendly). Results go to the home
  directory (`runs/*.json`), not to the collection.
- Compare two runs: differences of percentiles and throughput, overlaid charts.
- Export to JSON, CSV (time series) and a Markdown summary, and as snippets: the equivalent `wrk`, `oha`, `hey` or
  `k6` command, and a C++ aeronet load program.

### Engine

- A `LoadRunner` in `zalie_core` (no GUI dependency), with its own threads: the interactive executor stays available
  to the other tabs during a run. Both build their `HttpClientConfig` the same way.
- Workers publish their statistics every 100 ms without locking the UI (double buffer); the UI merges them. No
  allocation per request once running.
- Tested headless against the local aeronet server of the test suite (delays and statuses chosen by parameter).

## Exposing the aeronet client

### Available in aeronet, not exposed yet

The client already supports these, Zalie only needs settings and UI:

| Feature | aeronet | Zalie today | Plan |
| ------- | ------- | ----------- | ---- |
| Timeouts | `connectTimeout`, `requestTimeout`, `inactivityTimeout` | one value for all three | three settings: a long download with a short inactivity timeout |
| Retries | `RetryConfig`: attempts, backoff, jitter, statuses, `Retry-After` | none | per request or collection, attempts shown in the Timeline |
| Forward proxy | `withProxy()`: cleartext proxy, `CONNECT` for https, CA of an intercepting proxy (mitmproxy) | none | global or per collection, `HTTP_PROXY` / `NO_PROXY` from the environment |
| TLS | CA file or directory, client certificate and key (mTLS, files or PEM), TLS version bounds, cipher list | verification on / off | per collection, client certificates per host |
| Request compression | `requestCompression`: zstd, br, gzip | none | per request |
| File body | `HttpRequest::file()` (`sendfile` over cleartext) | none | "Binary file" body mode, no copy in memory |
| Request trailers | `trailerAddLine()` | none | trailers table of the body |
| Accept-Encoding | `withDefaultAcceptEncoding()` | automatic | override (`identity`...) |
| Redirects | `maxRedirects` | always 10 | setting |
| Connection pool | `keepAlive`, `keepAliveTimeout`, `maxIdleConnectionsPerHost`, `clearIdleConnections()` | defaults | keep-alive setting, "Close connections" action |
| HTTP/2 | `Http2Config`: window sizes, frame size, header table... | defaults | advanced settings |
| Memory | `releaseUnusedMemory()` | never called | called when the executor goes idle, so that the buffers of a large response do not stay resident |
| Configuration files | `LoadClientConfig()`, `dumpConfig()` (JSON / YAML, `AERONET_ENABLE_GLAZE`) | none | import / export request settings as an aeronet client configuration (glaze is already a dependency of Zalie: one version for both) |

The C++ aeronet snippet follows: it writes every setting above with the matching `HttpClientConfig` calls, and a
`ResponseBodySink` for a download to a file.

### On aeronet main, after the pinned commit

- `Expect: 100-continue`, and early responses (a `413` received while uploading stops the upload and is returned):
  `Expect` can then leave the headers managed by the client, with a "wait for 100 Continue" setting for large uploads.
- TLS writes larger than one record no longer wait between records: faster large uploads over https.
- Stricter HTTP/1.1 response parsing (status line, framing, connection reuse), header values with non-ASCII bytes
  accepted (`Content-Disposition` file names).
- Update the pin, then follow the tagged releases (1.8.0).

### Protocol Buffers (aeronet 1.8.0)

aeronet 1.8.0 adds `AERONET_ENABLE_PROTOBUF` (header-only, protobuf found on the system, never downloaded); a
`bodyJson`-like API is tracked in [aeronet#860](https://github.com/sjanel/aeronet/issues/860).

- A **Protobuf** body mode, edited as JSON and encoded with a `.proto` file or a descriptor set (full runtime: dynamic
  messages and the JSON mapping), and a decoded view of `application/x-protobuf` responses (field numbers and wire
  types without schema).
- Behind an optional `ZALIE_ENABLE_PROTOBUF` build option, off by default: protobuf is a large system dependency.
- gRPC would follow (length-prefixed messages over HTTP/2, status in the trailers), but needs the HTTP/2 response
  trailers apart from the headers (aeronet merges them today), and streaming.

### Needs new aeronet client APIs

An optional per-exchange observer or metadata structure, costing nothing when not requested, would cover most of
these:

- **Per-phase timings**: DNS resolution, TCP connect, TLS handshake, request sent, first byte, end, for each
  redirect hop. Zalie only measures the time to the response head and the total today.
- **Connection details**: negotiated protocol (h2 or http/1.1), connection new or reused, remote and local
  addresses, TLS version, cipher, certificate chain (subject, issuer, validity, alternative names).
- **Redirect chain**: status, `Location` and headers of every hop, and the final URL.
- **Bytes on the wire** against decoded bytes (head and body: the compression ratio).
- **Informational responses** (`100`, `103 Early Hints`) and **HTTP/1.1 response trailers**, parsed then dropped
  today.
- **Error details**: the OpenSSL reason, `errno`, the phase that failed ("certificate has expired" instead of "TLS
  handshake failed").
- **Cross-thread cancellation**: today a request still connecting when cancelled ends with its timeout (its result is
  discarded). Cancelling during the transfer is already immediate.
- **Asynchronous client with HTTP/2 multiplexing** (the coroutine client of the aeronet roadmap): many streams per
  connection in the performance tab, and no thread per request in flight.
- **Name resolution**: a `--resolve`-like override, IPv4 / IPv6 preference, DNS cache. **Unix domain sockets**
  (local daemons, the Docker API).
- **Proxies**: HTTPS and SOCKS5 proxies, proxy authentication.
- **TLS**: key log file (`SSLKEYLOGFILE`, for Wireshark), SNI override, session resumption (resumed handshakes
  measured in the performance tab).
- **Uploads produced while sent** (a callback instead of a body in memory or a file), and a client-side multipart
  builder (the server has the parser).
- **Extension methods** (WebDAV, the `QUERY` method draft): `http::Method` is a fixed set.
- **WebSocket client**: the frame codec works in client mode already; the handshake (`Upgrade` is a reserved request
  header) and the transport loop are missing. Server-Sent Events can be parsed by Zalie on top of the streaming sink
  meanwhile.
- **JavaScript engine**: [aeronet#738](https://github.com/sjanel/aeronet/issues/738) proposes an optional QuickJS
  (`AERONET_ENABLE_JS`); the same engine would run the scripts of Bruno and Postman collections.
- HTTP/3 (a research topic in aeronet).

## Requests

- Multipart form data bodies (files included), built by Zalie (aeronet has no builder).
- Cookie jar per collection: `Set-Cookie` stored with the RFC 6265 rules (domain, path, expiry, `Secure`), sent back
  automatically, shown and editable. aeronet has no cookie storage.
- Values extracted from a response into variables (JSONPath, header, status), like the `vars:post-response` of
  Bruno: chained requests (log in, then use the token) without scripts.
- Folder level headers, authentication and variables (imported from Bruno, currently dropped).
- Authentication: Digest, OAuth 2 (client credentials, authorization code with PKCE through a local redirect), AWS
  Signature v4, and a JWT viewer (header, claims, expiry) for tokens.
- Secret variables kept out of the collection file (a separate file, or the system keychain), masked in the
  interface and the snippets.
- More dynamic variables (`{{$randomInt}}` with bounds, random strings, e-mails...), shared with the generators of
  the performance tab.
- GraphQL bodies (query and variables, schema introspection).
- WebSocket and Server-Sent Events sessions.

## Responses

- Character-level text selection in the response views (lines today).
- A collapsible tree view of JSON bodies, with "filter here" (copy the JSONPath of a node).
- JSONPath filter expressions (`[?@.price < 10]`), and filtering very large bodies on a worker thread.
- A live view of streamed bodies (chunked, `text/event-stream`) while they arrive.
- Save a response body to a file, and download straight to a file without size limit (a `ResponseBodySink` writing
  to disk).
- The last responses of each request kept, and the diff between two of them.
- A waterfall of the timings, the redirect chain and the certificate, once aeronet exposes them.

## Automation

- Assertions and tests: declarative first (status, header, JSONPath, time, size: the checks of the performance tab),
  then scripts (pre-request and post-response; Bruno and Postman import them today as warnings).
- Running a whole folder or collection, with iterations from a data file, and a report.
- Command line runners sharing `zalie_core`, in a small executable without GUI dependency: `zalie-cli run` for a
  collection or folder (JUnit or JSON report), `zalie-cli bench` for the performance tab without window (thresholds
  give the exit code). For the CI.

## Interface

- Native Wayland by default on Linux (X11 through XWayland today, `-D ZALIE_GLFW_WAYLAND=ON` to opt in).
- More themes, and following the light / dark preference of the system.
- A command palette, and switching environment from the top bar.

## Distribution and project

- Prebuilt packages: AppImage / Flatpak, a signed macOS bundle, a Windows installer.
- Continuous integration on the three platforms (the workflows are in `.github/workflows`).
- A scheduled CI job building and testing Zalie against aeronet main: Zalie as an integration test of the client API.
- The version and features of aeronet (`fullVersionStringView()`) in the About dialog.

## Maybe later

- A local mock server built on the aeronet server (already linked by the tests): serve responses saved in a
  collection, or echo requests, to develop against an API that does not exist yet or to try the performance tab
  offline. Behind a build option, for the binary size.
