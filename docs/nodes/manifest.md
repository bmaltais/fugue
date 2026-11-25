# Manifest reference

Two files describe a pack: `pack.json` at its root and one `node.json` in each node's folder.
The full tables are spec [6.1](../spec.md#61-nodejson) to [6.4](../spec.md#64-packjson); this is
what you need to write one.

## `pack.json`

```json
{
  "manifest": 1,
  "id": "io.github.alice.tools",
  "version": "1.2.0",
  "name": "Alice's tools",
  "license": "MIT",
  "source": "https://github.com/alice/fugue-tools",
  "nodes": ["sharpen", "denoise"]
}
```

- `manifest`, `id`, `version`, `name`, `nodes` are required. The id is lowercase reverse-DNS, and
  the folder holding the pack is named after it (`io.github.alice.tools/`).
- `nodes` lists subfolder names, each holding a `node.json`.
- `types` declares port types ([types](types.md)); `settings` declares secrets the pack's nodes read (`[{"name": "token_1", "type": "text", "secret": true, "origin": "required", "label": "Token 1"}]`; `"origin": "required"` makes the person say which address the key may be sent to; the person sets the values under Settings > Packs or with `fugue pack secret set … --origin`, and a node's program gets them in the request's `secrets` object, see [protocol](protocol.md)); `setup` is `{"command": ["uv", "sync"], "description": "…"}`, a command that prepares the pack's
  own environment: it runs in the pack's folder, only after the person has seen the exact
  command and said yes. `catalog/*.json` in the pack lists models to download for its resource
  types, each with its licence shown first ([types](types.md#resource-types-models)).

## `node.json`

```json
{
  "manifest": 1,
  "id": "io.github.alice.tools/sharpen",
  "version": "1.2.0",
  "name": "Sharpen",
  "description": "Unsharp mask.",
  "category": "Image/Filter",
  "runtime": {"kind": "command", "entry": "./sharpen.sh"},
  "inputs": [
    {"name": "image", "type": "image"},
    {"name": "amount", "type": "number", "default": 1.0, "min": 0, "max": 5, "step": 0.1, "widget": "slider"}
  ],
  "outputs": [{"name": "image", "type": "image"}],
  "needs": {"memory_mb": 512}
}
```

| Field | Meaning |
| --- | --- |
| `id` | `<pack id>/<name>`; the name matches the folder. |
| `version` | Semver. A workflow remembers the version it was made with. |
| `category` | Where it sits in the node menu; `/` makes levels. |
| `description`, `keywords`, `aliases` | Search. `aliases` are old ids this node answers to after a rename. |
| `sink` | The node is an end of a graph (it saves or shows something). Default false. |
| `deterministic` | Default true: the same inputs give the same outputs, so a result is cached. Set false when the answer changes (a clock, a counter, the web); Fugue then runs it every time. |
| `runtime` | `{"kind": "command", "entry": "./run.sh"}` plus `mode`, `idle_timeout_s`, `timeout_s` ([persistent](persistent.md), [protocol](protocol.md)). |
| `inputs`, `outputs` | Lists; may be empty. |
| `needs` | [Resources and the GPU](resources.md). |

A manifest that does not validate disables that node, with the reason shown in Settings, Packs
and in `fugue pack list`. It never stops Fugue.

### Inputs

| Field | Meaning |
| --- | --- |
| `name` | Lowercase letters, digits and `_`, starting with a letter. Unique in the node. |
| `type` | A [port type](types.md). |
| `label`, `description` | Shown on the node face and in tooltips. |
| `default` | For `text`, `number` and `boolean`. An input with no default and not `optional` must be given. |
| `optional` | May be unset or unwired; then it is absent from the request. |
| `list`, `min_items`, `max_items` | A list input and its size limits. |
| `min`, `max`, `step`, `integer` | Number limits. A value outside them is `value_out_of_range`. |
| `choices` | Allowed values of a `text` input; it becomes a choice. |
| `widget` | One of the set below. |
| `section`, `advanced` | Group inputs on the node face; `advanced` ones start folded. |
| `over` | With widget `mask` or `paint`: the name of the `image` input of this node it is painted over. |
| `role` | `"mask"` on an `image` input that takes a mask (a one-channel image, 1 is the part to change). Only marks the port; an image still connects to it. Any other value is an error. |

Widgets come from a fixed set: no pack ships UI code, so every node looks alike.

| `widget` | For |
| --- | --- |
| `spin` (default), `slider` | number |
| `line` (default), `multiline` | text |
| `toggle` | boolean |
| `choice` (default when `choices` is set) | text |
| `file`, `folder` | text: opens the file picker; the value is a path |
| `color` | text: `#rrggbb` or `#rrggbbaa` |
| `mask` | text: a painted mask, the path of a gray PNG in the inputs folder; `over` names the `image` input it is painted over |
| `paint` | text: a painted canvas with layers, the path of an `.ora` file in the inputs folder; `over` names the `image` input it is painted over (the paint window edits it, spec 14.4) |
| `seed` | number: fixed, randomize or increment |

Outputs have `name`, `type`, `label`, `description`, `list`, `optional` and `role` (`"mask"` on an `image` output that holds a mask).

Unknown fields are a warning, not an error. `ui` must be absent.
