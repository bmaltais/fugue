# Quickstart: a shell-script node in five minutes

You need `fugue` and `jq` (the script below reads JSON with it; any language works, see
[protocol](protocol.md)).

## 1. Make the node

```sh
cd ~/code
fugue node new greeter
```

```
Made io.example.greeter/greeter (io.example.greeter/greeter).
Try it:  fugue node test io.example.greeter/greeter
Install:  fugue pack add io.example.greeter
```

This made a pack, the unit you install (`io.example.greeter/pack.json`), with one node in
its own folder:

```
io.example.greeter/
  pack.json            the pack: id, version, the node folders
  greeter/
    node.json          the node: its inputs, outputs and how it runs
    run.sh             the program
    test.json          sample inputs and the outputs to expect
```

Use `--id io.github.you.tools` to pick the pack's id (lowercase reverse-DNS; the folder is named
after it). Run `fugue node new other` inside the pack folder to add a second node to it.

## 2. Read the program

`greeter/run.sh`:

```bash
request=$(cat)
name=$(printf '%s' "$request" | jq -r '.inputs.name // "world"')
printf '{"op": "progress", "value": 0.5, "message": "greeting"}\n'
jq -cn --arg greeting "Hello, $name!" '{outputs: {greeting: $greeting}}'
```

Fugue starts the program for each run, writes one JSON request to its stdin, and reads JSON
lines from its stdout: any number of `progress` lines, then one result. Whatever it writes to
stderr is its log.

## 3. Test it

```sh
fugue node test io.example.greeter/greeter
```

```
ok    io.example.greeter/greeter  greets (0.01 s)
ok    io.example.greeter/greeter  default input (0.01 s)
```

Change a word in `run.sh` and run it again: the failing case names the output and what it
expected.

## 4. Use it

```sh
fugue pack add io.example.greeter
```

It shows the pack, what its nodes declare, and asks. Then `fugue pack trust io.example.greeter`
(or allow it the first time a node runs). The node is now in node search in the window
(**Greeter**, category Example) and in `fugue node list`.

While you work, you can skip the copy: add your folder's parent as an extra pack path in Settings,
Packs. Fugue reads the manifests again when the folder changes ([packs](packs.md)).

## Next

Give the node an image input, a number with a slider, a choice: [manifest](manifest.md). Keep a
model loaded between runs: [persistent mode](persistent.md).
