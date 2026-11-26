#!/bin/bash
# Lints .github/workflows with actionlint (and the shellcheck it runs on every run: block). A workflow error
# otherwise shows only when GitHub runs it, which for the release jobs means only on a tag.
# Uses actionlint from PATH, else the pinned container image if podman has it locally; skips (77) when neither.
set -euo pipefail
root="${1:?usage: workflows_lint.sh <checkout>}"
image='docker.io/rhysd/actionlint@sha256:1d74bfc9fd1963af8f89a7c22afaaafd42f49aad711a09951d02cb996398f61d'
if command -v actionlint >/dev/null; then
  cd "$root" && exec actionlint -color=false
elif command -v podman >/dev/null; then
  # bin/test gives each test its own HOME and XDG folders; podman's image store is the account's, so point it there.
  home="$(getent passwd "$(id -u)" | cut -d: -f6)"
  run=(env HOME="$home" XDG_DATA_HOME="$home/.local/share" XDG_CONFIG_HOME="$home/.config" XDG_RUNTIME_DIR="/run/user/$(id -u)" podman)
  if "${run[@]}" image exists "$image" 2>/dev/null; then
    exec "${run[@]}" run --rm --network=none -v "$root:/repo:ro" -w /repo "$image" -color=false
  fi
fi
echo "workflows_lint: no actionlint and no local image $image; skipped" >&2
exit 77
