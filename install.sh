#!/usr/bin/env bash
# Install or upgrade Fugue from its latest GitHub release (Arch Linux, Omarchy, Arch Linux ARM, Asahi).
#
#   curl -fsSL https://raw.githubusercontent.com/bmaltais/fugue/main/install.sh | bash
#   curl -fsSL .../install.sh | bash -s -- --dry-run      # show what would happen, change nothing
#   curl -fsSL .../install.sh | bash -s -- --version v0.1.0-test2
#
# It checks the system, downloads the packages for it, verifies them against the release's
# SHA256SUMS, and installs them with `sudo pacman -U`. The packages are unsigned: the checksums
# catch a damaged download, not a compromised release.
set -euo pipefail

repo="bmaltais/fugue"
dry=0
want=""
cuda=auto # auto | yes | no

usage() { echo "usage: install.sh [--dry-run] [--version TAG] [--cuda | --no-cuda]"; }
die() { echo "fugue install: $*" >&2; exit 1; }
say() { echo "==> $*"; }

while (($#)); do
  case "$1" in
    --dry-run) dry=1 ;;
    --version) want="${2:-}"; [[ -n $want ]] || die "--version needs a tag, e.g. v0.1.0-test2"; shift ;;
    --cuda) cuda=yes ;;
    --no-cuda) cuda=no ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown option: $1 (try --help)" ;;
  esac
  shift
done

# --- the system ---------------------------------------------------------------------------------
command -v pacman >/dev/null || die "this needs pacman (Arch Linux, Omarchy, Arch Linux ARM, Asahi). On another system, build from source: https://github.com/$repo#install-by-hand"
for tool in curl sha256sum vercmp; do command -v "$tool" >/dev/null || die "missing tool: $tool"; done
case "$(uname -m)" in
  x86_64) arch=x86_64 ;;
  aarch64) arch=aarch64 ;;
  *) die "unsupported architecture: $(uname -m) (packages exist for x86_64 and aarch64)" ;;
esac
have_nvidia=0
if command -v nvidia-smi >/dev/null && nvidia-smi -L >/dev/null 2>&1; then have_nvidia=1; fi
want_helper=0
if [[ $cuda == yes || ( $cuda == auto && $have_nvidia == 1 ) ]]; then want_helper=1; fi
if ((want_helper)) && [[ $arch != x86_64 ]]; then
  say "the CUDA helper package exists for x86_64 only; on $arch see docs for the helper tarball. Installing Fugue alone."
  want_helper=0
fi
say "system: $arch, NVIDIA GPU: $([[ $have_nvidia == 1 ]] && echo yes || echo no), CUDA helper: $([[ $want_helper == 1 ]] && echo yes || echo no)"

# --- the release ---------------------------------------------------------------------------------
api="https://api.github.com/repos/$repo/releases"
if [[ -n $want ]]; then url="$api/tags/$want"; else url="$api"; fi
json="$(curl -fsSL -H 'Accept: application/vnd.github+json' "$url")" || die "could not read the release list ($url)"
# The newest release is first in the list, pre-releases included (Fugue is still a test release).
urls="$(printf '%s' "$json" | grep -o '"browser_download_url": *"[^"]*"' | sed 's/.*"\(https[^"]*\)"/\1/')"
[[ -n $urls ]] || die "no release assets found"
tag="$(printf '%s' "$json" | grep -o '"tag_name": *"[^"]*"' | head -1 | sed 's/.*: *"\(.*\)"/\1/')"
# Only the first release's assets when listing several: they are grouped under their tag in order,
# so keep the assets whose path contains that tag.
urls="$(printf '%s\n' "$urls" | grep "/download/$tag/")"
say "release: $tag"

pick() { printf '%s\n' "$urls" | grep -E "/$1-[0-9][^/]*-$arch\.pkg\.tar\.zst\$" | head -1; }
fugue_url="$(pick fugue)"
[[ -n $fugue_url ]] || die "release $tag has no fugue package for $arch"
helper_url=""
if ((want_helper)); then
  helper_url="$(pick fugue-helper-cuda)"
  [[ -n $helper_url ]] || { say "release $tag has no CUDA helper package for $arch; installing Fugue alone"; want_helper=0; }
fi
sums_url="$(printf '%s\n' "$urls" | grep '/SHA256SUMS$' | head -1)"
[[ -n $sums_url ]] || die "release $tag has no SHA256SUMS: refusing to install unverified packages"

# --- install or upgrade? ---------------------------------------------------------------------------
pkgver_of() { # .../fugue-0.1.0_test2-1-x86_64.pkg.tar.zst -> 0.1.0_test2-1
  local f="${1##*/}"
  f="${f%"-$arch.pkg.tar.zst"}"
  echo "${f#fugue-}"
}
new_ver="$(pkgver_of "$fugue_url")"
if old_ver="$(pacman -Q fugue 2>/dev/null | cut -d' ' -f2)" && [[ -n $old_ver ]]; then
  c="$(vercmp "$old_ver" "$new_ver")"
  if [[ $c -ge 0 && -z $want ]]; then say "fugue $old_ver is installed and is up to date (latest: $new_ver)"; exit 0; fi
  say "upgrading fugue $old_ver -> $new_ver"
else
  say "installing fugue $new_ver"
fi

todo=("$fugue_url"); [[ -n $helper_url ]] && todo+=("$helper_url")
if ((dry)); then
  say "dry run: would download, verify and install:"; printf '    %s\n' "${todo[@]}"; exit 0
fi

# --- download, verify, install -----------------------------------------------------------------------
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
curl -fsSL -o "$work/SHA256SUMS" "$sums_url"
files=()
for u in "${todo[@]}"; do
  f="${u##*/}"; say "downloading $f"
  curl -fL --progress-bar -o "$work/$f" "$u"
  want_sum="$(grep -F " $f" "$work/SHA256SUMS" | awk '{print $1}' | head -1)"
  [[ -n $want_sum ]] || die "$f is not in SHA256SUMS"
  got_sum="$(sha256sum "$work/$f" | awk '{print $1}')"
  [[ $got_sum == "$want_sum" ]] || die "checksum mismatch for $f: refusing to install"
  files+=("$work/$f")
done
say "checksums match; installing with sudo pacman -U"
sudo pacman -U --noconfirm "${files[@]}"
say "done: run \`fugue\`"
