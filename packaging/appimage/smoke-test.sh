#!/usr/bin/env bash
# Starts an AppImage the way a user's machine would, offscreen and without FUSE (a container has none), and checks:
# the command line (`--version`, `run` of a node-only workflow), and that the window opens a workflow of the
# bundled templates. Everything comes out of the AppImage itself, so no checkout is needed.
#
#   packaging/appimage/smoke-test.sh Fugue-0.1.0-x86_64.AppImage
set -euo pipefail
image=$(readlink -f "${1:?usage: smoke-test.sh <file.AppImage>}")
[[ -x $image ]] || { echo "smoke: $image is not executable" >&2; exit 1; }
fail() { echo "smoke: FAIL: $*" >&2; exit 1; }
say() { echo "==> $*"; }

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
export HOME=$work/home XDG_RUNTIME_DIR=$work/run
mkdir -p "$HOME" "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
export APPIMAGE_EXTRACT_AND_RUN=1 QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software
unset DISPLAY WAYLAND_DISPLAY FUGUE_HELPER

# shellcheck source=/dev/null
say "host: $(. /etc/os-release && echo "$PRETTY_NAME"), glibc $(ldd --version | head -1 | grep -o '[0-9.]*$')"
say "--version"
version=$("$image" --version) || fail "--version exited with an error"
[[ -n $version ]] || fail "--version printed nothing"
echo "$version"

say "the files inside: the example, the templates, the desktop entry, the MIME types"
(cd "$work" && "$image" --appimage-extract 'usr/share/*' >/dev/null)
share=$work/squashfs-root/usr/share
for f in fugue/examples/demo.fugue fugue/templates/text-to-image/workflow.fugue applications/fugue.desktop mime/packages/fugue.xml icons/hicolor/256x256/apps/fugue.png; do
  [[ -f $share/$f ]] || fail "missing from the AppImage: usr/share/$f"
done

say "fugue run of a node-only workflow (the command line, from the same file)"
"$image" run "$share/fugue/examples/demo.fugue" --wait --json >"$work/run.json" || { cat "$work/run.json"; fail "fugue run failed"; }
grep -q . "$work/run.json" || fail "fugue run printed nothing"

say "a command that writes state: it goes under XDG, never beside the AppImage"
"$image" pack list >/dev/null || fail "pack list failed"
"$image" template list | grep -q 'text-to-image' || fail "template list does not show the bundled text-to-image template"
[[ -z $(find "$(dirname "$image")" -newer "$work/run.json" -type f ! -path "$image" 2>/dev/null | head -1) ]] || fail "something was written next to the AppImage"

say "the window opens the text-to-image workflow (offscreen: it saves a screenshot and quits)"
shot=$work/window.png
status=0
OMARCHY_LOOK_SCREENSHOT=$shot OMARCHY_LOOK_SIZE=1280x800 OMARCHY_LOOK_DELAY_MS=3000 \
  timeout 120 "$image" "$share/fugue/templates/text-to-image/workflow.fugue" >"$work/window.log" 2>&1 || status=$?
if ((status != 0)) || [[ ! -s $shot ]]; then
  echo "---- the window's output (exit status $status; 124 is the 120 s timeout) ----"
  cat "$work/window.log"
  echo "---- end ----"
  fail "the window did not start and quit cleanly (exit status $status, screenshot: $([[ -s $shot ]] && echo saved || echo none))"
fi
if grep -E 'module .* is not installed|Cannot load library|could not find the Qt platform plugin|QQmlApplicationEngine failed' "$work/window.log"; then
  fail "the window logged a loading problem"
fi
say "ok"
