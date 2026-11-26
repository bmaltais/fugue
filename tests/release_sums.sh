#!/usr/bin/env bash
# packaging/release-sums.sh against fake asset folders: no network, nothing outside a scratch folder.
set -euo pipefail
root="${1:-$(cd "$(dirname "$0")/.." && pwd)}"
script="$root/packaging/release-sums.sh"
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }
part() { printf '%s  %s\n' "$(printf '%s' "$2" | sha256sum | cut -d' ' -f1)" "$2" >"$1/$2.sha256"; }
expect="$work/expect"
printf '# comment\nfugue-[0-9]*-x86_64.pkg.tar.zst\nFugue-*-x86_64.AppImage\nfugue-helper-cuda-x86_64-linux.tar.zst\n' >"$expect"

# 1: everything there, written in name order, in install.sh's format
d="$work/ok"; mkdir "$d"
for n in fugue-0.1.0_t-1-x86_64.pkg.tar.zst Fugue-0.1.0-x86_64.AppImage fugue-helper-cuda-x86_64-linux.tar.zst; do part "$d" "$n"; done
printf '%s\n' "$d"/*.sha256 | sed 's|.*/||; s|\.sha256$||' >"$work/assets"
"$script" "$d" --expect "$expect" --assets "$work/assets" >/dev/null || fail "a complete release was refused"
[[ $(wc -l <"$d/SHA256SUMS") == 3 ]] || fail "wrong line count"
grep -Eq '^[0-9a-f]{64}  Fugue-0.1.0-x86_64.AppImage$' "$d/SHA256SUMS" || fail "line format"
(cd "$d" && while read -r _ n; do printf '%s' "$n" >"$n"; done <SHA256SUMS && sha256sum -c SHA256SUMS >/dev/null) || fail "sha256sum -c does not accept the file"
cp "$d/SHA256SUMS" "$work/first"; "$script" "$d" --expect "$expect" >/dev/null; cmp "$work/first" "$d/SHA256SUMS" || fail "not repeatable"

# 2: a job's files missing: exit 3, named, no SHA256SUMS written
d="$work/missing"; mkdir "$d"; part "$d" fugue-0.1.0_t-1-x86_64.pkg.tar.zst
rc=0; "$script" "$d" --expect "$expect" 2>"$work/err" >/dev/null || rc=$?
[[ $rc == 3 ]] || fail "missing assets gave exit $rc"
grep -q 'Fugue-\*-x86_64.AppImage' "$work/err" || fail "the missing AppImage is not listed"
grep -q 'fugue-helper-cuda' "$work/err" || fail "the missing helper is not listed"
[[ ! -e $d/SHA256SUMS ]] || fail "a partial SHA256SUMS was written"

# 3: checksum uploaded, asset not yet
d="$work/pending"; mkdir "$d"
for n in fugue-0.1.0_t-1-x86_64.pkg.tar.zst Fugue-0.1.0-x86_64.AppImage fugue-helper-cuda-x86_64-linux.tar.zst; do part "$d" "$n"; done
printf 'fugue-0.1.0_t-1-x86_64.pkg.tar.zst\nFugue-0.1.0-x86_64.AppImage\n' >"$work/assets"
rc=0; "$script" "$d" --expect "$expect" --assets "$work/assets" 2>"$work/err" >/dev/null || rc=$?
[[ $rc == 3 ]] || fail "an unfinished upload gave exit $rc"
grep -q 'not yet' "$work/err" || fail "the unfinished upload is not named"

# 4: two parts disagree; 5: a malformed line
d="$work/clash"; mkdir "$d"; part "$d" a.tar.zst; printf '%s  a.tar.zst\n' "$(printf 'x' | sha256sum | cut -d' ' -f1)" >"$d/a.tar.zst.copy.sha256"
rc=0; "$script" "$d" 2>/dev/null >/dev/null || rc=$?; [[ $rc == 2 ]] || fail "disagreeing parts gave exit $rc"
d="$work/bad"; mkdir "$d"; echo 'not a checksum' >"$d/x.sha256"
rc=0; "$script" "$d" 2>/dev/null >/dev/null || rc=$?; [[ $rc == 2 ]] || fail "a malformed part gave exit $rc"

# 6: the real expect file matches the names the workflows publish
d="$work/real"; mkdir "$d"
for n in fugue-0.1.0_test6-1-x86_64.pkg.tar.zst fugue-0.1.0_test6-1-aarch64.pkg.tar.zst fugue-helper-cuda-aarch64-linux.tar.zst \
  Fugue-0.1.0-x86_64.AppImage fugue-helper-cuda-x86_64-linux.tar.zst fugue-helper-vulkan-x86_64-linux.tar.zst fugue-helper-cpu-x86_64-linux.tar.zst; do part "$d" "$n"; done
"$script" "$d" --expect "$root/packaging/release-assets.txt" >/dev/null || fail "packaging/release-assets.txt does not match the asset names"
echo "release-sums: ok"
