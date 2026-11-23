#!/usr/bin/env bash
# Builds Fugue-<version>-<arch>.AppImage from this checkout. Decisions and what is rejected: docs/appimage.md.
#
#   QT_ROOT=/opt/qt/6.11.2/gcc_64 packaging/appimage/build-appimage.sh [--out DIR] [--work DIR]
#
# QT_ROOT is a Qt 6.11 (or newer) with qtmultimedia, qtimageformats and qtshadertools; the workflow
# (.github/workflows/appimage.yml) installs it with aqtinstall in an Ubuntu 22.04 container, the oldest glibc
# that Qt 6.11 supports. Needs cmake, ninja or make, a C++17 compiler, libvips and libarchive headers, the Vulkan
# loader headers, zlib and patchelf; curl and sha256sum for the tools below.
#
# Third-party tools are downloaded once into $WORK/tools and checked against the sha256 below. Set
# LINUXDEPLOY, LINUXDEPLOY_PLUGIN_QT, APPIMAGETOOL and APPIMAGE_RUNTIME to use copies you already trust.
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
here=$root/packaging/appimage
out=$root/dist
work=$root/build-appimage
while (($#)); do
  case "$1" in
    --out) out=${2:?--out needs a folder}; shift ;;
    --work) work=${2:?--work needs a folder}; shift ;;
    -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
    *) echo "build-appimage: unknown option $1" >&2; exit 2 ;;
  esac
  shift
done
die() { echo "build-appimage: $*" >&2; exit 1; }
say() { echo "==> $*"; }

arch=$(uname -m)
[[ $arch == x86_64 ]] || die "only x86_64 is built so far (see docs/appimage.md); this is $arch"
qt=${QT_ROOT:-}
[[ -x $qt/bin/qmake ]] || die "set QT_ROOT to a Qt 6.11 folder (the one with bin/qmake)"

# --- pinned third-party tools (versions and sha256 of the release assets) ----------------------------------
ld_url=https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20251107-1/linuxdeploy-x86_64.AppImage
ld_sha=c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d
ldqt_url=https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/1-alpha-20250213-1/linuxdeploy-plugin-qt-x86_64.AppImage
ldqt_sha=15106be885c1c48a021198e7e1e9a48ce9d02a86dd0a1848f00bdbf3c1c92724
tool_url=https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage
tool_sha=ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0
rt_url=https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64
rt_sha=2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d

fetch() { # fetch URL SHA256 FILE: reuse FILE when it already matches
  local url=$1 sha=$2 file=$3
  if [[ -f $file ]] && [[ $(sha256sum "$file" | cut -d' ' -f1) == "$sha" ]]; then return; fi
  say "downloading ${url##*/}"
  curl -fsSL --retry 3 -o "$file.part" "$url"
  [[ $(sha256sum "$file.part" | cut -d' ' -f1) == "$sha" ]] || { rm -f "$file.part"; die "checksum mismatch for $url"; }
  mv "$file.part" "$file"
  chmod +x "$file"
}
mkdir -p "$work/tools" "$out"
linuxdeploy=${LINUXDEPLOY:-$work/tools/linuxdeploy-x86_64.AppImage}
ldqt=${LINUXDEPLOY_PLUGIN_QT:-$work/tools/linuxdeploy-plugin-qt-x86_64.AppImage}
appimagetool=${APPIMAGETOOL:-$work/tools/appimagetool-x86_64.AppImage}
runtime=${APPIMAGE_RUNTIME:-$work/tools/runtime-x86_64}
[[ -n ${LINUXDEPLOY:-} ]] || fetch "$ld_url" "$ld_sha" "$linuxdeploy"
[[ -n ${LINUXDEPLOY_PLUGIN_QT:-} ]] || fetch "$ldqt_url" "$ldqt_sha" "$ldqt"
[[ -n ${APPIMAGETOOL:-} ]] || fetch "$tool_url" "$tool_sha" "$appimagetool"
[[ -n ${APPIMAGE_RUNTIME:-} ]] || fetch "$rt_url" "$rt_sha" "$runtime"
# linuxdeploy finds its plugins as linuxdeploy-plugin-<name>* on PATH, so give it the pinned one under that name.
mkdir -p "$work/plugins"
ln -sf "$ldqt" "$work/plugins/linuxdeploy-plugin-qt"

# The containers have no FUSE: every AppImage tool unpacks itself and runs from there.
export APPIMAGE_EXTRACT_AND_RUN=1

# --- the version, as the package computes it (packaging/PKGBUILD pkgver) --------------------------------------
if described=$(git -C "$root" describe --long --tags --match 'v*' 2>/dev/null); then
  version=$(sed -E 's/^v//; s/-0-g[0-9a-f]+$//; s/-([0-9]+)-g([0-9a-f]+)$/.r\1.g\2/; s/-/_/g' <<<"$described")
else
  version=$(printf '0.1.0.r%s.g%s' "$(git -C "$root" rev-list --count HEAD)" "$(git -C "$root" rev-parse --short HEAD)")
fi
file=Fugue-$version-$arch.AppImage
say "building $file with Qt $("$qt/bin/qmake" -query QT_VERSION)"

