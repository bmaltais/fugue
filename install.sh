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
#
# The AppImage ships no generation helper. On x86_64 without pacman the script then also downloads the helper
# tarball for the machine: the CUDA one when an NVIDIA driver is present, else the Vulkan one when a Vulkan loader
# is, and unpacks it into ~/.local/share/fugue/helper, where the AppImage looks (docs/helper.md). --no-helper skips
# that, --helper cuda|vulkan|cpu chooses the backend, --helper-only installs only the helper (what the window's
# "faster generation" notice copies) and works with an AppImage already installed. A release without the tarball
# is reported and the rest carries on.
set -euo pipefail

repo="bmaltais/fugue"
dry=0
want=""
cuda=auto # auto | yes | no
mode=auto # auto | appimage
bindir="${HOME}/.local/bin"
desktop=1
helper=auto # auto | none | cuda | vulkan | cpu: the helper tarball (x86_64, no pacman)
helper_only=0

usage() { echo "usage: install.sh [--dry-run] [--version TAG] [--cuda | --no-cuda] [--appimage] [--dir FOLDER] [--no-desktop] [--no-helper | --helper cuda|vulkan|cpu | --helper-only]"; }
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
    --no-helper) helper=none ;;
    --helper) helper="${2:-}"; [[ $helper =~ ^(cuda|vulkan|cpu)$ ]] || die "--helper needs cuda, vulkan or cpu"; shift ;;
    --helper-only) helper_only=1 ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown option: $1 (try --help)" ;;
  esac
  shift
done

# --- the system ---------------------------------------------------------------------------------
if ((helper_only)); then
  mode=appimage # only the tarball is installed, which needs no pacman
  [[ $helper != none ]] || die "--helper-only and --no-helper contradict each other"
elif [[ $mode == auto ]]; then
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
if command -v nvidia-smi >/dev/null && nvidia-smi -L >/dev/null 2>&1; then have_nvidia=1
elif [[ -e /proc/driver/nvidia/version ]]; then have_nvidia=1; fi
have_vulkan=0
ldconfig_bin="$(command -v ldconfig || echo /sbin/ldconfig)"
if { [[ -x $ldconfig_bin ]] && "$ldconfig_bin" -p 2>/dev/null | grep -q 'libvulkan\.so\.1'; } \
  || compgen -G '/usr/lib*/libvulkan.so.1' >/dev/null || compgen -G '/usr/lib/*-linux-gnu/libvulkan.so.1' >/dev/null; then have_vulkan=1; fi
want_helper=0
if [[ $cuda == yes || ( $cuda == auto && $have_nvidia == 1 ) ]]; then want_helper=1; fi
if [[ $mode == appimage ]]; then
  want_helper=0
fi
if ((want_helper)) && [[ $arch != x86_64 ]]; then
  say "the CUDA helper package exists for x86_64 only; on $arch see docs for the helper tarball. Installing Fugue alone."
  want_helper=0
fi
# The helper tarball: the AppImage has none inside (docs/helper.md). Portable builds exist for x86_64.
tarball=""
if [[ $mode == appimage && $helper != none ]]; then
  if [[ $arch != x86_64 ]]; then
    say "the helper tarballs exist for x86_64 only; skipping the helper on $arch"
  elif [[ $helper != auto ]]; then tarball=$helper
  elif [[ $cuda != no && $have_nvidia == 1 ]]; then tarball=cuda
  elif [[ $have_vulkan == 1 ]]; then tarball=vulkan
  else say "no NVIDIA driver and no Vulkan loader found: no generation helper installed (--helper cpu for the slow CPU one)"
  fi
fi
if [[ $mode == appimage ]]; then
  what="no pacman: using the AppImage"; ((helper_only)) && what="helper only"
  say "system: $arch, $what, NVIDIA GPU: $([[ $have_nvidia == 1 ]] && echo yes || echo no), Vulkan loader: $([[ $have_vulkan == 1 ]] && echo yes || echo no), helper tarball: ${tarball:-none}"
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
tarball_url=""
if ((helper_only)); then
  fugue_url=""
elif [[ $mode == appimage ]]; then
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
if [[ -n $tarball ]]; then
  tarball_url="$(printf '%s\n' "$urls" | grep -E "/fugue-helper-$tarball-$arch-linux\.tar\.zst\$" | head -1 || true)"
  if [[ -z $tarball_url ]]; then
    say "release $tag has no $tarball helper tarball for $arch: installing without it (docs/helper.md says how to build one)"
    ((helper_only)) && exit 0
    tarball=""
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
helper_dir="${XDG_DATA_HOME:-$HOME/.local/share}/fugue/helper"
app_current=0
if ((helper_only)); then
  say "installing the $tarball helper into $helper_dir"
elif [[ $mode == appimage ]]; then
  # The AppImage prints no release name, so the file name of what this script installed is kept beside it.
  dest="$bindir/fugue"
  marker="${XDG_DATA_HOME:-$HOME/.local/share}/fugue/appimage-installed"
  new_name="${fugue_url##*/}"
  if [[ -x $dest && -f $marker && $(cat "$marker") == "$new_name" && -z $want ]]; then
    app_current=1
    say "$dest is $new_name, the latest"
  else
    say "$([[ -x $dest ]] && echo upgrading || echo installing) $new_name to $dest"
  fi
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

