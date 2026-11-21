# Testing a node

```sh
fugue node test <node folder | pack folder> [--json]
```

It loads the pack (so a bad manifest fails the same way it would in Fugue), starts your program
the way a run would, gives it the inputs of each case, and checks the answer: the protocol, the
outputs against `node.json`, and the values the case expects. Nothing is trusted or installed; the
pack is read from where it is.

Cases are in `test.json` next to `node.json`:

```json
{"cases": [
  {"name": "defaults", "inputs": {}, "expect": {"text": "HELLO!"}},
  {"name": "three marks", "inputs": {"text": "is it", "mark": "?", "times": 3}, "expect": {"text": "IS IT???"}}
]}
```

- `inputs` gives literals by input name. Inputs left out use their default; a required input with
  neither is `missing_input`. A media input is a file name, relative to the node folder:
  `{"image": "samples/cat.png"}`.
- `expect` lists outputs and their exact values. Outputs you leave out are only checked to exist
  and to match their type. Media and data outputs are checked for existence, not content.
- With no `test.json` the node runs once with its defaults.
- All cases of a node run on one program, one after the other. A [persistent](persistent.md) node
  keeps its state between them, as it would in a graph; the example `counter` shows it.
- Resource and data inputs cannot be given in a test.

A pack folder tests every command node in it. The exit code is 0 when every case passed. A failure
is an error whose code is the node's own (`node_failed`, `node_bad_output`, `node_timeout`, …) or
`node_test_failed` when only an expectation was not met; the message names the node and the case.

Run the example pack's tests with
`fugue node test examples/packs/io.github.bmaltais.fugue-example`; Fugue's own test suite does.
