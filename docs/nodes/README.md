# Writing nodes for Fugue

For people who add nodes. A node is a program in any language that talks JSON over stdin and
stdout. The pages, in the order to read them:

1. [Quickstart](quickstart.md): a shell-script node in five minutes.
2. [Manifest reference](manifest.md): `node.json` and `pack.json`.
3. [Command protocol](protocol.md): what the program reads and writes.
4. [Packs and installing them](packs.md), with trust.
5. [Persistent mode](persistent.md): keep a program, and what it loaded, between runs.
6. [Resources and the GPU](resources.md): the `needs` block.
7. [Types and handles](types.md): media, resource and data values.
8. [Testing a node](testing.md): `fugue node test`.
9. [Publishing a pack](publishing.md).

A working pack to read and copy is [examples/packs/io.github.bmaltais.fugue-example](../../examples/packs/io.github.bmaltais.fugue-example):
`shout` is one-shot, `counter` is persistent. The tests run both.

The exact rules are in the [spec](../spec.md) (sections 6 and 7); these pages are the short way in.
The native plugin guide waits until the C interface is frozen (spec 6.9).

**Fugue enforces no sandbox.** A trusted pack's node is a program running as you, with your files
and your network. Install packs you would run in a terminal.
