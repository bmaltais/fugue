# AppImage

`Fugue-<version>-x86_64.AppImage` is Fugue for Linux distributions other than Arch (issue #202). One file is the
window and the command line: started with no argument, or with a `.fugue` file, it opens the window; started with
a command (`Fugue.AppImage run flow.fugue`, `--version`, `pack add`, `skill install`) it is the CLI, because the
binary decides that itself (`src/cli/args.cpp`). It is built by `.github/workflows/appimage.yml` with
`packaging/appimage/build-appimage.sh`, from the same sources as the pacman package.

## Decisions

| Question | Decision | Rejected, and why |
|---|---|---|
| Bundling tool | linuxdeploy + linuxdeploy-plugin-qt (MIT), then appimagetool and the type2 runtime. It is what Qt AppImages use, it walks the Qt plugins and the QML imports, and it needs no source change. | `linuxdeployqt`: Qt 6 and new glibc support lag, and it refuses to build on a newer host than its target. Hand-copying libraries with `ldd`: that is linuxdeploy's job, with an exclude list we would have to keep. `appimage-builder`: unmaintained. StartAppImage (the owner's repo) only installs and updates AppImages, it builds none, so there was nothing to reuse but its release-asset naming: `Fugue-<version>-x86_64.AppImage` is what it picks for a CPU. |
| Build base | Ubuntu 22.04 container (glibc 2.35, the oldest Qt 6.12 supports); runs on Ubuntu 22.04+, Debian 12+, Fedora 36+ and anything newer. | Building on the runner's Ubuntu 24.04: raises the glibc floor for no gain. Ubuntu 20.04: Qt 6.12 does not run there. |
| Qt | Qt 6.12.1 from aqtinstall 3.3.0 (it reads the version list from download.qt.io, so it installs releases newer than itself: it installed 6.11.2 in the first CI run; modules qtmultimedia, qtimageformats, qtshadertools). The app needs Qt 6.12 (`setExternalSingletonInstance`, see [aarch64-ci.md](aarch64-ci.md)); 6.11 lacks it too, and no Ubuntu release has 6.12 (22.04 has 6.2, 24.04 6.4, 26.04 6.10). aqtinstall checks every archive against the checksums download.qt.io publishes. | Distro Qt: too old to compile. Building Qt from source: an hour or more per run. The Qt online installer: needs an account. |
| CMake | Kitware's 3.31.12 release tarball (sha256 in the workflow, cached by version), first on `PATH`; the workflow prints `cmake --version`. Qt 6.12's CMake package refuses anything older than 3.25 and Ubuntu 22.04 has 3.22. | Setting `QT_FORCE_MIN_CMAKE_VERSION_FOR_USING_QT`: runs Qt's package on a CMake it does not support. The distro's `cmake` from pip or a PPA: not pinned by hash. |
| libvips | Built from the pinned 8.18.7 tarball (sha256 in the workflow) with meson in the build container, only JPEG, PNG, WebP, TIFF, HEIF/AVIF, GIF, lcms and exif, no modules, GUI, introspection, docs or tests; cached by version. The app needs 8.15 or newer (`VImage::clamp`, `VIPS_FOREIGN_KEEP_*`) and Ubuntu 22.04 ships 8.12. `build-appimage.sh` refuses an older libvips and checks the bundled copy is the one it built against. | An Ubuntu 24.04 base (libvips 8.15): raises the glibc floor to 2.39 and still pins nothing. Bundling the Ubuntu 22.04 libvips: too old to compile. |
| GPU stack | Never bundled. The host provides libGL/libEGL, the Vulkan loader and its drivers, Mesa. The script deletes `libvulkan`, `libGL*`, `libEGL`, `libOpenGL`, `libgbm`, `libdrm`, `libnvidia*`, `libcuda*`, `libLLVM`, `libwayland-*` from the AppDir after linuxdeploy. | Bundling Mesa or the loader: breaks on any machine whose driver differs. |
| Bundled | Qt 6.12 and its xcb and Wayland (generic, egl) platform plugins, imageformats (WebP), the QtMultimedia FFmpeg backend with the FFmpeg libraries Qt ships, libvips and the libraries it pulls in (libarchive too), the bundled templates, `examples/demo.fugue`, the icons, the `.desktop` and the MIME types. | System ffmpeg/openslide: libvips brings what it links; nothing else uses them. |
| Environment | No `LD_LIBRARY_PATH` and no `QT_*` variable: linuxdeploy writes a RUNPATH and `usr/bin/qt.conf` points Qt at `usr/plugins` and `usr/qml`. A variable would leak into the programs Fugue starts (nodes, `uv`, a pack's tools) and break them against the bundled libraries. `AppRun` is a short `sh` script. | The default AppRun with plugin hooks: it exports the Qt paths for every child process. |
| Helper | The AppImage ships no `fugue-helper-*`. `AppRun` sets `FUGUE_HELPER` (unless the user did) to the first of `fugue-helper-cuda` (NVIDIA hosts only), `-vulkan`, `-cpu` found in `$XDG_DATA_HOME/fugue/helper`, `~/.local/lib/fugue`, `/usr/local/lib/fugue`, `/usr/lib/fugue` (`FUGUE_HELPER_DIRS` replaces the list). Without one, Fugue talks to providers (Ollama, remote helpers, the mesh) as it always can. | Shipping the Vulkan helper: it adds the stable-diffusion.cpp build (shaderc, the patch, the pinned tarballs) to a job that is already the longest, and the CUDA one is 170 MB. A follow-up can build a portable Vulkan helper tarball the way `bin/package-helper` does for aarch64 and have `install.sh` fetch it. Until then `CudaHelperNotice` still says `sudo pacman -S fugue-helper-cuda`: wrong on these hosts, a small follow-up. |
| Read-only mount | Fugue already writes only under `QStandardPaths` (`~/.config`, `~/.local/share`, `~/.local/state`, `~/.cache`, `XDG_RUNTIME_DIR`) and reads templates from `../share/fugue/templates` of the binary, which is inside the mount. `fugue pack add` clones into the data folder; a pack's `uv` environment is built there, with the host's `uv` and Python (not bundled). The smoke test fails if a command writes next to the AppImage. | Bundling `uv`/Python: a pack picks its own interpreter. |
| Updates | None inside the file. `install.sh` upgrades it (it keeps the file name it installed to know), and StartAppImage can follow `bmaltais/fugue` releases. | AppImageUpdate with zsync: needs `.zsync` files published, a per-release asset with its own checksum, and AppImageUpdate on the user's side, for a file that already has a one-line upgrade command. Revisit if people ask. |
| Trust | Unsigned, like the pacman packages. `SHA256SUMS` catches a damaged download, not a compromised release. GPG signing of the release assets can come later (the runtime supports embedded signatures). Every third-party download in the build is pinned by version and sha256 in `build-appimage.sh`; aqtinstall is pinned by version only (its dependencies are not hashed). | |
| aarch64 | Not built yet. It needs an arm runner, aqtinstall's `linux_arm64` host and the aarch64 assets of the same tools (pin their sha256 the same way). The script refuses other architectures with a message. | |
| Flatpak | Not now. | |

Licences: linuxdeploy, its Qt plugin and appimagetool are MIT; the runtime is MIT; none of their source is here.
Qt is LGPL and is shipped as shared libraries, replaceable by extracting the AppImage (`--appimage-extract`).

## What the host needs

A desktop already has these: the GPU stack (OpenGL/EGL libraries, the Vulkan loader, Mesa drivers or NVIDIA's), fontconfig
with one font, libxkbcommon, D-Bus, glib. For the window on a bare server or container the smoke test installs exactly
that list (`.github/workflows/appimage.yml`, job `smoke`). Wayland and X11 both work (Qt picks Wayland when
`WAYLAND_DISPLAY` is set). `uv` is needed for packs that use it, as with the packaged build.

## Checking it

Locally (no Qt): `packaging/appimage/test-apprun.sh` runs `AppRun` against a fake AppDir.
In CI: `build` makes the file, `smoke` starts it on `ubuntu:22.04`, `ubuntu:24.04` and `fedora:latest`
(`packaging/appimage/smoke-test.sh`: `--version`, `fugue run` of `examples/demo.fugue`, `pack list` and `template list`,
the window opening `templates/text-to-image/workflow.fugue` offscreen and saving a screenshot, nothing written beside the file),
and on a `v*` tag `release` waits for `package.yml`'s release and adds the file and its line in `SHA256SUMS`.
By hand: Actions, AppImage, Run workflow, then download the `appimage` artifact. A packaging change runs the workflow
on its pull request.

Not covered by CI: a real GPU, file dialogs, drag and drop, a Wayland compositor. Those are tried on a desktop.
