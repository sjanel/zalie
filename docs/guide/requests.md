# Requests

## URL bar

- **Method**: GET, POST, PUT, PATCH, DELETE, HEAD, OPTIONS, TRACE, CONNECT, each with its color.
- **URL**: absolute, or without scheme (`localhost:8080/x` means `http://localhost:8080/x`). It may contain
  `{{variables}}`: the resolved URL is shown below the bar, with the undefined variables in orange.
- **Fetch** (++ctrl+enter++, from anywhere) sends it; **Cancel** (++esc++) while it is in flight.
- **Performance** (++ctrl+shift+enter++) opens its [Performance tab](performance.md), to send it many times.
- Pasting a **curl command** in the URL bar converts it into the request (see [Import](import.md)).

## Params

The query parameters of the URL, as an editable table. The table and the URL are kept in sync both ways: editing one
updates the other. Unchecking a parameter removes it from the URL but keeps it in the table, ready to be re-enabled.
Parameters are stored raw (not decoded): what you type is what is sent.

## Headers

Editable table of headers. Disabled rows are kept but not sent. The headers of the collection (see
[Collections](collections.md)) are sent too; a request header with the same name replaces it.

A few headers are managed by the HTTP client and cannot be set by hand: `Host`, `Content-Length`,
`Transfer-Encoding`, `TE`, `Trailer`, `Upgrade`, `Expect` and `Connection`. If you add one, it is not sent and a
warning is shown with the response. `User-Agent` defaults to `zalie/<version>` and can be overridden.
`Accept-Encoding` is advertised automatically for the decoders compiled in (gzip, deflate, zstd, br).

## Body

| Mode | Sent as | Default `Content-Type` |
| ---- | ------- | ---------------------- |
| None | no body | - |
| JSON | the text, variables resolved | `application/json` |
| Text | the text | `text/plain` |
| XML | the text | `application/xml` |
| Form URL-encoded | `key=value&...`, percent-encoded | `application/x-www-form-urlencoded` |

A `Content-Type` header of the request overrides the default. For JSON, Zalie tells whether the body is valid (bodies
with variables are not checked) and **Beautify** pretty-prints it.

## Auth

| Type | Effect |
| ---- | ------ |
| Inherit from the collection | the default: uses the authentication of the collection |
| No authentication | nothing |
| Basic | `Authorization: Basic base64(user:password)` |
| Bearer token | `Authorization: Bearer <token>` |
| API key | a header, or a query parameter, with the given name and value |

All the values may use variables (keep secrets in an environment). An explicit `Authorization` header wins over the
Basic and Bearer settings.

## Settings

| Setting | Default | |
| ------- | ------- | - |
| Timeout | 30000 ms | Connection, TLS handshake and the whole exchange |
| Follow redirects | on | Up to 10. HTTPS to HTTP redirects are never followed; `Authorization` and `Cookie` are dropped on a redirect to another origin |
| Verify TLS certificate | on | Turn off for self-signed certificates (local servers) |
| Decompress responses | on | Off shows the bytes as received |
| HTTP version | Automatic | HTTP/2 negotiated with ALPN over https, HTTP/1.1 otherwise. HTTP/2 forces h2 (h2c prior knowledge over http) |

## Docs

Free notes (markdown) about the request, and its name. They also appear in the **Notes** card of the inspector.

## How a request is sent

The request is resolved first (variables, collection headers, authentication), on the UI thread, in a few
microseconds. It then goes to a pool of worker threads, each with its own aeronet HTTP clients and keep-alive
connections: the interface never waits for the network. See [Architecture](../development/architecture.md).
