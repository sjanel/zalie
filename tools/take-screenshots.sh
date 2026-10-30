#!/usr/bin/env bash
# Regenerates the screenshots and images of the documentation (docs/assets) with the demo mode of Zalie.
# The windows are hidden (offscreen rendering): a display is still needed (X11 / Wayland, or Xvfb).
#
# Usage: tools/take-screenshots.sh [path/to/zalie]

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
zalie="${1:-${root}/build/bin/zalie}"
out="${root}/docs/assets"
home="$(mktemp -d)"
trap 'rm -rf "${home}"' EXIT

mkdir -p "${out}/screenshots"
"${zalie}" --home "${home}/coton" --demo response --theme coton --screenshot "${out}/screenshots/coton.png"
"${zalie}" --home "${home}/nuit" --demo response --theme nuit --screenshot "${out}/screenshots/nuit.png"
"${zalie}" --home "${home}/fetching" --demo pending --theme coton --frames 40 --screenshot "${out}/screenshots/fetching.png"

cp "${root}/assets/logo/zalie-logo-256.png" "${out}/logo.png"
cp "${root}/assets/logo/zalie-logo-64.png" "${out}/favicon.png"
cp "${root}/assets/logo/zalie-logo.svg" "${out}/zalie-logo.svg"
cp "${root}/assets/sprites/zalie-run.gif" "${out}/zalie-run.gif"
cp "${root}/assets/sprites/zalie-sprites.png" "${out}/zalie-sprites.png"
echo "documentation images updated in ${out}"