# --- build the app and install it into the AppDir -------------------------------------------------------------
appdir=$work/AppDir
rm -rf "$appdir" "$work/build"
# glib's gio headers have a struct field called `signals`, which Qt's `signals` keyword turns into `public`: a
# header that reaches them after Qt's macros fails (seen with aqtinstall's Qt 6.11.2 and Ubuntu 22.04's libvips,
# whose pkg-config flags put glib on the include path). Reading gio first makes any later include a no-op.
# The Arch build does not hit it, and the flag is harmless there.
pkg-config --exists gio-2.0 || die "libglib2.0-dev (gio-2.0) is needed: libvips's headers use it"
cxxflags="-include gio/gio.h $(pkg-config --cflags gio-2.0)"
generator=(); command -v ninja >/dev/null && generator=(-G Ninja)
cmake -S "$root" -B "$work/build" "${generator[@]}" -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$qt" -DCMAKE_CXX_FLAGS="$cxxflags" \
  -DCMAKE_INSTALL_PREFIX=/usr >/dev/null
cmake --build "$work/build" --target fugue --parallel "$(nproc)"
DESTDIR=$appdir cmake --install "$work/build" --prefix /usr
[[ -d $appdir/usr/share/fugue/templates ]] || die "the templates are missing: run git submodule update --init"

# usr/bin/qt.conf makes Qt look for its plugins and QML modules inside the AppDir, whatever the host sets.
cat >"$appdir/usr/bin/qt.conf" <<'CONF'
[Paths]
Prefix = ..
Plugins = plugins
Qml2Imports = qml
CONF

# --- bundle ------------------------------------------------------------------------------------------------------
# QML_SOURCES_PATHS: the QML is compiled into the binary, so linuxdeploy-plugin-qt finds its imports in the sources.
# The Wayland client plugins are named because the plugin only deploys xcb by default.
export QMAKE=$qt/bin/qmake
export QML_SOURCES_PATHS=$root/src
export EXTRA_PLATFORM_PLUGINS="libqwayland-generic.so;libqwayland-egl.so"
export EXTRA_QT_PLUGINS="iconengines;wayland-decoration-client;wayland-graphic-integration-client;wayland-shell-integration;imageformats;multimedia;tls;platforminputcontexts;xcbglintegrations;egldeviceintegrations"
export NO_STRIP=1 # linuxdeploy's own strip is older than the toolchain's relocation format
export LD_LIBRARY_PATH=$qt/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
export PATH=$work/plugins:$PATH
export LINUXDEPLOY_OUTPUT_VERSION=$version
"$linuxdeploy" --appdir "$appdir" --plugin qt \
  --executable "$appdir/usr/bin/fugue" \
  --desktop-file "$appdir/usr/share/applications/fugue.desktop" \
  --icon-file "$root/share/icons/hicolor/256x256/apps/fugue.png" --icon-filename fugue \
  --custom-apprun "$here/AppRun"

# --- what must not be inside --------------------------------------------------------------------------------------
# The GPU stack comes from the host: a bundled libGL, Vulkan loader or Mesa driver breaks on a machine whose driver
# differs (linuxdeploy's exclude list already skips most; this makes it certain).
find "$appdir/usr/lib" -maxdepth 2 \( -name 'libvulkan*' -o -name 'libGL.so*' -o -name 'libGLX*' -o -name 'libEGL.so*' \
  -o -name 'libOpenGL.so*' -o -name 'libGLESv2.so*' -o -name 'libgbm.so*' -o -name 'libdrm*' -o -name 'libnvidia*' \
  -o -name 'libcuda*' -o -name 'libLLVM*' -o -name 'libwayland-*' \) -print -delete

# --- checks: a missing piece fails the build here rather than on a user's machine -------------------------------
missing=0
need() { if [[ -z $(find "$appdir/usr" -path "$1" -print -quit) ]]; then echo "build-appimage: missing from the AppDir: $1" >&2; missing=1; fi; }
need '*/plugins/platforms/libqxcb.so'
need '*/plugins/platforms/libqwayland-generic.so'
need '*/plugins/platforms/libqoffscreen.so' # the smoke tests and headless use
need '*/plugins/imageformats/libqwebp.so'
need '*/plugins/multimedia/libffmpegmediaplugin.so'
need '*/lib/libvips.so*'
need '*/lib/libarchive.so*'
need '*/lib/libQt6Quick.so*'
need '*/qml/QtQuick/Controls/qmldir'
need '*/qml/QtMultimedia/qmldir'
need '*/share/fugue/templates/categories.json'
need '*/share/mime/packages/fugue.xml'
need '*/share/applications/fugue.desktop'
((! missing)) || die "the bundle is incomplete"
if readelf -d "$appdir/usr/bin/fugue" | grep -q 'RUNPATH\|RPATH'; then :; else die "usr/bin/fugue has no RUNPATH: the bundled libraries would not be found"; fi
if readelf -d "$appdir/usr/bin/fugue" | grep -E 'RUNPATH|RPATH' | grep -q "$qt"; then die "usr/bin/fugue still points at the build's Qt"; fi
desktop-file-validate "$appdir/usr/share/applications/fugue.desktop" 2>/dev/null || say "desktop-file-validate is not installed or complains: not fatal"

# --- pack ------------------------------------------------------------------------------------------------------------
rm -f "$out/$file"
ARCH=$arch "$appimagetool" --no-appstream --runtime-file "$runtime" "$appdir" "$out/$file"
chmod +x "$out/$file"
(cd "$out" && sha256sum "$file" | tee "$file.sha256")
say "done: $out/$file ($(du -h "$out/$file" | cut -f1))"
