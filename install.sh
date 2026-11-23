#!/usr/bin/env bash
# Install or upgrade Fugue from its latest GitHub release.
#
#   curl -fsSL https://raw.githubusercontent.com/bmaltais/fugue/main/install.sh | bash
#   curl -fsSL .../install.sh | bash -s -- --dry-run      # show what would happen, change nothing
#   curl -fsSL .../install.sh | bash -s -- --version v0.1.0-test2
#
# On Arch Linux, Omarchy, Arch Linux ARM and Asahi it downloads the packages for the system, verifies them
# against the release's SHA256SUMS, and installs them with `sudo pacman -U`. On any other Linux (no pacman) it
# falls back to the AppImage (x86_64): it goes to ~/.local/bin/fugue (--dir to change it), is verified the same
# way, and gets a launcher entry, an icon and the file types in ~/.local/share (--no-desktop to skip that).
# --appimage chooses the AppImage on an Arch system too. Packages and AppImage are unsigned: the checksums
# catch a damaged download, not a compromised release.
set -euo pipefail

repo="bmaltais/fugue"
dry=0
want=""
cuda=auto # auto | yes | no
mode=auto # auto | appimage
bindir="${HOME}/.local/bin"
desktop=1

usage() { echo "usage: install.sh [--dry-run] [--version TAG] [--cuda | --no-cuda] [--appimage] [--dir FOLDER] [--no-desktop]"; }
die() { echo "fugue install: $*" >&2; exit 1; }
say() { echo "==> $*"; }

while (($#)); do
  case "$1" in
    --dry-run) dry=1 ;;
    --version) want="${2:-}"; [[ -n $want ]] || die "--version needs a tag, e.g. v0.1.0-test2"; shift ;;
    --cuda) cuda=yes ;;
    --no-cuda) cuda=no ;;
    --appimage) mode=appimage ;;
    --dir) bindir="${2:-}"; [[ -n $bindir ]] || die "--dir needs a folder"; shift ;;
    --no-desktop) desktop=0 ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown option: $1 (try --help)" ;;
  esac
  shift
done

# --- the system ---------------------------------------------------------------------------------
if [[ $mode == auto ]]; then
  if command -v pacman >/dev/null; then mode=pacman; else mode=appimage; fi
else
  command -v curl >/dev/null || die "missing tool: curl"
fi
if [[ $mode == pacman ]]; then
  for tool in curl sha256sum vercmp; do command -v "$tool" >/dev/null || die "missing tool: $tool"; done
else
  for tool in curl sha256sum; do command -v "$tool" >/dev/null || die "missing tool: $tool"; done
fi
case "$(uname -m)" in
  x86_64) arch=x86_64 ;;
  aarch64) arch=aarch64 ;;
  *) die "unsupported architecture: $(uname -m) (packages exist for x86_64 and aarch64)" ;;
esac
if [[ $mode == appimage && $arch != x86_64 ]]; then
  die "the AppImage exists for x86_64 only, and this is $arch. On Arch Linux ARM or Asahi use the pacman packages; elsewhere build from source: https://github.com/$repo#install-by-hand"
fi
have_nvidia=0
if command -v nvidia-smi >/dev/null && nvidia-smi -L >/dev/null 2>&1; then have_nvidia=1; fi
want_helper=0
if [[ $cuda == yes || ( $cuda == auto && $have_nvidia == 1 ) ]]; then want_helper=1; fi
if [[ $mode == appimage ]]; then
  want_helper=0
fi
if ((want_helper)) && [[ $arch != x86_64 ]]; then
  say "the CUDA helper package exists for x86_64 only; on $arch see docs for the helper tarball. Installing Fugue alone."
  want_helper=0
fi
if [[ $mode == appimage ]]; then
  say "system: $arch, no pacman: using the AppImage (the generation helper is a separate download: docs/appimage.md)"
else
  say "system: $arch, NVIDIA GPU: $([[ $have_nvidia == 1 ]] && echo yes || echo no), CUDA helper: $([[ $want_helper == 1 ]] && echo yes || echo no)"
fi

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
helper_url=""
if [[ $mode == appimage ]]; then
  fugue_url="$(printf '%s\n' "$urls" | grep -E "/Fugue-[0-9][^/]*-$arch\.AppImage\$" | head -1)"
  [[ -n $fugue_url ]] || die "release $tag has no AppImage for $arch"
else
  fugue_url="$(pick fugue)"
  [[ -n $fugue_url ]] || die "release $tag has no fugue package for $arch"
  if ((want_helper)); then
    helper_url="$(pick fugue-helper-cuda)"
    [[ -n $helper_url ]] || { say "release $tag has no CUDA helper package for $arch; installing Fugue alone"; want_helper=0; }
  fi
