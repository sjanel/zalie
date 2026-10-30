# Responses

The response appears under the request editor as soon as it is complete. While it is in flight, Zalie runs across the
panel with the elapsed time and the bytes received so far (and a progress bar when the size is announced); ++esc++ or
**Cancel** stops it.

## Summary line

`200 OK  91 ms · 491 B · 15:22:40.302`: the status (green 2xx, blue 3xx, orange 4xx, red 5xx), the total time, the
size of the body (decoded) and the local time it was sent. Errors (unknown host, refused connection, TLS failure,
timeout...) are explained in plain words just below.

Then three views: **Body**, **Headers** and **Sent**.

## Body

The kind of the body is detected from its `Content-Type`, or from its first bytes:

| Kind | Display |
| ---- | ------- |
| JSON | syntax highlighted; pretty-printed by default (**Raw** for the body as received), filterable with a JSONPath (below) |
| XML, HTML | syntax highlighted (tags, attributes, strings, comments) |
| JavaScript, text | highlighted / plain text |
| PNG, JPEG, GIF, BMP | the image, fitted to the panel, with its size |
| Other binary | a hex dump (offset, bytes, ASCII) |

- **Find** (++ctrl+f++): case-insensitive, all matches highlighted, ++enter++ / ++shift+enter++ or the arrows to go
  to the next / previous one.
- **Select lines**: click a line, ++shift++ click or drag to extend; ++ctrl+c++ copies them, ++ctrl+a++ selects all.
  Right click for the same actions.
- **Copy** button: the whole body (pretty or raw, as displayed), or the matches of the filter.

Long lines are split in visual lines of 2 KB, so that a minified multi-megabyte document stays smooth.

## Filtering a JSON body

Type a [JSONPath](https://www.rfc-editor.org/rfc/rfc9535) in the `$.filter` field of a JSON body to show only what
it selects; the line below tells how many values matched (or why the path is invalid). Find and copy then work on the
matches. The filter is kept per tab and applied again to the next responses, handy to watch one field across sends.

| Path | Selects |
| ---- | ------- |
| `$.results` | the member `results` of the root object (the leading `$` may be omitted: `results`, `.results`) |
| `$.results[0]`, `$.results[-1]` | the first, the last element |
| `$.results[0:10]`, `$.results[::2]` | a slice (`start:end:step`, negative values count from the end) |
| `$.results[*].id`, `$.results.*` | every element / member |
| `$..price` | every `price`, at any depth |
| `$['content-type']`, `$.items[0,2]` | quoted names (any character), unions |

A path made of names and indexes only shows the value itself; any other path shows the array of the matches. Values
are copied as they are in the body: key order, number precision (big identifiers) and escapes are kept. Filter
expressions (`[?@.price < 10]`) are not supported yet.

Up to 1 MB the filter follows your typing; above, it is applied when you press ++enter++.

## Headers

The response headers, as received (names in lower case). Click a value to copy it.

!!! note
    The client decodes the body (de-chunking, decompression): `Content-Length` and `Content-Type` describe what was
    announced, and `Content-Encoding` is not shown once the body is decoded. Disable **Decompress responses** in the
    request settings to see the bytes as received.

## Sent

The request exactly as Zalie built it, as raw HTTP: request line, headers (variables resolved, authentication
applied) and body. The client adds `User-Agent` (unless set), `Accept-Encoding` and the connection management headers.

## Timeline

The **Timeline** card of the inspector details the time: waiting for the head (DNS, connection, TLS, upload, server
processing and redirects) versus downloading the body, plus the sizes.
