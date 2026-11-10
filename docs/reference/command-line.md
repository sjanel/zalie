# Command line

```text
zalie [options] [collection paths...]
```

Paths are opened (`*.zalie.json`) or imported (Bruno collection folders or their `bruno.json`, Bruno requests
`.bru`, Postman exports), as if dropped on the window.

| Option | Description |
| ------ | ----------- |
| `--home <dir>` | Data directory (settings, history, collections). Default: `$ZALIE_HOME`, else the platform directory (see [Files and formats](file-format.md)) |
| `--theme <coton\|nuit>` | Start with this theme |
| `--screenshot <file.png>` | Render a hidden window offscreen, save it as PNG, then exit. Used to generate the documentation images |
| `--size <WxH>` | Window size, default `1440x900` |
| `--frames <n>` | Frames rendered before the screenshot, default 10 |
| `--demo <response\|pending\|performance>` | Demo state for the screenshots: a response shown without network, a request in flight, or the Performance tab of a request after a run (made-up numbers) |
| `--no-desktop-entry` | Linux: do not write the desktop entry at launch (see [Install](../getting-started/install.md#icon-in-the-task-bar-linux)) |
| `--version` | Print the version |
| `--help` | Print the help |

Examples:

```bash
zalie ~/collections/dog-park/bruno         # import a Bruno collection at startup
zalie --home /tmp/zalie-sandbox            # a throwaway profile
TOKEN=s3cr3t zalie                         # {{$env.TOKEN}} / {{process.env.TOKEN}} in requests
tools/take-screenshots.sh build/bin/zalie   # regenerate docs/assets
```
