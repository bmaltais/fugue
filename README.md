# Fugue

Node-based workflows for local AI on Omarchy, in C++ and Qt Quick.

Wire steps together (a prompt, an image, an edit, a clip, a song) and run the graph on the
models [omarchy-local-ai](https://github.com/omacom/omarchy-local-ai) serves, on this machine
or on a paired host. Add your own nodes as a folder with a `node.json` and a program, in any
language.

![Fugue: a text-to-image workflow on the canvas](docs/images/hero.png)

Fugue is at v0.1, a test release. The design is in [docs/spec.md](docs/spec.md) and the
vocabulary in [docs/GLOSSARY.md](docs/GLOSSARY.md).

## A short tour

- **Canvas.** Place nodes, wire their ports, search for a node, undo, work in tabs and save.
  Run with Ctrl+Enter: each node shows its state, progress and results. Settings (Ctrl+,)
  apply live.
- **Templates.** File > Browse templates lists ready-to-run workflows (text to image, image
  edits, prompt enhancing and more) with a preview each; open one and run it. The CLI does the
  same: `fugue template list`, `fugue workflow new <file> --template <id>`.
- **Groups.** Select nodes and make a group (Ctrl+Shift+E): it collapses into one reusable
  node with the inputs and outputs you choose, and can be entered, nested and ungrouped. Frames
  (Ctrl+G) are colored boxes that only organize the canvas.
- **Local and paired-host runs.** The `omarchy` provider serves models from this machine and
  from every host paired through omarchy-local-ai, so one graph can use a model on another
  machine. Heavy generation runs in a separate helper process (stable-diffusion.cpp).
- **Share packages.** File > Export for sharing writes one `.fuguepack`: the workflow, its
  groups, the input images and a preview, with models kept as references (hash, source, size)
  the receiver downloads. From the CLI: `fugue workflow export <file> --share`. The desktop
  opens `.fuguepack` files with Fugue.
- **Agents.** Everything above works with no window and with `--json`, so an agent can build,
  validate, run and inspect workflows. `fugue skill install` teaches it how: one copy in
  `~/.agents/skills/fugue` (Pi and others) linked into Claude Code's skills folder; run it again to
  update, `--dry-run` to see where it writes, `fugue skill uninstall` to remove. It never
  overwrites a file you edited. Agents cannot install packs or start models: a person confirms.
- **Writing nodes.** A node is a program that speaks JSON over stdin and stdout, described by a
  `node.json`. Start with the [five-minute quickstart](docs/nodes/quickstart.md); the rest of
  the guide is in [docs/nodes](docs/nodes/README.md). Fugue enforces no sandbox: install packs
  you would run in a terminal.

## Install

Fugue is packaged for Arch and Omarchy. Download the packages from the
[v0.1.0-test1 release](https://github.com/bmaltais/fugue/releases/tag/v0.1.0-test1) and install:

```sh
sudo pacman -U fugue-*.pkg.tar.zst
```

For generation on an NVIDIA GPU, also install the helper with `fugue-helper-cuda-*.pkg.tar.zst`
(`sudo pacman -U fugue-helper-cuda-*.pkg.tar.zst`). The release includes `SHA256SUMS`.

This is a test release and the packages are unsigned.

## Docs

- [docs/spec.md](docs/spec.md): the design and behavior.
- [docs/nodes](docs/nodes/README.md): writing nodes and packs.
- [docs/GLOSSARY.md](docs/GLOSSARY.md): the vocabulary.
- [docs/ui-ergonomics.md](docs/ui-ergonomics.md): rules window UI follows.
- Work is tracked in the [issues](https://github.com/bmaltais/fugue/issues) by milestone.

## Build

The bundled templates are a submodule ([fugue-workflows](https://github.com/bmaltais/fugue-workflows)):
clone with `--recurse-submodules`, or run `git submodule update --init` once.

```sh
bin/build        # the app and the tests
bin/test         # builds, then runs every test (arguments go to ctest)
bin/build fugue  # only the app; any CMake target works, e.g. bin/build test_menus
```

The app is `build/fugue`.

The generation helper, stable-diffusion.cpp with Fugue's stage patch, builds on its own:

```sh
bin/build-helper cuda    # or vulkan, cpu; into build-helper-<backend>/
```

## License

MIT.
