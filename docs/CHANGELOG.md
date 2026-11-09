# Changelog

All notable changes of Zalie. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the
project uses [semantic versioning](https://semver.org/).

## [0.1.0] - Unreleased

First version.

### Requests

- Methods `GET`, `POST`, `PUT`, `PATCH`, `DELETE`, `HEAD`, `OPTIONS`, `TRACE`, `CONNECT`.
- URL bar with the resolved URL and undefined variables shown below it, kept in sync both ways with the query
  parameters table.
- Headers and query parameters with per-row enable / disable.
- Bodies: JSON (validated while typing, beautify), text, XML, form URL-encoded.
- Authentication: Basic, Bearer token, API key (header or query), inherited from the collection by default.
- Per-request settings: timeout, redirects, TLS verification, decompression, HTTP version (automatic, 1.1, 2).
- **Fetch** sends the request on a worker thread with keep-alive connection reuse, never blocking the interface.
  Cancel with ++esc++.
- Paste a curl command in the URL bar to turn it into a request.

### Responses

- Status, total time, size, local time; progress (bytes, and a bar when the size is announced) while in flight.
- Pretty-printed and highlighted JSON, highlighted XML / HTML / JavaScript, image preview (PNG, JPEG, GIF, BMP), hex
  dump of other binary bodies.
- Virtualized views: any body size scrolls smoothly. Find, line selection, copy.
- JSONPath filter of JSON bodies (RFC 9535 without filter expressions), evaluated on the text: key order and number
  precision are kept. Kept per tab and applied to the next responses.
- Response headers, and the request exactly as sent (raw HTTP).

### Organization

- Collections of folders and requests: drag and drop, rename, duplicate, delete, filter.
- Variables at collection and environment level, dynamic variables (`{{$timestamp}}`, `{{$randomUUID}}`,
  `{{process.env.NAME}}`...), with nested references.
- History of the last 500 sent requests. Click an entry to open its request, double click it to put its method and URL
  back in the request.
- One pretty-printed JSON file per collection, autosaved atomically.
- Import Bruno collections (requests, folders, environments, timeouts) from their folder or `bruno.json`, single
  Bruno requests (`.bru`), Postman v2.0 / v2.1 exports, and curl commands.

### Inspector

- Configurable right panel: code snippet, timeline, variables, response headers, notes.
- Snippets in curl, HTTPie, Wget, PowerShell, Python requests, JavaScript fetch, Go net/http, C++ aeronet and raw
  HTTP, editable before copying.

### Performance tests

- A Performance tab per request (Fetch button menu, or the request menu of the sidebar): sends it many times from
  its own threads, one per connection, limited by duration, number of requests, requests per second or one pass over
  the values, with an optional warm-up. Confirmation before loading another host than the local machine, or with a
  method that may change data.
- Parameter table: every `{{variable}}` and query parameter can vary, from a fixed value, a list, integer, decimal or
  date ranges, random strings, UUIDs, timestamps or a data file (text, CSV, JSON Lines; never loaded in memory, drop
  it on its row), in order, at random or shuffled, combined independently, together or in all combinations, with a
  seed for reproducible runs. Samples, value counts and the first requests are shown before the run.
- Live dashboard: requests per second, latency percentiles, success and error shares, throughput; charts of the
  requests per second by status class, of the response time over time, of the latency distribution and of the
  latency percentiles (hover, hide a series, zoom); status codes, errors by kind and response times per class.
- Export of a whole run as JSON (settings, seed, totals, percentiles, histograms, statuses, time series).

### Interface

- Two themes drawn after Zalie: Coton (light) and Nuit (dark). HiDPI aware.
- Zalie gallops while a request is in flight, and naps while nothing has been sent yet.
- Tabs for open requests, restored at startup; keyboard shortcuts for the frequent actions.
- The response below the request editor, or beside it on wide windows.
- Location of the request (`Collection / Folder / Request`) above the URL bar, click to rename.
- Monospace keys and values in the parameter and header tables.

### Platform

- Linux, macOS and Windows; one self-contained executable (fonts and images embedded), in `<build>/bin`.
- Link-time optimization of Release builds.
- Linux desktop integration: desktop entry and icons installed by `cmake --install`, matched to the window (the dog
  in the task bar). Without an installed entry, Zalie writes one in `~/.local/share` at launch
  (`--no-desktop-entry` to disable).
- Event driven rendering: no frame drawn while nothing changes.