fi
sums_url="$(printf '%s\n' "$urls" | grep '/SHA256SUMS$' | head -1)"
[[ -n $sums_url ]] || die "release $tag has no SHA256SUMS: refusing to install unverified files"

# --- install or upgrade? ---------------------------------------------------------------------------
pkgver_of() { # .../fugue-0.1.0_test2-1-x86_64.pkg.tar.zst -> 0.1.0_test2-1
  local f="${1##*/}"
  f="${f%"-$arch.pkg.tar.zst"}"
  echo "${f#fugue-}"
}
if [[ $mode == appimage ]]; then
  # The AppImage prints no release name, so the file name of what this script installed is kept beside it.
  dest="$bindir/fugue"
  marker="${XDG_DATA_HOME:-$HOME/.local/share}/fugue/appimage-installed"
  new_name="${fugue_url##*/}"
  if [[ -x $dest && -f $marker && $(cat "$marker") == "$new_name" && -z $want ]]; then
    say "$dest is $new_name, the latest: nothing to do"; exit 0
  fi
  say "$([[ -x $dest ]] && echo upgrading || echo installing) $new_name to $dest"
else
  new_ver="$(pkgver_of "$fugue_url")"
  if old_ver="$(pacman -Q fugue 2>/dev/null | cut -d' ' -f2)" && [[ -n $old_ver ]]; then
    c="$(vercmp "$old_ver" "$new_ver")"
    if [[ $c -ge 0 && -z $want ]]; then say "fugue $old_ver is installed and is up to date (latest: $new_ver)"; exit 0; fi
    say "upgrading fugue $old_ver -> $new_ver"
  else
    say "installing fugue $new_ver"
  fi
fi

todo=("$fugue_url"); [[ -n $helper_url ]] && todo+=("$helper_url")
if ((dry)); then
  say "dry run: would download and verify:"; printf '    %s\n' "${todo[@]}"
  if [[ $mode == appimage ]]; then
    say "dry run: would install it as $dest (mode 755)"
    ((desktop)) && say "dry run: would add the launcher entry, icon and file types under ${XDG_DATA_HOME:-$HOME/.local/share} (applications, icons/hicolor, mime/packages)"
    case ":$PATH:" in *":$bindir:"*) ;; *) say "dry run: $bindir is not in PATH; you would add it to run \`fugue\` from a terminal" ;; esac
  else
    say "dry run: would install with sudo pacman -U"
  fi
  exit 0
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
if [[ $mode == pacman ]]; then
  say "checksums match; installing with sudo pacman -U"
  sudo pacman -U --noconfirm "${files[@]}"
  say "done: run \`fugue\`"
  exit 0
fi

say "checksums match; installing to $dest"
mkdir -p "$bindir"
install -m 755 "${files[0]}" "$dest.new" && mv -f "$dest.new" "$dest"
mkdir -p "$(dirname "$marker")" && printf '%s\n' "$new_name" >"$marker"
if ((desktop)); then
  # What the pacman package installs under /usr/share, taken out of the AppImage (extracting needs no FUSE).
  data="${XDG_DATA_HOME:-$HOME/.local/share}"
  (cd "$work" && "$dest" --appimage-extract 'usr/share/applications/fugue.desktop' 'usr/share/mime/packages/fugue.xml' 'usr/share/icons/hicolor/*' >/dev/null) \
    || die "could not read the launcher files out of $dest (it is installed; rerun with --no-desktop to skip them)"
  share="$work/squashfs-root/usr/share"
  mkdir -p "$data/applications" "$data/mime/packages" "$data/icons"
  sed "s|^Exec=fugue |Exec=$dest |" "$share/applications/fugue.desktop" >"$data/applications/fugue.desktop"
  cp "$share/mime/packages/fugue.xml" "$data/mime/packages/fugue.xml"
  cp -r "$share/icons/hicolor" "$data/icons/"
  command -v update-mime-database >/dev/null && update-mime-database "$data/mime" >/dev/null 2>&1 || true
  command -v update-desktop-database >/dev/null && update-desktop-database "$data/applications" >/dev/null 2>&1 || true
  command -v gtk-update-icon-cache >/dev/null && gtk-update-icon-cache -q -t "$data/icons/hicolor" >/dev/null 2>&1 || true
fi
case ":$PATH:" in *":$bindir:"*) ;; *) say "note: $bindir is not in your PATH" ;; esac
say "done: run \`fugue\` (window) or \`fugue run file.fugue\` (command line). It needs the host's GPU libraries, see docs/appimage.md"
