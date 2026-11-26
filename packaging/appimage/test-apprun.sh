#!/usr/bin/env bash
# Runs AppRun against a fake AppDir: arguments pass through unchanged, and the helper is found (or not) in the
# places AppRun documents. No Qt, no build. `packaging/appimage/test-apprun.sh`
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }

mkdir -p "$work/app/usr/bin" "$work/home"
cp "$here/AppRun" "$work/app/AppRun"
cat >"$work/app/usr/bin/fugue" <<'FAKE'
#!/bin/sh
echo "helper=${FUGUE_HELPER-<unset>}"
echo "fromapprun=${FUGUE_HELPER_FROM_APPRUN-<unset>}"
for a in "$@"; do echo "arg=$a"; done
FAKE
chmod +x "$work/app/AppRun" "$work/app/usr/bin/fugue"
run() { env -i HOME="$work/home" PATH="$PATH" FUGUE_HELPER_DIRS="$work/home/.local/lib/fugue" "$@" "$work/app/AppRun" "${ARGS[@]}"; }

ARGS=(run "a b.fugue" --json)
out=$(run)
for want in 'arg=run' 'arg=a b.fugue' 'arg=--json'; do grep -qxF "$want" <<<"$out" || fail "arguments changed: $out"; done
grep -qx 'helper=<unset>' <<<"$out" || fail "a helper was invented: $out"

ARGS=()
run | grep -q '^arg=' && fail "arguments appeared from nowhere"

mkdir -p "$work/home/.local/lib/fugue"
touch "$work/home/.local/lib/fugue/fugue-helper-cpu"; chmod +x "$work/home/.local/lib/fugue/fugue-helper-cpu"
grep -qx "helper=$work/home/.local/lib/fugue/fugue-helper-cpu" <<<"$(run)" || fail "cpu helper not found"
touch "$work/home/.local/lib/fugue/fugue-helper-vulkan"; chmod +x "$work/home/.local/lib/fugue/fugue-helper-vulkan"
grep -qx "helper=$work/home/.local/lib/fugue/fugue-helper-vulkan" <<<"$(run)" || fail "vulkan should win over cpu"
mkdir -p "$work/xdg/fugue/helper"; touch "$work/xdg/fugue/helper/fugue-helper-cpu"; chmod +x "$work/xdg/fugue/helper/fugue-helper-cpu"
grep -qx "helper=$work/xdg/fugue/helper/fugue-helper-cpu" <<<"$(run XDG_DATA_HOME="$work/xdg" FUGUE_HELPER_DIRS="$work/xdg/fugue/helper")" || fail "the XDG data folder comes first"
ARGS=(x "y z")
grep -qxF 'arg=y z' <<<"$(run)" || fail "the helper search lost an argument"
grep -qx 'helper=' <<<"$(run FUGUE_HELPER=)" || fail "an empty FUGUE_HELPER must stay empty (no helper)"
grep -qx 'helper=/x' <<<"$(run FUGUE_HELPER=/x)" || fail "FUGUE_HELPER from the user must win"
grep -qx 'fromapprun=1' <<<"$(run)" || fail "a helper AppRun found must be marked FUGUE_HELPER_FROM_APPRUN=1"
grep -qx 'fromapprun=<unset>' <<<"$(run FUGUE_HELPER=/x)" || fail "a helper the user named must not be marked"
grep -qx 'fromapprun=<unset>' <<<"$(run FUGUE_HELPER=/x FUGUE_HELPER_FROM_APPRUN=1)" || fail "a stale mark must go when the user names a helper"
grep -qx 'fromapprun=<unset>' <<<"$(run FUGUE_HELPER=)" || fail "no helper means no mark"
env -i HOME="$work/home" PATH="$PATH" "$work/app/AppRun" | grep -q LD_LIBRARY && fail "AppRun must not add LD_LIBRARY_PATH" || true
echo "AppRun: ok"
