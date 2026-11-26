#!/bin/bash
# The PKGBUILD builds the helper from tarballs of its own; they must be the ones helper/CMakeLists.txt pins
# (helper/README.md, Changing the engine, step 3). CI's Helper job and the local suite (CTest `packaging_pins`)
# run this, so a pin bumped in one file only fails before it is merged.
set -euo pipefail
root="${1:-$(cd "$(dirname "$0")/.." && pwd)}"
cmake_file="$root/helper/CMakeLists.txt"
# shellcheck source=/dev/null
source "$root/packaging/PKGBUILD"
status=0
# shellcheck disable=SC2154 # _sdcpp, _ggml and _cutlass come from the PKGBUILD sourced above
for commit in "$_sdcpp" "$_ggml" "$_cutlass"; do
  grep -q "$commit" "$cmake_file" || { echo "packaging/PKGBUILD pins $commit, helper/CMakeLists.txt does not"; status=1; }
done
while read -r name hash; do
  grep -q "$hash" "$root/packaging/PKGBUILD" || { echo "helper/CMakeLists.txt's $name $hash is not in packaging/PKGBUILD's sha256sums"; status=1; }
done < <(sed -n 's/^set(\([a-z_]*_sha256\) \([0-9a-f]\{64\}\))$/\1 \2/p' "$cmake_file")
exit $status
