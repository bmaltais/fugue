# aarch64 CI

`.github/workflows/test.yml` has two jobs: `test` (x86_64, Arch container) and `test-aarch64`
(GitHub's free `ubuntu-24.04-arm` runner). Both run `bin/build` and `bin/test` offscreen, with ccache.

## Why Ubuntu 26.04 on the ARM job

- Arch has no official arm64 container. Arch Linux ARM is a separate distribution with its own
  mirrors; its only Docker images are third-party, which is a supply-chain risk for CI.
- The app needs Qt 6.10 (`QQmlEngine::setExternalSingletonInstance`, src/qml_app.cpp). Ubuntu 24.04
  (the runner's own OS) has Qt 6.4 and Debian 13 has 6.8; both fail to compile. Ubuntu 26.04 has
  Qt 6.10.2 and a current libvips.
- The package list is kept in the workflow; the PKGBUILD stays the list for Arch only. A new
  dependency goes in both.

## Not covered

- GPU: runners have none, so tests that need Vulkan or CUDA are not run on either architecture.
- Apple Silicon Linux (Asahi) uses 16 KB pages. Nothing in CI checks that; it needs a real Asahi machine.
