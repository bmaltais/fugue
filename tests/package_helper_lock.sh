#!/usr/bin/env bash
# bin/package-helper's shared-folder steps under contention: many runs at once publish their archives, each
# SHA256SUMS line must stay, and a download fetched by several runs at once ends whole. Uses the script's own
# functions (cut out of it), a scratch folder and file:// URLs: no container, no network.
set -euo pipefail
root="${1:-$(cd "$(dirname "$0")/.." && pwd)}"
script="$root/bin/package-helper"
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }
command -v flock >/dev/null || { echo "package_helper_lock: flock missing, skipped"; exit 0; }
die() { echo "package-helper: $*" >&2; exit 2; }
for fn in locked write_sums publish fetch_pinned; do
  body="$(sed -n "/^$fn() {/,/^}/p" "$script")"
  [[ -n $body ]] || fail "$fn() not found in bin/package-helper"
  eval "$body"
done

out="$work/dist"; mkdir -p "$out"
runs=12
for i in $(seq 1 "$runs"); do
  (
    name="fugue-helper-t$i-x86_64-linux"
    head -c 200000 /dev/urandom >"$out/.$name.tar.zst.tmp"
    for _ in 1 2 3; do locked "$out/.lock" publish "$out/.$name.tar.zst.tmp" || exit 1; head -c 1000 /dev/urandom >"$out/.$name.tar.zst.tmp"; done
  ) &
done
wait
# one more run replaces its own line, never another's
[[ $(grep -c . "$out/SHA256SUMS") == "$runs" ]] || fail "SHA256SUMS has $(grep -c . "$out/SHA256SUMS") lines, expected $runs"
(cd "$out" && sha256sum -c SHA256SUMS >/dev/null) || fail "a line does not match its archive"
ls "$out"/SHA256SUMS.?????? >/dev/null 2>&1 && fail "a temporary SHA256SUMS was left"

# downloads: several runs want the same pinned file
src="$work/tool.tar"; head -c 300000 /dev/urandom >"$src"
sum="$(sha256sum "$src" | cut -d' ' -f1)"
dl="$work/ccache/downloads"; mkdir -p "$dl"
for _ in $(seq 1 6); do
  ( locked "$dl/.lock" fetch_pinned "$dl" "file://$src" "$sum" tool.tar ) &
done
wait
[[ $(sha256sum "$dl/tool.tar" | cut -d' ' -f1) == "$sum" ]] || fail "the download is not whole"
[[ ! -e $dl/tool.tar.part ]] || fail "a partial download was left"
# a wrong pin is refused and nothing is kept
rc=0; (locked "$dl/.lock" fetch_pinned "$dl" "file://$src" "$(printf 0%.0s {1..64})" other.tar) 2>/dev/null || rc=$?
[[ $rc != 0 && ! -e $dl/other.tar && ! -e $dl/other.tar.part ]] || fail "a download that does not match its pin was kept"
echo "package_helper_lock: ok"
