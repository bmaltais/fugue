# aarch64 CI

`.github/workflows/test.yml` has two jobs: `test` (x86_64, Arch container) and `test-aarch64`
(GitHub's free `ubuntu-24.04-arm` runner). Both run `bin/build` and `bin/test` offscreen, with ccache.

## Why Arch Linux ARM, in a container the job makes

- The app needs Qt 6.11 or newer (`QQmlEngine::setExternalSingletonInstance`, src/qml_app.cpp).
  Ubuntu 24.04 (the runner's OS) has Qt 6.4, Debian 13 has 6.8 and Ubuntu 26.04 has 6.10: all fail to
  compile it. Arch (6.12) and Arch Linux ARM (6.12) are current.
- Arch Linux ARM is the distribution the aarch64 package targets, with the same pacman packages and
  the same PKGBUILD dependency lists as the x86 job, so there is no second list to keep.
- It has no official Docker image, so the job downloads the project's own root filesystem tarball
  (os.archlinuxarm.org, checked against its published md5) and `docker import`s it. No third-party image.
- Cost: about 830 MB to download per run, and `pacman -Syu` on first start.

## Not covered

- GPU: runners have none, so tests that need Vulkan or CUDA are not run on either architecture.
- Apple Silicon Linux (Asahi) uses 16 KB pages. Nothing in CI checks that; it needs a real Asahi machine.
