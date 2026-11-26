# aarch64 CI

`.github/workflows/package.yml` has a third arm job, `package-aarch64`: it makes the same container, runs
`makepkg` as user `builder` (like the x86 job, `FUGUE_SKIP_CUDA=1`), prints the checksums, runs
`scripts/check-elf-align` and uploads the `package-aarch64` artifact. A `v*` tag puts its
`fugue-*-aarch64.pkg.tar.zst` and its `.sha256` file in the release beside the x86 package, and `release-sums.yml` lists it in `SHA256SUMS` (docs/release.md; the file names
carry the arch). Its ccache key is `package-aarch64-ccache-`.

`.github/workflows/test.yml` has two jobs: `test` (x86_64, Arch container) and `test-aarch64`
(GitHub's free `ubuntu-24.04-arm` runner). Both run `bin/build` and `bin/test` offscreen, with ccache.

## Why Arch Linux ARM, in a container the job makes

- The app needs Qt 6.12 or newer (`QQmlEngine::setExternalSingletonInstance`, src/qml_app.cpp; 6.11 does not have it; CMakeLists.txt asks for 6.12).
  Ubuntu 24.04 (the runner's OS) has Qt 6.4, Debian 13 has 6.8 and Ubuntu 26.04 has 6.10: all fail to
  compile it. Arch (6.12) and Arch Linux ARM (6.12) are current.
- Arch Linux ARM is the distribution the aarch64 package targets, with the same pacman packages and
  the same PKGBUILD dependency lists as the x86 job, so there is no second list to keep.
- It has no official Docker image, so the job downloads the project's own root filesystem tarball
  (os.archlinuxarm.org, checked against its published md5) and `docker import`s it. No third-party image.
- Cost: about 830 MB to download per run, and `pacman -Syu` on first start.

## Not covered

- GPU: runners have none, so tests that need Vulkan or CUDA are not run on either architecture.
- Apple Silicon Linux (Asahi) uses 16 KB pages. `package-aarch64` checks statically that every ELF binary in the
  package has LOAD segments aligned to 0x4000 or more (`scripts/check-elf-align`); running it needs a real Asahi machine.
