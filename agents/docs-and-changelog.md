# Docs and changelog

## Docs

The MkDocs page that covers the feature (`docs/guide/`, `docs/reference/`, `docs/development/`) for new behaviour;
`docs/ROADMAP.md` when relevant.

- **Don't forget the rest of the MkDocs site** (`mkdocs.yml`, pages under `docs/`), which agents often miss. A new or
  changed CMake option goes in `docs/reference/build-options.md`, a command line option in
  `docs/reference/command-line.md`, a saved field in `docs/reference/file-format.md`, a change of design in
  `docs/development/architecture.md`; a new page must be added to the `nav` of `mkdocs.yml`.
- Screenshots come from `tools/take-screenshots.sh` (demo mode of the app). It needs a display, unlike the tests.
- Check the site with `mkdocs build --strict` (the CI does). Keep both the venv and the site in RAM: the venv in
  `/run/user/$UID`, not `/dev/shm`, whose `noexec` prevents compiled Python extensions from loading. Remove both when
  done:

  ```bash
  V=/run/user/$(id -u)/zalie-docs-venv
  python3 -m venv $V && $V/bin/python -m pip install --quiet --requirement requirements-docs.txt
  $V/bin/python -m mkdocs build --strict --site-dir /dev/shm/zalie-docs-site
  rm -rf $V /dev/shm/zalie-docs-site
  ```

## Changelog

**`docs/CHANGELOG.md`**: a short entry for user visible changes, *but only when the change is observable relative to
the last released version*. The changelog tracks what changed between releases, so don't add an entry for work that
only touches something already introduced earlier in the current unreleased section (optimizing or fixing an
as-yet-unreleased feature): there's no net change versus the last release, so the entry is pure noise. Fold such tweaks
into the existing entry only if they change its description, otherwise leave the changelog alone. No need to mention
the tests covering the change.
