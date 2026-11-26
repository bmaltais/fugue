# Contributing to Fugue

Thanks for looking. Fugue is a small project and feedback, bug reports and pull requests are all welcome.
You do not need to ask first for a small fix, a doc change, a template or a node.

## Three ways to contribute

1. **A template** (a ready-to-run workflow). These live in the
   [fugue-workflows](https://github.com/bmaltais/fugue-workflows) repo, checked out here as `templates/`. Its
   [README](https://github.com/bmaltais/fugue-workflows#readme) says how to add one and which checks to run
   (`bin/check`, then `bin/validate-all`). You need no C++ and no build of Fugue beyond the binary.
2. **A node or a pack.** A node is a program in any language that speaks JSON over stdin and stdout, described by
   a `node.json`. You do not touch the C++. Start with the [five-minute quickstart](docs/nodes/quickstart.md), then
   [docs/nodes](docs/nodes/README.md); `fugue node test` tests one. A working pack to copy is
   `examples/packs/io.github.bmaltais.fugue-example`.
3. **A change to the app** (C++ and QML). Read on.

Not sure where to start? Issues labelled
[good first issue](https://github.com/bmaltais/fugue/labels/good%20first%20issue) are small and need no GPU.
[docs/GLOSSARY.md](docs/GLOSSARY.md) explains the words (node, pack, group, provider, helper).

## Building

Fugue needs Qt 6.12 and a C++ compiler, so the easy path is Arch Linux or Omarchy. The build dependencies
(from `packaging/PKGBUILD`, without `cuda`), with `ninja`, `lld` and `ccache` to build faster:

```sh
sudo pacman -S --needed base-devel clang cmake ffmpeg git hicolor-icon-theme libarchive libvips pkgconf \
  qt6-base qt6-declarative qt6-imageformats qt6-multimedia qt6-multimedia-ffmpeg qt6-shadertools qt6-wayland \
  shaderc spirv-headers vulkan-headers vulkan-icd-loader ninja lld ccache
```

```sh
git clone --recurse-submodules https://github.com/bmaltais/fugue.git
cd fugue                 # already cloned? run: git submodule update --init
bin/build                # the app and the tests; the app is build/fugue
bin/test                 # builds, then runs every test (arguments go to ctest)
bin/test --changed       # only the tests your change can affect (what to run while you work)
```

The `templates/` submodule must be checked out: with it empty, six tests fail.
`bin/build` and `bin/test` take a machine-wide lock, so a second build waits quietly for the first to finish.

On another distro, the AppImage ([docs/appimage.md](docs/appimage.md)) runs Fugue but does not build it. The Qt
6.12 that CI builds with comes from aqtinstall, in `packaging/appimage/build-appimage.sh`; that is a place to
start, but it is not a supported dev setup. If you are stuck, open an issue.

Generating pictures needs the helper (stable-diffusion.cpp with Fugue's patch) and a model. You do not need
either to work on the UI, the engine or the nodes. To build the helper:

```sh
bin/build-helper cuda    # or vulkan, cpu; into build-helper-<backend>/
```

Run the window offscreen if you only want to check that it starts: `QT_QPA_PLATFORM=offscreen build/fugue`.

## Tests and code rules

[docs/testing.md](docs/testing.md) has the full rules. The short version:

- Test logic, contracts, data handling and a fixed bug's regression. Do not test the framework, trivial
  getters or pixel layout.
- A test file runs in about 5 s and the whole suite takes about a minute. Test at the model or engine level
  without a window when you can; wait on conditions (`QTRY_*`, signals), never on fixed sleeps; never raise a
  timeout to make a test pass. A flaky test is fixed or deleted, never re-run until green.
- Fix every compiler, qmllint and test warning you meet in the files you touch.
- Update the docs in the same change. [docs/spec.md](docs/spec.md) is the spec: behavior it describes is the
  behavior Fugue has, so change it when you change the behavior. A new user-visible string goes in
  `src/i18n/en.json` (the `fugue_tests` check fails if it is missing).
- Window UI (a `.qml` file, a menu, a dialog) follows [docs/ui-ergonomics.md](docs/ui-ergonomics.md); update that
  file in the same change.
- Tests never open a window on your desktop and never write to your real data. They use a scratch home
  (`FUGUE_HOME`).

Run `bin/test` once before you open the pull request.

## Pull requests

- One topic per pull request. Small ones get reviewed quickly.
- For something big (a new node type, a new view, a change to the file format or the spec), open an issue first
  and say what you plan, so we can agree before you spend a weekend on it.
- Say what you verified: which tests you ran, and what you tried by hand. Say what you did not check (for
  example "not tried on a GPU").
- A UI change needs screenshots, light and dark if the colors changed. `bin/look-shot` takes a picture without
  touching your desktop (see [docs/testing.md](docs/testing.md)).
- If you delete a test, say why in the description.
- Run `bin/test` locally before you open the pull request; that is still expected. On a pull request from a
  fork, CI then runs a fast lane on Arch: the x86_64 build and the full test suite, offscreen. The first run
  waits until a maintainer approves it, and takes about 10 to 30 minutes. The heavier builds (CUDA helper
  tarballs, the AppImage, aarch64) do not run for ordinary changes: they run on a release, once a week on main,
  and on a pull request that changes how they are built (a workflow file, `packaging/`, the PKGBUILD), each waiting
  for approval the same way. A pull request that changes `.github/` is reviewed by hand before its runs are approved. The table in [docs/testing.md](docs/testing.md) lists which. The check to watch is `ci-ok`. If it flags something, fix it; ask in the pull request if you
  cannot reproduce it.

## Conduct

Be kind and patient. Assume good faith, criticize the code and not the person, and remember that people here
are mostly doing this for fun. Harassment and personal attacks get a comment removed, and repeat offenders get
blocked.

## License

Fugue is MIT licensed ([LICENSE](LICENSE)). By opening a pull request you agree that your contribution is
released under the same license. Templates and packs you share keep whatever license you give them, but say
which one, and do not include models or pictures you may not redistribute.
