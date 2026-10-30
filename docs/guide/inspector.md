# Inspector and code snippets

The right panel (++ctrl+j++) shows cards about the active request. Choose which cards are shown, and their order,
with the `...` button of its header: the configuration is remembered.

| Card | Content |
| ---- | ------- |
| Code snippet | the request in the language of your choice, ready to copy |
| Timeline | sent at, time to head, download time, total, head / body / request sizes, resolved URL |
| Variables | the variables used by the request, their values and where they come from |
| Response headers | a compact view of the last response headers (click to copy a value) |
| Notes | the docs of the request (hidden by default) |

## Code snippets

The snippet describes the request as Zalie would send it: variables resolved, collection headers and authentication
applied, and the settings translated (redirects, TLS verification, timeout, HTTP version).

| Language | Tool / library | Notes |
| -------- | -------------- | ----- |
| cURL | `curl` | `-L` when redirects are followed, `-k` without TLS verification, `--max-time` for a custom timeout |
| HTTPie | `http` | `--follow`, `--verify=no`, `--raw` body |
| Wget | `wget` | `--method`, `--body-data`, prints the body |
| PowerShell | `Invoke-WebRequest` | `-ContentType` for the content type, `-SkipCertificateCheck` (PowerShell 6+) |
| Python | `requests` | always with a timeout; non ASCII bodies encoded as UTF-8 |
| JavaScript | `fetch` | `redirect: "manual"`, `AbortSignal.timeout()` |
| Go | `net/http` | a client with the timeout, redirect and TLS settings |
| C++ | [aeronet](https://github.com/sjanel/aeronet) | `HttpClient` + `HttpRequest`, raw string literal bodies |
| Raw HTTP | - | the request line, headers and body |

Strings are quoted for each language (single quotes for the shells, escapes for the others). The snippet is generated
only when the request changes, never at every frame.

Click the pencil to **edit** the snippet before copying it (handy to tweak a flag); click it again to go back to the
generated one.
