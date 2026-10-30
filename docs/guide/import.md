# Import from Bruno, Postman, curl

Drop a folder or a file on the window, use **Import...** (main menu or the `+` of the sidebar), or pass paths on the
[command line](../reference/command-line.md). Imported collections are converted to Zalie collections (new files in
the collections directory): the originals are not modified.

## Bruno

Drop the **folder of the collection** (the one containing `bruno.json`), or its `bruno.json` file.

A single **request file** (`.bru`) can be dropped too: it opens in a new, unsaved tab (like a pasted curl command),
and **Save to...** keeps it in a collection. Its `{{variables}}` come from the environments of its Bruno collection:
import the whole folder to get them.

| Bruno | Zalie |
| ----- | ----- |
| `*.bru` requests (`get`, `post`... blocks), ordered by `seq` | requests |
| sub-folders, `folder.bru` names and `seq` | folders |
| `headers`, `params:query` (disabled `~` entries included) | headers, params |
| `body:json`, `body:text`, `body:xml`, `body:form-urlencoded` | the matching body mode |
| `auth:basic`, `auth:bearer`, `auth:apikey`, `auth: inherit` | the same authentication |
| `docs` | docs |
| `settings`: `timeout` | the timeout of the request (`0`, Bruno's default, keeps Zalie's) |
| `collection.bru`: headers, auth, `vars:pre-request`, docs | collection headers, auth, variables, docs |
| `environments/*.bru`: `vars`, `vars:secret` | environments (secrets imported with empty values: Bruno stores them outside the collection) |

Not imported (reported as warnings when relevant): scripts, tests, assertions, request variables, folder level
headers / auth (a folder in the default `auth: inherit` mode is fine: its requests use the collection's auth), GraphQL bodies, multipart bodies (imported as url-encoded fields), the authentication schemes Zalie does
not support (digest, OAuth 2, AWS signature, NTLM).

!!! tip "Secrets"
    Bruno keeps the values of secret variables out of the collection. After the import, either fill them in the
    environment, or reference the process environment: `{{process.env.TOKEN}}` works as in Bruno.

## Postman

Export the collection from Postman (*Export > Collection v2.1*) and drop the JSON file. Folders, requests, headers,
query parameters (disabled ones included), raw bodies (JSON / XML / text from the language option or the
`Content-Type`), url-encoded and form-data bodies, descriptions, collection variables and the Basic / Bearer / API key
authentication (collection or request level) are imported. Scripts are not (a warning tells it).

## curl

Paste a curl command in the URL bar of a request, or in the **curl command** area of the import dialog. The usual
options are understood: `-X`, `-H`, `-d` / `--data-raw` / `--data-binary` / `--data-urlencode` / `--json`, `-G`,
`-u`, `-A`, `-e`, `-b`, `-I`, `-k`, `-L`, `-m`, `--http1.1`, `--http2`, `--oauth2-bearer`, `--compressed`... as well
as the quoting of "Copy as cURL" in the browsers (`'...'`, `"..."`, `$'...'`, line continuations with `\`, `^` or
`` ` ``). Options without equivalent (proxy, client certificates, output files...) are ignored with a warning.

A form body (`-d a=1&b=2`, curl's default) becomes editable form fields; a JSON-looking body becomes a JSON body.
