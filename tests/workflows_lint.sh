#!/bin/bash
# Lints .github/workflows with actionlint (and the shellcheck it runs on every run: block). A workflow error
# otherwise shows only when GitHub runs it, which for the release jobs means only on a tag.
# Uses actionlint from PATH, else the pinned container image if podman has it locally; skips (77) when neither.
set -euo pipefail
root="${1:?usage: workflows_lint.sh <checkout>}"
image='docker.io/rhysd/actionlint@sha256:1d74bfc9fd1963af8f89a7c22afaaafd42f49aad711a09951d02cb996398f61d'
if command -v actionlint >/dev/null; then
  cd "$root" && exec actionlint -color=false
elif command -v podman >/dev/null && podman image exists "$image" 2>/dev/null; then
  exec podman run --rm --network=none -v "$root:/repo:ro" -w /repo "$image" -color=false
fi
echo "workflows_lint: no actionlint and no local image $image; skipped" >&2
exit 77