# --- what to download ---------------------------------------------------------------------------------
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
curl -fsSL -o "$work/SHA256SUMS" "$sums_url"
sum_of() { grep -F " $1" "$work/SHA256SUMS" | awk '{print $1}' | head -1; }
todo=()
((helper_only || app_current)) || todo+=("$fugue_url")
[[ -n $helper_url ]] && todo+=("$helper_url")
helper_marker=""
if [[ -n $tarball_url ]]; then
  tarball_file="${tarball_url##*/}"
  tarball_sum="$(sum_of "$tarball_file")"
  [[ -n $tarball_sum ]] || die "$tarball_file is not in SHA256SUMS"
  helper_marker="$helper_dir/.installed-$tarball"
  if [[ -f $helper_marker && $(cat "$helper_marker") == "$tag $tarball_sum" ]]; then
    say "the $tarball helper in $helper_dir is $tag's: nothing to do for it"
  else
    todo+=("$tarball_url")
  fi
fi
if ((${#todo[@]} == 0)); then say "nothing to do"; exit 0; fi

if ((dry)); then
  say "dry run: would download and verify:"; printf '    %s\n' "${todo[@]}"
  if [[ $mode == appimage ]]; then
    ((helper_only || app_current)) || say "dry run: would install it as $dest (mode 755)"
    ((desktop && ! helper_only && ! app_current)) && say "dry run: would add the launcher entry, icon and file types under ${XDG_DATA_HOME:-$HOME/.local/share} (applications, icons/hicolor, mime/packages)"
    [[ -n $tarball_url ]] && say "dry run: would unpack the $tarball helper into $helper_dir (the AppImage looks there)"
    case ":$PATH:" in *":$bindir:"*) ;; *) say "dry run: $bindir is not in PATH; you would add it to run \`fugue\` from a terminal" ;; esac
  else
    say "dry run: would install with sudo pacman -U"
  fi
  exit 0
fi

# --- download, verify, install -----------------------------------------------------------------------
files=()
for u in "${todo[@]}"; do
  f="${u##*/}"; say "downloading $f"
  curl -fL --progress-bar -o "$work/$f" "$u"
  want_sum="$(sum_of "$f")"
  [[ -n $want_sum ]] || die "$f is not in SHA256SUMS"
  got_sum="$(sha256sum "$work/$f" | awk '{print $1}')"
  [[ $got_sum == "$want_sum" ]] || die "checksum mismatch for $f: refusing to install"
  [[ $f == *.AppImage || $f == *.pkg.tar.zst ]] && files+=("$work/$f")
done
if [[ $mode == pacman ]]; then
  say "checksums match; installing with sudo pacman -U"
  sudo pacman -U --noconfirm "${files[@]}"
  say "done: run \`fugue\`"
  exit 0
fi

if [[ -n $tarball_url && -f $work/$tarball_file ]]; then
  # The tarball holds one folder; its content goes into the folder AppRun searches. A CUDA one carries its own lib/.
  command -v zstd >/dev/null || die "the helper tarball needs zstd to unpack (install the zstd package) and the AppImage is not changed"
  say "unpacking the $tarball helper into $helper_dir"
  mkdir -p "$work/helper" "$helper_dir"
  tar --zstd --strip-components=1 -xf "$work/$tarball_file" -C "$work/helper"
  rm -rf "${helper_dir:?}/lib" "${helper_dir:?}/licenses"
  cp -a "$work/helper/." "$helper_dir/"
  printf '%s %s\n' "$tag" "$tarball_sum" >"$helper_marker"
fi
if ((helper_only)); then
  say "done: the AppImage finds the helper in $helper_dir; restart Fugue"
  exit 0
fi

if ((${#files[@]})); then
  say "checksums match; installing to $dest"
  mkdir -p "$bindir"
  install -m 755 "${files[0]}" "$dest.new" && mv -f "$dest.new" "$dest"
  mkdir -p "$(dirname "$marker")" && printf '%s\n' "$new_name" >"$marker"
fi
if ((desktop && ${#files[@]})); then
  # What the pacman package installs under /usr/share, taken out of the AppImage (extracting needs no FUSE).
  data="${XDG_DATA_HOME:-$HOME/.local/share}"
  (cd "$work" && "$dest" --appimage-extract 'usr/share/applications/fugue.desktop' 'usr/share/applications/fugue-app.desktop' 'usr/share/mime/packages/fugue.xml' 'usr/share/icons/hicolor/*' >/dev/null) \
    || die "could not read the launcher files out of $dest (it is installed; rerun with --no-desktop to skip them)"
  share="$work/squashfs-root/usr/share"
  mkdir -p "$data/applications" "$data/mime/packages" "$data/icons"
  sed "s|^Exec=fugue |Exec=$dest |" "$share/applications/fugue.desktop" >"$data/applications/fugue.desktop"
  sed "s|^Exec=fugue |Exec=$dest |" "$share/applications/fugue-app.desktop" >"$data/applications/fugue-app.desktop" # opens a .fugueapp as an app
  cp "$share/mime/packages/fugue.xml" "$data/mime/packages/fugue.xml"
  cp -r "$share/icons/hicolor" "$data/icons/"
  command -v update-mime-database >/dev/null && update-mime-database "$data/mime" >/dev/null 2>&1 || true
  command -v update-desktop-database >/dev/null && update-desktop-database "$data/applications" >/dev/null 2>&1 || true
  command -v gtk-update-icon-cache >/dev/null && gtk-update-icon-cache -q -t "$data/icons/hicolor" >/dev/null 2>&1 || true
fi
case ":$PATH:" in *":$bindir:"*) ;; *) say "note: $bindir is not in your PATH" ;; esac
say "done: run \`fugue\` (window) or \`fugue run file.fugue\` (command line). It needs the host's GPU libraries, see docs/appimage.md"
