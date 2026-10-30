# Your first request

On the first launch, Zalie creates a small *Zalie examples* collection, calling [httpbin.org](https://httpbin.org).

1. In the sidebar, click **Basics > Post JSON**: it opens in a tab.
2. Press ++ctrl+enter++ (or click **Fetch**). Zalie runs while the request is in flight, then the response appears
   below the request (or beside it, see [the interface](../guide/interface.md#response-below-or-beside-the-request)):
   status, time, size, and the pretty-printed JSON body.
3. Look at the **Inspector** on the right: the **Code snippet** card shows the same request as a `curl` command. Pick
   another language (Python, Go, C++ with aeronet...) and click the copy button.
4. The URL is `{{baseUrl}}/anything?source=zalie`: `baseUrl` is a variable of the collection. The line under the URL
   bar shows the resolved URL, and the **Variables** card where each value comes from.
5. Choose the `local` environment at the top of the window: `baseUrl` now points to `http://localhost:8080`.

## A new request

- ++ctrl+t++ opens a new, unsaved request. Type a URL (`localhost:8080/health` is enough: `http://` is assumed) and
  send it. **Save to...** moves it into a collection.
- Or right click a collection or a folder in the sidebar > **New request**.
- Or paste a curl command in the URL bar: it becomes a request, headers, body and authentication included. "Copy as
  cURL" in the network panel of your browser works great.

## Bring your Bruno or Postman collections

Drop the folder of a Bruno collection, or a Postman export, on the window. See
[Import from Bruno, Postman, curl](../guide/import.md).
