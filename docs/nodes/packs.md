# Packs and installing them

A pack is a folder (or git repository) with `pack.json` at its root and one subfolder per node.
The folder is named after the pack's id.

## Where packs live

`~/.local/share/fugue/packs/<pack id>/`, plus any extra folders listed in Settings, Packs. The
built-in nodes are the pack `fugue`. The folder `local` of the first path is your group library.

At startup Fugue reads only manifests; programs load on first use. When you install, update or
remove a pack, or a pack folder changes on disk, every open window reads the manifests again
after half a second. Open workflows follow: a placeholder whose type is now installed becomes the
node, and a node whose pack went becomes a placeholder that keeps its inputs and links.

Two things to know while you develop:

- A command node's `node.json` and its entry script are **sealed when loaded**. If you edit them
  while Fugue is open, the node stops until Fugue reads the pack again (a change on disk does
  this by itself, within a moment, for a folder under the packs path or an extra path).
- A persistent program started from older files is stopped and started again after a reload.

## Installing

```sh
fugue pack add ./io.github.alice.tools                  # copies the folder
fugue pack add https://github.com/alice/fugue-tools --ref v1.2.0
fugue pack list
fugue pack update io.github.alice.tools [--ref v1.3.0]
fugue pack trust io.github.alice.tools
fugue pack remove io.github.alice.tools
```

- A git address (`https://`, `ssh://`, `git://`, `file://`, `user@host:path`) is cloned and **must
  be pinned** with `--ref <tag or commit>`.
- A pack with the same id at another major version goes beside the first, to `packs/<id>@<major>`.
  Workflows made with the older major keep running on it. `update`, `remove` and `trust` take
  that folder name (`fugue pack trust io.github.alice.tools@2`); an update stays in its major.
  Adding a major that is already installed is refused.
- The pack goes to `packs/<id>` with a `.fugue-source.json` saying where it came from, so
  `update` can fetch it again. A pack copied in by hand has no source and cannot be updated.
- It is checked in a staging folder first and put in place only when it loads and you agreed. A
  failure leaves no half-installed pack.
- Nothing updates by itself, and there is no catalog or signing in v0.1.
- Settings, Packs does the same with the same questions.

All of `add`, `update`, `remove` and `trust` need a person at a terminal (or the window). Without
one they fail with `needs_confirmation` and change nothing; an agent can never install a pack.

## Trust

The first time a node of a pack would run, Fugue shows the pack, its version and source, the
runtime kinds of its nodes and what they declare (network, files outside the run folder, GPU),
and you must allow it. The decision is stored per pack id and version, so an update asks again.
The `fugue` and `local` packs are trusted. From the CLI an untrusted pack is the error
`pack_untrusted`.

**There is no sandbox.** `network`, `files` and `gpu` in `needs` are what you declare, shown in the
prompt; they are not enforced. Declare them honestly, and read other people's packs before you trust them.

## Setup commands

`pack.json` may name a `setup` command, such as `{"command": ["uv", "sync"]}`, which prepares a virtual
environment inside the pack folder (it runs there, and the pack's entry script starts `.venv/bin/python`). Fugue shows the exact command and runs it only after a yes.
There is no shared environment, so packs cannot conflict with one another.
