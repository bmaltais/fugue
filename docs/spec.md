# Fugue v0.1 spec

Status: accepted design, nothing built yet. Written 2026-11-18 from the project's design map
(the private notes where each decision was researched and made).
Terms are defined in [GLOSSARY.md](GLOSSARY.md); this document uses them exactly.

How to read it:

- Section numbers are stable. New material is appended; sections are never renumbered, so an
  issue can point at "spec 7.3" and stay right.
- "Must" is a requirement of v0.1. "Later" means designed for but not built in v0.1.
- Every JSON example is illustrative of shape. Field tables are the contract.
- Things still to measure or confirm are collected in [section 17](#17-open-items), and the
  choices first made in this document (not in the design map) are listed in
  [appendix A](#appendix-a-choices-made-in-this-document) so they are easy to overrule.
- [Appendix B](#appendix-b-issue-to-section-map) maps each build issue to its sections.

Contents:

1. [Principles and architecture](#1-principles-and-architecture)
2. [Port types and values](#2-port-types-and-values)
3. [Nodes](#3-nodes)
4. [The workflow document](#4-the-workflow-document)
5. [Groups](#5-groups)
6. [Node manifests and packs](#6-node-manifests-and-packs)
7. [Command-node protocol](#7-command-node-protocol)
8. [Providers](#8-providers)
9. [Execution](#9-execution)
10. [Result cache](#10-result-cache)
11. [Workflow files](#11-workflow-files)
12. [Canvas](#12-canvas)
13. [CLI and agent interface](#13-cli-and-agent-interface)
14. [Built-in nodes](#14-built-in-nodes)
15. [Milestones](#15-milestones)
16. [User docs outline](#16-user-docs-outline)
17. [Open items](#17-open-items)

---

## 1. Principles and architecture

### 1.1 What Fugue is

A node-based workflow app for local AI, in C++ and Qt Quick, built for Omarchy first and
portable by design. Users wire nodes into a graph and run it. The reference is ComfyUI: Fugue
matches what its users expect and is deliberately better where ComfyUI is weak (listed in
[12.12](#1212-better-than-comfyui-on-purpose)).

### 1.2 Principles

1. **Small kernel, independent modules.** Supporting a new model, engine or node never needs a
   recompile of Fugue. Modules are node packs, providers, port types declared by packs, and
   import or export converters. Built-in nodes ship as a pack through the same registry path as
   third-party ones. Manifests are indexed at startup; code loads on first use.
2. **One engine.** The canvas and the CLI run the same headless engine. The engine has no Qt
   Quick dependency and never sees layout or groups, only the compiled graph ([4.8](#48-the-compiled-graph)).
3. **Third-party code stays out of process.** Command nodes, providers and the generation
   helper are separate programs, so a crash or an out-of-memory kill takes one node, not Fugue.
   The native tier ([6.9](#69-native-tier)) is the one deliberate exception.
4. **Everything that crosses a wire is serializable and hashable.** No live objects between
   nodes. This is what makes caching, the CLI, and process isolation possible.
5. **Say what is wrong.** Fugue never quietly does less: a missing model, an invalid link or a
   failed node is reported on the node with how to fix it.
6. **Nothing changes silently.** Pack updates, group updates, model starts and installs are
   explicit and confirmed by a human.

### 1.3 Processes

| Process | What it is |
| --- | --- |
| `fugue` (window) | The canvas, with the engine in-process. |
| `fugue <command>` | The display-less CLI, same binary, same engine ([13](#13-cli-and-agent-interface)). |
| run worker | A detached `fugue` process that executes one run started from the CLI ([9.8](#98-runs-across-processes)). |
| command node | One program per node execution, or one long-lived program in persistent mode ([7](#7-command-node-protocol)). |
| provider module | A program or server that offers capabilities ([8](#8-providers)). |
| model servers | Run by omarchy-local-ai or by the user. Fugue talks to them; it does not own them. |

### 1.4 Files and folders

XDG variables are honored; the defaults are shown.

| Path | Holds |
| --- | --- |
| `~/.config/fugue/settings.json` | Settings. |
| `~/.config/fugue/providers.json` | Provider instances and their keys, mode 0600 ([8.2](#82-provider-instances)). |
| `~/.config/fugue/keybindings.json` | Keymap diff ([12.5](#125-command-registry-and-keymap)). |
| `~/.local/share/fugue/packs/<pack-id>/` | Installed packs ([6.5](#65-where-packs-live-and-how-they-load)). |
| `~/.local/share/fugue/providers/<provider-id>/` | Installed provider modules. |
| `~/.local/share/fugue/models/<type>/` | Fugue's models folder, one subfolder per resource type ([8.9](#89-models-on-disk)). |
| `~/.local/state/fugue/recovery/` | Recovery copies ([11.3](#113-recovery)). |
| `~/.local/state/fugue/trust.json` | Which packs the user has allowed ([6.7](#67-trust)). |
| `~/.local/state/fugue/hashes.json` | Remembered file hashes, keyed by path, size and modification time. |
| `~/.cache/fugue/results/` | The result cache ([10](#10-result-cache)). |
| `~/.cache/fugue/runs/<run-id>/` | Run folders ([9.7](#97-run-folder)). |
| `~/Documents/Fugue/` | Suggested place for workflows. Not required. |
| `~/Pictures/Fugue/` | Default output folder for Save nodes. A setting. |

### 1.5 Hashes and canonical JSON

- All content hashes are SHA-256, written `sha256:<64 hex digits>`.
- When JSON is hashed it is first put in the canonical form of RFC 8785 (JCS): keys sorted, no
  insignificant whitespace, shortest round-trip numbers, UTF-8.
- A file's hash is computed once and remembered by path, size and modification time.

---

## 2. Port types and values

### 2.1 Built-in port types

| Type id | Value | In JSON |
| --- | --- | --- |
| `text` | Unicode text | string |
| `number` | One number type. Integer or decimal is a constraint on the input, not a separate type. | number |
| `boolean` | true or false | boolean |
| `image` | A media handle ([2.4](#24-media-handles)) | object |
| `video` | A media handle, always file-backed | object |
| `audio` | A media handle | object |

Integers must fit in ±2^53 so they survive JSON. Seeds are integers in 0 to 2^53 − 1.

A choice is a `text` input with a list of allowed values. A mask is an `image` with one channel;
there is no mask port type in v0.1.

### 2.2 Declared types

A pack can declare more port types in its `pack.json` ([6.4](#64-packjson)). A declared type has
a namespaced id and an integer version, written `namespace/name@N` (`@1` may be omitted), and one
of two kinds:

- **resource**: values are resource handles ([2.5](#25-resource-handles)) naming something
  loadable, such as a model, a LoRA or a ControlNet.
- **data**: values are files a node produced, such as a latent or conditioning
  ([2.6](#26-data-values-latents-and-conditioning)).

The type system is nominal: the id and version are the type. There is no structural matching.

### 2.3 Compatibility

- An output connects to an input only when both have the same type id and version. There is no
  "accept anything" type. The one exception: inputs of built-in `fugue/` utility nodes (Preview)
  may be declared `any`. Packs cannot declare `any`.
- No implicit conversion in v0.1 (not even number to text). Later, the canvas may offer to insert
  a converter node where a safe one exists.
- **Lists** are a modifier on any type, not separate types. An input or output is declared
  `list: true`. A single value linked into a list input becomes a one-item list. A list input may
  take several links and literals. A list output cannot feed a non-list input: v0.1 has no
  implicit iteration (batches and loops are not in v0.1).
- The canvas checks compatibility during the drag and says why a target is invalid.

### 2.4 Media handles

An `image`, `video` or `audio` port carries a media handle. Inside the engine a handle can stand
for a file, a decoded form, or both; the engine converts on demand and writes a file only when
something needs one.

| Type | Strategy |
| --- | --- |
| image | A file, a decoded libvips image, or both. Pixel nodes and native nodes get memory; provider and command nodes get a file. |
| video | Always file-backed and never fully decoded. Frames are streamed through ffmpeg. Trim and crop are kept as pending edits on the handle and applied when a file or frames are needed. |
| audio | A file. An in-memory sample buffer exists only while a node needs samples. |

Serialized form (in command-node messages, cache records and events):

```json
{"type": "image", "path": "/…/runs/r1/nodes/gen/image.png", "hash": "sha256:…",
 "width": 1024, "height": 1024, "channels": 3, "format": "png"}
```

| Field | Types | Meaning |
| --- | --- | --- |
| `type` | all | `image`, `video` or `audio`. |
| `path` | all | Absolute path of the file. |
| `hash` | all | Content hash of the file. |
| `format` | all | Container or codec name. |
| `width`, `height` | image, video | Pixels. |
| `channels` | image, audio | Channel count. |
| `fps`, `frames`, `duration`, `has_audio` | video | As read from the file. |
| `sample_rate`, `duration` | audio | As read from the file. |
| `edits` | video | Pending `trim` (`start`, `end` in seconds) and `crop` (`x`, `y`, `width`, `height`). |

Images a node produces are stored losslessly (PNG, 8 or 16 bit) unless the node declares
otherwise. Every node result is written to the run folder as soon as the node finishes
([9.7](#97-run-folder)).

### 2.5 Resource handles

A resource handle is data that names a loadable resource. It is never the loaded weights.

```json
{"resource": "fugue/lora", "hash": "sha256:…", "name": "detail-tweaker.safetensors"}
```

| Field | Meaning |
| --- | --- |
| `resource` | The declared type id. |
| `hash` | Content hash of the model file. This is the identity. Absent only for a model a provider serves by name. |
| `name` | File name or the provider's model name. Display only when `hash` is present. |
| `derived` | A derived resource described as data, for example a base model with LoRAs applied: `{"base": <handle>, "apply": [{"lora": <handle>, "strength": 0.8}]}`. |
| `needs` | Optional hints (GPU memory in MB) the scheduler may use. |

Rules:

- A handle never contains an absolute path. The engine finds the file by hash in the model
  folders ([8.9](#89-models-on-disk)).
- A handle's identity for caching is the hash of its canonical JSON without display-only fields.
- Resolving a handle (finding the file or the server) is separate from making it resident
  (loaded). The scheduler decides when a model is loaded and keeps it across nodes
  ([9.3](#93-residency)).
- An unwired input of a resource type shows a model picker listing the models of that type found
  on disk and offered by providers. The chosen handle is stored as the input's literal.

### 2.6 Data values: latents and conditioning

Values of a data type are files in the run folder referenced by a handle with a content hash:

```json
{"data": "fugue/latent", "path": "…/latent.safetensors", "hash": "sha256:…",
 "meta": {"width": 1024, "height": 1024, "channels": 16, "family": "flux2"}}
```

- A latent is a tensor file plus a small descriptor.
- Conditioning is a recipe, not a tensor bag: tensor files plus a JSON of named modifiers (text
  and encoder, ControlNet references with strengths, masks, areas, timestep ranges) that the
  sampler interprets.
- Because each value has a content hash, changing only the sampler does not re-encode the prompt.

The concrete data and resource types for generation (`fugue/latent`, `fugue/conditioning`,
`fugue/diffusion-model`, `fugue/text-encoder`, `fugue/vae`, `fugue/lora`, `fugue/controlnet`,
`fugue/upscale-model`) are declared by the built-in component pack in M5 and are provisional
until then. The type system above is fixed now so they need no redesign.

---

## 3. Nodes

### 3.1 Node types

A node is an instance of a node type. A node type has:

- an id `namespace/name`. Built-ins are `fugue/...`. A pack's node types are `<pack id>/<name>`,
  for example `io.github.alice.tools/sharpen`. Namespaces are lowercase reverse-DNS or a single
  reserved word (`fugue`, `local`); names match `[a-z][a-z0-9-]*`.
- a semver version ([3.6](#36-versions)).
- a list of named, typed inputs and a list of named, typed outputs.
- flags: `sink`, `deterministic`.
- what it needs to run ([6.3](#63-the-needs-block)) and how it runs ([6.1](#61-nodejson)).

The manifest of every node type is available as data (`fugue node describe`), for the canvas, the CLI and
agents alike.

### 3.2 Inputs

There is one list of named inputs per node type and no separate "parameter" kind.

- Each input has a type. `text`, `number`, `boolean` and choice inputs carry a default and
  constraints and show an editable control when unwired; the control is grayed when wired.
- Media and data inputs are wire-only. Resource inputs are a wire or a picked handle ([2.5](#25-resource-handles)).
- An input is required unless it has a default or is marked `optional`.
- In a workflow every input value is stored by name. Nothing is positional.

### 3.3 Outputs

- Outputs are named and typed, stored and wired by name. One output can feed any number of inputs.
- Outputs also carry the values the node actually used, as ordinary outputs: the seed it ran
  with, the final size, the model it resolved.
- An output can be declared `optional` and come back empty. A node whose required input is
  linked to an empty output is skipped, not failed; an optional input treats it as absent.
- Outputs have no controls. After a run the canvas shows each output's last value on its port.

### 3.4 What runs

- A node type can be marked a **sink** (Save, Preview, Export).
- A normal run executes every sink and everything upstream of one.
- Any node can be run alone ("run to here"): that node and everything it needs, with no Preview
  added. Sinks elsewhere do not run.
- A node with no sink downstream does not run, and the canvas marks it "not part of any output".

### 3.5 Bypass and mute

- **Bypass**: the node does not run and each output takes the value of the first input, in
  declaration order, with the same type. An output with no matching input is empty.
- **Mute**: the node does not run and every node that depends on it is skipped.

### 3.6 Versions

Node types use semver:

- patch: no change to inputs, outputs or results.
- minor: a new optional input with a default, or a new output.
- major: a renamed or removed input or output, a changed type, or changed results for the same
  inputs.

A workflow records each node's type id and the version it was made with. On open:

| Installed versus recorded | Result |
| --- | --- |
| same major, same or newer minor | Loads normally. |
| same major, older minor | Loads; inputs the installed version lacks are kept and flagged. |
| different major | The node opens in a "needs review" state that shows what changed. It does not run until the user accepts; compiling reports `node_version_mismatch`. Accepting rewrites the node's `version` to the installed one, which is also how an agent accepts: by editing the file. |
| type missing | A placeholder that keeps its inputs and links, so saving loses nothing. |

A node type can declare `aliases`: old ids it answers to, so a rename does not break saved
workflows. An alias applies only when no installed type has that id.

The version is part of the cache key ([10.1](#101-keys)), so a new version never reuses old results.

---

## 4. The workflow document

### 4.1 Top level

A workflow is one UTF-8 JSON document, extension `.fugue`.

```json
{
  "fugue": 1,
  "graph": {"nodes": {}},
  "groups": {},
  "layout": {},
  "requires": {}
}
```

| Key | Required | Holds |
| --- | --- | --- |
| `fugue` | yes | Document format version, an integer. This spec defines 1. |
| `graph` | yes | The nodes, their inputs and links. No positions, no UI state. |
| `groups` | no | Group definitions used by the graph ([4.4](#44-groups-section)). |
| `layout` | no | Positions and everything visual. Removing it never changes what the graph does. |
| `requires` | no | What the workflow needs. Written on save, informational on load. |
| `run` | no | Only in a copy embedded in an output ([11.7](#117-embedding-in-outputs)): the values used. Never affects a run. |

A reader must reject a document whose `fugue` is greater than it supports, with the error
`unsupported_format_version`. Unknown keys are preserved on a round trip and otherwise ignored.

### 4.2 `graph`

`graph.nodes` is an object keyed by node id. `graph.provider` is optional: the name of a
provider instance pinned for every node of the workflow that does not pin its own
([8.3](#83-selection)).

```json
{
"graph": {
  "nodes": {
    "prompt": {"type": "fugue/text", "version": "1.0.0",
               "inputs": {"text": "a red fox in fresh snow"}},
    "gen":    {"type": "fugue/generate-image", "version": "1.0.0",
               "inputs": {"prompt": {"from": "prompt.text"},
                          "width": 1024, "height": 1024,
                          "seed": {"auto": "random"}}},
    "save":   {"type": "fugue/save-image", "version": "1.0.0",
               "inputs": {"image": {"from": "gen.image"}, "name": "fox"}}
  }
}
}
```

**Node ids** match `[a-z][a-z0-9_-]{0,31}`, are unique within their graph and never change once
made. Fugue generates six random characters; a hand-written or agent-written id such as `prompt`
is equally valid. Ids cannot contain `.` or `/`, which are separators.

**Node fields**

| Field | Required | Meaning |
| --- | --- | --- |
| `type` | yes | Node type id, or a group id from `groups`. |
| `version` | yes | The version the node was made with. |
| `inputs` | no | Input values by name. An absent input uses its default. |
| `mode` | no | `"bypass"` or `"mute"`. Absent means normal. |
| `provider` | no | Name of a provider instance to pin ([8.3](#83-selection)). Absent means the workflow's pin, or `auto`. |
| `frozen` | no | A result-cache key the node's output is frozen to ([10.4](#104-freeze)). |

**Input values**

| Form | Meaning |
| --- | --- |
| string, number, boolean | A literal. |
| `{"from": "<node id>.<output name>"}` | A link. |
| `{"auto": "random"}` | A value the engine resolves at the start of each run. Only for inputs with the seed widget. |
| `{"resource": …}` | A resource handle literal ([2.5](#25-resource-handles)). |
| array | A list, for `list` inputs only. Each item is any form above. |

An object is a link if and only if it has the key `from`. `null` is not a value; leave the input out.

### 4.3 `requires`

```json
{
"requires": {
  "fugue": "0.1.0",
  "nodes": {"fugue/text": "1.0.0", "io.github.alice.tools/sharpen": "1.2.0"},
  "packs": {"io.github.alice.tools": {"version": "1.2.0",
                                      "source": "https://github.com/alice/fugue-tools"}},
  "models": [{"resource": "fugue/lora", "hash": "sha256:…",
              "name": "detail-tweaker.safetensors", "source": "https://…"}]
}
}
```

- `fugue`: the app version that wrote the file.
- `nodes`: every node type used, including inside groups, with its version.
- `packs`: the packs those types come from, with an optional `source` Fugue can offer to install
  from (a prompt; nothing installs silently).
- `models`: every resource handle with a hash that appears as a literal, with an optional
  `source`. On open Fugue checks the model folders and reports what is missing without blocking.

### 4.4 `groups` section

`groups` is an object keyed by group id. Each value is one group definition; a workflow holds
exactly one definition, at one version, per group id.

```json
{
"groups": {
  "local/two-pass": {
    "version": "1.0.0",
    "name": "Two-pass generate",
    "inputs": [
      {"name": "prompt", "targets": ["first.prompt", "second.prompt"]},
      {"name": "seed", "targets": ["first.seed"], "label": "Seed"},
      {"name": "steps", "targets": ["second.steps"], "section": "Advanced",
       "min": 1, "max": 12}
    ],
    "outputs": [{"name": "image", "from": "second.image"}],
    "nodes": {
      "first":  {"type": "fugue/generate-image", "version": "1.0.0", "inputs": {}},
      "second": {"type": "fugue/generate-image", "version": "1.0.0",
                 "inputs": {"image": {"from": "first.image"}, "strength": 0.4}}
    }
  }
}
}
```

| Field | Meaning |
| --- | --- |
| `version`, `name`, `description`, `category` | As for any node type. |
| `inputs[].name` | The exposed input's name on the group's face. |
| `inputs[].targets` | One or more `<inner node id>.<input name>` it drives. All targets must have the same type. |
| `inputs[].label`, `section`, `default`, `min`, `max`, `choices` | Display name, section, and a default or narrowed constraints. Everything else is taken from the first target. |
| `outputs[].name`, `outputs[].from` | An output on the face and the inner `<node id>.<output name>` it carries. |
| `nodes` | The inner graph, same shape as `graph.nodes`. Inner ids are local to the definition. |

An instance of a group is an ordinary entry in `graph.nodes` whose `type` is the group id and
whose `inputs` are the exposed inputs. An exposed input the instance does not set falls back to
the group's `default`, then to the inner input's own value. Behavior is specified in
[section 5](#5-groups).

### 4.5 `layout`

```json
{
"layout": {
  "nodes": {"gen": {"pos": [420, 160], "size": [280, 0], "collapsed": false,
                    "color": "accent", "title": "Fox"}},
  "frames": [{"id": "f1", "title": "Inputs", "rect": [0, 0, 380, 300],
              "color": "muted", "nodes": ["prompt"]}],
  "links": {"save.image": {"points": [[700, 200]]}},
  "comments": [{"id": "c1", "text": "Try 8 steps", "pos": [0, 320], "size": [240, 80]}],
  "view": {"center": [400, 200], "zoom": 1.0},
  "groups": {"local/two-pass": {"nodes": {}, "frames": [], "links": {}}}
}
}
```

- `nodes`: per node id: position, size (0 means automatic), collapsed, color (a theme role, not
  a hex value), a display title, pinned.
- `frames`: Frames ([5.1](#51-frame)).
- `links`: reroute points, keyed by the target `<node id>.<input name>`, with `#<index>` added
  for an item of a list input (`merge.images#1`).
- `comments`: free text notes on the canvas.
- `view`: the last viewport.
- `groups`: the same structure for the inside of each group definition.

Layout holds nothing that changes what a run does.

### 4.6 Serialization rules

So that git diffs stay readable and any tool can reproduce the file:

- Object keys are sorted (the same order as canonical JSON, [1.5](#15-hashes-and-canonical-json)).
- Two-space indent, one key per line, a trailing newline, UTF-8 without a byte order mark.
- Integers are written without a fraction or exponent.
- Saving twice with no change produces identical bytes.

### 4.7 Validation

Loading validates the document shape. Compiling validates meaning ([9.1](#91-compile)). Both
report structured errors ([13.3](#133-errors)); neither stops at the first error.

### 4.8 The compiled graph

At run time the engine compiles `graph` and `groups` into a flat form. The engine and the CLI
only ever execute this form, and `fugue compile` prints it.

```json
{
  "fugue-compiled": 1,
  "source": "sha256:…",
  "nodes": {
    "prompt": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": "…"}},
    "g7/first": {"type": "fugue/generate-image", "version": "1.0.0",
                 "inputs": {"prompt": {"from": "prompt.text"}}},
    "g7/second": {"type": "fugue/generate-image", "version": "1.0.0",
                  "inputs": {"prompt": {"from": "prompt.text"},
                             "image": {"from": "g7/first.image"}, "strength": 0.4}},
    "save": {"type": "fugue/save-image", "version": "1.0.0", "sink": true,
             "inputs": {"image": {"from": "g7/second.image"}}}
  },
  "skipped": {"old-branch": "muted"}
}
```

| Field | Meaning |
| --- | --- |
| `fugue-compiled` | Format version of the compiled graph. This spec defines 1. |
| `source` | Hash of the canonical JSON of the workflow's `graph` and `groups`. |
| `nodes` | Keyed by path id. Each has `type` and `version` (never a group), `inputs` (literals, `auto` values, handles, and links by path id), and, when set, `sink`, `provider` (the node's pin or else the workflow's) and `frozen`. `mode` is resolved and never appears. |
| `skipped` | Path ids left out, each with the reason: `muted`, `upstream_muted`, `upstream_empty`, `not_part_of_output`. |
| `target` | For "run to here": the path id the run stops at. |

- Groups are flattened. An inner node gets a **path id**: the instance ids from the outside in,
  joined with `/`, for example `g7/first`, or `g7/inner3/s2` for a nested group.
- Bypassed nodes are removed and their links rewired ([3.5](#35-bypass-and-mute)). A link to a
  bypassed output that has no matching input is removed: an optional input becomes unset, and a
  node with a required input left that way is dropped as `upstream_empty`. Muted nodes and their
  dependents are dropped and listed in `skipped` with the reason.
- Sinks inside a group are dropped ([5.5](#55-sinks-inside-a-group)).
- Nodes with no sink downstream are dropped, unless the run is "run to here".
- `auto` values are kept; the engine resolves them when the run starts.
- A compiled graph can be run directly: `fugue run compiled.json`. `--set`, `--to` and
  `--provider` change a compile, so with a compiled graph they are `usage` errors.

---

## 5. Groups

Two separate things, as in every mature node tool.

### 5.1 Frame

A purely visual box with a color and a title. It moves and copies as one. It lives only in
`layout` and has no effect on a run. Made with Ctrl+G.

### 5.2 Group

A reusable unit made from selected nodes that collapses into one node. A group is a node type
with an id and a version, and it appears in node search.

- **Make group** (Ctrl+Shift+E): Fugue creates the group's inputs and outputs from every link
  that crosses the selection boundary. The user can rename, reorder and add to them.
- **Enter and leave**: Tab on a selected group, or a double-click on its body, enters it, with
  a breadcrumb trail. Escape or the breadcrumb leaves it.
- **Nesting** is allowed. **Recursion** is forbidden and checked when a group is made or
  edited, directly or through another group (error `group_recursion`).
- **Ungroup** (Ctrl+Shift+G) puts the inner nodes back with their links and values.

### 5.3 Definitions, copies and the library

- Within one workflow every instance of a group references one definition, so editing it
  updates all instances.
- **Make unique** detaches an instance into its own definition with a new id.
- A group made in a workflow has the id `local/<name>`. Saving it to the library writes it to
  the reserved `local` pack (`~/.local/share/fugue/packs/local/<name>/`), where it is a node type
  like any other. A pack can ship groups too ([6.1](#61-nodejson), `runtime.kind: "group"`).
- A workflow always embeds the exact definition and version it used, so opening it never depends
  on the user's library.
- Updates are explicit. When the library has a newer version the group's instances show "update
  available"; updating shows what changed under the rules of [3.6](#36-versions) and replaces the
  workflow's definition, so every instance moves together. Nothing changes silently.

### 5.4 Exposed inputs and outputs

- Any inner input can be exposed with one action. An exposed input keeps the inner input's type,
  default and constraints, and can get a display name and a narrowed range.
- One exposed input can drive several inner inputs (one seed for two samplers).
- Exposed inputs can be reordered and put in sections. A section named "Advanced" is collapsed
  by default.
- Inner inputs that are not exposed keep their values and stay visible inside the group.
- Outputs work the same way, including "values used" outputs.

### 5.5 Sinks inside a group

A group never starts a run and never writes a file or shows a preview from inside when it runs
as a node. Whatever is wanted from it is exposed as an output and the sink sits outside.

- While the user has entered a group, its inner sinks work; they go quiet on leaving.
- Making a group from a selection that contains a sink, Fugue offers to leave the sink outside
  and wire it to the new group's output.

### 5.6 Groups at run time

- The compiler flattens groups ([4.8](#48-the-compiled-graph)); the engine never sees one.
- Each inner node is cached on its own, so changing one exposed value re-runs only the inner
  nodes it affects.
- A group's face shows aggregate progress. An error on an inner node is reported with its path id
  and highlights both the group and the inner node.

---

## 6. Node manifests and packs

### 6.1 `node.json`

One manifest describes every node type, whatever its tier. The tier only decides how it runs;
search, versions, groups, caching, the canvas and workflows treat all tiers alike.

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
    {"name": "amount", "type": "number", "default": 1.0, "min": 0, "max": 5,
     "step": 0.1, "widget": "slider"}
  ],
  "outputs": [{"name": "image", "type": "image"}],
  "needs": {"memory_mb": 512}
}
```

**Top level**

| Field | Required | Meaning |
| --- | --- | --- |
| `manifest` | yes | Manifest format version. This spec defines 1. |
| `id` | yes | `<pack id>/<name>`. Must start with the pack's id. |
| `version` | yes | Semver ([3.6](#36-versions)). |
| `name` | yes | Display name. |
| `description`, `category`, `keywords` | no | For search. `category` uses `/` for levels. |
| `aliases` | no | Old ids this type answers to. |
| `sink` | no | Default false. |
| `deterministic` | no | Default true. False adds the run id to the cache key. |
| `runtime` | yes | See below. |
| `inputs`, `outputs` | yes | Lists; may be empty. |
| `needs` | no | [6.3](#63-the-needs-block). |
| `capabilities` | no | Capabilities the node asks a provider for ([8.1](#81-capabilities)). |
| `ui` | no | Reserved. Must be absent in v0.1. |

**`runtime`**

| `kind` | Other fields | Status in v0.1 |
| --- | --- | --- |
| `command` | `entry` (program path relative to the node folder), `mode` (`oneshot` default, or `persistent`), `idle_timeout_s` (default 300), `timeout_s` (optional) | Ships in M3. |
| `group` | `definition` (a file holding a group definition, [4.4](#44-groups-section)) | Ships in M3. |
| `native` | `entry` (a shared library), `abi` (`"<major>.<min_host_minor>"`, [6.9](#69-native-tier)) | Defined, ships after the first command nodes ([6.9](#69-native-tier)). |
| `script` | | Reserved, not in v0.1 ([6.10](#610-script-tier)). |
| `builtin` | | Only for the `fugue` pack: implemented in the kernel. |

**Input fields**

| Field | Meaning |
| --- | --- |
| `name` | Matches `[a-z][a-z0-9_]*`. Unique among the node's inputs. |
| `type` | A port type id ([2](#2-port-types-and-values)). |
| `label`, `description` | Display text. |
| `default` | For `text`, `number`, `boolean`. |
| `optional` | The input may be left unset or unwired. |
| `list`, `min_items`, `max_items` | A list input and its size limits. |
| `min`, `max`, `step`, `integer` | Number constraints. |
| `choices` | Allowed values for a `text` input, making it a choice. |
| `widget` | One of the fixed set ([6.2](#62-widgets)). |
| `section`, `advanced` | Grouping on the node face; `advanced` inputs are collapsed. |

**Output fields**: `name`, `type`, `label`, `description`, `list`, `optional`.

### 6.2 Widgets

Widgets come from a fixed set. No pack ships UI code in v0.1, so every node looks consistent.

| `widget` | For | Notes |
| --- | --- | --- |
| `spin` (default for number), `slider` | number | |
| `line` (default for text), `multiline` | text | |
| `toggle` | boolean | |
| `choice` | text with `choices` | Default when `choices` is present. |
| `file`, `folder` | text | Opens the portal picker. The value is a path. |
| `color` | text | Value is `#rrggbb` or `#rrggbbaa`. |
| `seed` | number | Fixed, randomize or increment. Randomize is stored as `{"auto": "random"}`. Increment is a canvas action that rewrites the stored number after each run it starts; in the file it is a fixed number. |

Resource inputs always show the model picker ([2.5](#25-resource-handles)). Output previews
follow the port type (image, video, audio, text, number).

### 6.3 The `needs` block

```json
{
"needs": {
  "gpu": {"api": "any", "vram_mb": 6000, "exclusive": false, "unified_ok": true},
  "memory_mb": 2000,
  "network": false,
  "files": false,
  "resources": ["fugue/upscale-model"]
}
}
```

| Field | Meaning |
| --- | --- |
| `gpu.api` | `any`, `cuda` or `vulkan`. Presence of `gpu` makes the node a GPU node. |
| `gpu.vram_mb` | Estimated GPU memory. |
| `gpu.exclusive` | The node must have the GPU to itself. |
| `gpu.unified_ok` | Works on unified-memory machines. |
| `gpu.parallel` | Opt in to running beside other GPU work. Default false. |
| `memory_mb` | Estimated system memory. |
| `network` | The node uses the network. |
| `files` | The node reads or writes outside the run folder. |
| `resources` | Resource types the node reads. |

Estimates are declared, not trusted: the scheduler measures free memory itself
([8.10](#810-gpu-memory-and-the-helper)), and a node may report its actual peak so the estimate
can be refined. `network`, `files` and `gpu` are also the permissions shown in the trust prompt.
They are declared, not enforced.

### 6.4 `pack.json`

A pack is a folder or git repository with `pack.json` at its root and one subfolder per node
holding that node's `node.json`.

```json
{
  "manifest": 1,
  "id": "io.github.alice.tools",
  "version": "1.2.0",
  "name": "Alice's tools",
  "license": "MIT",
  "source": "https://github.com/alice/fugue-tools",
  "nodes": ["sharpen", "denoise"],
  "types": [{"id": "io.github.alice.tools/palette", "version": 1, "kind": "data",
             "name": "Palette"}],
  "setup": {"command": ["uv", "sync"], "description": "Install Python dependencies"}
}
```

| Field | Required | Meaning |
| --- | --- | --- |
| `manifest`, `id`, `version`, `name` | yes | The id is lowercase reverse-DNS. |
| `license`, `source`, `description` | no | |
| `nodes` | yes | Subfolder names, each with a `node.json`. |
| `types` | no | Declared port types ([2.2](#22-declared-types)): `id`, `version`, `kind` (`resource` or `data`), `name`, and for resources `extensions` (file extensions found in the models folder). |
| `setup` | no | A command that prepares the pack's private environment ([6.8](#68-per-pack-environments)). |
| `converters` | no | Import and export converters ([11.9](#119-converters)). |

### 6.5 Where packs live and how they load

- Packs live in `~/.local/share/fugue/packs/<pack-id>/`, plus extra paths set in settings.
- The built-in nodes are the pack `fugue`, shipped with the app and loaded the same way.
- At startup Fugue reads only manifests and builds the registry. A manifest that fails
  validation disables that node (or pack) with a visible reason; it never stops Fugue.
- Programs, libraries and group definitions load on first use.

### 6.6 Installing

- Copy a folder into the packs folder, or `fugue pack add <git-url|path>` (a git clone pinned
  to a tag or commit). Also `fugue pack list`, `remove`, `update`.
- Updates are explicit and pinned. Nothing updates on its own.
- No registry or catalog in v0.1. No signing in v0.1.

### 6.7 Trust

- The first time a node from a pack would run, Fugue shows the pack, its runtime kinds and its
  declared permissions (network, files outside the run folder, GPU), and the user must allow it.
  The decision is stored per pack id and version in `trust.json`; a pack update asks again.
- Opening a workflow that contains nodes from an unknown or untrusted pack shows the same prompt
  before anything from that pack runs.
- The `fugue` and `local` packs are trusted. A group is only a graph: trust applies to the packs
  of the nodes inside it.
- **No sandbox is enforced in v0.1**, and the user docs say so plainly: a trusted command node
  is a program running as the user. Optional isolation (bubblewrap, Landlock) is later.
- From the CLI an untrusted pack is the error `pack_untrusted`; it is never bypassed
  ([13.6](#136-safety)).

### 6.8 Per-pack environments

A pack may declare `setup` (for example `uv sync` into a virtual environment inside the pack
folder). It runs only after the user is shown the exact command and confirms. There is no shared
environment, so packs cannot conflict with one another.

### 6.9 Native tier

- `runtime.kind: "native"` is part of the manifest now.
- The plugin interface is a plain C ABI: a versioned table of function pointers, with no C++ or
  Qt types across the boundary and a thin C++ header wrapper for authors.
- Its shape comes from the prior-art research ([spikes/06](spikes/06-plugin-abi.md): OBS, VST3,
  LV2, CLAP, OpenFX, Vulkan, SQLite, Node-API). The rules: one exported function,
  `fugue_plugin_entry(host)`; every table starts with a `struct_size` set by the side that builds
  it, and a slot is read only if it lies inside that size; tables are append-only; optional
  features are extensions found by a string id (`fugue.$NAME/$REV`) that never changes; values are
  opaque handles and images are views (8-bit, 16-bit or float32), so no libvips or Qt type
  crosses; errors use the codes of [7.2](#72-one-shot-mode-default) and [9.5](#95-errors); no C
  enum and no C++ exception crosses the boundary.
- `runtime.abi` in `node.json` is `"<major>.<min_host_minor>"`. The registry checks it when it
  indexes manifests, before loading any code; the plugin checks the host again at load.
- The slot list is frozen only after the first command nodes have proved the node model and two
  built-in nodes (a pixel node and one that keeps state across runs) use the header.
- Cancel is cooperative only: an in-process node cannot be killed, so a timeout ([7.4](#74-cancel-and-timeouts)) cannot be
  enforced for it. A node that must be killable uses the command tier.
- Native plugins run in-process and are not sandboxed. The trust prompt applies and the docs say so.

### 6.10 Script tier

There is no QML or JavaScript node tier in v0.1. `runtime.kind: "script"` is reserved so adding
it later needs no format change. Command nodes cover light logic.

### 6.11 Authoring tools

- `fugue node new <name>` scaffolds a node folder with a manifest and a small program.
- `fugue node test <path>` runs a node against sample inputs and checks its outputs against the
  manifest.

### 6.12 The registry

The kernel has one registry of node types, port types and providers. An entry is the data of a
manifest plus something that can execute it. Packs on disk are one way in (M3); the built-in
pack and test fakes register through the same registry in code. M0 therefore builds the
registry, the manifest data model for inputs and outputs ([6.1](#61-nodejson)) and its
validation, and registers fake node types and a fake provider in tests; loading packs from disk
is added in M3 without changing the compiler or the engine. The provider entries are the
configured instances ([8.2](#82-provider-instances)) in the user's order, each with the provider
made for it. What a provider offers is not kept in the registry: it changes while the app runs,
so each run asks again.

---

## 7. Command-node protocol

A command node is a program that speaks JSON over stdio. Any language works; the protocol needs
no library. All text is UTF-8. Each message is one JSON object on one line.

### 7.1 Process setup

- Working directory: the node's own working folder inside the run folder
  ([9.7](#97-run-folder)).
- Environment: `FUGUE_PROTOCOL=1`, `FUGUE_NODE_DIR` (the folder holding `node.json`),
  `FUGUE_WORK_DIR` (the working folder).
- stdin and stdout carry protocol messages. stderr is the node's log.

### 7.2 One-shot mode (default)

Fugue starts the program for each execution, writes one request to stdin and closes it.

Request:

```json
{"protocol": 1, "op": "run", "id": "r1:sharpen",
 "node": {"type": "io.github.alice.tools/sharpen", "version": "1.2.0"},
 "inputs": {"image": {"type": "image", "path": "/…/results/blobs/3f9a….png", "hash": "sha256:…",
                      "width": 1024, "height": 1024, "channels": 3, "format": "png"},
            "amount": 1.5},
 "work_dir": "/…/runs/r1/nodes/sharpen"}
```

- `id` is an opaque name for this execution; persistent mode echoes it.
- `text`, `number`, `boolean` inputs are JSON values. Media inputs are media handles with a
  `path` the program can read; pending video edits are applied to a file first, so a program
  only ever needs `path`. Resource handles get an added `path` to the resolved file. Data values
  are their handles. List inputs are arrays.
- Unset optional inputs are absent.

The program writes zero or more progress lines and then exactly one result or error line to stdout:

```jsonl
{"op": "progress", "value": 0.4, "message": "pass 2 of 5"}
{"op": "result", "outputs": {"image": {"path": "out.png"}}}
```

```json
{"op": "error", "code": "bad_input", "message": "image is smaller than the kernel"}
```

- `op` may be left out of the result line, so the smallest node is one `echo` of
  `{"outputs": {…}}`.
- Media and data outputs are files the program wrote inside `work_dir`, returned as
  `{"path": …}` (relative paths resolve against `work_dir`). A path outside `work_dir` is an
  error. Fugue reads the file's metadata and hash itself.
- Input files are read-only to the node by convention: they are shared with the result cache.
- Other outputs are JSON values. An optional output may be left out (empty).
- `progress.value` is 0 to 1. Optional `{"op": "preview", "image": {"path": …}}` shows an
  intermediate image.
- A result may include `"stats": {"peak_memory_mb": …, "peak_vram_mb": …}`.
- Exit 0 with a result is success. A nonzero exit, or exit 0 without a result, is a failure; the
  last 20 lines of stderr become the error message. Outputs are checked against the manifest;
  a missing required output or a wrong type is the error `node_bad_output`.

### 7.3 Persistent mode

For nodes with costly startup, such as loading a model. Set `runtime.mode: "persistent"`.

- Fugue starts the program once. The program writes `{"op": "ready"}` when it can take work.
- Fugue sends `run` messages, the same shape as in one-shot mode, one per line. The program
  answers with `progress`, `preview`, `result` or `error` lines that carry the request's `id`.
- One run at a time per process in v0.1.
- `{"op": "cancel", "id": …}` asks the program to stop that run; it must answer with an `error`
  whose `code` is `cancelled`, or with the `result` if it finished first.
- `{"op": "shutdown"}` asks it to exit. Fugue sends it after the idle timeout
  (`idle_timeout_s`, default 300) or when it needs the memory, then SIGTERM, then SIGKILL.
- If the process dies, the running node fails with `node_failed` and the next run starts a new one.

### 7.4 Cancel and timeouts

- One-shot: SIGTERM, then SIGKILL after a 5 second grace period.
- Persistent: the `cancel` message; if no answer within the grace period, the process is killed.
- No timeout by default. `runtime.timeout_s` sets one; exceeding it is `node_timeout`.

### 7.5 Helpers

The docs ship small helper snippets for shell and Python. They are conveniences only.

---

## 8. Providers

### 8.1 Capabilities

A node asks for a capability, never a server.

| Capability | Meaning | First used |
| --- | --- | --- |
| `image.generate` | Text (and optional start image) to image | M1 |
| `image.edit` | Instruction plus reference images to image | M1 |
| `video.generate` | Text or first frame to video | M4 |
| `audio.generate` | Music from a prompt and optional lyrics | M4 |
| `text.generate` | LLM text, optionally with image input | M4 |
| `image.upscale` | Model upscaling | M5 |
| `text.encode`, `latent.sample`, `latent.decode`, `latent.encode` | Component-level generation | M5 |

Capability ids are open: a provider module can advertise a new one and a node pack can ask for it.

A provider advertises, per capability: the models it can serve (as resource handles), properties
used for routing (model family, edit support, accepted input kinds), limits (maximum reference
images, minimum and maximum size, size multiple), defaults (steps, guidance, size) and status.

Standard parameters for the two M1 capabilities. A provider ignores an optional parameter it
does not support and must report the values it actually used.

| Capability | Parameters | Result |
| --- | --- | --- |
| `image.generate` | `prompt`, `negative_prompt`, `width`, `height`, `steps`, `guidance`, `seed`, `image` (start image), `strength`, `model` | `image`; used: `seed`, `width`, `height`, `steps`, `model`, `engine` |
| `image.edit` | `prompt` (the instruction), `images` (references), `width`, `height`, `steps`, `guidance`, `seed`, `model` | same |

Parameters of later capabilities are fixed when their nodes are built, following this pattern.

### 8.2 Provider instances

A provider instance is a configured use of a provider module, stored in
`~/.config/fugue/providers.json` (mode 0600):

```json
{"providers": [
  {"name": "This machine", "type": "omarchy", "enabled": true},
  {"name": "Studio box", "type": "omarchy", "address": "https://studio.example.ts.net"},
  {"name": "My endpoint", "type": "http", "address": "https://api.example.com/v1", "key": "…"}
]}
```

- Fields: `name` (unique), `type` (a provider module id), `address`, `key`, `enabled`, plus
  settings the module declares.
- The list order is the user's preference order.
- **Local or remote is only the address.** It is never a property of a graph.
- Keys live only here. They are never written into a workflow, an export or an embedded graph.

### 8.3 Selection

- A node's provider is `auto` by default: the first enabled, ready instance, in the user's
  order, that offers the capability with the required properties and the requested model.
- A node can pin an instance by name, and a workflow can pin one for all its nodes (`provider`
  and `graph.provider` in [4.2](#42-graph)). `fugue run --provider <name>` pins one for a single
  run. A pin is a name only; a pinned name that is not configured is the error `unknown_provider`.
- The engine and model actually used are reported as values used and are part of the cache key
  ([10.1](#101-keys)). The instance name is not: the same engine and model at another address
  give the same key.

### 8.4 Model resolution

- A node's model input is a resource handle. The provider resolves it to something that can run.
- An unset model input means "any ready model that satisfies the capability". The model that was
  used is an output, so the result is reproducible.
- A handle that does not match what is running is a clear error with an offer to start the right
  model. Fugue never substitutes another model silently.

### 8.5 When nothing can serve a capability

The node shows what is missing and how to fix it (start a model, add a provider, enable one).
Error codes: `capability_unavailable`, `model_not_running`, `model_missing`,
`provider_unreachable`. `fugue validate` reports a missing model as a warning; a run that reaches
the node reports it as an error.

### 8.6 Results

A provider fetches results the moment a job finishes and writes them to the run folder.
Servers drop finished jobs (the Omarchy image server after 600 seconds).

### 8.7 Shipped providers

| Type | What it is | Milestone |
| --- | --- | --- |
| `omarchy` | The model servers omarchy-local-ai runs, on this machine or a paired host. | M0 |
| `http` | A generic endpoint: OpenAI-compatible images and text, and the stable-diffusion.cpp native API. | M4 |
| `fugue-helper` | Fugue's own out-of-process generation helper and a Fugue-managed server, started on demand ([8.10](#810-gpu-memory-and-the-helper)). | M5 |
| `comfyui` | Optional, off by default. Talks HTTP and websocket to a ComfyUI server the user installs and runs. Fugue bundles none of its code or node schemas and never starts it. | after v0.1 |

The shipped providers are not special: they sit behind the same interface and registry as a
third-party module ([8.11](#811-the-provider-interface)).

**The `omarchy` provider** (from reading omarchy-local-ai and Fantasi's source, checked against
omarchy-local-ai at commit 703ae0c when the provider was built):

- Discovery: `~/.local/state/omarchy/agent-local/status.json`. Each model row gives `kind`
  (one of `text`, `image`, `video` or `music`), `edit`, `endpoint` and `unit`. A local row has
  no `input` field. Only the rows of a paired host have one, in the tailnet `models.json` and in
  `network.json`, and `["text"]` is the default. Fugue routes by these fields. It does not copy
  Fantasi's "model in use" routing.
- A paired host is read from `network.json` in the same folder, with its own key in
  `~/.config/omarchy/agent-local/remotes/<host>.key`. Pairing itself is done with
  `omarchy-agent-local pair <host>`.
- One preloaded model per server. Changing models goes through `omarchy-agent-local`, only with
  the user's confirmation.
- Auth: a bearer key read from `~/.config/omarchy/agent-local/key`.
- Fantasi is Omarchy's simple image app; Fugue reuses its way of talking to these servers.
- Images and video: the native `/sdcpp/v1` job API. Its root is the endpoint minus its trailing
  `/v1`, not the endpoint's origin, because a paired host's endpoint has a path. Submit, poll, fetch
  at once, cancel and keep polling until the server reports a terminal state. Defaults (steps,
  guidance, size) come from the server's capabilities. Progress is per sampling pass. Step
  previews are requested for images.
- Video jobs must send every LoRA the server lists in its capabilities; the server does not
  apply them on its own.
- A model served this way is identified by its recipe name (no content hash is available), so a
  model replaced under the same name on the server is not detected by the cache.
- No pixel-parity promise across engines: the same seed gives different images on different
  engines. A result records which engine made it.

### 8.8 Provider modules

A third party adds a provider as a folder in `~/.local/share/fugue/providers/<id>/` with a
`provider.json`. No change to Fugue is needed.

```json
{
  "manifest": 1,
  "id": "io.example.engine",
  "version": "0.1.0",
  "name": "Example engine",
  "transport": "http",
  "start": {"command": ["./server", "--port", "{port}"], "when": "on_demand",
            "idle_timeout_s": 600},
  "address": "http://127.0.0.1:{port}",
  "capabilities": ["image.generate"],
  "settings": [{"name": "address", "type": "text"}, {"name": "key", "type": "text", "secret": true}],
  "needs": {"gpu": {"api": "any"}}
}
```

| Field | Meaning |
| --- | --- |
| `transport` | `http` or `stdio`. |
| `start` | Optional. How Fugue starts it: `command`, `when` (`on_demand` or `never`), `idle_timeout_s`. Without `start` the user runs it. |
| `address` | Default address for `http`. `{port}` is a free loopback port Fugue picks. |
| `capabilities` | What it may offer; the live list comes from `describe`. |
| `settings` | Fields an instance can set. `secret` fields are stored like keys. |
| `needs` | As in [6.3](#63-the-needs-block), for scheduling. |

**Protocol.** The same operations on both transports. Over `http` each is
`POST <address>/fugue/v1/<op>` with a JSON body and a JSON reply; the instance key is sent as a
bearer token. Over `stdio` each is one JSON line with `"op"` and an `"id"` echoed in the reply.

| Operation | Request | Reply |
| --- | --- | --- |
| `describe` | none | `protocol` (1), `engine` (a stable id of the inference engine, such as `sdcpp`), per capability: `models`, `properties`, `limits`, `defaults`, `status` (`ready`, `loading`, `unavailable`) and a `reason` |
| `submit` | `capability`, `model`, `params`, `inputs` | `job` |
| `status` | `job` | `state` (`queued`, `running`, `done`, `error`, `cancelled`), `progress` (0 to 1), `stage`, optional `preview`, `error` |
| `cancel` | `job` | `state` |
| `result` | `job` | `outputs` (values and file references), `used` (the values actually used) |
| `shutdown` | none | none. Only for providers Fugue started. |

- Media in `submit` and `result`: over `stdio`, and over `http` to a loopback address when
  `describe` reports `"shares_filesystem": true`, media are `{"path": …}`. Otherwise media are
  sent as `{"data": "<base64>", "format": "png"}` and results are fetched with
  `GET <address>/fugue/v1/result/<job>/<index>`.
- A provider may stream status instead of being polled: `GET …/events/<job>` as server-sent
  events over `http`, or unsolicited `status` lines over `stdio`.
- Errors are `{"error": {"code": …, "message": …}}`. `out_of_memory` is a reserved code.
- A provider process that dies fails the jobs it held and is restarted on the next request.

A new model family that an existing engine already runs needs only a node pack and a model
reference. A new inference runtime is a new provider module.

### 8.9 Models on disk

- Fugue's models folder is `~/.local/share/fugue/models/<type>/`, one subfolder per resource
  type, plus extra search paths set in settings (so an existing ComfyUI `models/` folder can be
  shared without copying).
- A model is identified by content hash, never by path or name. The hash is computed once and
  remembered.
- No downloads in v0.1. Fugue shows where a missing model can be had and which folder it goes in.
- Fugue never modifies or deletes a model file.

### 8.10 GPU memory and the helper

- Fugue starts its helper on demand, keeps it warm for an idle timeout, then stops it to give
  memory back.
- Before starting GPU work Fugue measures free GPU memory itself against what the nodes declare,
  by Vulkan device type ([spikes/04](spikes/04-gpu.md)): on a discrete card the device-local
  heap's budget minus usage (`VK_EXT_memory_budget`); on an integrated or unified-memory device
  `MemAvailable` from `/proc/meminfo`, capped at the heap size (on the GB10 nvidia-smi reports no
  memory figures and the Vulkan budget does not see other processes). CPU devices are skipped.
  omarchy-local-ai does not model a discrete NVIDIA card, so its figures are not relied on.
  M0 builds the `MemAvailable` reading only, and nothing uses it yet: telling the device type,
  the cap at the heap size and the discrete card's figure need Vulkan, which the engine first
  links with the helper (#32). Until then free GPU memory is not measured before a node starts.
- GPU work in the engine and the CLI uses Vulkan compute directly or runs in the helper, never
  Qt RHI: Qt's `offscreen` platform gives neither a Vulkan instance nor a GL context. Fugue links
  no CUDA and bundles no NVIDIA files; the helper builds against the system CUDA package.
- If memory is short, Fugue names the processes holding it (`nvidia-smi --query-compute-apps` on
  NVIDIA, including the GB10) and offers to stop Omarchy's model
  servers, only with confirmation. It never kills another application.
- Out of memory is a typed error on the node (`out_of_memory`). A helper crash kills only the helper.
- The component-level helper is based on stable-diffusion.cpp with separate encode, sample and
  decode steps, C++ only. The M0 spike showed the split works
  ([spikes/02](spikes/02-sdcpp-split.md)).
- Fugue carries the split as its own patch against a pinned stable-diffusion.cpp commit; it does
  not depend on upstream adopting it. To keep that cheap: the patch stays as small as the spike's
  (hooks at the existing stage seams, no change to sampling or model code); the pin moves only on
  purpose; and every move of the pin must pass a test that the staged run gives the same pixels
  as a single run of the same build.

### 8.11 The provider interface

Inside the kernel a provider is one interface with exactly the operations of
[8.8](#88-provider-modules): `describe`, `submit`, `status` (or a status stream), `cancel`,
`result`, `shutdown`, with the same data in and out. The engine only ever talks to this
interface.

- M0 builds the interface, a fake provider for tests, and the `omarchy` provider as an adapter
  compiled into Fugue: it implements the interface by calling the Omarchy servers' own HTTP
  APIs. No inference code is linked into Fugue; the engine it talks to is still another process.
- M4 adds the module adapter, which implements the same interface by speaking the protocol of
  8.8 to a third-party program. From then on each shipped provider also has a `provider.json`,
  so the registry lists shipped and third-party providers the same way.
- `engine` from `describe` is the engine id used in cache keys and recorded in results. For
  `omarchy` it is the server kind (`sdcpp` for images and video).

---

## 9. Execution

### 9.1 Compile

Every run starts by compiling ([4.8](#48-the-compiled-graph)). Compile checks, and reports all
problems at once as structured errors ([13.3](#133-errors)):

- every node type exists and its major version matches;
- every link names an existing node and output, and the types are compatible;
- required inputs are set; literals fit their constraints and choices;
- no cycles; no group recursion;
- pinned providers exist.

`fugue validate` is this step with nothing executed. It also reports warnings: nodes that are
not part of any output, missing models, untrusted packs, a frozen result that is gone.

### 9.2 Scheduling

- The engine keeps a ready queue in dependency order.
- **CPU nodes** run in parallel, up to the number of cores minus one. libvips' own thread pool
  and cache are capped so they do not fight the executor: concurrency 1 per node and cache 0
  ([spikes/03](spikes/03-libvips.md)).
- **Local GPU work** (GPU nodes, and jobs on providers running on this machine) runs one at a
  time. A node opts into parallel GPU use with `needs.gpu.parallel`.
- **Each provider instance** runs one job at a time. A remote instance does not occupy the local
  GPU lane.
- One run is active at a time. Further runs queue first in, first out, can be moved to the front
  ("run first"), and can be removed or cancelled by id ([9.8](#98-runs-across-processes)).

### 9.3 Residency

The scheduler is a residency planner, not a memory manager: each model server holds one model,
and swapping means stopping and starting.

- Among ready nodes it prefers the ones that reuse a model or helper state already loaded.
- On out-of-memory it evicts the least recently used model, not everything.
- Resident models are an in-memory cache, separate from the result cache.

### 9.4 Events

Per-node events go to the canvas and, as JSON lines, to the CLI:

| Event | Fields beyond `event`, `run`, `time` |
| --- | --- |
| `run.queued`, `run.started` | |
| `node.queued` | `node` (path id) |
| `node.running` | `node` |
| `node.progress` | `node`, `value` (0 to 1), `stage`, `message` |
| `node.preview` | `node`, `image` |
| `node.cached` | `node`, `outputs`, `shown` (a sink with no outputs) |
| `node.done` | `node`, `outputs`, `seconds`, `shown` (a sink with no outputs) |
| `node.skipped` | `node`, `reason` (`muted`, `upstream_muted`, `upstream_failed`, `upstream_empty`, `cancelled`) |
| `node.error` | `node`, `error` |
| `run.done` | `status` (`ok`, `failed`, `cancelled`), `errors` |

A sink whose type has no outputs (Preview) has nothing in `outputs`; `shown` holds its inputs as
resolved (`{"value": ...}`), on `node.done` and `node.cached` and in the node's entry in run.json,
so what it shows reaches the CLI (`runs result`) for a cached sink too.

The compiler's reason for leaving a node out ([4.8](#48-the-compiled-graph)) is passed through
as it is, so a node downstream of a muted one is `upstream_muted`. A node that is only
`not_part_of_output` gets no event: it is not part of the run.

Clients must ignore events and fields they do not know. Sampling previews stream from providers
that offer them.

### 9.5 Errors

- Errors are per node. Independent branches keep running; nodes downstream of a failed node are
  skipped.
- Every error names the node by its path id, so a node inside groups is found at once.
- Out of memory is its own code so the canvas can offer the right fix.

### 9.6 Cancel

Cancel is cooperative and cleans up: provider jobs through their cancel operation (then waiting
for the terminal state), persistent nodes by message, one-shot nodes by SIGTERM then SIGKILL.
Nodes that already finished stay cached.

### 9.7 Run folder

Each run has a folder `~/.cache/fugue/runs/<run-id>/`:

| Entry | Holds |
| --- | --- |
| `run.json` | The run's state: status, the worker's process id, start and end times, the workflow's path and hash, the resolved `auto` values, providers and models, and the cache key of each node. |
| `compiled.json` | The compiled graph that ran. |
| `events.jsonl` | Every event, appended as it happens. |
| `roots.json` | The folders the run's nodes may read and write ([13.6](#136-safety)), when `fugue run` started it. |
| `nodes/<path id>/` | Each node's working directory, where it writes its outputs. |

- A run id is an opaque string of `[a-z0-9-]`, unique and sortable by start time.
- When a node finishes, the engine records its outputs in the result cache
  ([10](#10-result-cache)): each media or data file is moved into the cache's store and a hard
  link is left in the node's folder. Handles passed on to other nodes carry the store's path,
  which stays valid after the run folder is gone.
- Run folders are removed 7 days after the run ends
  ([10.5](#105-eviction-and-garbage-collection)).

### 9.8 Runs across processes

v0.1 has no daemon. The window runs its own runs. `fugue run` from the CLI starts a detached
run worker, prints the run id and returns; `--wait` stays attached and streams events.

- `fugue runs status|events|result|cancel <run-id>` read the run folder; cancel signals the worker.
- The queue is shared by the window and all workers, because they share the GPU. It is a folder,
  `~/.local/state/fugue/queue/`, with one small file per waiting or active run, named so that
  sorting gives the order. A process starts its run only when its entry sorts first and it holds
  the advisory lock `~/.local/state/fugue/engine.lock`. "Run first" writes an entry that sorts
  ahead of the waiting ones; removing a queued run deletes its entry. An entry whose process is
  gone is discarded, and its run is ended as `failed`.
- A process that holds the lock is alive, so an active entry is dead exactly when someone else
  can take the lock. A waiting entry is dead when the process id it records is not that run's
  worker; a process id alone is not trusted, because ids are reused.
- A run's `run.json` and `run.done` have one writer at a time: `fugue run` until it has started
  the worker, then nobody while the run waits, then the worker once it has made its entry
  active. Whoever takes a waiting entry out of the queue (a cancel, or a process that found it
  dead) ends that run. `run.done` decides a run's status once it is there. A run ends once:
  two processes that both find its worker gone may both try to end it, so ending takes a lock on
  `events.jsonl`, and an end that finds `run.done` already there changes nothing.

### 9.9 Not in v0.1

Loops, batches, conditionals and several runs in parallel. The engine must not rule out dynamic
graph expansion later.

---

## 10. Result cache

There are two caches and they are separate: the **result cache** on disk, specified here, and
the resident models in memory ([9.3](#93-residency)).

### 10.1 Keys

A node's key is the SHA-256 of the canonical JSON of:

- the node type id and version;
- the pack id and version it came from;
- every input: a literal's value, or for a link the content hash of the upstream output;
- the identity hash of every resource handle ([2.5](#25-resource-handles));
- for provider nodes, the engine id ([8.11](#811-the-provider-interface)) and the identity of
  the model the provider resolved;
- for a node with `deterministic: false`, the run id.

Rules:

- An `auto` value is resolved before keying: a randomized seed becomes a concrete seed, which is
  also recorded as a "values used" output. Every result is therefore reproducible.
- A provider node's provider and model are resolved the same way, before keying. If nothing can
  serve the node, it cannot be looked up and reports the provider error instead.
- A file a node reads by path (Load Image) is keyed by its content hash, not its path.
- A model file's identity is its content hash, so a checkpoint replaced under the same name
  invalidates correctly.
- Because keys use the hashes of upstream outputs, a node whose upstream re-ran but produced the
  same bytes is still a hit.

### 10.2 Store

Under `~/.cache/fugue/results/`:

- `keys/<key>.json`: a record mapping a key to its outputs (values, and media or data handles by
  content hash), with the node type, sizes, last-used time, and `used_by`: the workflow paths
  and node path ids that produced or reused it. `used_by` is what the per-workflow and per-node
  views and clears read.
- `blobs/<hex hash>.<extension>`: the files, stored once per content hash.
- `frozen/<key>`: a marker for each frozen result ([10.4](#104-freeze)).

Final outputs written by Save nodes go to the user's output folder and are never cache entries.
A sink records what it wrote; with unchanged inputs and the file still present, it does not
write a duplicate.

### 10.3 Per-node state

The canvas and `fugue validate --json` show one of these per node: `cached`, `stale` (would
run), `running`, `frozen`, `error`, `skipped`. `fugue validate --json` gives them in `data` as
`nodes`, keyed by path id: `{"state": "stale"}`, and for a node the compiler left out
`{"state": "skipped", "reason": "muted"}`. Without `--json` it prints one line per node. A size view shows what the cache holds per
workflow and per node.

### 10.4 Freeze

Freezing a node keeps its current result: it is kept, never evicted, and never invalidated by
upstream changes until unfrozen. The node stores the frozen key (`frozen` in [4.2](#42-graph))
and shows the frozen state; the cache holds a marker for that key, removed on unfreeze. The
cache view lists frozen results so one left by a deleted workflow can be released. If the frozen result is missing (the file moved to another machine)
validation warns and the node runs normally.

### 10.5 Eviction and garbage collection

- A size limit, default 20 GiB, configurable. Over the limit, the least recently used entries
  are evicted. Frozen entries are never evicted.
- Clear all, per workflow or per node (`fugue cache clear`, and in the canvas).
- Garbage is collected after a grace period of 7 days: run folders whose run has ended, blobs no
  key record refers to, and key records whose blobs are gone. A cached result is otherwise kept
  until it is evicted for size or cleared; it does not expire with age.

---

## 11. Workflow files

### 11.1 Format and place

- One format: the `.fugue` document of [section 4](#4-the-workflow-document).
- Workflows are ordinary files wherever the user saves them. File dialogs go through the desktop
  portal, never `QFileDialog`. Fugue keeps a recent list and suggests `~/Documents/Fugue`.
- A few example workflows ship with the app, and a user templates folder is supported.

### 11.2 Saving

- Saves are atomic (`QSaveFile`).
- "Unsaved" means the content differs from what was last saved, not that an edit happened.
- Closing a workflow with unsaved content asks: save, discard or cancel.

### 11.3 Recovery

On by default. Every 30 seconds Fugue writes a recovery copy of each workflow with unsaved
content to `~/.local/state/fugue/recovery/`. It never overwrites the user's file. On the next
start it offers to recover.

### 11.4 Import

- Drag and drop, or open: a `.fugue` file; an image with an embedded workflow (PNG and WebP in
  v0.1); JSON pasted from the clipboard.
- The kind is detected from the content, not the extension.
- Later: video outputs, opening from a URL, best-effort import of ComfyUI workflows.

### 11.5 Export

- The full workflow.
- A **share copy**: local paths replaced by hashes or bare file names, and optionally prompts
  and layout removed.
- The compiled graph, for the CLI (`fugue compile`).

### 11.6 Missing nodes and models

Missing node types open as placeholders that keep their inputs ([3.6](#36-versions)). Missing
models are reported from `requires.models` with their source if one is recorded. Neither blocks
opening or editing.

### 11.7 Embedding in outputs

- Save Image embeds the workflow in PNG and WebP by default, so dragging an output back onto
  Fugue reloads its graph.
- Absolute local paths are never embedded: they are replaced as in a share copy, including a path
  typed into a node whose output reaches a file or folder input through links. Layout is
  embedded, so the graph comes back as it looked. A `run` section is added with the values used
  per node (seed, model, engine), so a randomized result can be reproduced.
- Each Save node has a visible setting: full, without prompts, or none. A global setting decides
  what new Save nodes start with.
- A first-run notice says the workflow is embedded.
- A run from a compiled graph has no workflow document, so its images hold none; Save Image says so
  with a `workflow_not_embedded` warning on the node.
- Container detail: a PNG `iTXt` chunk with the keyword `fugue`; in WebP, the XMP chunk.

### 11.8 Editing from outside

An open window watches its file. When the file changes on disk (an agent, git, another editor):
with no unsaved content the window reloads it as one undoable step; with unsaved content it asks
which to keep. It never overwrites silently in either direction.

### 11.9 Converters

Import and export go through one converter interface in the kernel: detect a format from
content, turn a foreign file into a workflow document, or turn a document into a foreign file.
v0.1 ships the converters of [11.4](#114-import) and [11.5](#115-export) built in. A pack can
declare converters so a new format needs no change to Fugue:

```json
{
"converters": [{"name": "comfyui", "direction": "import", "extensions": ["json"],
                "entry": "./import-comfyui"}]
}
```

An import converter is a program given the file path as its argument that writes a `.fugue`
document to stdout, or exits nonzero if the file is not its format. An export converter reads a
document on stdin and writes the file named by its argument. Converters are trusted with their
pack ([6.7](#67-trust)). No v0.1 milestone ships a pack converter; the interface exists so the
first one (ComfyUI import, after v0.1) is a module.

---

## 12. Canvas

The look and feel of the prototype is approved. This section fixes behavior.

### 12.1 Targets

- Pan and zoom stay smooth (about 60 frames per second) with 300 nodes and 600 wires.
- The window is up in well under half a second.
- No CPU use at idle.
- No costly effects on nodes (shadows, animation) at scale.

### 12.2 Construction

Our own canvas, no library: node bodies as QML items in a Repeater under one transformed item,
wires as shape paths. Not `Canvas`, not `QQuickPaintedItem`, not Qt Canvas Painter (GPL or
commercial). Each wire is its own `Shape` with the default geometry renderer, not `CurveRenderer`
([spikes/05](spikes/05-canvas.md): same cost as one Shape, 1.5 ms per frame at 300 nodes;
CurveRenderer drops to 15 fps when dragging at 1000 nodes).

### 12.3 Navigation

Two presets, chosen in settings:

| | Legacy (default, ComfyUI's default) | Standard |
| --- | --- | --- |
| Left-drag on empty canvas | pans | selects |
| Wheel | zooms at the pointer | pans |
| Select with a marquee | Ctrl+drag | left-drag |
| Zoom | wheel | Ctrl+wheel |

In both: middle-drag and Space+drag pan. A trackpad is told from a wheel by `pixelDelta`: on a
trackpad, scrolling pans and a pinch zooms. Right-click opens the context menu; right-drag does
nothing. Marquee selection adds with Shift and subtracts with Alt.

Space: holding Space and dragging pans. A Space press with no drag does nothing. This departs
from the Omarchy "Space is the main action" default for ComfyUI familiarity; the main action is
Run on Ctrl+Enter, and the shortcuts overlay says so.

### 12.4 Default keymap

Basics match ComfyUI exactly. Deviations exist only for Omarchy conventions or to fill a gap,
and the `?` overlay marks each one.

| Command id | Action | Default binding | Versus ComfyUI |
| --- | --- | --- | --- |
| `run.start` | Run | Ctrl+Enter | same |
| `run.start-first` | Run, ahead of the queue | Ctrl+Shift+Enter | same |
| `run.to-here` | Run to the selected node | Shift+Enter | added |
| `run.cancel` | Cancel the run | Ctrl+Alt+Enter, Escape while running (Escape cancels first, then leaves a group) | Escape added |
| `file.new` | New workflow | Ctrl+N | added |
| `file.open` | Open | Ctrl+O | same |
| `file.save` | Save | Ctrl+S | same |
| `file.save-as` | Save as | Ctrl+Shift+S | added |
| `file.close` | Close the workflow | Ctrl+W | Omarchy |
| `app.quit` | Quit (asks if there is unsaved content) | Q, Ctrl+Q | Omarchy |
| `app.shortcuts` | Shortcuts overlay | `?`, F1, Ctrl+? | Omarchy |
| `app.settings` | Settings | Ctrl+, | same |
| `edit.undo` | Undo | Ctrl+Z | same |
| `edit.redo` | Redo | Ctrl+Y, Ctrl+Shift+Z | same |
| `edit.copy` | Copy | Ctrl+C | same |
| `edit.paste` | Paste | Ctrl+V | same |
| `edit.paste-connected` | Paste with connections | Ctrl+Shift+V | same |
| `edit.clone` | Clone | Alt+drag, Ctrl+D | Ctrl+D added |
| `edit.delete` | Delete | Delete, Backspace | same |
| `edit.select-all` | Select all | Ctrl+A | same |
| `edit.rename` | Rename | F2, double-click a node's title | F2 added |
| `node.bypass` | Bypass | Ctrl+B | same |
| `node.mute` | Mute | Ctrl+M | same |
| `node.collapse` | Collapse | Alt+C | same |
| `node.pin` | Pin in place | P | same |
| `node.search` | Add a node by search | double-click empty canvas, Tab, Shift+A | keys added |
| `frame.make` | Frame the selection | Ctrl+G | same |
| `group.make` | Make a group | Ctrl+Shift+E | same |
| `group.ungroup` | Ungroup | Ctrl+Shift+G | added |
| `group.enter` | Enter the selected group | Tab, double-click the group's body | Tab added |
| `group.leave` | Leave the group | Escape, the breadcrumb | same |
| `view.fit` | Fit the view | `.`, Home | Home added |
| `view.zoom-in`, `view.zoom-out` | Zoom | Alt+=, Alt+-, Ctrl+=, Ctrl+- | Ctrl added |
| `view.toggle-links` | Hide or show links | L | same |
| `panel.nodes`, `panel.workflows` | Toggle the node library, the workflows panel | N, W | same |

Tab enters a group when exactly one group is selected and opens node search otherwise, inside a
group too. A command states the context it applies in, and the more specific context owns the key.

Mouse:

- Double-click on empty canvas opens node search.
- Releasing a dragged link on empty canvas opens search filtered to nodes with a compatible port.
- Shift+drag an output moves all its links. Ctrl+Alt+click on a port disconnects it.
- Shift or Ctrl+click adds to the selection.

Text fields: while one has focus, bare-letter and Shift-only keys, and Ctrl+A, C, V, X, Z, Y,
belong to the field. Hence the Ctrl and F1 alternatives for quit and the overlay.

### 12.5 Command registry and keymap

- Every action is a command with a string id in one registry. Menus, the `?` overlay, the
  settings panel and the CLI's listing all read the same registry.
- Each key has one owner at a time in a given context (canvas, a selected group, a text field,
  a run in progress). Contexts belong to commands, not to the keymap file.
- The user's keymap is a diff against the defaults at `~/.config/fugue/keybindings.json`:

```json
{"add": [{"command": "view.fit", "key": "F"}],
 "unset": [{"command": "app.quit", "key": "Q"}]}
```

### 12.6 Wiring feedback

- Port color by type, plus a second channel so color is not the only signal: a circle for value
  types (text, number, boolean), a square for media, a diamond for declared types.
- While dragging a link, invalid targets dim and the reason is shown.

### 12.7 Inspection

On each node: the last value on each output port (thumbnail, number, text excerpt), time taken,
cache state ([10.3](#103-per-node-state)), progress, and warnings in the title. Errors show on
the node and in an error panel with the node's path id.

### 12.8 Node search

Search-first node creation. Results show category, ports and version, and are filtered by port
type when opened from a dragged link. Groups appear in search like any node type.

### 12.9 Furniture

v0.1 has collapse, hide unused ports, reroute points and Frames. A minimap and snap-to-grid are later.

### 12.10 Several workflows

Tabs inside one window. Each tab tracks its own unsaved state and its own undo stack.

### 12.11 Undo, look and language

- Every edit is a command on one undo stack per workflow, group edits included.
- The window follows the Omarchy theme live, the theme's corner radius and the desktop text size.
- Translations use the app template's translator, English first, with right-to-left layouts working.

### 12.12 Better than ComfyUI on purpose

Run to here; reasons for invalid connections; search filtered by port type; a Frame and a Group
as separate things; visible cache and frozen state; per-node errors with group paths; one undo
stack across everything; one file format; on-disk recovery; per-output control of what is
embedded; versioned node types; no shared Python environment.

---

## 13. CLI and agent interface

Fugue must be usable by agents. The design is one command table: every operation exists once and
is exposed through the CLI now, and later through an MCP server and a live-session channel. There
are never three implementations.

### 13.1 Commands

Each command works with no display, never opens a dialog, and takes `--json`. For `node`,
`pack`, `provider` and `model` the plural is an alias (`fugue nodes list` is `fugue node list`).
`fugue run` starts a run and `fugue runs` manages runs; those two are different commands.

| Command | Does |
| --- | --- |
| `fugue run <file> [--wait] [--first] [--to <node>] [--provider <name>] [--set <node>.<input>=<value>]` | Start a run. Prints a run id and returns; `--wait` streams events and exits when it ends; `--first` queues it ahead of waiting runs. Short for `fugue workflow run`. |
| `fugue validate <file>` | Compile and report errors and warnings; execute nothing. Short for `fugue workflow validate`. |
| `fugue compile <file>` | Print the compiled graph. Short for `fugue workflow compile`. |
| `fugue workflow new <file>` | Write an empty workflow. |
| `fugue workflow describe <file>` | A readable summary: nodes, links, sinks, what is missing. |
| `fugue workflow export <file> --share [--no-prompts] [--no-layout]` | Write a share copy. |
| `fugue runs list` | Recent and queued runs. |
| `fugue runs status <run-id>` | State and per-node progress. |
| `fugue runs events <run-id> [--follow]` | The event stream as JSON lines. |
| `fugue runs result <run-id>` | Sink outputs: values and file paths. |
| `fugue runs cancel <run-id>` | Cancel. |
| `fugue node list [--type <port type>]` | Installed node types. |
| `fugue node describe <type id>…` | Full manifests as data. |
| `fugue node new <name>`, `fugue node test <path>` | Authoring ([6.11](#611-authoring-tools)). |
| `fugue pack list`, `add`, `remove`, `update`, `trust` | Packs. All but `list` need a human ([13.6](#136-safety)). |
| `fugue provider list` | Instances, their capabilities and status. |
| `fugue model list [--type <resource type>]` | Models found on disk and offered by providers. |
| `fugue cache status`, `fugue cache clear [--workflow <file>] [--node <id>]` | The result cache. |
| `fugue command list` | The command registry and current key bindings. |
| `fugue skill install` | Install the bundled agent skill ([13.5](#135-agent-skill)). |

Exit codes: 0 for success, 1 for any failure. A run that ends `failed` or `cancelled` under
`--wait` exits 1.

### 13.2 Output

With `--json`, stdout carries exactly one JSON object:

```json
{"ok": true, "data": {"run": "20261118-153012-123-k3f9", "status": "queued"}}
```

A command that succeeds with warnings adds a top-level `warnings` array beside `ok` and `data`.
It is present only when there are warnings, and each entry has the form of an error
([13.3](#133-errors)) with severity `warning`:

```json
{"ok": true, "data": {"valid": true, "nodes": {"old": {"state": "skipped", "reason": "not_part_of_output"}}},
 "warnings": [{"code": "not_part_of_output", "severity": "warning", "node": "old",
               "message": "Node old is not part of any output: no sink depends on it."}]}
```

Streaming commands (`run --wait`, `runs events --follow`) instead write one event per line
([9.4](#94-events)), ending with `run.done`. Without `--json` the same information is printed
for people: an error keeps its node, input or output, its valid options and its hint. Logs and
diagnostics go to stderr.

### 13.3 Errors

With `--json`, failures are structured, on stdout:

```json
{"ok": false, "errors": [
  {"code": "value_not_in_choices", "severity": "error",
   "node": "g7/shrink", "input": "method",
   "message": "\"lancos\" is not a valid method.",
   "valid": ["nearest", "bilinear", "bicubic", "lanczos", "area"],
   "hint": "Did you mean \"lanczos\"?"}
]}
```

| Field | Meaning |
| --- | --- |
| `code` | A stable identifier from the list below. |
| `severity` | `error` or `warning`. |
| `node` | The path id, when the error is about a node. |
| `input`, `output` | The port, when it is about one. |
| `message` | One sentence a person can read. |
| `valid` | The valid options, in full and never truncated. |
| `hint` | How to fix it. |

Codes in v0.1:

| Area | Codes |
| --- | --- |
| Files | `file_not_found`, `invalid_json`, `invalid_document`, `unsupported_format_version`, `permission_denied`, `media_unreadable` |
| Graph | `unknown_node_type`, `node_version_mismatch`, `unknown_input`, `missing_input`, `unknown_link_source`, `unknown_output`, `type_mismatch`, `value_out_of_range`, `value_not_in_choices`, `cycle`, `group_recursion`, `unknown_provider` |
| Warnings | `not_part_of_output`, `model_missing`, `frozen_result_missing`, `pack_update_available`, `workflow_not_embedded` |
| Trust | `pack_untrusted`, `needs_confirmation` |
| Providers | `capability_unavailable`, `model_not_running`, `provider_unreachable` |
| Running | `node_failed`, `node_bad_output`, `node_timeout`, `out_of_memory`, `cancelled` |
| Other | `unknown_run`, `usage`, `internal` |

New codes may be added; existing ones do not change meaning.

### 13.4 Authoring by agents

- JSON only in v0.1. An agent writes the `.fugue` document directly, using
  `fugue node describe` for manifests and `fugue validate` as a dry run that returns per-node errors.
- `layout` may be left out; the canvas lays out nodes that have no position.
- Live sharing is by file watch ([11.8](#118-editing-from-outside)): the agent edits the file
  and an open window shows it.
- Later: `fugue mcp` (an MCP server from the same command table) and a token-gated loopback
  control channel with per-command undo. A text DSL is undecided.

### 13.5 Agent skill

`fugue skill install` installs a short bundled skill the way Fantasi does: one copy in
`~/.agents/skills`, linked into Claude's skills directory. It covers discovering node types,
authoring, validating, running and fetching results.

### 13.6 Safety

- These always need a human: trusting a pack, `pack add`, `pack update`, `pack remove`, running
  a pack's `setup`, adding a provider or a key, starting or stopping a model server. From the CLI
  they ask on an interactive terminal. With no terminal they fail with `needs_confirmation`.
  There is no flag that answers for the human.
- A run that reaches an untrusted pack fails with `pack_untrusted`.
- File access by CLI commands is limited to configured roots. Defaults: read anywhere under the
  home folder; write under the current directory, the output folder and `~/Documents/Fugue`.
  Outside them is `permission_denied`. "The current directory" is the one `fugue run` was started
  in; `fugue run` records the roots in the run folder for the worker. Paths are judged after `..`
  and symbolic links are resolved. A run started without roots (the canvas, for now) is not
  limited.
- This is a guard against mistakes and prompt injection, not a security boundary: anything with
  a shell as the user can edit Fugue's files. The docs say so.

---

## 14. Built-in nodes

Built-in nodes are the pack `fugue`. The tables are the starting manifests; once a node is
merged, its `node.json` is the source of truth.

### 14.1 M1: the first twelve

| Type id | Inputs | Outputs | Notes |
| --- | --- | --- | --- |
| `fugue/text` | `text` (multiline) | `text` | |
| `fugue/number` | `value` | `value` | |
| `fugue/load-image` | `file` (file picker) | `image`, `width`, `height` | Keyed by the file's content hash. The image is upright: an EXIF orientation is applied, and `width`, `height` are the size it is seen at. |
| `fugue/save-image` | `image`, `name`, `format` (png, webp), `embed` (full, no-prompts, none), `folder` (optional) | `file` | Sink. Writes to the output folder. Never overwrites a file: `<name>.png`, else `<name>-2.png`, `-3`, and so on, also when several Save nodes write one name at once. [11.7](#117-embedding-in-outputs). |
| `fugue/preview` | `value` (any) | none | Sink. Shows the value on the node; from the CLI it is in the run result. |
| `fugue/generate-image` | `prompt`, `negative_prompt`, `model` (optional), `width` (optional), `height` (optional), `steps` (optional), `guidance` (optional), `seed`, `image` (optional start image), `strength` | `image`, `seed`, `width`, `height`, `model` | Capability `image.generate`. Unset optional inputs use the model's defaults; an unset size is the start image's, else the model's default. |
| `fugue/edit-image` | `instruction`, `images` (list, 1 to the model's limit, else 4), `model` (optional), `steps` (optional), `seed` | `image`, `seed`, `width`, `height`, `model` | Capability `image.edit`; `instruction` is sent as its `prompt`. |
| `fugue/resize` | `image`, `width`, `height`, `fit` (stretch, contain, cover), `method` (nearest, bilinear, bicubic, lanczos, area) | `image` | ComfyUI's resampling. `contain` fits inside the box without padding, so the result can be smaller than the box on one side. |
| `fugue/crop` | `image`, `x`, `y`, `width`, `height` | `image` | |
| `fugue/brightness-contrast` | `image`, `brightness`, `contrast` | `image` | |
| `fugue/blend` | `base`, `top`, `mode`, `opacity` | `image` | Modes at least: normal, multiply, screen, overlay, soft light, difference. Difference is \|base - top\| as in Photoshop, GIMP and CSS; subtract is base - top clamped, which ComfyUI calls difference. |
| `fugue/text-concat` | `a`, `b`, `separator` | `text` | |

Pixel nodes decode and hold images with libvips (C++ API, dynamically linked, CPU); their
arithmetic is plain C++ over float buffers, because libvips' own resize kernels do not give
ComfyUI's pixels (Appendix A). Each has a parity test against ComfyUI's formula for the same
operation, written before the node. The four-reference limit on Edit Image is Fugue's starting
choice; a limit the provider reports for the model replaces it, higher or lower.

### 14.2 M4: the rest of v0.1

- Generate Video (first frame optional, file-backed output), Generate Song (prompt, optional
  lyrics), Text Generate (LLM, optional image input), Load Video, Load Audio.
- Pad (for outpainting), Rotate, Flip, Blur, Sharpen, Invert.
- Mask tools: grow, shrink, invert, from alpha, solid.
- Number Math.

### 14.3 M5: component level

The spike passed ([spikes/02](spikes/02-sdcpp-split.md)): model pickers, Text Encode, Empty Latent, Sampler, VAE Decode and
Encode, LoRA load and apply (strength, stacking; the result is a derived handle,
[2.5](#25-resource-handles)). Then ControlNet, upscale with a model, hires fix as a group, inpaint
models. A coarse Generate Image can later be rebuilt as a group of these.

---

## 15. Milestones

| Milestone | Contents | Issues |
| --- | --- | --- |
| Spec | This document. | #1 |
| **M0** engine, CLI and spikes | Spikes first: component-level go or no-go, libvips benchmark, GPU checks, canvas frame rate, plugin-interface research. Then the document model, compiler, scheduler and result cache, run folder and media handles, the provider interface with the `omarchy` provider, and the CLI skeleton. Driven by tests with fake nodes; no canvas. | #2 to #12, #34 |
| **M1** first nodes | The twelve nodes of [14.1](#141-m1-the-first-twelve). | #13 to #15 |
| **M2** canvas | The canvas of [section 12](#12-canvas) on the M0 engine: wires, search, copy and paste, undo, run, per-node state, save, open, recovery, import, tabs, keymap, theme. | #16 to #22 |
| **M3** extension | Manifests and the loader, the command-node protocol, pack install and trust, groups, user docs with a five-minute tutorial and an example pack. | #23 to #27 |
| **M4** rest of v0.1 | Video, song and text nodes, more pixel and mask nodes, the agent skill, providers as modules. | #28 to #31 |
| **M5** component level | The helper and the component nodes. The M0 spike passed ([spikes/02](spikes/02-sdcpp-split.md)), so these are in v0.1. The optional ComfyUI provider is no longer needed as a fallback and moves after v0.1. | #32, #33 |

After v0.1: an MCP server, a live control channel, a node catalog, a minimap, loops and batches,
regional prompting, conditioning math, ComfyUI workflow import, model downloads, a sandbox for
command nodes, the script tier, packaging for Windows and macOS.

---

## 16. User docs outline

For people who add nodes. Written in M3.

1. Quickstart: a shell-script node in five minutes.
2. Manifest reference (`node.json`, `pack.json`).
3. Command protocol reference.
4. Packs and installing them.
5. Persistent mode.
6. Resources and the GPU (`needs`).
7. Types and handles (media, resource, data).
8. Testing a node (`fugue node test`).
9. Publishing a pack.
10. The native plugin guide, once the interface is frozen.

An example pack lives in the repository and is used by the quickstart and the tests.

---

## 17. Open items

To measure or confirm before the part that depends on each is built. None blocks starting M0.

| Item | Blocks | Tracked |
| --- | --- | --- |
| ~~Frame rate with 300 nodes, and the wire renderer~~ Done: a Shape per wire ([spikes/05](spikes/05-canvas.md)) | [12.2](#122-construction) | #5 |
| ~~libvips speed on 1 to 4 megapixel images, thread and cache settings~~ Done: keep libvips ([spikes/03](spikes/03-libvips.md)) | [14.1](#141-m1-the-first-twelve) pixel nodes | #3 |
| ~~Offscreen GPU compute, Vulkan device enumeration, free-memory measurement, CUDA terms~~ Done ([spikes/04](spikes/04-gpu.md)); AMD free memory unmeasured | [8.10](#810-gpu-memory-and-the-helper) | #4 |
| ~~Plugin-interface prior art~~ Done: rules in [6.9](#69-native-tier) ([spikes/06](spikes/06-plugin-abi.md)); the slot list freezes after two built-in nodes use it | freezing [6.9](#69-native-tier) | #6 |
| Whether talking to a separate ComfyUI process, bundling none of its code, is acceptable for an MIT app | the `comfyui` provider | owner decision |
| ~~How pairing with a remote Omarchy host works beyond reading its key file~~ Done: pairing goes through `omarchy-agent-local pair` and the `remotes/<host>.key` files, and the `omarchy` provider uses it ([8.7](#87-shipped-providers)). Still open: a model shared after the last `network.json` write stays invisible until that file is rewritten | remote `omarchy` instances | #11 |
| ~~Whether Omarchy's engine runs on a discrete NVIDIA card~~ Answered: no. At omarchy-local-ai 703ae0c the engine detects no discrete NVIDIA card and has no sd-server runtime for one; the `omarchy` provider reports `capability_unavailable` with how to fix it. Local use on such machines waits for the helper | local use on such machines | #11, #32 |
| The exact WebP container field for the embedded workflow | [11.7](#117-embedding-in-outputs) | with #13 |
| Whether Alt shortcuts clash with the window manager's bindings | [12.4](#124-default-keymap) | with #17 |
| ~~An existing package named `fugue`~~ None: the AUR has no package named `fugue` (checked 2026-11-19) | packaging | #34 |

---

## Appendix A: choices made in this document

The design map decided what and why. Writing a buildable spec needed these further choices.
Each has a confidence and can be overruled; say so on the issue that builds it.

| Choice | Section | Confidence |
| --- | --- | --- |
| SHA-256 for all hashes; RFC 8785 canonical JSON for hashing | 1.5 | high |
| Sorted keys and two-space indent as the on-disk form | 4.6 | high |
| Node ids are 1 to 32 characters; readable ids are valid | 4.2 | high |
| Input value forms: `from`, `auto`, `resource`, arrays | 4.2 | high |
| A randomized seed is `{"auto": "random"}`; increment is a canvas action | 4.2, 6.2 | medium-high |
| Lists: one value wraps into a list; a list cannot feed a non-list input | 2.3 | medium-high |
| No implicit conversions between built-in types | 2.3 | medium-high |
| A mask is a one-channel image, not its own port type | 2.1 | medium |
| Declared types have a kind, `resource` or `data`, and an integer version | 2.2 | medium-high |
| Resource inputs show a model picker and store the handle as a literal | 2.5 | medium-high |
| An unset model input means any ready model; the one used is an output | 8.4 | medium-high |
| Group definitions keyed by id; exposed inputs as `targets`; inner layout under `layout.groups` | 4.4, 4.5 | medium-high |
| Workflow-made groups are `local/<name>`; the library is the `local` pack | 5.3 | medium |
| Nested groups allowed (recursion forbidden) | 5.2 | high |
| Bypass maps each output to the first input of the same type | 3.5 | medium-high |
| A node type id must start with its pack's id; aliases never shadow an installed type | 6.1, 3.6 | high |
| Message shapes of the command protocol, `ready` and `shutdown`, the 5 second grace period, the 300 second idle timeout | 7 | medium-high |
| The provider protocol's operations and media transfer | 8.8 | medium |
| Standard parameter names for `image.generate` and `image.edit` | 8.1 | medium |
| One job at a time per provider instance; remote instances do not occupy the local GPU lane | 9.2 | medium-high |
| Event names and fields | 9.4 | high |
| Run folder layout; detached run workers and one per-user lock instead of a daemon | 9.7, 9.8 | medium |
| Cache store split into key records and content blobs; keys use upstream content hashes | 10.1, 10.2 | medium-high |
| A sink with unchanged inputs does not write a duplicate file | 10.2 | medium |
| The frozen key is stored on the node; a missing frozen result warns and runs | 10.4 | medium |
| CLI shape: `fugue run <file>` to start, `fugue runs …` to manage, singular groups with plural aliases | 13.1 | medium-high |
| The error envelope and the list of codes | 13.2, 13.3 | high |
| Human-only operations fail with `needs_confirmation` when there is no terminal; no override flag | 13.6 | medium-high |
| Default file roots for CLI commands | 13.6 | medium |
| Command ids in the keymap; `run.to-here` on Shift+Enter; Tab enters a selected group | 12.4 | medium |
| Embedded workflows carry a `run` section with the values used | 11.7 | medium-high |
| Input and output lists of the twelve M1 nodes | 14.1 | medium |
| The kernel provider interface has the module protocol's operations; `omarchy` is an adapter compiled in | 8.11 | medium-high |
| The cache key uses the engine and model, not the provider instance | 8.3, 10.1 | medium-high |
| A workflow-wide provider pin is `graph.provider` | 4.2 | medium-high |
| The run queue is a folder of entries plus a lock | 9.8 | medium |
| Cached results do not expire with age; only orphans and old run folders are collected after 7 days | 10.5 | medium-high |
| Frozen results are held by markers in the cache | 10.2, 10.4 | medium |
| Escape or the breadcrumb leaves a group; Tab is search unless one group is selected | 5.2, 12.4 | medium |
| Tabs and import of outputs are in M2 (the design tickets disagreed; the issues decide) | 12.10, 15 | medium |
| Pack converters as programs declared in `pack.json` | 11.9 | medium |
| The registry is built in M0 and packs on disk plug into it in M3 | 6.12 | high |

Building M0 made these further choices. Each changes or pins down what a section above says.

| Choice made in M0 | Section | Confidence |
| --- | --- | --- |
| A run id is `YYYYMMDD-HHMMSS-mmm-xxxx`: the UTC start time to the millisecond and four random characters | 9.7 | medium-high |
| A queue entry is named `<class>-<enqueue time in ms, 14 digits>-<run id>`, with class 0 active, 3 "run first" and 5 waiting | 9.8 | medium-high |
| The run worker is the hidden command `fugue __worker <run id>`; a waiting entry's process is checked by that command line, an active one by the engine lock | 9.8 | medium-high |
| One writer per run at a time, and `run.done` decides the status | 9.7, 9.8 | medium-high |
| The model input of image nodes uses the resource id `fugue/image-model`; M1's nodes must use it or no `omarchy` model matches | 2.5, 8.4 | medium |
| Status, properties, limits and defaults in a provider's description are per model, not per capability: one instance can run several servers | 8.8 | medium-high |
| `fugue validate` reports every provider problem of 8.5 as a warning, not only a missing model, and a provider instance that cannot be made is a warning too | 8.5, 9.1 | medium-high |
| The success envelope has a top-level `warnings` array | 13.2 | high |
| A frozen node serves its result even when a node upstream of it failed | 10.4 | medium-high |
| A video's pending edits stay pending in handles and cache records, and are applied (lossless FFV1 and FLAC in Matroska) only for a node that needs a plain file | 2.4 | medium |
| A new error code, `media_unreadable`, for a file that is not the image, video or audio its port says | 13.3 | high |
| The cache key's pack part is the type id's namespace with no pack version until packs exist | 10.1 | medium |
| Garbage is collected by a run worker, at most once an hour | 10.5 | medium |
| Load-time errors inside a group definition name the inner node id and start "Group <id>:", because no instance path exists at load; compile errors name path ids | 13.3 | medium |
| Free GPU memory: only the `MemAvailable` reading is built; the device type and the discrete figure wait for the helper | 8.10 | medium |
| Omarchy's own folders are built from the home folder, as omarchy-local-ai builds them, not from XDG variables | 1.4, 8.7 | medium-high |

Building M1 made these further choices.

| Choice made in M1 | Section | Confidence |
| --- | --- | --- |
| Pixel arithmetic is plain C++ over float32 0..1 buffers that libvips decodes and holds, not libvips operations: libvips' resize kernels give other pixels than torch and PIL (spike 03), and crop, blend and brightness-contrast share the buffers so every pixel node reads and writes one form | 14.1 | medium-high |
| Resize is ComfyUI's: torch nearest-exact, bilinear, bicubic (A = -0.75), area, and PIL lanczos (a = 3, premultiplied RGBA), without PIL's uint8 rounding in lanczos (up to about 2/255) | 14.1 | high |
| Resize `fit`: stretch is ComfyUI's crop "disabled", cover its crop "center"; a width or height of 0 follows the other side, both 0 pass the image through | 14.1 | high |
| Resize `contain` fits inside the box keeping the aspect and never pads, so one side can be smaller than the box (padding is the M4 Pad node) | 14.1 | medium |
| Image nodes clamp their output to 0..1 and return float32 RGB or RGBA; gray input becomes RGB | 14.1 | high |
| Crop is ComfyUI's ImageCrop: x and y are held inside the image, the far edge is cut at the image's edge | 14.1 | high |
| Brightness/Contrast (no ComfyUI core node) is clamp((v - 0.5) * contrast + 0.5 + brightness) with alpha untouched: a mid-gray pivot, so contrast does not brighten (spike 03's form did) | 14.1 | medium |
| Blend: `difference` is \|base - top\| (Photoshop, GIMP, CSS) and `subtract` is base - top clamped, which ComfyUI calls difference; opacity is ComfyUI's blend_factor, the top is resized to the base (bicubic, center crop), the result keeps the base's alpha | 14.1 | medium-high |
| Text Concat is ComfyUI's StringConcatenate: a + separator + b | 14.1 | high |
| Images are upright everywhere: an EXIF orientation is applied when an image is probed and read (Fantasi's choice) | 2.4, 14.1 | medium-high |
| A sink with no outputs reports its resolved inputs as `shown` on node.done, node.cached and in run.json | 9.4 | high |
| Save Image's `file` output is an image handle to the file written, so the cache can check it still exists | 14.1, 10.2 | medium |
| Save Image never overwrites: the name is claimed in the same step as the file appears, and taken names go to `-2`, `-3`; a name may not hold `/` or `\` or start with `.`; a relative folder is under the output folder; WebP is lossless | 14.1 | medium |
| The embedded `run` section is `{"nodes": {<path id>: {"auto", "engine", "model"}}}` for the saving node and everything upstream | 11.7 | high |
| A "prompt" is the literal of a `text` input with the `multiline` widget; a "path" is an absolute literal that names something (`/x`, `~/x`, `C:\x`; not a lone `/` or `~`) of a `file` or `folder` input, or of any node upstream of one | 11.5, 11.7 | medium |
| A value that arrives by link is checked against the input's `min`, `max`, `integer`, `choices`, `min_items` and `max_items` when the node is about to run, as the compiler checks a literal; failing fails the node on that input with the same code | 3.2, 9.5 | high |
| A failed file write takes its code from the cause (errno): a missing folder (ENOENT, ENOTDIR) is `file_not_found`, a refusal (EACCES, EPERM, EROFS) `permission_denied`, anything else such as a full disk `node_failed`; the message names the cause | 13.3 | medium-high |
| File roots: `fugue run` records 13.6's defaults with its own working directory in the run folder (`roots.json`); Load Image (and the scheduler, before it hashes a `file` input) checks read, Save Image checks write, on canonical paths; outside is `permission_denied` on the input with a hint naming the roots. A relative Save folder may not lead out of the output folder. Runs without roots are not limited | 9.7, 13.6 | medium-high |
| A run from a compiled graph embeds nothing and warns `workflow_not_embedded`; a kernel's warnings go to run.done on its node | 11.7, 13.3 | medium-high |
| `~` in a file or folder literal is expanded before the node runs and before its key is made, so `~/x.png` is keyed by content | 10.1 | high |
| A cached Save Image does not rewrite its file when only the layout or an unrelated node changed, so the embedded copy can lag the canvas | 10.2, 11.7 | medium |
| Generate Image's width and height are optional: unset is the start image's size, else the model's default | 14.1 | high |
| Size precedence for Generate Image: set on the node, then the start image's fitted size, then the model's default; with one side set, the other follows the start image's shape in steps of 64 | 14.1 | medium-high |
| Input pictures go as they are when they are an upright PNG in steps of 64 at 0.4 to 1.6 MP; else they are fitted near 1 MP in steps of 64 and written as RGB PNG (Fantasi's prepareBase) | 14.1 | medium-high |
| Edit Image has no size inputs: it sends the first reference's prepared size | 14.1 | medium |
| Edit Image's reference limit is the model's `max_reference_images` when it reports one, higher or lower, else 4; it is checked when the job is made, not under validate | 14.1 | medium |
| Seeds default to 0; `strength` defaults to 0.7 and is sent only with a start image; an empty negative prompt is not sent | 14.1 | medium |
| Unset steps, guidance and size are filled from the model's defaults in the request, so the job is fully specified | 8.1 | medium-high |

Building M2 made these further choices.

| Choice made in M2 | Section | Confidence |
| --- | --- | --- |
| A node's face is ComfyUI's: wire-only inputs (media, data, resource, `any`) and the outputs side by side under the title, then one row per value input showing its value or default | 12.2, 3.2 | medium-high |
| Port shapes: circle for text, number and boolean, square for image, video and audio, diamond for declared types, a ring for `any`; a list port has a second outline behind it. Colors are the theme's port colors (`theme.portColor`); `any` takes the text color and a placeholder's port the muted one | 12.6 | medium |
| Link checks during a drag and on connect: the engine's compatibility (2.3), a node into itself, a cycle, and a node type that is not installed, each with a sentence saying why | 12.6, 2.3 | high |
| Dragging a linked single input picks its link up and carries it from its source (ComfyUI); dragging an unlinked input reaches back to an output | 12.4 | high |
| A new link into a list input adds an item at the end: the first is stored as a plain link, a second turns the value into an array | 2.3, 4.2 | medium |
| Nodes the layout does not place (agent-written files) go in columns by depth, below the placed ones | 4.5, 13.4 | medium |
| Positions are written to `layout.nodes.<id>.pos` as whole numbers, the viewport to `layout.view` (center whole, zoom to three decimals) | 4.5, 4.6 | medium-high |
| Zoom runs 0.1 to 4, 1.1 per wheel step (ComfyUI's step); below 0.5 node text and ports are not drawn, and a press there is on the node, not a port (ComfyUI draws a low-detail node below about 0.6) | 12.1, 12.3 | medium-high |
| A node's port rows are made the first time it is in view at a readable zoom, so a 300-node file opens at the cost of what is on screen | 12.1 | high |
| A trackpad is a wheel event with `pixelDelta` and pans in both presets; Ctrl+wheel zooms in both; a pinch arrives as a native zoom gesture | 12.3 | medium-high |
| Space+drag pans with the left button held; Space and a move with no button does nothing (ComfyUI pans on that too) | 12.3 | medium |
| The canvas is never mirrored in right-to-left languages: a graph's direction and its saved positions are geometry | 12.11 | medium-high |
| A click selects a node, Shift or Ctrl+click adds, dragging a selected node moves the selection, and a click on empty canvas clears it (legacy). A marquee selects every node it touches, as ComfyUI's does | 12.3, 12.4 | high |
| Furniture (12.9) in #16 is drawn from the layout only (collapsed nodes, Frames, reroute points); making and editing it waits for the command registry and undo (#17, #21) | 12.9 | medium |
| Node search ranks by name, then keywords, category and description, with letters-in-order as the last resort; every word of the query must match. A dragged link keeps the node types with a compatible port (the engine's rules) and links the first such port; a node dropped from an input goes left of the point so its output faces it | 12.8, 12.4 | medium-high |
| Undo: one `QUndoStack` per workflow, in the canvas model. Each step keeps the whole document before and after it, and undo or redo makes the canvas match by node id, remaking only what differs. A gesture is one step: a drag, a dragged link (one picked up included), an Alt+drag clone with its move, a Shift+drag of links. An edit that leaves the content hash of "unsaved" (11.2) as it was is no step; the viewport and the selection are not edits. Each tab keeps its own stack (12.10) | 12.10, 12.11 | high |
| The clipboard holds a `.fugue` document of the selection (its nodes, their layout and the group definitions they use) as `application/x-fugue` and as plain text, so any workflow text pastes. A paste gets new ids, puts the copy's top left at the pointer (else the view's center) and selects it. A plain paste keeps the links between the pasted nodes; Ctrl+Shift+V also keeps links from nodes still in the workflow that pass the link checks | 12.4 | medium-high |
| Clone keeps no links from outside, as ComfyUI's does. Ctrl+D puts the copy 20 units right and down; Alt+drag clones the node, or the selection when the node is in it, where it is and drags the copy. Alt+click with no drag takes the node out of the selection | 12.4 | medium |
| Shift or Ctrl+click on a node already selected, with no drag, takes it out of the selection (litegraph's toggle); dragging it still moves the selection | 12.4 | medium-high |
| Bypass, mute and collapse on a mixed selection: every node takes the state, unless all have it already, and then all lose it (ComfyUI flips each node, which leaves a mixed selection mixed) | 12.4 | medium |
| Delete removes the nodes, every link into or out of them, and their layout: entries, reroute points into them and their places in Frames | 12.4, 4.5 | high |
| Shift+drag from an output lets go on another output that can feed every one of its inputs to move them all, and anywhere else to leave them be. Ctrl+Alt+click on an input removes its links (every list item), on an output every link out of it | 12.4 | medium |
| Zoom keys step 1.1 at the canvas's center (ComfyUI). Fit (`.`, Home) frames the selection, or every node when none is selected, in three quarters of the canvas, within the zoom range | 12.4 | medium-high |
| Until the design pass (#47): a bypassed node takes the palette's magenta (ComfyUI's purple), a muted one fades to half | 3.5, 12.6 | medium |

## Appendix B: issue to section map

| Issue | Sections |
| --- | --- |
| #2 component-level spike | 2.6, 8.10, 14.3 |
| #3 libvips benchmark | 2.4, 14.1 |
| #4 GPU checks | 6.3, 8.10 |
| #5 canvas measurement | 12.1, 12.2 |
| #6 plugin-interface research | 6.9 |
| #7 document model and JSON format | 4, 1.5, 2.1 to 2.3, 6.1 (inputs and outputs), 6.12 |
| #8 compiler | 4.8, 2.3, 3, 5.6, 6.12, 9.1, 13.3 |
| #9 scheduler and result cache | 9, 10 |
| #10 run folder and media handles | 2.4, 9.7 |
| #11 provider interface and the omarchy provider | 8.1 to 8.7, 8.11, the operations table of 8.8 |
| #12 CLI skeleton | 13.1 to 13.3, 9.8 |
| #13 basic nodes | 14.1, 11.7 |
| #14 Generate Image and Edit Image | 14.1, 8.1, 8.4 |
| #15 pixel nodes | 14.1 |
| #16 canvas | 12.1 to 12.3, 12.6, 12.9 |
| #17 selection, clipboard, undo | 12.4, 12.11 |
| #18 node search | 12.8, 12.4 |
| #19 run and per-node state | 12.7, 9.4, 10.3, 3.4 |
| #20 save, open, recovery, import | 11, 3.6, 6.7, 12.10 |
| #21 command registry and keymap | 12.5, 12.4 |
| #22 theme, text size, translations | 12.11 |
| #23 manifests and the loader | 6.1 to 6.5, 2.2 |
| #24 command-node protocol | 7 |
| #25 pack install, trust, environments | 6.6 to 6.8, 13.6 |
| #26 groups | 5, 4.4 |
| #27 user docs | 16, 6.11 |
| #28 video, song and text nodes | 14.2, 8.1, 8.7 |
| #29 more pixel and mask nodes | 14.2, 2.1 |
| #30 agent skill | 13.4, 13.5 |
| #31 providers as modules | 8.8 |
| #32 component-level helper | 8.10, 2.6 |
| #33 component nodes | 14.3, 2.5, 2.6, 8.9 |
| #34 CI recipe | 17 |
