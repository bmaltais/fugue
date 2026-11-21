# Command protocol reference

A command node is a program that speaks JSON over stdio. Each message is one JSON object on one
line, in UTF-8. Spec [7](../spec.md#7-command-node-protocol) is the full text.

## Process

- Working directory: the node's own folder inside the run folder; also in `FUGUE_WORK_DIR`.
- Environment: `FUGUE_PROTOCOL=1`, `FUGUE_NODE_DIR` (the folder holding `node.json`),
  `FUGUE_WORK_DIR`.
- stdin and stdout carry messages. **stderr is your log**: the last 20 lines go into the message of
  a failure.
- The program runs in its own process group, so cancelling reaches what a shell script started.

## One-shot (the default)

Fugue starts the program for each execution, writes one request, closes stdin.

```json
{"protocol": 1, "op": "run", "id": "r1:sharpen",
 "node": {"type": "io.github.alice.tools/sharpen", "version": "1.2.0"},
 "inputs": {"image": {"type": "image", "path": "/…/blobs/3f9a….png", "hash": "sha256:…",
                      "width": 1024, "height": 1024, "channels": 3, "format": "png"},
            "amount": 1.5},
 "work_dir": "/…/runs/r1/nodes/sharpen"}
```

- `text`, `number` and `boolean` inputs are JSON values; list inputs are arrays.
- Media inputs are [handles](types.md) with a `path` you can read. Do not write to input files:
  they are shared with the result cache.
- An optional input nobody set is absent. One with a `default` is always there.

The program answers with zero or more progress lines and one result or error:

```jsonl
{"op": "progress", "value": 0.4, "message": "pass 2 of 5"}
{"op": "result", "outputs": {"image": {"path": "out.png"}}}
```

```json
{"op": "error", "code": "bad_input", "message": "image is smaller than the kernel"}
```

- `"op": "result"` may be left out: the smallest node is one `echo` of `{"outputs": {…}}`.
- A media or data output is a file you wrote **inside `work_dir`**, returned as `{"path": …}`
  (relative paths are taken from `work_dir`). A path outside it is an error. Fugue reads the type,
  size and hash itself.
- Other outputs are plain JSON values. An optional output may be left out.
- `progress.value` is 0 to 1.
- `{"op": "preview", "image": {"path": "step.png"}}` shows an intermediate picture on the node. Write
  each preview to a new file; a bad one is skipped (the first of a run is a warning
  `node_bad_preview`).
- A result may carry `"stats": {"peak_memory_mb": …, "peak_vram_mb": …}` so Fugue can refine your estimate.

Exit 0 with a result is success. A nonzero exit, or exit 0 with no result, is a failure
(`node_failed`). A missing required output or a wrong type is `node_bad_output`.

## Cancel and timeouts

One-shot: SIGTERM, then SIGKILL after 5 seconds. Persistent: a `cancel` message, then the kill
after the same grace period ([persistent](persistent.md)). There is no timeout unless
`runtime.timeout_s` is set; exceeding it is `node_timeout`.

## Helpers

Shell, with `jq`:

```bash
request=$(cat)
value=$(jq -r '.inputs.amount' <<<"$request")
progress() { printf '{"op":"progress","value":%s,"message":"%s"}\n' "$1" "$2"; }
fail()     { jq -cn --arg c "$1" --arg m "$2" '{op:"error",code:$c,message:$m}'; exit 1; }
```

Python, with no library:

```python
import json, sys
request = json.load(sys.stdin)
text = request["inputs"]["text"]
print(json.dumps({"outputs": {"text": text.upper()}}))
```

## Error codes to use

Any short code works for your own failure (`bad_input`); `fugue` itself uses `node_failed`,
`node_bad_output`, `node_timeout`, `out_of_memory` (say it when memory ran out: Fugue frees a
loaded model and tries again) and `cancelled` (spec 13.3).
