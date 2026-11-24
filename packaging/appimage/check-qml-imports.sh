#!/usr/bin/env bash
# Every Qt module the app's QML imports must be in qml-imports/imports.qml (what the AppImage bundles). No Qt needed.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "$here/../../src" && pwd)
used=$(grep -rhE '^\s*import Qt[A-Za-z.]*' "$src" --include='*.qml' | sed -E 's/^\s*import (Qt[A-Za-z.]*).*/\1/' | sort -u)
listed=$(grep -E '^import Qt' "$here/qml-imports/imports.qml" | sed -E 's/^import (Qt[A-Za-z.]*).*/\1/' | sort -u)
missing=$(comm -23 <(echo "$used") <(echo "$listed"))
if [[ -n $missing ]]; then
  echo "check-qml-imports: src/ imports these Qt modules, add them to packaging/appimage/qml-imports/imports.qml:" >&2
  echo "$missing" >&2
  exit 1
fi
echo "check-qml-imports: ok ($(wc -l <<<"$used") modules)"
