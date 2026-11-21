# aarch64 CI

`.github/workflows/test.yml` has two jobs: `test` (x86_64, Arch container) and `test-aarch64`
(GitHub's free `ubuntu-24.04-arm` runner). Both run `bin/build` and `bin/test` offscreen, with ccache.

## Why Debian 13 on the ARM job

- Arch has no official arm64 container. Arch Linux ARM is a separate distribution with its own
  mirrors; its only Docker images are third-party, which is a supply-chain risk for CI.
- Ubuntu 24.04 (the runner's own OS) ships Qt 6.4. Debian 13 (trixie) ships Qt 6.8 and libvips 8.16,
  close to what Arch ships and what the app is written against.
- The package list is kept in the workflow; the PKGBUILD stays the list for Arch only. A new
  dependency goes in both.

## Not covered

- GPU: runners have none, so tests that need Vulkan or CUDA are not run on either architecture.
- Apple Silicon Linux (Asahi) uses 16 KB pages. Nothing in CI checks that; it needs a real Asahi machine.
