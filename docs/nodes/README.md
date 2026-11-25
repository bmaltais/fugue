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
10. [HTTP Request](http-request.md): the first-party node for web APIs (webhooks, lookups, uploads) and its `network` permission.

A working pack to read and copy is [examples/packs/io.github.bmaltais.fugue-example](../../examples/packs/io.github.bmaltais.fugue-example):
`shout` is one-shot, `counter` is persistent. The tests run both.
A pack with a Python environment, a model and a download catalog:
[examples/packs/io.github.bmaltais.fugue-remove-background](../../examples/packs/io.github.bmaltais.fugue-remove-background)
(BiRefNet through ONNX Runtime; [masking](../masking.md#remove-a-background)); [examples/packs/io.github.bmaltais.fugue-remove-object](../../examples/packs/io.github.bmaltais.fugue-remove-object)
(LaMa through ONNX Runtime, a node with two inputs and a group; [masking](../masking.md#remove-an-object)); its sibling
[examples/packs/io.github.bmaltais.fugue-segment](../../examples/packs/io.github.bmaltais.fugue-segment) (SAM 2.1 from points or a box, and GroundingDINO to find what words name;
[masking](../masking.md#select-an-object-by-points-or-a-box)) shows a model that is a zip of two files, a persistent node that keeps what it computed between runs, and two small nodes that compose by wiring a text output to a text input. The smallest ONNX Runtime pack to copy is [examples/packs/io.github.bmaltais.fugue-depth](../../examples/packs/io.github.bmaltais.fugue-depth) (Depth Anything V2 Small, one node, one model).

The exact rules are in the [spec](../spec.md) (sections 6 and 7); these pages are the short way in.
The native plugin guide waits until the C interface is frozen (spec 6.9).

**Fugue enforces no sandbox.** A trusted pack's node is a program running as you, with your files
and your network. Install packs you would run in a terminal.
