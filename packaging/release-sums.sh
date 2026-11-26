#!/usr/bin/env bash
# Builds a release's SHA256SUMS from the per-asset checksum files its jobs upload (docs/release.md).
#
#   packaging/release-sums.sh PARTS_DIR [--expect FILE] [--assets FILE] [--out FILE]
#
# Every release job uploads its assets and, for each one, <asset>.sha256 (one line, `sha256sum` format). Nobody edits
# a shared file: the one job that writes SHA256SUMS (.github/workflows/release-sums.yml) downloads the *.sha256 files
# and runs this. The output is a pure function of the parts, so a rerun gives the same file.
#
#   PARTS_DIR   holds the downloaded *.sha256 files.
#   --expect    a file of shell globs, one per line (# comments): each must match the name of some asset. packaging/
#               release-assets.txt lists what a v* tag must publish.
#   --assets    a file with the release's asset names, one per line: a part whose asset is not listed is not
#               counted (its upload has not finished).
#   --out       where to write the result (default PARTS_DIR/SHA256SUMS), only when nothing is missing.
#
# Exit: 0 written; 2 a bad part or two parts that disagree; 3 something expected is missing (listed on stderr).
set -euo pipefail

die() { echo "release-sums: $*" >&2; exit 2; }

dir="" expect="" assets="" out=""
while (($#)); do
  case "$1" in
    --expect) expect="${2:?--expect needs a file}"; shift ;;
    --assets) assets="${2:?--assets needs a file}"; shift ;;
    --out) out="${2:?--out needs a file}"; shift ;;
    -h|--help) sed -n '2,/^set -euo/p' "$0" | sed '$d;s/^# \{0,1\}//'; exit 0 ;;
    -*) die "unknown option $1" ;;
    *) [[ -z $dir ]] || die "two folders: $dir and $1"; dir="$1" ;;
  esac
  shift
done
[[ -d $dir ]] || die "usage: release-sums.sh PARTS_DIR [--expect FILE] [--assets FILE] [--out FILE]"
out="${out:-$dir/SHA256SUMS}"

declare -A sum_of=()
pending=()
shopt -s nullglob
for part in "$dir"/*.sha256; do
  while read -r line || [[ -n $line ]]; do
    [[ -n $line ]] || continue
    [[ $line =~ ^([0-9a-f]{64})\ \ ([^/[:space:]]+)$ ]] || die "${part##*/}: not a 'sha256  name' line: $line"
    hash="${BASH_REMATCH[1]}" name="${BASH_REMATCH[2]}"
    if [[ -n $assets ]] && ! grep -qxF -- "$name" "$assets"; then pending+=("$name (checksum uploaded, file not yet)"); continue; fi
    if [[ -n ${sum_of[$name]:-} && ${sum_of[$name]} != "$hash" ]]; then die "$name has two different checksums"; fi
    sum_of[$name]=$hash
  done <"$part"
done

missing=("${pending[@]}")
if [[ -n $expect ]]; then
  while read -r glob; do
    [[ -n $glob && $glob != '#'* ]] || continue
    found=0
    for name in "${!sum_of[@]}"; do
      # shellcheck disable=SC2254 # the pattern is the point
      case $name in $glob) found=1; break ;; esac
    done
    ((found)) || missing+=("$glob")
  done <"$expect"
fi
if ((${#missing[@]})); then
  echo "release-sums: not ready, missing:" >&2
  printf '  %s\n' "${missing[@]}" >&2
  exit 3
fi
((${#sum_of[@]})) || { echo "release-sums: no checksum parts in $dir" >&2; exit 3; }

tmp="$(mktemp "$out.XXXXXX")"
for name in "${!sum_of[@]}"; do printf '%s  %s\n' "${sum_of[$name]}" "$name"; done | LC_ALL=C sort -k2 >"$tmp"
mv -f "$tmp" "$out"
echo "release-sums: ${#sum_of[@]} files in $out"
