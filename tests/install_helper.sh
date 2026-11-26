#!/usr/bin/env bash
# install.sh --helper-only against a fake release: curl, uname and the release are fixtures, HOME and XDG_DATA_HOME
# a scratch folder, no network. Covers switching backends, a repair, and zstd missing before any download.
set -euo pipefail
root="${1:-$(cd "$(dirname "$0")/.." && pwd)}"
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }
command -v zstd >/dev/null || { echo "install_helper: zstd missing, skipped"; exit 0; }

fix="$work/fixtures" fakebin="$work/fakebin" tools="$work/tools"
mkdir -p "$fix" "$fakebin" "$tools" "$work/home"
tag=v9.9.9
for backend in cuda vulkan cpu; do
  name="fugue-helper-$backend-x86_64-linux"
  mkdir -p "$work/pack/$name/lib" "$work/pack/$name/licenses"
  printf '#!/bin/sh\n' >"$work/pack/$name/fugue-helper-$backend"; chmod +x "$work/pack/$name/fugue-helper-$backend"
  echo svc >"$work/pack/$name/fugue-helper.service"
  echo "$backend" >"$work/pack/$name/lib/libgomp.so.1"
  echo notice >"$work/pack/$name/licenses/README"
  tar -C "$work/pack" --zstd -cf "$fix/$name.tar.zst" "$name"
done
(cd "$fix" && sha256sum ./*.tar.zst | sed 's| \./| |' >SHA256SUMS)
{
  echo '[{"tag_name": "'$tag'", "assets": ['
  sep=""
  for f in "$fix"/*.tar.zst "$fix"/SHA256SUMS; do
    printf '%s{"browser_download_url": "https://example.test/bmaltais/fugue/releases/download/%s/%s"}' "$sep" "$tag" "${f##*/}"; sep=","
  done
  echo ']}]'
} >"$fix/release.json"

cat >"$fakebin/curl" <<'FAKE'
#!/usr/bin/env bash
# Serves $FIXTURES: the release list for the API url, the file of that name for a download.
out="" url=""
while (($#)); do case $1 in -o) out=$2; shift ;; -*) ;; *) url=$1 ;; esac; shift; done
echo "$url" >>"$CURL_LOG"
case $url in
  https://api.github.com/*) cat "$FIXTURES/release.json" ;;
  *) cp "$FIXTURES/${url##*/}" "$out" ;;
esac
FAKE
printf '#!/bin/sh\necho x86_64\n' >"$fakebin/uname"
chmod +x "$fakebin/curl" "$fakebin/uname"
# The tools install.sh uses, without zstd, for the "missing" case.
for t in bash env grep sed awk head tail cat tar mktemp rm cp mkdir dirname printf cut sort sha256sum install mv basename tr; do
  p="$(command -v "$t" || true)"; [[ -n $p ]] && ln -sf "$p" "$tools/$t"
done

export FIXTURES="$fix" CURL_LOG="$work/curl.log" HOME="$work/home" XDG_DATA_HOME="$work/data"
hd="$work/data/fugue/helper"
install() { PATH="$fakebin:$PATH" bash "$root/install.sh" --helper-only --helper "$1" >"$work/out" 2>&1 || { cat "$work/out" >&2; fail "install $1 failed"; }; }
has() { [[ -e $hd/$1 ]]; }
need() { local f; for f in "$@"; do has "$f" || fail "missing after install: $f"; done; }

install cuda
need fugue-helper-cuda .installed-cuda lib/libgomp.so.1

install cpu
need fugue-helper-cpu .installed-cpu
has fugue-helper-cuda && fail "the cuda binary is still there: AppRun would pick it first"
has .installed-cuda && fail "the cuda marker is still there: a cuda reinstall would be skipped"
[[ $(cat "$hd/lib/libgomp.so.1") == cpu ]] || fail "the cuda libraries were not replaced"

install cuda
need fugue-helper-cuda .installed-cuda
has fugue-helper-cpu && fail "switching back to cuda left the cpu binary"

: >"$work/curl.log"; install cuda
grep -q 'nothing to do' "$work/out" || fail "the same release again should be skipped"
grep -q 'tar.zst' "$work/curl.log" && fail "a current helper was downloaded again"

rm "$hd/fugue-helper-cuda" # marker stays: a repair must still reinstall
install cuda
need fugue-helper-cuda

# no zstd: refuse before any download (the release list is read to learn whether there is a tarball)
rm -rf "$hd"; : >"$work/curl.log"
if PATH="$fakebin:$tools" bash "$root/install.sh" --helper-only --helper cuda >"$work/out" 2>&1; then fail "installed without zstd"; fi
grep -q 'zstd' "$work/out" || fail "the missing zstd is not named: $(cat "$work/out")"
grep -q 'tar.zst' "$work/curl.log" && fail "a tarball was downloaded before the zstd check: $(cat "$work/curl.log")"
[[ ! -e $hd ]] || fail "a helper folder was made without zstd"

# no zstd and a release without the tarball: nothing to unpack, so no complaint
rm -rf "$hd"; : >"$work/curl.log"
printf '[{"tag_name": "%s", "assets": [{"browser_download_url": "https://example.test/bmaltais/fugue/releases/download/%s/SHA256SUMS"}]}]\n' "$tag" "$tag" >"$fix/release.json"
PATH="$fakebin:$tools" bash "$root/install.sh" --helper-only --helper cuda >"$work/out" 2>&1 || fail "no tarball in the release must not need zstd: $(cat "$work/out")"
grep -q 'zstd' "$work/out" && fail "zstd was asked for with no tarball to unpack"
echo "install_helper: ok"
