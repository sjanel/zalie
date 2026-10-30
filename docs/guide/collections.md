# Collections

A collection groups requests in folders, with its own variables, environments, headers and authentication. Each
collection is **one JSON file** (`*.zalie.json`), pretty-printed with a stable order: it diffs nicely and can live in
the git repository of your project.

## In the sidebar

- **Click** a request to open it (or focus its tab), **double click** to rename it.
- **Drag and drop** a request on a folder or a collection to move it.
- **Right click** for the actions:

| On | Actions |
| -- | ------- |
| a collection | New request, New folder, Variables / environments / auth, Rename, Close (the file is kept) |
| a folder | New request, New sub-folder, Rename, Delete (with its requests) |
| a request | Open, Rename, Copy as cURL, Duplicate, Delete |

- The **filter** (++ctrl+k++) keeps the requests whose name or URL contains the text (and their folders).
- Unsaved changes show a `*` after the collection name.

## Collection settings

Right click a collection > **Variables, environments, auth...** (or *Manage environments...* in the environment
selector):

| Tab | |
| --- | - |
| Variables | variables of the collection, see [Variables](variables.md) |
| Environments | add, rename, delete, activate environments and edit their variables |
| Headers | headers sent with every request of the collection (a request header with the same name wins) |
| Auth | the authentication of the requests in *Inherit* mode (the default for new requests) |
| Docs | notes about the collection |

## Saving

Changes are saved automatically less than a second after the last edit (atomically: a temporary file renamed over the
previous one, so a crash never leaves a truncated collection). ++ctrl+s++ saves everything immediately. Autosave can be
disabled in **Settings**.

New collections are created in the `collections` directory of the [Zalie home](../reference/file-format.md). To use a
collection stored elsewhere (in a git repository...), drop its `.zalie.json` file on the window, or pass its path on
the [command line](../reference/command-line.md): Zalie edits it in place.

## History

The **History** tab of the sidebar lists the last 500 sent requests (method, status, URL, when). Click one to open its
request, or a new unsaved request with the same method and URL if it was not saved. Double click one to also put its
method and URL, as sent (variables resolved), back in the request: handy to get back a URL edited since.
