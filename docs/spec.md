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
7. **Small, reusable parts.** Components and nodes do one thing and stand alone: a UI component
   takes a value and reports a change without knowing its caller; a node that does too much is
   split into simple nodes, assembled as a group ([5](#5-groups)) plus a new node only for the
   missing part ([3.7](#37-designing-node-types)).

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
| `~/.config/fugue/settings.json` | Settings, written by the settings panel (`app.settings`, Ctrl+,): `navigation`, `language`, `output_folder`, `inputs_folder`, `model_paths`, `model_folder_kinds`, `download_folder`, `pack_paths`, `new_seed`, `embed_workflow`, `corner_radius`, `hover_magnifier` (`src/engine/settings.h` has the table). Keys Fugue does not know are kept. |
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
| `~/Pictures/Fugue/inputs/` | Images pasted or dropped into Load Image, `<name>-<10 hex of the content hash>.<ext>` ([14.1](#141-m1-the-first-twelve)). Follows the output folder unless set on its own. |

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
there is no mask port type in v0.1. In a mask 1 is the part to change and 0 the part to keep; a
mask port is marked `"role": "mask"` and drawn as a hollow square ([14.4](#144-masking-and-inpainting-167)).

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
  on disk and offered by providers, with a search field that filters them by every word typed
  (name, folder or provider, #84). In Load Models, once a diffusion model is picked, the text
  encoder and VAE lists put the files that fit it first; so does Apply LoRA's list for the model
  it is given (#86). The rest are under "Other", those that
  do not fit dimmed, by what the files' headers say (#85, `engine/model_shape.h`). The canvas check
  warns `model_mismatch` on a Load Models whose parts do not fit, and a stage given them fails
  with it before the helper loads them. The chosen
  handle is stored as the input's literal. A file on
  disk is hashed when it is picked, on a worker thread, with the input showing how far; the hash
  is remembered, so each file is read once.

### 2.6 Data values: latents and conditioning

Values of a data type are files in the run folder referenced by a handle with a content hash:

```json
{"data": "fugue/latent", "path": "…/latent.safetensors", "hash": "sha256:…",
 "meta": {"width": 1024, "height": 1024, "channels": 16, "family": "flux2"}}
```

- A latent is a tensor file plus a small descriptor (`meta`). An Empty Latent is the descriptor
  alone, a size: the Sampler makes the noise.
- Conditioning is what the encode leaves for sampling, in the helper's stage file (8.10): every
  conditioning set, negative included, and the state image extensions set while encoding. Later
  modifiers (ControlNet references with strengths, masks, areas, timestep ranges) are applied by
  nodes between Text Encode and the Sampler.
- Because each value has a content hash, changing only the sampler does not re-encode the prompt.

The built-in pack declares the generation types (#79): data `fugue/latent` and
`fugue/conditioning`; resources `fugue/diffusion-model` (folders `diffusion-model`,
`diffusion_models`, `unet`), `fugue/text-encoder` (`text-encoder`, `text_encoders`, `clip`),
`fugue/vae` (`vae`), `fugue/lora` (`lora`, `loras`) and `fugue/model` (`checkpoints`). A
`fugue/model` is everything stable-diffusion.cpp loads as one model: a checkpoint file, or the
derived handle Load Models makes, `{"base": <diffusion model>, "text_encoder": …, "vae": …}`, with
Apply LoRA's `apply` steps. A latent's `meta` holds the image size, and for a sampled or encoded
latent the model, which VAE Decode decodes with; an Empty Latent's is only the size
(`"empty": true`), the Sampler starting from noise. A video latent (#105) is the same file kind
with `frames` and `fps` in its `meta` too; the tensor has the time axis, tens of MB for a few
seconds, and travels to a paired host like any latent. Wan's 4 n + 1 frames: the model rounds
another count to one. `fugue/controlnet` and
`fugue/upscale-model` come with their nodes.

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
| different major, that major installed | The node runs on that major (its newest installed minor) and is offered the newer major: compiling gives the warning `node_newer_major`, never a block. Moving to the newer major rewrites the node's `version`; the person reviews what changed first, and an agent moves by editing the file. On the canvas the node shows a chip "Move to <version>" (and the Node menu has "Move to newer version…"); either asks first, listing the inputs and outputs that come and go, and moving is one undo step (#118). |
| different major, that major not installed | The node opens in a "needs review" state that shows what changed. It does not run until the user accepts; compiling reports `node_version_mismatch`. Accepting rewrites the node's `version` to the installed one, which is also how an agent accepts: by editing the file. |
| type missing | A placeholder that keeps its inputs and links, so saving loses nothing. |

Several majors of a node type can be installed at once (#90). The registry holds each type per
major; a workflow node runs on the major it recorded, so an older workflow does not change when a
newer major arrives. Node search, the node library and a new node use the newest major. A major
comes from:

- the built-in pack: `pack.json` lists a node folder named `<name>@<major>` beside `<name>` for
  each earlier major it keeps, with its own `node.json` (the version's major must match the
  name) and its own kernel in code (`Kernels::addAt`);
- another pack folder: packs install side by side by major ([6.6](#66-installing)).

Where an id is installed at two majors the same minor-and-patch rules hold within each. An alias
applies to every major of the type it names. `fugue node describe <id>@<major>` shows an older
major's manifest.

A node type can declare `aliases`: old ids it answers to, so a rename does not break saved
workflows. An alias applies only when no installed type has that id.

The version is part of the cache key ([10.1](#101-keys)), so a new version never reuses old results, and
neither does one major another's.

---

### 3.7 Designing node types

Prefer several simple nodes composed as a group over one complex node, and add a new node only
for the part no existing node does. Split where:

- the value passed between the parts is meaningful and cheap to carry: an image, mask, latent,
  number or text. Not half-finished state such as a sampler mid-run or a loaded model, which costs
  memory or reloads and is useless alone;
- each part is useful by itself or in another workflow.

Keep one node where the split would run a hot path through many tiny steps (per-pixel work
wants one kernel), or where the intermediate value has no meaning outside the node. A group
must behave like a node ([5.4](#54-exposed-inputs-and-outputs)): exposed inputs and outputs,
collapsed to one box, reused from the library. Groups pin their nodes' versions ([3.6](#36-versions)),
so a split does not break older workflows.

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
              "name": "detail-tweaker.safetensors", "source": "https://…", "size": 151000000}]
}
}
```

- `fugue`: the app version that wrote the file.
- `nodes`: every node type used, including inside groups, with its version.
- `packs`: the packs those types come from, with an optional `source` Fugue can offer to install
  from (a prompt; nothing installs silently). A save writes each pack installed here at its
  version (of the pack that holds the newest major of its nodes in use), with the git URL it was added from (`.fugue-source.json`, 6.6) and the commit it is
  at (`ref`), when it was added from git (#91). The URL never carries a credential (https
  user and password, ssh password are dropped) and a `file://` one is not written.
- `models`: every resource handle with a hash that appears as a literal, with an optional
  `source` (where its file can be downloaded, 8.9) and `size` (its bytes). On open Fugue checks
  the model folders and reports what is missing without blocking: the banner names each missing
  file, where to get it (`source`) and the folder it goes in (#83), and offers to download those
  with a source (#89). The check reads no model file (8.9).

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
| `outputs[].label` | Optional display name on the face (#72); else the inner output's. |
| `nodes` | The inner graph, same shape as `graph.nodes`. Inner ids are local to the definition. |
| `library` | The library entry (`local/<name>`) the definition was saved to or added from. Saving to the library updates only that entry ([5.3](#53-definitions-copies-and-the-library)). |

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

- `nodes`: per node id: position, size (0 means automatic), collapsed, color (a theme role such as `blue`, or a hex value `#rrggbb` from the color picker, [12.5](#125-command-registry-and-keymap);
  a hex value is a hue, not a final color: the look mixes it into the theme's own surfaces as it does a role, so it
  reads on light and dark themes, and the file stores only the string), a display title, pinned, `advanced` (true: the node's Advanced section is open,
  [5.4](#54-exposed-inputs-and-outputs)), and `increment`: the seed inputs the canvas adds one to
  after each run it starts ([6.2](#62-widgets)).
- `frames`: Frames ([5.1](#51-frame)).
- `links`: reroute points, keyed by the target `<node id>.<input name>`, with `#<index>` added
  for an item of a list input (`merge.images#1`).
- `comments`: free text notes on the canvas.
- `view`: the last viewport. A workflow with none (a template, a file written by hand) is
  fitted to its nodes as it opens, as Fit does (12.4), so it never opens at 100% with nodes cut
  off; one with a `view` keeps it.
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
`layout` and has no effect on a run. Made with Ctrl+G around the selection.

- Dragging its title band moves it with what lies in it: the nodes whose middle is inside it and
  the Frames inside it. Its `nodes` list is rewritten to those nodes after the move.
- Its context menu renames it, colors it and deletes it (the nodes stay); a double-click on the
  title renames it too.

### 5.2 Group

A reusable unit made from selected nodes that collapses into one node. A group is a node type
with an id and a version, and it appears in node search.

- **Make group** (Ctrl+Shift+E): Fugue creates the group's inputs and outputs from every link
  that crosses the selection boundary. The user can rename, reorder and add to them.
- **Enter and leave**: Tab on a selected group, or a double-click on its body, enters it, with
  a breadcrumb trail. Escape or the breadcrumb leaves it. Entering and leaving are not edits;
  an edit inside is an edit of the definition and one undo step, and an undo always restores the
  whole workflow (when it takes away the group shown, the canvas goes up to the nearest level
  that is still there).
- Making a group inside a group keeps the outer face working: an exposed input that drove a node
  now in the new group drives it through the new group's face, and so do outputs; Ungroup inside
  does the reverse.
- A group's id follows its name: renaming "Group" to "Two pass" makes it `local/two-pass` (when
  free), on every instance.
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
- **Save to library** writes `local/<name>/node.json` (the face as inputs and outputs, `runtime:
  {"kind": "group", "definition": "group.fugue"}`) and `group.fugue` (a workflow document with no
  nodes whose `groups` holds the group and every group it uses, and whose `layout.groups` their
  insides), and lists the folder in `local/pack.json`. Saving over an earlier version bumps it
  under 3.6: major when an exposed input or output went or changed type, minor when one was
  added, patch for any other change; saving the same definition keeps the version. An entry is
  updated only by the group that came from it (`library`, 4.4): another group of the same name is
  refused until renamed. The workflow's
  definition and instances take the version saved. Every open window lists the saved group at
  once, and its instances there show "update available" (6.5).
- Adding a library group brings its definition (and the groups it uses) into the workflow, as a
  paste does: a nested group that clashes with another definition of the workflow comes in under
  a new id. Updating drops the values and links of exposed inputs and outputs that went or
  changed type, the same ones the update listed.
- **Delete from the library** is in two places that call one function: Settings > Packs > Library
  (Remove…, then Remove) and the trash can on a library group's row in the node library panel (a
  separate question, then Delete, [12.14](#1214-side-panels)). Only the library's folder and its
  line in `local/pack.json` go; a workflow that used the group keeps the definition it embeds, and
  opens and runs as before.
- **The list follows the disk.** `local/pack.json` lists only folders that hold a `node.json`:
  every write of it drops the others, and so does loading the library, which rewrites it once
  (atomically) when a listed folder is gone or has no `node.json` (removed by hand, or a removal
  that stopped half way), with no warning (one `qInfo` line at most). Only the list is fixed:
  no folder or file is ever deleted by it. A removal rewrites the list whatever became of the
  folder; a folder that could not be removed stays listed, with an error, so it can be tried
  again. Only the `local` pack heals; another pack keeps its "node is disabled" warning. A
  folder on disk that the list does not name is not adopted (it keeps the warning of 6.5): the
  list is what the library holds, and adopting would bring back what a removal took out.
- The groups of the workflow appear in node search too, except one the level shown is inside of.

### 5.4 Exposed inputs and outputs

- Any inner input can be exposed with one action (right-click its port inside the group: Expose
  on the group; Stop exposing undoes it). An exposed input keeps the inner input's type,
  default and constraints, and can get a display name and a narrowed range. Inside a group each
  exposed port is marked with the name it has on the face.
- The interface editor (Node > Group inputs and outputs…) renames the group and its exposed inputs
  and outputs, sets labels, sections, ranges, defaults and choices (a subset of the inner input's),
  reorders them and adds inner inputs of the same type to an exposed input. Renaming an exposed
  input or output renames it on every instance, and in the face of every group around it.
- An exposed input or output whose inner node is deleted leaves the face, and every instance
  loses its value or links for it; a group around it that exposed that port loses it too, outwards
  through every level.
- One exposed input can drive several inner inputs (one seed for two samplers).
- Exposed inputs can be reordered and put in sections. A section named "Advanced" is collapsed
  by default. On every node face (a manifest's `section` and `advanced` inputs too, 6.1) a
  section starts with a header row; the Advanced header says how many inputs it folds and a click
  opens or folds it (`layout.nodes.<id>.advanced`). A linked advanced input always shows.
- Inner inputs that are not exposed keep their values and stay visible inside the group.
- Outputs work the same way, including "values used" outputs.

### 5.5 Sinks inside a group

A group never starts a run and never writes a file or shows a preview from inside when it runs
as a node. Whatever is wanted from it is exposed as an output and the sink sits outside.

- While the user has entered a group, its inner sinks work; they go quiet on leaving. The canvas
  passes the entered instance's path id to the compiler, which keeps that level's sinks (not those
  of groups nested in it).
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
| `group` | `definition` (a workflow document whose `groups` holds the group, under the node's id, and every group it uses, [5.3](#53-definitions-copies-and-the-library)) | Ships in M3. A workflow runs a group from the definition it embeds; one that names a library or pack group only by its type id (as an agent may write it, 13.4) gets that definition put in before the compile, and only one that cannot be read is `unknown_node_type`. |
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
| `role` | `mask` on an `image` input that takes a mask: the port is drawn hollow. Changes no compatibility ([14.4](#144-masking-and-inpainting-167)). |
| `over` | For widget `mask`: the image input the mask is painted over. |

**Output fields**: `name`, `type`, `label`, `description`, `list`, `optional`, `role`.

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
| `mask` | text | A painted mask: the value is the path of an 8-bit gray PNG in the inputs folder, empty for none. The face shows it over the picture of the input named in `over` and opens the mask editor ([14.4](#144-masking-and-inpainting-167)). |
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
| `types` | no | Declared port types ([2.2](#22-declared-types)): `id`, `version`, `kind` (`resource` or `data`), `name`, and for resources `extensions` (file extensions found in the models folder; default ComfyUI's list plus `.gguf`), `folders` (the folder names under each models root, such as `["upscaler", "upscale_models"]`; default the last part of the id, [8.9](#89-models-on-disk)) and `files` (what its files are by their headers, so a model folder of one kind is told: `diffusion-model`, `text-encoder`, `vae`, `lora` or `checkpoint`). |
| `setup` | no | A command that prepares the pack's private environment ([6.8](#68-per-pack-environments)). |
| `converters` | no | Import and export converters ([11.9](#119-converters)). |

### 6.5 Where packs live and how they load

- Packs live in `~/.local/share/fugue/packs/<pack-id>/`, plus extra paths set in settings. A pack
  folder is named `<pack-id>`, or `<pack-id>@<major>` (the major of its version) when another major of
  the pack is installed beside it ([6.6](#66-installing)). Several majors of a pack load together;
  the same major twice loads once, from the earlier path. Each node type remembers the pack version
  it came from, which trust and the cache key use.
- The built-in nodes are the pack `fugue`, shipped with the app and loaded the same way.
- At startup Fugue reads only manifests and builds the registry. A manifest that fails
  validation disables that node (or pack) with a visible reason; it never stops Fugue.
- Programs, libraries and group definitions load on first use.
- What is installed changes while Fugue runs: the window reads the manifests again after an
  install, update, removal or trust on the Packs page, a save to the library, or a change of the
  pack paths, and when the pack folders change on disk (a `fugue pack add`, another window's save,
  an edit by hand), once they have been quiet for half a second. Every open workflow follows: a
  placeholder whose type is now installed becomes the node, a node whose pack went becomes a
  placeholder that keeps its inputs and links (11.6), node search lists what is there now, and
  nothing of the document changes. Runs take the new types between runs, never during one: the
  next compile uses them, and a run waiting in the queue keeps the graph it was compiled to. A
  command node runs the files read at the last reload; a persistent program started from older
  files is stopped and started again.
- The folder `local` of the first pack path is the group library ([5.3](#53-definitions-copies-and-the-library)):
  the one disk pack with the id `local`, holding groups only. A `local` folder elsewhere is refused.

### 6.6 Installing

- Copy a folder into the packs folder, or `fugue pack add <folder|git-url>`. A folder is copied;
  a git address (`https://`, `ssh://`, `git://`, `file://` or `user@host:path`) is cloned and
  must be pinned with `--ref <tag|commit>`. Also `fugue pack list`, `remove`, `update`, `trust`.
- The pack goes to `packs/<id>`, or to `packs/<id>@<major>` when `packs/<id>` holds another major
  (#90): `pack add` of another major neither replaces nor untrusts the one installed, and `pack add`
  of a major already installed is `pack_exists`. `remove`, `update` and `trust` take the folder
  name, `<id>` or `<id>@<major>`; an update stays in its major (a new major is an `add`). The pack goes with a `.fugue-source.json` beside `pack.json` saying where it
  came from (and the commit a ref resolved to), so `update` can fetch it again. The `.git`
  folder is not kept. A pack is checked in a staging folder first and put in place only when
  it loads and the person agreed; a refusal or a failure leaves no half-installed pack.
- A template may carry the exact pack it needs as `packs/<id>/` in its folder ([11.10](#1110-templates)). The window
  offers it from the banner and the CLI names `fugue pack add <that folder>`; it is installed by
  this same question and trusted by the same one ([6.7](#67-trust)), never silently, and what
  ships with Fugue does not matter. Installing beside another major needs no replace (above).
- The prompt shows the pack, its version and source, the runtime kinds of its nodes and what
  they declare ([6.3](#63-the-needs-block)). Installing does not trust: `fugue pack trust <id>`
  or the first run does ([6.7](#67-trust)).
- Updates are explicit and pinned. Nothing updates on its own. `pack update` fetches the pack
  again at the same ref, or at `--ref`, shows the new version and asks; a pack already at that
  commit is left alone. A new version asks for trust again. A pack that was copied in by hand
  has no source and cannot be updated.
- `pack remove` removes a pack from the packs folder (not one found through a settings path)
  and forgets its trust.
- The window does the same on the Packs page of the settings panel (`Ctrl+,`): the installed
  packs with why a pack or node did not load, add (folder or pinned git address), update, trust
  and remove. It asks the same questions on the page, with the same details, and acts only after
  a yes. What it changed is usable in every open window at once (6.5).
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
  manifest. `<path>` is a node folder or a pack folder (every command node in it). The cases are
  in `test.json` next to `node.json` (`inputs`, and `expect` for exact output values); with none,
  the node runs once on its defaults. Cases of one node share a program, so a persistent node
  keeps its state. The pack is read where it is: no install, no trust. `fugue node new <name>`
  inside a pack folder adds a node; elsewhere it makes the pack `io.example.<name>` (`--id` picks
  another) in a folder named after its id, with a one-shot shell node and a `test.json`.

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
  intermediate image on the node, as a `node.preview` event ([9.4](#94-events)), until the next
  preview or a result with a picture; a result without one leaves the last preview in the slot. The path follows the outputs' rule (inside `work_dir`, relative to it). The
  file is read after the line is read, so each preview writes a new file, or writes elsewhere and
  renames over the last. Previews that come faster than the canvas shows them are dropped, all but
  the newest. A bad preview (no path, no such file, outside `work_dir`) is not shown and the run
  goes on; the first one of a run is the warning `node_bad_preview`.
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
  "When it needs the memory": a program whose type declares `needs.gpu` is a resident model
  ([9.3](#93-residency)) while it runs, and is stopped when the scheduler drops it to make room
  for GPU work, least recently used first. A program in the middle of a run is not stopped.
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
| `video.generate` | Text or first frame to video | M4 (#28) |
| `audio.generate` | Music from a prompt and optional lyrics | M4 (#28) |
| `text.generate` | LLM text, optionally with image input | M4 |
| `image.upscale` | Model upscaling (#134): one image through an upscale model, tiled | M5 |
| `text.encode`, `latent.sample`, `latent.decode`, `latent.encode` | Component-level generation; for a video model (Wan) the same capabilities make and decode a video latent (#105, [8.10](#810-gpu-memory-and-the-helper)) | M5 |

Capability ids are open: a provider module can advertise a new one and a node pack can ask for it.

A provider advertises, per capability: the models it can serve (as resource handles), properties
used for routing (model family, edit support, accepted input kinds), limits (maximum reference
images, minimum and maximum size, size multiple), defaults (steps, guidance, size) and status.

Standard parameters for the two M1 capabilities. A provider ignores an optional parameter it
does not support and must report the values it actually used.

| Capability | Parameters | Result |
| --- | --- | --- |
| `image.generate` | `prompt`, `negative_prompt`, `width`, `height`, `steps`, `guidance`, `seed`, `image` (start image), `strength`, `mask` (an input with `image`: an 8-bit gray PNG of its size, white redrawn; only to a provider whose `inputs` property lists `mask`, [14.4](#144-masking-and-inpainting-167)), `model` | `image`; used: `seed`, `width`, `height`, `steps`, `model`, `engine` |
| `image.edit` | `prompt` (the instruction), `images` (references), `width`, `height`, `steps`, `guidance`, `seed`, `model` | same |
| `text.generate` | `prompt`, `system` (optional), `image` (optional, an input), `seed`, `max_tokens`, `temperature`, `thinking` (optional boolean), `model` | `text` (the answer, reasoning left out); used: `seed`, `model`, `engine` |

| `image.upscale` | `model_path` (in `context`: the upscale model's file, or for a paired host a handle `{name, sha256, size}`), `tile_size` (pixels per tile side, tiles overlap by a quarter; an image no bigger than one is done whole), `image` (an input: an 8-bit RGB PNG), or `frames` (an input: a folder of PNGs, a video's chunk, #138) | `image` (the size times the model's scale), or `frames` (a folder `frames-out/` of the same names); `scale` |
| `video.generate` | `prompt`, `negative_prompt`, `width`, `height`, `video_frames`, `fps`, `steps`, `guidance`, `seed`, `image` (first frame, an input), `model` | `video` (a file); used: `seed`, `fps`, `frames`, `model`, `engine` |
| `audio.generate` | `prompt` (what the song is like), `lyrics` (`[Instrumental]` for none, empty for the model's own), `duration` (seconds), `language`, `seed`, `model` | `audio` (a file) and `lyrics` (what was sung); used: `seed`, `model`, `engine` |

`image.upscale` is the one helper capability with no sd context. The upscaler (a file of tens of megabytes) stays loaded between jobs, keyed by the model file and the tile size, so the jobs of a video's chunks read it once; it is freed before a diffusion context loads and on out-of-memory, as the contexts are (8.10). For one image the library reports each tile as progress and has no cancel of its own: a cancel ends the helper after the grace period ([9.6](#96-cancel)). `frames` is a folder on the helper's disk (a paired host is refused: the node says so): each PNG goes through one at a time in file-name order, is written beside as `.part` and renamed (a frame is whole or absent), progress is the frames done, and a cancel is heard between frames and answers `cancelled`. The helper runs ESRGAN (RRDBNet, the 4x models) and MoSR; a model that unshuffles pixels to its input (Real-ESRGAN x2plus and the other 2x models) fails to load and is `node_failed` naming it.

A video's size, rate and length are read from the file the provider wrote, not from the used values:
a model rounds the frame count to what it can make. A model of a capability may report `min_duration`
and `max_duration` (audio) or `min_`/`max_` `width` and `height` (video) as limits.

Parameters of later capabilities are fixed when their nodes are built, following this pattern.
A text model may report `max_tokens` (its longest answer) as a limit and `input` (`["text",
"image"]`) as a property; one that does not say what it takes is still sent an image.

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
- **No file, the default.** Without `providers.json` Fugue uses: `This machine` (type `omarchy`,
  no address), then one `omarchy` instance per host omarchy-local-ai's `network.json` says this
  machine is paired with, named after the host (`gx10-f5f4`), with the address
  `https://<its dns name>`, in name order. A paired host that is offline is kept (its models show
  as offline, and a pin on it stays valid); a host that is not paired is not used. The default is
  read again wherever the file is read (the CLI, each window run, each time a model picker opens),
  and nothing is written: the file is made only when the user changes a provider in the settings.
  A file that exists wins entirely; nothing is added to it.
  When a `fugue-helper` is installed, the default ends with `Fugue helper` (type
  `fugue-helper`): last, so "any ready model" stays the model servers', while only the helper
  runs models from disk.
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
- The model picker of a provider node lists the models every enabled instance that describes
  itself offers for the node type's capabilities (Generate Image: `image.generate`; Edit Image:
  `image.edit`, so only models that edit), grouped under each instance's name in the user's order,
  by the name the provider gives them (`FLUX.2 Klein 4B`, display only: the handle keeps the
  served name). "Any ready model" (unset) comes first. A model that is not ready says so (`not
  running`, `loading`) with why and how to fix it under its name; until Fugue can start a model
  with the user's confirmation ([13.6](#136-safety)) that is the `omarchy-agent-local` command to
  run. With no model the picker says so: "No image model available: start one in
  omarchy-local-ai or add a provider".

### 8.5 When nothing can serve a capability

The node shows what is missing and how to fix it (start a model, add a provider, enable one):
before a run the canvas check's warning is a chip under the node, and a run's error of these codes
carries its fix.
Error codes: `capability_unavailable`, `model_not_running`, `model_missing`,
`provider_unreachable`. `fugue validate` reports a missing model as a warning; a run that reaches
the node reports it as an error. `model_mismatch`, parts of a model that do not fit together
(#85), is reported the same way.

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
third-party module ([8.11](#811-the-provider-interface)). `omarchy` and `fugue-helper` are listed
as modules with transport `builtin` ([8.8](#88-provider-modules)): they are code compiled into
Fugue, because `omarchy` speaks its servers' own APIs, not the protocol of 8.8, and the helper's
program is found by Fugue ([8.10](#810-gpu-memory-and-the-helper)). `http` and `comfyui` are
not built yet: each is a module folder (`http` for OpenAI-compatible and sd.cpp native endpoints,
`comfyui` for ComfyUI's own API), written against 8.8 when its milestone comes, with no change to Fugue.

**The `fugue-helper` provider** runs `fugue-helper --serve` ([8.10](#810-gpu-memory-and-the-helper))
and speaks the protocol of 8.8 to it over `stdio`:

- The program: `lib/fugue/fugue-helper-cuda` on an NVIDIA card (the faster one), else
  `fugue-helper-vulkan`, beside the app (`FUGUE_HELPER` names another; a source checkout's
  `build-helper-<backend>/` is used when nothing is installed).
- Started on the first job, not by `describe`: a model picker describes every provider each time
  it opens, so the provider answers `describe` itself. It offers `text.encode`, `latent.sample`,
  `latent.decode` and `latent.encode`, each with one model with an empty handle: the models come
  as files in the job (`params.context`, by path, as stable-diffusion.cpp's context options
  name them), so the offer is chosen for "any ready model" and a picker does not list it.
- Kept warm while it has jobs and for `idle_timeout_s` after the last (an instance setting,
  default 600), then sent `shutdown`, and killed when it is not out within 5 seconds.
- One job at a time: the helper runs the jobs it is sent in order.
- `submit` carries `output_dir`, where the helper writes the job's files (it shares the
  filesystem).
- A helper that dies fails the jobs it held with `node_failed` and the last lines of its log, and
  is started again on the next job. `out_of_memory` from the helper stays `out_of_memory`; its
  other errors are `node_failed`.
- Cancel is cooperative (stable-diffusion.cpp stops at its next step); a job not over within 5
  seconds is ended by killing the helper.

**A helper on a paired host** (#80) is an instance of the same type with an `address`:
`{"name": "gx10", "type": "fugue-helper", "address": "http://<tailnet address>:<port>", "key": "…"}`
in providers.json (the key stays there, like every key). It speaks the protocol of 8.8 over `http`
to `fugue-helper --serve-http` ([8.10](#810-gpu-memory-and-the-helper)), which the host's owner
runs (a systemd user unit, `helper/fugue-helper.service`); Fugue never starts or stops it, and
`idle_timeout_s` means nothing for it.

- `describe` reaches the host: an unreachable host, a refused key (401, 403) or three failed polls in
  a row is `provider_unreachable`. It reports `shares_filesystem: false`, its own free memory
  (`MemAvailable`, which is what the GB10 has) and `host`. The engine id is `sdcpp@<host>`: a stage run
  on a host does not give the pixels of another machine or build (8.10), and the engine is in the cache
  key, so a result of one machine never answers for another.
- Media are data, since the host does not share the filesystem. Every `{"path"}` of a job's inputs
  (conditioning and latent files, images, reference images) goes as `{"data": base64, "format"}`;
  each file the host's `result` lists is fetched with `GET /fugue/v1/result/<job>/<i>` into the job's
  output folder, so the nodes downstream see the same `{"path"}` as with the local helper. A stage's
  conditioning or latent file made on one machine therefore feeds the next stage on the other: a workflow
  can mix the machines (encode on the host, sample here), only if both have the same model.
- Models go as handles, `{name, sha256, size?}`, in place of paths: the stage nodes do not look for the
  files on this machine, and so do not run the checks that read them (parts that do not fit, LoRAs that do
  not fit, 2.5). The host does not run them either: a set of files that does not fit fails at its load, as
  stable-diffusion.cpp reports it (`node_failed`, with the helper's message). The host resolves a handle in
  its own model folders (`--models`, any layout): the files with the handle's name, checked against `size`
  when the handle has one, and then against `sha256` (hashed once per file and remembered in
  `$XDG_STATE_HOME/fugue/model-hashes.json`, inside the job, stage `models`); no such file is `model_missing`,
  naming the folders searched, and the files found with another content. A string path outside the host's
  folders is refused: the host reads no other file.
- Memory: a job on a paired host is not on this machine's GPU lane and Fugue does not measure this machine's
  memory for it (9.2). The host reports its free memory in `describe` (`free_memory_bytes`), which is shown,
  not compared with a node's estimate; the host frees its own least recently used contexts before a load and
  a shortage comes back as `out_of_memory` (8.10). Two jobs, one here and one on the host, run at the same time.
- Status is polled (500 ms). A job runs on the host one at a time. Errors keep their codes
  (`out_of_memory`, `cancelled`, `model_missing`); the others are `node_failed`.

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
- Video (#28): each `kind: video` row is a `fugue/video-model` of `video.generate`, probed with
  `GET /sdcpp/v1/capabilities` (defaults from `defaults_by_mode.vid_gen`: size, steps, guidance, `fps`).
  A job is `POST /sdcpp/v1/vid_gen` and the same job API as images; it asks for WebM with the VAE
  tiled, sends `init_image` for a first frame and every listed LoRA, and the finished job's whole
  container (base64) is written to `video.webm` at once (8.6). No previews are asked for.
- Music (#28): each `kind: music` row is a `fugue/music-model` of `audio.generate`, probed with
  `GET /health` (ace-server has no capabilities call; 10 to 600 seconds is its documented range). A song is
  two ace-server jobs in turn: `POST /lm` writes it out (lyrics, tempo, key, audio codes), `POST /synth`
  renders what `/lm` wrote, as an mp3; `GET /job?id=N` is `running`, `done`, `failed` or `cancelled`,
  `GET /job?id=N&result=1` the result (a JSON array, then `multipart/mixed` whose first part is the audio),
  `POST /job?id=N&cancel=1` cancels. There is no queue state, step counter or preview, so progress is
  the half each job is. The lyrics the model wrote are an output of the node.
- Text: each `kind: text` row is a `fugue/text-model` of `text.generate`, served by its
  OpenAI-compatible API (omarchy-local-ai runtimes' `api = "openai"`; the served model name is
  the recipe). Ready is asked with `GET /v1/models` (503 while the weights load). A job is one
  `POST /v1/chat/completions` with `stream: true`, read as server-sent events: content adds to
  the answer, `reasoning_content` does not; a server that ignores `stream` answers in one JSON
  object; a stream that ends without its finish (`[DONE]` or a `finish_reason`) was cut off and
  fails the job. An image goes as a data URL (`image_url` content part), PNG or JPEG as the node
  read it. Cancel closes the stream.
  A paired host's row gives `input` and `max_tokens`; a local row gives neither (#87). The
  share proxy streams the events through, so a paired host's text models (the DGX's) serve
  exactly like its image models.
- A model served this way is identified by its recipe name (no content hash is available), so a
  model replaced under the same name on the server is not detected by the cache.
- No pixel-parity promise across engines: the same seed gives different images on different
  engines. A result records which engine made it.

### 8.8 Provider modules

A third party adds a provider as a folder in `~/.local/share/fugue/providers/<id>/` with a
`provider.json`. No change to Fugue is needed: the folder is found when a providers.json entry has
`"type": "<id>"` ([8.2](#82-provider-instances)), and read again each time providers are made.

```json
{
  "manifest": 1,
  "id": "io.example.engine",
  "version": "0.1.0",
  "name": "Example engine",
  "transport": "http",
  "start": {"command": ["./server", "--port", "{port}"], "when": "on_demand",
            "idle_timeout_s": 600, "start_timeout_s": 30},
  "address": "http://127.0.0.1:{port}",
  "capabilities": ["image.generate"],
  "settings": [{"name": "address", "type": "text"}, {"name": "key", "type": "text", "secret": true}],
  "needs": {"gpu": {"api": "any"}}
}
```

| Field | Meaning |
| --- | --- |
| `manifest` | 1. |
| `id` | Letters, digits and `. - _`. The folder is named after it. A module cannot use the id of a provider that comes with Fugue (`omarchy`, `fugue-helper`). |
| `version`, `name` | Required, and for people (`name` defaults to the id). |
| `transport` | `http` or `stdio`. (`builtin`, in the registry only, marks a provider compiled into Fugue.) |
| `start` | Optional for `http`, required for `stdio`. How Fugue starts it: `command` (a list; a first item with a slash is a file of the module's folder, a bare name is looked up on PATH; the folder is the working directory), `when` (`on_demand` or `never`), `idle_timeout_s` (default 600, 0 never stops it), `start_timeout_s` (`http`: how long it has to answer `describe`, default 30). Without `start` the user runs it. |
| `address` | Default address for `http`. `{port}` here and in `command` is a free loopback port Fugue picks at each start. An instance's own `address` replaces it, and then Fugue does not start anything. |
| `capabilities` | What it may offer; the live list comes from `describe`. |
| `settings` | Fields an instance can set. `secret` fields are stored like keys. |
| `needs` | As in [6.3](#63-the-needs-block), for scheduling. |

A folder whose `provider.json` does not read is skipped with a warning; the other modules still
load. A providers.json entry whose type is no module is `unknown_provider`, with every module id
as the valid ones (and the reason, when that module's file is the one that does not read).

**Protocol.** The same operations on both transports. Over `http` each is
`POST <address>/fugue/v1/<op>` with a JSON body and a JSON reply; the instance key, when there is
one, is sent as a bearer token. Over `stdio` each is one JSON line with `"op"` and an `"id"`
echoed in the reply; the provider writes to stdout only protocol lines (its log goes to stderr, whose
last lines come with a failure).

| Operation | Request | Reply |
| --- | --- | --- |
| `describe` | none | `protocol` (1), `engine` (a stable id of the inference engine, such as `sdcpp`; the module's id when absent), `shares_filesystem`, `free_memory_bytes`, `host`, and `capabilities` (below) |
| `submit` | `capability`, `model`, `params`, `inputs`; over `stdio` also `output_dir` | `job` |
| `status` | `job` | `state` (`queued`, `running`, `done`, `error`, `cancelled`), `progress` (0 to 1), `stage`, optional `preview`, `error` |
| `cancel` | `job` | `state` |
| `result` | `job` | `outputs` (values and file references), `used` (the values actually used) |
| `shutdown` | none | none. Only for providers Fugue started. |

`capabilities` in the `describe` reply is either a list of ids (each is served with the model
files given in the job: one anonymous ready model, as the helper's stage capabilities), or an
object by id:

```json
{"image.generate": {
   "status": "ready", "reason": "", "hint": "",
   "properties": {"edit": false}, "limits": {"max_width": 2048}, "defaults": {"steps": 20},
   "models": [{"name": "toy", "hash": "…", "resource": "diffusion", "title": "Toy",
               "status": "ready", "properties": {}, "limits": {}, "defaults": {}}]}}
```

A model is its name (a string) or an object with `name`, `hash`, `resource` (its resource handle,
[2.5](#25-resource-handles)) and its own `title`, `status` (`ready`, `loading`; anything else is
unavailable), `reason`, `hint`, `properties`, `limits`, `defaults`. What the capability gives for
`status`, `reason`, `hint`, `properties`, `limits` and `defaults` holds for each model that does
not say its own. A capability without `models` is one model that is not named, with the
capability's status (a module that takes model files in the job).

- **Starting.** A started module is started by the first `describe` or `submit`, whichever comes
  first (so a model picker starts a module, unlike the helper, which answers `describe` itself,
  8.10), and stopped after `idle_timeout_s` without a job: `shutdown` is sent, then it is ended
  (terminate after one second, kill after five). A module Fugue did not start is never sent
  `shutdown`. A module that does not answer `describe` in `start_timeout_s`, or exits first, fails
  with `provider_unreachable` and the last lines of its log.
- **Jobs.** Over `stdio` the provider pushes `status` lines (`{"op": "status", "job", "state", …}`)
  as the job changes, and the `result` is fetched by Fugue the moment the job is `done`; Fugue does
  not poll. Over `http` Fugue polls `status` (every 500 ms) and fetches `result` at `done`. One
  job runs at a time per instance ([9.2](#92-scheduling)); a provider may queue.
- Media in `submit` and `result`: over `stdio`, and over `http` to a loopback address when
  `describe` reports `"shares_filesystem": true`, media are `{"path": …}`. Otherwise (the `http`
  default: `shares_filesystem` is absent) media are sent as `{"data": "<base64>", "format": "png"}`
  and a result lists each file as `{"file": i, "format", "size"}`, fetched with
  `GET <address>/fugue/v1/result/<job>/<i>` into the job's folder.
- A provider may stream status instead of being polled: `GET …/events/<job>` as server-sent
  events over `http` (not used by Fugue yet; it polls), or unsolicited `status` lines over `stdio`.
- Errors are `{"error": {"code": …, "message": …}}`. `out_of_memory`, `cancelled` and (`http`)
  `model_missing` keep their codes; any other code is `node_failed` with the provider's message.
  An unreachable address, a refused key (401, 403) or three failed polls in a row is
  `provider_unreachable`.
- A provider process that dies fails the jobs it held and is restarted on the next request.

A new model family that an existing engine already runs needs only a node pack and a model
reference. A new inference runtime is a new provider module. `tests/provider_module_support`
holds a tiny example provider, `fake_module_provider`, over both transports; the tests install
it as a module folder and run it with no code of its own in Fugue.

### 8.9 Models on disk

- Fugue's models folder is `~/.local/share/fugue/models/<type>/`, one subfolder per resource
  type, plus extra search paths set in settings (so an existing ComfyUI `models/` folder can be
  shared without copying).
- A model is identified by content hash, never by path or name. The hash is computed once and
  remembered.
- Under each root (Fugue's folder first, then the settings' paths in order) a resource type's files
  are in the folders its pack declares in `folders`, else in the folder named after its id's last
  part, with their subfolders. A type that declares ComfyUI's folder names (`text_encoders`, `clip`)
  lets a ComfyUI `models/` folder be added as it is. Names starting with "." are skipped; a file
  reached twice (a link, overlapping roots) counts once.
- An extra path may instead hold one kind of model, its files directly in it (a LoRA folder,
  #86). Settings say what each path holds (`model_folder_kinds`: `layout` or a resource type);
  unsaid, a path with any type's folder inside is in ComfyUI's layout, and one with none is the
  kind most of a few of its files are, by their headers (LoRA, VAE, text encoder, diffusion
  model, checkpoint; the type whose pack.json `files` says so). Fugue's own folder is always in the
  layout.
- Finding a handle's file looks at the remembered hashes, then hashes the files with the handle's
  `name` (once). Other files are not hashed to find a model: hashing every model of a large folder
  takes minutes. A missing model is `model_missing`, with the folder to put it in. A check
  (`fugue validate`, the canvas) reads no model file: a file with the handle's name that has no
  remembered hash is taken to be the one, and the run that reaches the node finds out.
- Downloads (#89): a missing model with a `source` (4.3) can be downloaded, from the banner, the
  Templates dialog or `fugue model get`. It goes in the model folder the person picks (settings'
  `download_folder`, remembered; Fugue's own folder by default): under a root in the layout, the
  type's folder that is there (`text_encoders` before `clip` when both are), else its first; a
  root of one kind takes files of that kind only: when the files of one download are not all
  that kind, they all go in Fugue's own folder, so one download is never split. The file is
  the handle's `name` under that folder, a relative path of a file with one of the type's
  extensions. The source must be https (http only to a loopback address); redirects are followed,
  never from https to http.
- A download is written to `<file>.part`, hashed as it arrives, and renamed into place, never
  over a file that is there, only once its sha256 is the handle's; the hash is then remembered.
  A mismatch, or a size other than `requires` records, deletes the part
  (`download_mismatch`). A break is taken up again with a Range request (five times, the wait
  doubling from 2 s; 60 s of silence is a break); a part an earlier Fugue left is hashed and
  resumed; Cancel deletes the part. Free space is checked once the size is known. Files come one
  after another. Any https source is safe to take, since a wrong file is never kept.
- Catalog (#134): Fugue ships lists of models a person can fetch, for any resource type
  (`src/catalog/*.json`, compiled in; the person's own, and updates, are files in
  `~/.local/share/fugue/catalog/*.json`, read after Fugue's, an entry of the same `id` replacing it).
  An entry has the file `name`, an https `url` (http only to this machine), `size`, `sha256`,
  and what a person reads before deciding: `title`, `description`, `scale`, `architecture`,
  `license` (an SPDX id), `license_url`, `commercial` (false when the license forbids it),
  `attribution`, `author` and the `source` page. An entry with no https URL, no 64-digit sha256,
  no size, a file name that is not a plain relative path, or a repeated id is left out and
  reported: a model whose hash was not checked has no place in a catalog. A resource input's
  picker lists the entries of its type that are not on disk (none by that name, none with that
  hash remembered) under "Download", after the files; a typed filter also reads their architecture,
  license, author and tags. Picking one fetches nothing: a question names the license (in the error
  color when it is not for commercial use), the size, the host, and the folder it goes in, and only
  Download queues it, on the same line as every other download (the banner shows it), with the
  checks above. When the file is in place its handle is the input's value, as for a file picked
  from disk. Nothing downloads without that answer (6.7: nothing silent).
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
  The engine links Vulkan for this and only asks about devices; it creates none. The device is
  the first discrete card, else the first integrated one. Without a Vulkan driver or a GPU
  nothing is measured.
- GPU work in the engine and the CLI uses Vulkan compute directly or runs in the helper, never
  Qt RHI: Qt's `offscreen` platform gives neither a Vulkan instance nor a GL context. Fugue links
  no CUDA and bundles no NVIDIA files. The helper is built twice: for Vulkan, in the `fugue`
  package, so it runs on any GPU; and for CUDA, against the system CUDA package, in the
  `fugue-helper-cuda` package, the faster one on NVIDIA cards. Both install to `lib/fugue/`
  (`fugue-helper-vulkan`, `fugue-helper-cuda`).
- On an NVIDIA card (the kernel driver is loaded) where `fugue-helper-vulkan` is the helper found
  and no `fugue-helper-cuda` is installed, the window shows a dismissible notice under the tabs:
  "Faster generation on your NVIDIA card: install fugue-helper-cuda", with a button that copies
  `sudo pacman -S fugue-helper-cuda` to the clipboard (#97). It shows at every start until it is
  dismissed; the dismissal is a marker file in Fugue's state folder
  (`cuda-helper-notice-dismissed`). With the CUDA helper installed, no NVIDIA card, no helper, or
  `FUGUE_HELPER` set, nothing shows. Fugue downloads no helper: the package is the signed,
  updatable way to install it.
- If memory is short, Fugue names the processes holding it (`nvidia-smi --query-compute-apps` on
  NVIDIA, including the GB10) and offers to stop Omarchy's model
  servers, only with confirmation. It never kills another application. In v0.1 the offer is the
  error's fix: a GPU node out of memory with no resident model left to drop names the processes,
  and its hint is `omarchy-agent-local stop` (every running model).
- The helper keeps the models it loaded (one stable-diffusion.cpp context per set of context
  options) between jobs. Before it loads more, it frees its least recently used contexts while the
  device has less free memory than the new files take (their size, plus a fifth); a load or a
  generation that runs out of memory frees the others and is tried once more. Fugue's own
  residency ([9.3](#93-residency)) records the helper as one model of its instance: dropping it
  shuts the helper down.
- stable-diffusion.cpp loads a model as one context of all its files and checks at load that
  every component is there, so every stage job names all of them, even the ones it does not run:
  one `fugue/model` gives the same context to every stage, and the helper keeps that one loaded
  for the whole graph.
- Out of memory is a typed error on the node (`out_of_memory`). A helper crash kills only the helper.
- The component-level helper is based on stable-diffusion.cpp with separate encode, sample and
  decode steps, C++ only. The M0 spike showed the split works
  ([spikes/02](spikes/02-sdcpp-split.md)).
- Fugue carries the split as its own patch against a pinned stable-diffusion.cpp commit; it does
  not depend on upstream adopting it. To keep that cheap: the patch stays small: hooks at the
  existing stage seams and the stage file format, no change to sampling or model code; the pin moves only on
  purpose; and every move of the pin is checked where the patch can break, not model by model
  (stable-diffusion.cpp tests its families): staged runs of Z-Image (LLM conditioning), of SDXL
  with a negative prompt (CLIP conditioning, two sets) and of image-to-image from a latent, the
  Z-Image one giving the same pixels as a single run of the same build; IP-Adapter, PhotoMaker or
  PuLID only when the move changes their code.
- The helper's source is `helper/`: the pin (stable-diffusion.cpp, its ggml and CUTLASS commits,
  with their hashes), the patch `helper/sdcpp.patch`, and `fugue-helper`. `bin/build-helper cuda`
  (or `vulkan`, `cpu`) builds it; `helper/README.md` says how, and how to move the pin. The patch adds image options `fugue.*` that name the files a
  stage reads in place of running or writes after it, and the stage to stop after:
  `fugue.init_latent_in`/`_out` (the start latent), `fugue.cond_in`/`_out`, `fugue.latent_in`/`_out`
  (the sampled latent, after a hires pass), `fugue.stop_after` (`init`, `cond` or `sample`). On
  them `fugue-helper` builds its operations: `encode` (prompts to conditioning), `sample`
  (conditioning, and optionally a start latent, to a latent), `decode` (latent to image),
  `vae_encode` (image to latent) and `generate` (all at once). `fugue-helper <request.json>`
  runs one operation, the request naming it, its files, and the context and generation options
  as stable-diffusion.cpp names them; `fugue-helper --serve` takes the same operations as jobs
  of the 8.8 protocol over `stdio`, for the `fugue-helper` provider (8.7). Both end with
  `_Exit`, without the exit-time teardown, which NVIDIA's Vulkan driver can crash in.
- The conditioning file carries everything the encode step leaves for sampling: every field of
  the conditioning sets, and the state PhotoMaker, PuLID and IP-Adapter set while encoding. A file
  is read only by a model of the version it was written for. Every call VAE-encodes the request's
  images (init, reference, control), so `sample` takes the reference images as `encode` does, and
  a stage with images loads the VAE beside its own component. An inpaint model cannot start from
  a latent alone (its extra input is made from the init image).
- **Masks (#167, designed):** `sample` and `generate` take a one-channel `mask`, and an image
  model's `sample` with a mask takes the picture too: an inpaint model starts from the picture,
  another from its start latent with the mask applied at every step. One patch hook refreshes an
  inpaint model's `c_concat` when a sample reads its conditioning. The design and what the library
  does with a mask are in [14.4](#144-masking-and-inpainting-167).
- **Video (#105)** is the same stages on the same capabilities, no parallel path. A context that
  makes video (`sd_ctx_supports_video_generation`: Wan 2.1 and 2.2, as the pin supports them) is run
  with the library's video options and `sd_generate_video`; the patch gives the `fugue.*` options to
  video too (`cond_in`/`_out`, `latent_in`/`_out`, `stop_after` `cond` or `sample`; a video has no
  `init` stage), shares the file code with the image path (`src/pipeline/fugue_stage_io.h`), and
  hooks `generate_video` at the same three seams: after the text encode, after the sampling loop
  (both Wan 2.2 passes, high noise then low), and before the VAE decode. The conditioning file is
  the same (its `c_concat` and `c_vector` already carry an image-to-video model's extra inputs);
  the latent file's descriptor gains `frames` and `fps`. `encode` and `sample` take the generation
  options of the video (`video_frames`, `fps`, `width`, `height`, `sample.*`; a Wan 2.2 14B
  context also `high_noise_diffusion_model_path`, its UMT5 as `t5xxl_path`); `decode` writes
  `video.y4m` instead of `image.png`, a YUV4MPEG2 file (4:2:0, one `FRAME` per frame) that every
  ffmpeg reads and the helper needs no codec for; over HTTP its format is `y4m`. Fugue's VAE Decode
  Video turns it into the engine's lossless intermediate (FFV1 in Matroska, [2.4](#24-media-handles)).
  An image-to-video model (Wan 2.2 I2V; Wan 2.2 TI2V from its first frame) is staged with a start
  picture, the job input `image` of `text.encode` and `latent.sample` (the library's `init_image`;
  an image model refuses it with `input_invalid`): the encode turns it into the conditioning's
  `c_concat` at the clip's width, height and length, so those are the encode's options too, and the
  sample sends the same picture so the stages make one generation (TI2V starts its sampling from the
  frame, which is not in the conditioning). Wan 2.1 I2V's CLIP-vision output is made in the same
  encode.
  Not staged yet: `vae_encode` of a clip, a latent as the start of a Sampler, S2V and the audio
  models, and LTX/MiniMax (audio and hires latent upscale in the same call); the helper says so
  (`input_invalid`).
  A video latent split across machines (text encoder and VAE decode on a GB10, the sampling loop on
  a discrete card, or two clips at once on two hosts) uses the paired-host transport as it is.

### 8.11 The provider interface

Inside the kernel a provider is one interface with exactly the operations of
[8.8](#88-provider-modules): `describe`, `submit`, `status` (or a status stream), `cancel`,
`result`, `shutdown`, with the same data in and out. The engine only ever talks to this
interface.

- M0 builds the interface, a fake provider for tests, and the `omarchy` provider as an adapter
  compiled into Fugue: it implements the interface by calling the Omarchy servers' own HTTP
  APIs. No inference code is linked into Fugue; the engine it talks to is still another process.
- The module adapter (#31) implements the same interface by speaking the protocol of 8.8 to a
  third-party program: `HelperProvider` (`stdio`) and `RemoteHelperProvider` (`http`) are the two
  adapters, each used by the shipped helper and by modules, with a `StdioLaunch` or
  `RemoteProviderOptions` that says what differs (the program and its arguments, the wording of
  messages, whether describe is asked of the process). The registry lists `omarchy` and
  `fugue-helper` as modules with transport `builtin`, ahead of the installed ones, so shipped and
  third-party providers are listed the same way; `omarchy` itself stays an adapter to its servers'
  own APIs, because those servers do not speak 8.8.
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
- **A node of several jobs** (a video through an upscaler a chunk at a time, #138): after each
  finished job the kernel's `next` may return the next job, to the same instance with the node's
  work folder as its output folder; the node holds its lane throughout. A job says which part of
  the node's bar it covers (`progressFrom`, `progressSpan`). A kernel keeps no state of its own, so
  what comes between the jobs is a file in the work folder, and `abandon` is called when the node
  ends in an error or a cancel so it can remove what is left (the scheduler never removes files).
- One run is active at a time. Further runs queue first in, first out, can be moved to the front
  ("run first"), and can be removed or cancelled by id ([9.8](#98-runs-across-processes)).

### 9.3 Residency

The scheduler is a residency planner, not a memory manager: each model server holds one model,
and swapping means stopping and starting.

- Among ready nodes it prefers the ones that reuse a model or helper state already loaded.
- On out-of-memory it evicts the least recently used model, not everything.
- A provider's model is recorded as held by its instance. When the last one of an instance is
  dropped, the instance is sent `shutdown`: a provider Fugue started (the helper) gives its
  memory back that way, and one it did not start ignores it. A model of an instance with a job
  in flight is not dropped, nor a node's own models (its persistent program among them).
- Dropping a model returns once its memory is back (a program or the helper has exited, or was
  killed after the grace period), so free memory is measured again only then.
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
| `node.cached` | `node`, `outputs`, `shown` (a sink's inputs), `frozen` (true when it is the node's frozen result, 10.4) |
| `node.done` | `node`, `outputs`, `seconds`, `shown` (a sink's inputs) |
| `node.skipped` | `node`, `reason` (`muted`, `upstream_muted`, `upstream_failed`, `upstream_empty`, `cancelled`) |
| `node.error` | `node`, `error` |
| `run.done` | `status` (`ok`, `failed`, `cancelled`), `errors` |

A sink (Preview, which has no outputs; Save Image and Compare Images, which have) reports `shown`,
its inputs as resolved (`{"value": ...}`), on `node.done` and `node.cached` and in the node's entry in run.json,
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

v0.1 has no daemon. The window runs its own runs, in its own process, and its queue entries are
live while that process lives (Appendix A). `fugue run` from the CLI starts a detached
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
- the pack id and the version of the pack it came from, for a node from a pack on disk (the
  built-in pack's versions are the nodes' own);
- every input: a literal's value, or for a link the content hash of the upstream output;
- the identity hash of every resource handle ([2.5](#25-resource-handles));
- for provider nodes, the engine id ([8.11](#811-the-provider-interface)) and the identity of
  the model the provider resolved;
- for a node with `deterministic: false`, the run id;
- for a node that reads data of its own which decides its answer, that data's fingerprint (a
  kernel's `keyExtra`; Enhance Prompt's template, so editing a template is a new result). Absent
  for every other node, so their keys are as they were.

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

- A size limit, default 10 GB (`cache_results_gb`, 0 for none), configurable. Over the limit, the least recently used entries
  are evicted, when a run ends and when the cache is cleaned ([10.6](#106-cache-management)). Frozen entries are never evicted.
- Clear all, per workflow or per node (`fugue cache clear`, and Settings > Cache).
- Garbage is collected after a grace period of 7 days: run folders whose run has ended, blobs no
  key record refers to, and key records whose blobs are gone. A cached result is otherwise kept
  until it is evicted for size or cleared; it does not expire with age.

### 10.6 Cache management

Fugue keeps three kinds of copy under `~/.cache/fugue`, and each can be measured, cleared and
kept within a limit (#141):

| Kind | Folder | Holds | If it is gone |
| --- | --- | --- | --- |
| `results` | `results/` | The result cache ([10.2](#102-store)). | The node runs again. |
| `thumbnails` | `thumbnails/` | The 512 px pictures nodes show ([12.7](#127-inspection)), one per image by content hash. | Made again when the result is shown (a cached node shows its result at once on the next run). |
| `runs` | `runs/` | Run folders ([9.7](#97-run-folder)): events, the compiled graph, working files. | The run's history (`fugue runs status|events|result`) is gone; its results are not. |

- **Never removed:** the output folder and the inputs folder (they are not under the cache),
  frozen results, and a run that is queued or going (the queue's entries, and a folder whose run
  has not ended). A run folder's files that are links into the store free nothing, so a run is
  charged only for the files it alone holds.
- **Safe with the window open.** Results and previews are removed only while the engine lock
  ([9.8](#98-runs-across-processes)) is held by the cleaner, so a run never loses a file in the
  middle and a run that waits for its turn starts after the cleaning. When a run is going, `cache
  clear` leaves results and previews alone and says so (`cache_busy`); run folders are always
  safe to clear.
- **Limits** (settings.json, [1.4](#14-files-and-folders)): `cache_results_gb` (default 10; 0 is no limit),
  `cache_thumbnails_gb` (default 1; 0 for none) and `runs_kept` (default 20). Results over the
  limit lose the least recently used first, frozen ones excepted, a result being used again
  counts as used now. Previews over the limit lose the oldest first (a preview is touched each
  time its picture is shown), and any not shown for 7 days go. An ended run goes 7 days after it
  ended, except the newest `runs_kept`, which stay whatever their age.
- **When:** a run's turn (before it starts, at most once an hour), a run's end (the result
  limit), and the window cleans at start and again every 30 minutes while it is idle. Never
  while a run is going: the cleaner takes the engine lock and skips when it cannot.
- **By hand:** `fugue cache status|clear` ([13.1](#131-commands)) and Settings > Cache: the size of
  each kind with a Clear button that first says what is lost (results: the nodes run again, so
  what took minutes takes minutes; previews: pictures on the canvas show again after the next
  run; runs: the history), and the limits. File menu > Clear image preview cache opens that
  question for previews.

---

## 11. Workflow files

### 11.1 Format and place

- One format: the `.fugue` document of [section 4](#4-the-workflow-document).
- Workflows are ordinary files wherever the user saves them. File dialogs go through the desktop
  portal, never `QFileDialog`. Fugue keeps a recent list and suggests `~/Documents/Fugue`.
- Templates ship with the app, and the person has a templates folder of their own ([11.10](#1110-templates)).

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
- A **package to share** (#91, File > Export for sharing…, `fugue workflow export --share`): one
  `.fuguepack` file, a zip (MIME type `application/x-fugue-package`, which the desktop entry
  opens with Fugue), holding a template (11.10): `<id>/template.json` (the workflow's name), `workflow.fugue`
  (the share copy, holding the definition of every group it uses, a library's or a pack's too,
  and its `requires` naming the packs it uses and where to get them, 4.3),
  `inputs/` (each image a `file` input names that Fugue takes in, 14.1: by its bytes, at most
  256 MB; renamed to letters, digits and `._-`; the input then names `inputs/<name>`) and
  `preview.<ext>` (the newest picture a Save Image or Preview shows). Only such images go in,
  so a package never carries another file a node names (a key, a document): that input keeps
  its bare name, as one whose file is gone does, and the export's notice names each, and the
  images it put in. At most 3 GB of images. No model is in it: models stay references by hash,
  source and size, which the person downloads (8.9). Never written over a file.
- Opening a package (File > Open, a drop, the desktop; told by its first bytes, so one renamed
  `.zip` opens too) unpacks it and opens it as
  its template opens: a new unsaved workflow, its images taken into the inputs folder, the
  banners naming its missing models (Download) and the first pack it says where to get
  (Install…, which opens Settings > Packs at the recorded source and commit, asking there).
  It is unpacked under the cache folder, not `/tmp` (memory on Arch), and removed once open;
  an input naming a file it did not hold keeps the bare name. Only plain files and folders with
  relative names inside it are taken: an absolute name, `..`, `\` or `:`, a link or a device is
  refused, and so is a file over 256 MB, more than 4 GB or 10000 entries; macOS's `__MACOSX`
  and dot files are skipped. A zip of the folder's contents (files at its top) is taken too,
  into a new folder.
- The compiled graph, for the CLI (`fugue compile`; File > Export the compiled graph… writes
  the same JSON, never over a file).

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
- Container detail: a PNG `iTXt` chunk with the keyword `fugue`; in WebP, the XMP chunk; in a video or sound (Save Video, Save Audio), a container tag `fugue` (MP4 `use_metadata_tags`, WebM and Opus/Ogg tags, FLAC Vorbis comment, MP3 ID3 `TXXX`) written by ffmpeg from an ffmetadata file, since a workflow is longer than one argument may be. The file's own other tags are replaced when a workflow is embedded.

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

### 11.10 Templates

Ready-to-run workflows, listed by File > Browse templates (`file.templates`) and `fugue template
list`, and copied out by opening one or by `fugue workflow new <file> --template <id>` (#83).

- A template is a folder: `template.json` (title, description, category, tags), `workflow.fugue`,
  a preview (`preview.webp`, `.png` or `.jpg`) and `inputs/`, the sample pictures its Load Image
  nodes name as `inputs/<name>`. A root of templates may hold `categories.json`, the order and
  titles of its categories. Nothing indexes the templates: a root's folders are its templates.
- A template folder may also hold `packs/<id>/`, the exact pack its nodes need (a pack folder,
  [6.4](#64-packjson)), so it runs whatever Fugue ships. It is offered, never
  installed on its own: the banner of the opened template says it carries the pack and its button
  goes through the install-and-trust questions of [6.6](#66-installing) and [6.7](#67-trust); a card
  says "Carries the pack to install"; `fugue template list` gives `carried_packs` and `fugue
  workflow new --template` names the `fugue pack add` to run. The workflow's `requires.packs`
  stays the record of the pack's version and where else to get it (4.3); a carried pack of another
  version than that record is a problem of the bundled templates. A shared package carries none.
- What a template needs is read from its workflow, not written again: its models (handles, with
  each `source` in `requires.models`) and the node versions it was made with. A template whose
  built-in nodes this Fugue lacks, or has at another major or an older minor ([3.6](#36-versions)),
  is not listed; one whose pack is not installed, or installed at another version, is listed
  with the pack named.
- Two roots: the person's own folder (`~/Documents/Fugue/templates`, listed first, where a plain
  `.fugue` file is a template too) and the bundled one. The bundled templates are the
  `bmaltais/fugue-workflows` repo, a submodule (`templates/`) at the commit a release pins,
  installed to `share/fugue/templates`; `$FUGUE_TEMPLATES` names another. Its CI checks every
  template against Fugue's main branch; Fugue's tests check the pinned ones.
- The dialog shows categories, a search over title, description and tags, and cards with the
  preview and what this machine lacks (models, packs; a check, reading no model file). Opening
  one makes a new unsaved tab named after it, its samples taken into the inputs folder as a
  pasted image is, so it runs as it opens and a save keeps it portable. The CLI writes the
  workflow with its samples copied into `inputs/` beside it, never replacing a file: a sample
  whose name is taken by another file takes `-2`, `-3`..., and the workflow names that. A sample
  must be a file inside its template's folder; one that leads out, by `..` or a link, is not read.
  The person's own template wins over a bundled one of the same id, even when it is broken.

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

Two presets, chosen in settings (`navigation`, applied at once):

| | Legacy (default, ComfyUI's default) | Standard |
| --- | --- | --- |
| Left-drag on empty canvas | pans | selects |
| Wheel | zooms at the pointer | pans |
| Select with a marquee | Ctrl+drag | left-drag |
| Zoom | wheel | Ctrl+wheel |

In both: middle-drag and Space+drag pan. A touchpad is told from a mouse wheel by its scroll
phase (anything but `NoScrollPhase`) alone, never by the device type (Qt on Wayland reports an ordinary mouse as a touchpad) and never by `pixelDelta`, which a mouse wheel
can carry too (Wayland, libinput high-resolution wheels). A mouse wheel zooms by 1.1x per 120
units at the pointer (legacy) or pans (standard). On a touchpad, scrolling pans and a pinch zooms.
A wheel over an open multiline field scrolls the field when it has more text than shows. Right-click opens the context menu; right-drag does
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
| `run.cancel` | Cancel the run | Ctrl+Alt+Enter, Escape while running (Escape cancels first, then leaves a group; while the queue panel has the focus, Escape closes it instead) | Escape added |
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
| `panel.queue` | Toggle the run queue panel | Alt+Q | added (ComfyUI's `q` is Quit here) |
| `view.toggle-menu-bar`, `view.toggle-tabs`, `view.toggle-run-toolbar`, `view.toggle-selection-toolbar`, `view.toggle-breadcrumb`, `view.toggle-run-status`, `view.toggle-zoom-readout` | Show or hide each bar | none (View > Bars and panels) | added |
| `view.focus-mode` | Hide all bars and panels; the same key brings them back as they were | H | added |
| `view.reset-bars` | Every bar back, the panels closed | none (View > Bars and panels) | added |
| `app.menu-bar` | Open the menu bar | F10 | added |
| `app.context-menu` | Open the context menu | Shift+F10, Menu | added |

Commands with no default key (the colors `node.color.<role>`, `edit.clear`, the exports, the help
links, the `view.toggle-...` bars, `view.reset-bars`) are in the menus, and the keymap file can bind any of them.

Align and distribute (#58): `node.align-left`, `-right`, `-top`, `-bottom` put the selected nodes
on the selection's outermost edge; `node.distribute-horizontal` and `-vertical` even out the gaps
between three or more, in the order they start, never closer than 24 units: nodes that do not fit
the span get that gap and the run grows past it (ComfyUI's Align Selected To and Distribute Nodes).
They have no default keys; they are in the Node menu, and in the node's menu when enough nodes are
selected (two to align, three to distribute). A pinned node stays where it is. One undo step each.

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
- The menu bar and the context menus list command ids; each item's name, key and enabled
  state come from the registry when the menu opens, and a command not built yet shows as
  unavailable. Alt+letter opens a top menu where 12.4 binds nothing to it.
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

A double click on a node's picture (Preview, Save Image, Load Image, any result; a video's, its player, [14.2](#142-m4-the-rest-of-v01)) opens it in a large
view (#73): the full file, the wheel zooms at the pointer, a drag pans, Fit and 100% (keys 0 and 1, + and -),
the pixel size and the zoom shown, Escape closes. A Compare Images node's opens its own viewer (#62); a
Crop node's opens the crop window ([14.1](#141-m1-the-first-twelve), `fugue/crop`).

### 12.8 Node search

Search-first node creation. Results show category, ports and version, and are filtered by port
type when opened from a dragged link. Groups appear in search like any node type.

### 12.9 Furniture

v0.1 has collapse, hide unused ports, reroute points and Frames. A minimap and snap-to-grid are later.

### 12.10 Several workflows

Tabs inside one window. Each tab tracks its own unsaved state and its own undo stack.

### 12.11 Undo, look and language

- Every edit is a command on one undo stack per workflow, group edits included.
- The window follows the Omarchy theme live and the desktop text size. The canvas corners
  (nodes, wells, chips, menus, popups, thumbnails, search) are 8 px unless the corner radius
  setting says otherwise: 0 to 16 px, or "Follow Omarchy" for Hyprland's rounding (#63).
- Translations use the app template's translator, English first, with right-to-left layouts working.

### 12.12 Better than ComfyUI on purpose

Run to here; reasons for invalid connections; search filtered by port type; a Frame and a Group
as separate things; visible cache and frozen state; per-node errors with group paths; one undo
stack across everything; one file format; on-disk recovery; per-output control of what is
embedded; versioned node types; no shared Python environment; a queue panel that lists runs
started from a terminal too ([12.13](#1213-run-toolbar-and-queue-panel)).

### 12.13 Run toolbar and queue panel

A toolbar floats over the canvas (#60, M3), as ComfyUI's action bar, and a panel hangs from it. Both
read and write the queue of [9.8](#98-runs-across-processes) and the commands of [12.4](#124-default-keymap);
they add no queue of their own.

- **Toolbar** (`RunToolbar.qml`): a grip, the batch count, **Run** with a menu behind it (the Run menu's
  rows: Run, Run first, Run to here, Cancel), **Cancel**, and the **queue** button with the number
  of runs going and waiting. It floats anywhere over the canvas (#120): dragging the grip moves it
  across as well as up and down, and the arrow keys on the grip move it by 2u (Shift: 10u). It stays
  inside the canvas, 4u from the edges, also when the window is resized, and a drag is pulled
  lightly (within 3u) to the edges and the middle. It starts centered at the top. The place is
  remembered with the batch count in `~/.local/state/fugue/run-bar.json` as fractions of the room
  the bar has (`place`: x and y, 0 to 1), so a resized window keeps it where it was relative to the
  edges; an older file's `dock` ("top" or "bottom") is read as the centered top or bottom place. `view.toggle-run-toolbar`
  (View > Bars and panels) hides and shows it. A thin bar under it shows the progress of the node going.
- **Batch count** (1 to 99): one Run makes that many runs, queued together in order, each taking the
  document as it is then, so a seed set to increment gives each its own number (ComfyUI's batch
  count). It applies to Run, Run first and Run to here.
- **Queue panel** (`RunQueuePanel.qml`, `panel.queue`, Alt+Q, or the toolbar's queue button): hung from
  the bar wherever it is, centered on it (held inside the canvas), under it when there is room, else
  on the side with more room, and kept shorter to fit there. *Running and
  waiting* lists the run going first, then the waiting ones in queue order, **whoever started them**:
  this window, or `fugue run` from a terminal. A row has the workflow's name, the node going with its
  progress or "Waiting", where it came from, the time since it began (or was queued), **move to front**
  (waiting runs but the first) and **cancel**. *Finished* lists the last eight runs that ended, newest
  first: how it ended, how long it took, when; a click on the row **shows its results on the nodes**
  (when it is the workflow of the tab shown: a run's node ids mean something only there), and a
  button opens its run folder. Cancel of a run of this window goes through the run thread; of any
  other run it does what `fugue runs cancel` does, so both ways end a run the same way. Move to
  front renames a waiting entry into class 3 ahead of the other waiting ones (9.8); it never goes
  ahead of the run going.
- The panel and the toolbar are made only when shown, and the window keeps its item budget.
  The panel reads the queue folder (a file watch, nothing at idle); only while it is open does it
  read the going run's events once a second and list the history.

### 12.14 Side panels

A dockable panel beside the canvas (#56), as ComfyUI's sidebar, with two tabs: the **node library**
(`panel.nodes`, N) and the **workflows panel** (`panel.workflows`, W). One is open at a time: its key
opens it, the same key closes it, the other key replaces it. Each is a checked item in the View menu's Bars and panels submenu
while it is the open one. The canvas gives up the panel's width; the panel is made only while open,
and the window keeps its item budget.

- **Tab strip** (#159, `SideTabStrip.qml`): a slim vertical strip on the window's **left edge**, under the menu bar and
  the workflow tabs and beside the canvas, with one icon per registered panel (a table in `panels/side_panels.cpp`:
  tab id, registry id, command, Lucide glyph, name; a new panel is one row and gets a tab). A click opens the panel,
  a second click on the same tab closes it, a click on the other tab replaces it, exactly as the keys do (one path:
  `SidePanels::toggle`, so the strip, N and W and the View menu's checks always agree). The open tab is marked (accent
  glyph, a wash, a bar on the edge); the tooltip says the name and its key, "Node library (N)"; every tab takes Tab
  focus and Enter or Space; the strip is 44 px wide with 44 px tabs and 24 px glyphs. It is always there, also with
  no panel open (it is the way in), except in focus mode (12.15), which hides it like every other bar (registry id
  `side-tabs`, no toggle of its own; a panel asked for in focus mode still shows). When the window is narrow the
  panel gives way, never the strip: the canvas keeps at least 160 px. The strip stays on the left when a panel is
  docked on the right. The run queue (`panel.queue`) is a floating panel of the run toolbar, not a docked side
  panel, and the media results are a view inside the workflows panel; neither has a tab (follow-ups: a queue and a
  results tab once they are docked panels).
- **Container** (`SidePanel.qml`): a title bar with a button that moves the panel to the other side
  and one that closes it, the content below, and a grip on the canvas-facing edge (drag, or Left and
  Right on it with the focus) that sets the width, 200 to 640 px. It knows nothing of the canvas or
  of what it holds. Which panel is open, its side and its width are remembered in
  `~/.local/state/fugue/side-panels.json`; a window starts with what it closed with.
  The sides are the window's, left and right: the canvas never mirrors, and neither does the side.
- **Node library** (`NodeLibraryPanel.qml`, `search/node_library.h`): a search field over every node
  type in a tree by category (the `/` parts of `category`; closed at first, a click, Enter, Left and
  Right open and close one, with the count of nodes under it). The filter is node search's own
  ranking ([12.8](#128-node-search)), so a word finds the same nodes in both, and a query opens every
  category on the way to a match. A **click or Enter adds the node** in the middle of the view; **dragging
  a row onto the canvas** adds it where it is let go (a drop over the panel adds nothing); one undo step
  either way, as with search. A node's tooltip is its description. Groups of the library
  (`local/<name>`) are listed with the other nodes, and each has a **trash can** at the far end of
  its row (a built-in node and another pack's node have none; a pack is removed from Settings >
  Packs). The can, or Delete on the row, opens a separate question that names the group and says
  that only the library copy goes, that workflows already using it keep their embedded copy and
  still open and run, and that it cannot be undone; Cancel is the default (Enter and Escape), Delete
  comes after it. After a yes the row leaves the panel and node search at once and the notice line
  says "Deleted <name> from the library". It is the same call as Settings > Packs > Library > Remove
  (`PacksModel::removeLibraryGroup`).
- **Workflows panel** (`WorkflowsPanel.qml`, `workflow/recent_files.h`): *Open* lists the tabs, with
  their color, an unsaved dot and a close button (a click shows one; Delete closes it);
  *Recent* lists the last 12 files opened or saved that are not open now and still exist, newest first,
  kept in `~/.local/state/fugue/recent-files.json` (a click opens one; one that fails to open is
  dropped). The library of groups ([5.3](#53-definitions-copies-and-the-library), #26) is a third list here later.
- **Results view** (`media/`, #133): a Files / Results switch at the top of the workflows panel. Results
  shows the images, videos and sounds the *current workflow's* runs made, in the **media browser**
  (`MediaBrowser.qml`), made only while Results is shown. The browser is standalone: it takes a list of items
  `{url, kind, title, thumb, group}` and details of the current one, and says the current item and the
  action asked (`actionRequested`, `activated`); it knows nothing of the canvas, files or runs. Modes: *Grid*
  (rows under a heading per run, newest first), *Filmstrip* (the current item large over a strip), *Single*
  (one item large; a video or sound in `MediaView`). Keys: Left/Right (PageUp/PageDown) step, Up/Down move a
  grid row, Home/End, Enter opens the item large, Escape goes back to the mode it came from and then asks to
  close, I shows the details. Actions: Open, Show folder, Copy path. The core is plain C++
  (`media/media_items.h`): a run's results are the media outputs of the nodes nothing else reads (from
  `workflow.json`), `mediaOfWorkflow` takes the newest 40 runs whose `run.json` names the workflow's path
  (an unsaved workflow has none), a file in several runs stays with the newest, a missing file is left out;
  `embeddedSummary` reads prompt, seed and model from a PNG or WebP's embedded workflow (11.7). Thumbnails
  (`media/thumbnail_cache.h`) are PNGs of at most 256 px in `~/.cache/fugue/thumbs/`, named by a hash of path,
  size, modification time and box, made off the main thread by the `image://fuguemedia/` provider the first
  time and read back after. The panel refreshes the list when Results opens, the workflow changes, a run ends
  or the refresh button is pressed.
- **Not built** (follow-ups of #133): hosting the browser on a node's face and in the large view, a Media
  Browser node, Preview showing a batch as a filmstrip, Compare Images taking two items from the list;
  thumbnails of videos (a frame) and the details of a video or sound (container tag, 11.7); saved files that
  are in no run folder (a scan of the output folder grouped by embedded workflow); favorites, delete,
  re-run with the same seed, send to a Load Image node, open the workflow that made it, drag back onto the
  canvas; remembering the mode and view; the queue's "show results" row opening the browser; zoom and
  fine-grained image compare in Single.
- **Not built** (follow-ups): a model library tab (M), assets, a log or terminal panel at the bottom,
  bookmarks and a preview of a node in the library.

### 12.15 Bars and panels: the menu and focus mode

Every bar and panel can be shown and hidden (#129), as Photoshop's Window menu does. One small registry
keeps which are shown (`panels/chrome_visibility.h`: plain C++, no QML, tested alone); each bar
registers an id with it and the window shows the bar while the id is shown. The commands, the menus,
focus mode and the file all read that one place.

| Id | Command | What it is |
| --- | --- | --- |
| `menu-bar` | `view.toggle-menu-bar` | the menu bar |
| `workflow-tabs` | `view.toggle-tabs` | the workflow tabs (12.10); still only from the second workflow on |
| `run-toolbar` | `view.toggle-run-toolbar` | the run toolbar (12.13); the queue panel hangs from it and goes with it |
| `selection-toolbar` | `view.toggle-selection-toolbar` | the toolbar above a selection |
| `breadcrumb` | `view.toggle-breadcrumb` | the group breadcrumb |
| `run-status` | `view.toggle-run-status` | the run pill at the top right |
| `zoom-readout` | `view.toggle-zoom-readout` | the zoom pill at the bottom left |
| `side-tabs` | none | the side tab strip (12.14): shown by default, hidden by focus mode, no toggle of its own |
| `node-library`, `workflows-panel` | `panel.nodes`, `panel.workflows` | the side panels (12.14), one at a time (an exclusive group of the registry) |

- **Bars and panels** (a submenu at the end of the View menu, after fit, zoom and links, with **Focus mode** under it): one row per bar with a check while it is shown, then the
  panels (`panel.nodes`, `panel.workflows`, `panel.queue`), then **Focus mode** and **Reset bars and
  panels**. The same menu is a submenu of the canvas's context menu, so a right click finds it even
  with the menu bar hidden.
- **Focus mode** (`view.focus-mode`, **H** on the canvas): hides every bar and panel; H again brings
  back exactly what was shown, no more and no less. Tab is not used: it already enters a group and opens
  node search (12.4). While it is on, what is asked for shows alone: a panel opened with N or W, the
  menu bar brought back with F10; leaving focus mode keeps it, with the rest. Focus mode is not
  remembered: a window always opens with its bars, so a window closed in focus mode loses nothing. A line
  at the bottom says how to leave it for four seconds.
- **Nothing is lost.** The menu bar is the only bar that holds the commands to bring the others
  back, so it has three ways back: F10 (`app.menu-bar`) shows it first when it is hidden, then opens it;
  the canvas's context menu has the same submenu; **Reset bars and panels** (a key if the keymap file
  binds `view.reset-bars`) shows every bar and closes the panels. Hiding the menu bar says so for four
  seconds: "Menu bar hidden. F10 brings it back." A newly hidden bar keeps its command in the keymap:
  any `view.toggle-...` can be bound in the keymap file (12.5).
- **Remembered** in `~/.local/state/fugue/chrome.json`: `{"visible": {"<id>": true|false}}`, an entry per
  choice made. An id this version does not know is kept as it is; a file that cannot be read gives the
  defaults. The run toolbar's older `visible` in `run-bar.json` and the side panels' older `tab` in
  `side-panels.json` are read once, when `chrome.json` has nothing for them; those files keep the toolbar's
  place and batch count and the panel's side and width.
- The queue panel (`panel.queue`) is open or not for the window's life, and is not an id of its own:
  it hangs from the run toolbar, so hiding the toolbar or entering focus mode hides it, and it comes
  back with the toolbar.
- Not built: workspaces (a named set of shown bars, Photoshop's Essentials), a minimap (12.9, later), a
  status bar and a bottom panel (none exist).

---

## 13. CLI and agent interface

Fugue must be usable by agents. The design is one command table: every operation exists once and
is exposed through the CLI now, and later through an MCP server and a live-session channel. There
are never three implementations.

### 13.1 Commands

Each command works with no display, never opens a dialog, and takes `--json`. For `node`,
`pack`, `provider`, `model` and `template` the plural is an alias (`fugue nodes list` is `fugue node list`).
`fugue run` starts a run and `fugue runs` manages runs; those two are different commands.

| Command | Does |
| --- | --- |
| `fugue run <file> [--wait] [--first] [--to <node>] [--provider <name>] [--set <node>.<input>=<value>]` | Start a run. Prints a run id and returns; `--wait` streams events and exits when it ends; `--first` queues it ahead of waiting runs. Short for `fugue workflow run`. |
| `fugue validate <file>` | Compile and report errors and warnings; execute nothing. Short for `fugue workflow validate`. |
| `fugue compile <file>` | Print the compiled graph. Short for `fugue workflow compile`. |
| `fugue workflow new <file> [--template <id>]` | Write an empty workflow, or a template's ([11.10](#1110-templates)) with its sample inputs beside it. |
| `fugue workflow describe <file>` | A readable summary: nodes, links, sinks, what is missing. |
| `fugue workflow export <file> --share [--no-prompts] [--no-layout] [--out <file.fuguepack>]` | Write the package to share (11.5, #91), by default `<name>.fuguepack` in the current folder (`.fuguepack` added to a name that does not end so), never over a file; a file input whose file is gone is a warning. |
| `fugue runs list` | Recent and queued runs. |
| `fugue runs status <run-id>` | State and per-node progress. |
| `fugue runs events <run-id> [--follow]` | The event stream as JSON lines. |
| `fugue runs result <run-id>` | Sink outputs: values and file paths. |
| `fugue runs cancel <run-id>` | Cancel. |
| `fugue node list [--type <port type>]` | Installed node types. |
| `fugue node describe <type id>[@<major>]…` | Full manifests as data. |
| `fugue node new <name>`, `fugue node test <path>` | Authoring ([6.11](#611-authoring-tools)). |
| `fugue pack list`, `add <folder\|git-url> [--ref <tag\|commit>]`, `remove <id>`, `update [id] [--ref <tag\|commit>]`, `trust <id>` (an `<id>@<major>` where majors of a pack sit side by side) | Packs ([6.6](#66-installing)). All but `list` need a human ([13.6](#136-safety)). |
| `fugue provider list` | Instances, their capabilities and status. |
| `fugue template list` | The templates, each with the models it names (`here` or not, and the folder a missing one goes in) and the packs it lacks; what is wrong with one is a warning. |
| `fugue model get <file\|template id> [--into <model folder>]` | Download the models a workflow or template names that are not here and have a `source` (8.9, #89), into the model folder downloads go in (`--into` names another, for this call); progress on stderr. Each failure is an error; what arrived stays. |
| `fugue model list [--type <resource type>] [--hash]` | Models found on disk and offered by providers. A file on disk has a `handle` only once its hash is known (listing reads no model file); `--hash` hashes the others, once each, so every file gets a full handle; a file it cannot read is a `media_unreadable` warning. |
| `fugue cache status` | What Fugue keeps under `~/.cache/fugue` ([10.6](#106-cache-management), #141): per kind (results, thumbnails, runs) the bytes, files, oldest and limit, and the results per workflow and per node. |
| `fugue cache clear [--kind results\|thumbnails\|runs\|all] [--workflow <file>] [--node <id>] [--older-than <age>] [--dry-run]` | Clear one kind (results unless `--kind` says), narrowed by workflow (results and runs), node (results) and age (`30d`, `12h`, `2w`); `--dry-run` says what would go and changes nothing. Frozen results, saved outputs and a run going or queued are never removed; while a run is going its results and previews are left alone (the `cache_busy` warning). |
| `fugue command list` | The command registry and current key bindings. |
| `fugue skill install [--target agents\|claude\|all] [--dry-run] [--force]`, `fugue skill uninstall` (same options) | Install, update or remove the bundled agent skill ([13.5](#135-agent-skill)). |

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
| Files | `file_not_found`, `invalid_json`, `invalid_document`, `unsupported_format_version`, `permission_denied`, `media_unreadable`, `download_failed` (no source, a source or name Fugue does not take, an error status, a break it gave up on, no room, a file at the name), `download_mismatch` (the file is not the handle's, by hash or size; deleted) |
| Graph | `unknown_node_type`, `node_version_mismatch`, `node_newer_major` (a warning), `unknown_input`, `missing_input`, `unknown_link_source`, `unknown_output`, `type_mismatch`, `value_out_of_range`, `value_not_in_choices`, `cycle`, `group_recursion`, `unknown_provider` |
| Warnings | `not_part_of_output`, `model_missing` (an error when a run reaches the node, 8.5), `model_mismatch` (a text encoder, VAE or LoRA that does not fit its diffusion model, by the files' headers, #85, #86; an error when a run reaches a stage), `model_unsupported` (a model file in a format the helper cannot load, such as ComfyUI's mixed quantization; likewise), `frozen_result_missing`, `pack_update_available`, `workflow_not_embedded`, `node_bad_preview` |
| Trust | `pack_untrusted`, `needs_confirmation` |
| Packs | `pack_invalid`, `pack_exists`, `unknown_pack`, `fetch_failed`, `setup_failed` |
| Groups | `group_invalid` (an edit a group cannot take: a list input fed from both sides of the selection, a name taken or not valid, a port exposed already) |
| Providers | `capability_unavailable`, `model_not_running`, `provider_unreachable` |
| Masks (#167) | `mask_empty`, `mask_unsupported`, `stitch_mismatch`; warnings `mask_full`, `mask_resized` ([14.4](#144-masking-and-inpainting-167)) |
| Running | `node_failed`, `node_bad_output`, `node_timeout`, `out_of_memory`, `cancelled`, `node_test_failed` (`fugue node test`: an output is not what the test expects) |
| Cache | `cache_busy` (a warning: `fugue cache clear` left the results and previews alone because a run is going) |
| Skill | `skill_modified` (`fugue skill install\|uninstall`: the installed skill is not the one Fugue wrote, so it is left alone) |
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

The binary carries one skill, `fugue` (`share/skills/fugue/SKILL.md`, compiled in), a folder with a
`SKILL.md` and `name` and `description` frontmatter, the layout Claude Code, Pi and the other
agents that read `~/.agents/skills` share. It teaches an agent the loop: discover node types
(`fugue node list`, `fugue node describe`), write the `.fugue` file (4.2), `fugue validate`,
`fugue run --wait`, `fugue runs result`, and how to write a node; and it names what an agent cannot
do (13.6). It does not copy node manifests, which would go stale: it sends the agent to
`fugue node describe`.

`fugue skill install [--target agents|claude|all] [--dry-run] [--force]`:

- `agents` is `~/.agents/skills/fugue`, the one real copy (Pi reads that folder, so Pi needs no
  copy of its own; a name in both Pi folders would shadow). `claude` is Claude Code's skills folder,
  `~/.claude/skills/fugue`, or `$CLAUDE_CONFIG_DIR/skills/fugue` when that variable is set. `all`,
  the default, writes the agents copy and makes Claude's a link to it, so an update is one write.
  `--target claude` alone writes a copy of its own.
- It writes `SKILL.md` and `.fugue-skill.json` (the Fugue version and the SHA-256 of what it wrote).
  Run again it is an update: an unchanged copy is `unchanged`; a copy that still matches what an
  earlier install recorded is replaced by the new one.
- It never overwrites or removes a file the person changed, nor a folder or link it did not make:
  that is `skill_modified`, and the whole plan fails before anything is written. `--force`
  sets the old one aside as `<name>.bak-<time>` and goes on. A copy of its own that Claude holds
  becomes the link when `all` is installed.
- `--dry-run` prints the plan (`create`, `update`, `link`, `unchanged`, `remove`, ...) and the
  full paths, and writes nothing. Every run prints where it wrote; `--json` carries the same as
  `data.actions`.
- `fugue skill uninstall` takes the same options and removes only what an install made, the link
  first. Both need no confirmation: they write fixed folders under the home folder, from text the
  binary carries, so the file roots of 13.6 do not apply.

A test keeps the skill in step: every command of the table and each of its options, every error
code of 13.3 (and no code that 13.3 lacks) must appear in it, and its example workflow must
validate and run.

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
  and symbolic links are resolved. A run started without roots is not limited; the canvas
  starts its runs without them (Appendix A). `fugue model get` writes only into the model
  folders (8.9): Fugue's own and the ones the settings name, which the person chose.
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
| `fugue/image-size` | `aspect` (1:1, 16:9, 9:16, 3:2, 2:3, 4:3, 3:4, 5:4, 4:5, 6:5, 5:6, 21:9, 9:21), `megapixels` (default 1), `multiple` (advanced, default 64) | `width`, `height` | The size of that many megapixels in that shape, each side a multiple; the same function sizes Generate Image's start image. |
| `fugue/load-image` | `file` (file picker) | `image`, `width`, `height` | Keyed by the file's content hash. The image is upright: an EXIF orientation is applied, and `width`, `height` are the size it is seen at. |
| `fugue/save-image` | `image`, `name`, `format` (png, webp), `embed` (full, no-prompts, none), `folder` (optional) | `file` | Sink. Writes to the output folder. Never overwrites a file: `<name>.png`, else `<name>-2.png`, `-3`, and so on, also when several Save nodes write one name at once. [11.7](#117-embedding-in-outputs). |
| `fugue/preview` | `value` (any) | none | Sink. Shows the value on the node; from the CLI it is in the run result. |
| `fugue/generate-image` | `prompt`, `negative_prompt`, `model` (optional), `width` (optional), `height` (optional), `steps` (optional), `guidance` (optional), `seed`, `image` (optional start image), `strength` | `image`, `seed`, `width`, `height`, `model` | Capability `image.generate`. Unset optional inputs use the model's defaults; an unset size is the start image's, else the model's default; a default of 0 (sd-server's "the model's own") is no default, so the server picks. |
| `fugue/edit-image` | `instruction`, `images` (list, 1 to the model's limit, else 4), `model` (optional), `steps` (optional), `seed` | `image`, `seed`, `width`, `height`, `model` | Capability `image.edit`; `instruction` is sent as its `prompt`. |
| `fugue/resize` | `image`, `width`, `height`, `fit` (stretch, contain, cover), `method` (nearest, bilinear, bicubic, lanczos, area) | `image` | ComfyUI's resampling. `contain` fits inside the box without padding, so the result can be smaller than the box on one side. |
| `fugue/resize-megapixels` | `image`, `megapixels` (default 1), `multiple` (advanced, default 16), `method` (as Resize, default lanczos) | `image`, `width`, `height` | ComfyUI's ImageScaleToTotalPixels: each side is `round(side * sqrt(megapixels * 1024 * 1024 / (W * H)) / multiple) * multiple`, rounded half to even, then stretched with Resize's resampling. No crop, so the shape can drift by the rounding. A side that rounds to 0 becomes `multiple`. `width` and `height` can feed an Empty Latent. |
| `fugue/upscale-image` | `image`, `scale` (default 2, 0.1 to 16), `method` (as Resize, default lanczos) | `image`, `width`, `height` | #134. No model: Resize's resampling by a factor (ComfyUI's ImageScaleBy), each side `round(side * scale)` half to even. The same kernel as Resize, so the same pixels for the same size. To fit a size use Resize; for added detail, Upscale with Model. |
| `fugue/upscale-with-model` | `image`, `model` (`fugue/upscale-model`), `tile` (advanced, default 512) | `image`, `width`, `height` | #134. Provider capability `image.upscale` ([8.1](#81-capabilities)). The size grows by the model's own scale. An alpha channel is resized with lanczos and put back (the helper takes RGB); a gray or 16-bit image is sent as 8-bit sRGB. A missing model file is `model_missing` on `model`. |
| `fugue/upscale-video-with-model` | `video`, `model` (`fugue/upscale-model`), `tile` (advanced, default 512) | `video` | #138. Provider capability `image.upscale`, with `frames`. One node, not decode / upscale / encode nodes: the value between the parts would be a folder of thousands of frames, half-finished state that costs disk and is useless alone (3.7). ffmpeg decodes a chunk of frames to PNGs (64, fewer as the frames grow: about 30 frames of 1080p, at least 4), one helper job upscales the folder with the model kept loaded, ffmpeg encodes the result to a segment (H.264 CRF 12, Matroska) and the chunk's frames are deleted; at the end the segments are joined as they are with the source's first audio track copied. So memory is one frame and disk is one chunk in and out plus the result so far. The frame rate is the source's exact fraction and chunks are cut by frame number, so the frame count, rate, length and sound are the source's (a variable-rate video comes out at its average rate). The size grows by the model's scale; Upscale Video after it sets a final size. Progress is the frames done over the frames the length says; cancel stops ffmpeg and the helper job and removes the chunk folders and segments. Needs a helper on this machine. Not done: resume of a cancelled run (a new run starts over), a temporal option (see Appendix A). |
| `fugue/load-upscale-model` | `model` (`fugue/upscale-model`, the picker) | `model` | #134. The handle its picker holds, passed on, so one choice feeds several upscalers. Upscale with Model has the same picker on its own input. |
| `fugue/upscale-video` | `video`, `scale` (default 2, 0.1 to 8), `method` (as Resize, default lanczos) | `video` | #134. The video twin of Upscale Image: ffmpeg's `scale` filter on the streamed frames (never decoded here), each side rounded down to even; the frame rate, length and audio track (copied) are the input's; H.264 CRF 12 in Matroska (close to lossless, far smaller than FFV1 at the new size, so Save Video's encode is the only visible one). Progress is the output time over the length, and cancel kills ffmpeg. A pending trim or crop on the input is applied first. |
| `fugue/crop` | `image`, `x`, `y`, `width`, `height` | `image` | Visual (#73): its face shows the picture arriving at `image` (the last run's, or a Load Image's file feeding it directly; "No picture yet" before) with the frame over it and the outside dimmed. Dragging inside the frame moves it, a corner or edge handle resizes it (Shift keeps the ratio, Alt resizes from the center); each drag writes `x`, `y`, `width` and `height` as one undo step, and typing the numbers moves the frame. A double click opens the crop window: the same frame at full size, the four numbers, an aspect lock (Free, Original, 1:1, 4:3, 3:2, 16:9, Turn), a rule-of-thirds grid while dragging; OK or Enter writes, Cancel or Escape leaves the node as it was. With one of the four linked the frame is not drawn. The frame is what the kernel cuts (the origin held inside, the size stopping at the edge). |
| `fugue/brightness-contrast` | `image`, `brightness`, `contrast` | `image` | |
| `fugue/blend` | `base`, `top`, `mode`, `opacity` | `image` | Modes at least: normal, multiply, screen, overlay, soft light, difference. Difference is \|base - top\| as in Photoshop, GIMP and CSS; subtract is base - top clamped, which ComfyUI calls difference. |
| `fugue/compare-images` | `a`, `b`, `mode`, `position`, `gain` | `difference`, `composite` | A sink (#62). The node face and the large view show A and B in a viewer (docs/ui-ergonomics.md, Compare view); the outputs are for saving: `difference` is \|A - B\| times `gain` (Blend's difference formula; identical images give black), `composite` is the picture of `mode` (slider, slider_horizontal, side_by_side, a, b, difference) at `position`. B is fitted to A as Blend fits its top; alpha is left off. It writes no file. |
| `fugue/text-concat` | `a`, `b`, `separator` | `text` | |

Pixel nodes decode and hold images with libvips (C++ API, dynamically linked, CPU); their
arithmetic is plain C++ over float buffers, because libvips' own resize kernels do not give
ComfyUI's pixels (Appendix A). Each has a parity test against ComfyUI's formula for the same
operation, written before the node. The four-reference limit on Edit Image is Fugue's starting
choice; a limit the provider reports for the model replaces it, higher or lower.

### 14.2 M4: the rest of v0.1

- Shipped (#28): Load Video, Load Audio, Generate Video and Generate Song.
- Shipped (#116): Save Video and Save Audio, and a player on the node face (below).

| Type id | Inputs | Outputs | Notes |
| --- | --- | --- | --- |
| `fugue/load-video` | `file` | `video`, `width`, `height`, `fps`, `duration` | Probed with ffprobe, never decoded; keyed by the file's content hash. |
| `fugue/load-audio` | `file` | `audio`, `duration`, `sample_rate` | As Load Video. |
| `fugue/generate-video` | `prompt`, `negative_prompt`, `model` (optional), `image` (first frame, optional), `duration` (seconds, default 2), `width`, `height` (optional, steps of 32), `fps`, `steps`, `guidance` (advanced, optional), `seed` | `video`, `seed`, `width`, `height`, `fps`, `duration`, `model` | Capability `video.generate`. Unset size: a first frame's shape at about 0.4 megapixel in steps of 32, else the model's default, else 864 x 480; the frame goes to the model as a PNG of exactly that size (scaled to cover, cut from the middle, as Generate Image's start image). Unset rate: the model's, else 24. The size, rate and length out are the file's. |
| `fugue/generate-song` | `prompt`, `lyrics` (empty: the model writes them), `instrumental`, `model` (optional), `duration` (seconds, 10 to 600, default 30), `language` (advanced, default `en`; `pt-BR` is sent as `pt`), `seed` | `audio`, `lyrics`, `seed`, `duration`, `model` | Capability `audio.generate`. `lyrics` out is what was sung (or the model's own words), so it can feed Text nodes. |
| `fugue/save-video` | `video`, `name`, `format` (mp4, webm; default mp4), `embed` (full, no-prompts, none), `folder` (optional) | `file` (video) | Sink, as Save Image: the same `name`, `folder`, `embed` inputs, naming and never-overwrite rule (one shared code, `nodes/save_files`), roots check and `workflow_not_embedded` warning. A video already in the chosen container is copied as it is, with no new encoding; otherwise ffmpeg encodes it (mp4: H.264 CRF 18, yuv420p, AAC 192k, faststart; webm: VP9 CRF 30, Opus 128k). Pending trim and crop are applied first (7.2). The workflow goes in a container tag `fugue` ([11.7](#117-embedding-in-outputs)). |
| `fugue/save-audio` | `audio`, `name`, `format` (flac, mp3, opus; default flac), `embed`, `folder` (optional) | `file` (audio) | As Save Video. A flac or mp3 file saved in its own format is copied as it is; mp3 is VBR about 190 kb/s (libmp3lame -q:a 2), opus is 128 kb/s. |

Both generate nodes return a file-backed handle ([2.4](#24-media-handles)): the clip and the song are
never held whole, so what reads them next (the video stages of #105, a Save node) streams the file. The
video and audio types, Load Video and Load Audio are the base those stages build on. A video's audio
track stays inside the clip: there is no separate audio output until a node needs one. A video or
song result (Save Video, Save Audio and Preview of either) shows a player in the node's picture slot
at full detail (`MediaView.qml`, Qt Multimedia): a bar with play and pause, the time, a seek track,
mute and, for a video, a button for the large view; Space plays and pauses, Left and Right seek 5 s,
M mutes. The video's first frame is shown paused. A sound's slot is a bar a quarter as tall as wide.
The file is opened only while the node is in view. A video's double click on its picture or the bar's
button opens the large view (`MediaDialog.qml`, the same player in `LargeViewDialog`, which plays at
once and stops on close). Zoomed out, the slot is the label ("Video · 0:04") as before.
- Shipped (#87): Text Generate (`fugue/text-generate`: prompt, system prompt, optional image,
  model, seed, and in Advanced the longest answer, temperature and thinking (off); outputs `text`, `seed`,
  `model`) and Enhance Prompt (`fugue/enhance-prompt`: an idea, a style, more instructions,
  model, seed; outputs the prompt as `text`, with `seed` and `model`), both on `text.generate`
  (8.1, 8.7). The templates `enhance-prompt` (Generate Image, any model) and
  `z-image-turbo-enhance` (the component nodes) are ready to open (11.10).
- Shipped (#140): Enhance Prompt's Style is a list of prompt templates, data and not code
  (`src/prompts/*.json`, compiled in; the person's own, and replacements, are files in
  `~/.local/share/fugue/prompts/*.json`, read after Fugue's, a template of the same `id` replacing it,
  as for the model catalog in 8.9). A file is `{"prompt_templates": 1, "templates": [...]}`; a template has
  an `id` (lower case words joined by hyphens), a `label`, a `family` (`image`, `video` or `audio`), a
  `target` (the models it suits), `system` (the instructions: a string, or a list of lines), `notes`, and
  optionally `examples` (`{idea, prompt}`, shown to the language model after the instructions),
  `duration_note` (with `{seconds}`: what the node's Length adds; a template without one ignores the
  length) and `language_note` (with `{language}`; else "Write the prompt in {language}."). A template that
  is not valid is left out and said on standard error (`fugue prompts: ...`). The list is read when
  Fugue starts (and on a pack reload), so a new file shows in the dropdown after a restart; a workflow naming a
  style that is gone fails with the styles it can use. Shipped: `detailed`, `photograph`, `tags` (the three
  originals, word for word), `video`, `minimax-video`, `video-from-image` and `music` (for Generate Song's
  description). Two advanced inputs came with them (node version 1.1.0): `duration` (seconds, 0 = not said)
  and `language` (empty = the idea's own).
- Shipped (#29), each with a parity test against ComfyUI's own outputs (`tests/nodes/golden/`):

| Type id | Inputs | Outputs | Notes |
| --- | --- | --- | --- |
| `fugue/pad` | `image`, `left`, `top`, `right`, `bottom`, `feathering` (advanced, default 40) | `image`, `mask` | ComfyUI's Pad Image for Outpainting: the new area is 0.5 gray (alpha 1), the mask is 1 there and 0 over the original; feathering fades the mask inward as ((f - d) / f)^2 and is ignored when twice it is not smaller than the image's sides. |
| `fugue/rotate` | `image`, `rotation` (none, 90 degrees, 180 degrees, 270 degrees) | `image` | Clockwise. |
| `fugue/flip` | `image`, `direction` (horizontally, vertically) | `image` | |
| `fugue/blur` | `image`, `radius` (1 to 31), `sigma` | `image` | ComfyUI's Blur Image, alpha included. |
| `fugue/sharpen` | `image`, `radius`, `sigma`, `amount` (advanced) | `image` | ComfyUI's Sharpen Image (`amount` is its `alpha`); alpha is left alone. |
| `fugue/invert` | `image` | `image` | Alpha is left alone. |
| `fugue/grow-mask` | `mask`, `expand` (negative shrinks), `tapered_corners` (advanced) | `mask` | ComfyUI's Grow Mask. |
| `fugue/invert-mask` | `mask` | `mask` | |
| `fugue/image-to-mask` | `image`, `channel` (red, green, blue, alpha) | `mask` | ComfyUI's Convert Image to Mask. |
| `fugue/solid-mask` | `value`, `width`, `height` | `mask` | |
| `fugue/number-math` | `a`, `operation` (add, subtract, multiply, divide, modulo, power, min, max), `b` | `value` | Python's arithmetic. |

### 14.3 M5: component level

The spike passed ([spikes/02](spikes/02-sdcpp-split.md)). Shipped (#79), each stage a job of the
helper's stage capabilities: Load Models (diffusion model, text encoder and VAE picked from disk,
one `fugue/model`, [2.6](#26-data-values-latents-and-conditioning)), Apply LoRA (strength,
stacking; the result is a derived handle, [2.5](#25-resource-handles)), Text Encode (prompt and
negative prompt, one conditioning; optional reference pictures for an edit, #88), Empty Latent, Sampler (seed, steps, guidance, sampler,
scheduler, denoise), VAE Decode and VAE Encode. A text-to-image graph of them gave the same
pixels as one `generate` run of the same helper for Z-Image Turbo and FLUX.2 Klein 4B at guidance
1, checked by hand on an RTX 3090 (SDXL's size conditioning differs: Appendix A);
the templates `z-image-turbo`, `flux2-klein-4b` and `flux2-klein-9b` are ready to open (11.10).
Video (#105): the same nodes on a video model, with Empty Video Latent (width, height, frames, rate)
in place of Empty Latent, VAE Decode Video in place of VAE Decode, and Load Models' optional
high-noise diffusion model for Wan 2.2 14B. Text Encode and Sampler are unchanged: the latent says
it is a video, and Text Encode takes an optional First frame and Clip (the Empty Video Latent) for an
image-to-video model (#130): the picture is cut to the clip's size and goes to the encode, the conditioning carries it
(`meta.first_frame`, `meta.clip`), and the Sampler sends it again and refuses a latent of another size
or length; the template `wan22-i2v` (fugue-workflows) is this graph on Wan 2.2 I2V A14B Q4_K_S, checked through the CLI on an RTX 3090 (640 x 384, 33 frames, 20 steps: three minutes). A latent of the other kind at a decode is `node_failed` naming the right node. Checked on an RTX 3090 with Wan 2.2 T2V A14B (Q4_K_S, both passes, UMT5 fp8, Wan 2.1 VAE; 320 x 192, 9 frames, 4 steps): the three stages as separate helper processes give a video byte for byte the same as one `generate` run, and the graph above gives the same through Fugue, locally and through `--serve-http` (conditioning and latent files identical). Not yet measured: the split across the GB10 and the 3090 (#105 stays open).
Image editing (#88), as ComfyUI's FLUX.2 Klein 9B templates do it: Load Image, Resize to
Megapixels (1 MP), Text Encode's references, an Empty Latent of the resized size, Sampler, VAE
Decode; the templates `flux2-klein-9b-edit` (distilled: 4 steps, guidance 1),
`flux2-klein-9b-base-edit` (base: 20 steps, guidance 5, empty negative prompt) and
`flux2-klein-9b-edit-2-refs`, each with sample pictures. Checked on an RTX 3090: staged equals one `generate`
run with the same reference, byte for byte. Not built: ComfyUI's KV-cache edit (FluxKVCache) and
its outpaint template (pad, mask, color match). Then text
encoders other than one LLM (FLUX.1's CLIP-L and T5, SD3), ControlNet,
hires fix as a group, inpaint models (designed in [14.4](#144-masking-and-inpainting-167)). A coarse Generate Image can later be rebuilt as a group of
these.

### 14.4 Masking and inpainting (#167)

Designed, not built yet: the parts are the issues at the end of this section. The research and
each choice with its confidence are in Appendix A (the rows marked #167); the person's guide is
[masking.md](masking.md).

**The mask.** A mask stays an `image` with one channel (2.1), values 0 to 1, where 1 is the part
to change and 0 the part to keep: the polarity of Pad, of ComfyUI's inpaint nodes, of
stable-diffusion.cpp's `mask_image` and of A1111's uploaded masks. A node that takes a mask takes
the first channel of whatever image arrives (#29). A mask port says so in its manifest,
`"role": "mask"` (6.1), and the canvas draws it as a hollow square (`PortDot.qml`'s ring on the
square), so a mask and a picture look different before they are wired. Compatibility does not
change: a mask still connects to any image input and an image to any mask input. The mask ports
of #29 (Grow Mask, Invert Mask, Image to Mask, Solid Mask, Pad's `mask`) get the role at a patch
version.

**The mask editor.** One component, opened from any node with an input of widget `mask` (6.2),
so a pack's node gets it too:

- Files: `src/engine/mask_paint.{h,cpp}`, plain C++ (the buffer, dabs, strokes, shapes and undo;
  tests `tests/engine/test_mask_paint.cpp`); `src/canvas/mask_layer.{h,cpp}`, a `QQuickItem`
  that draws the buffer as one texture tinted over the picture and uploads only the rectangle a
  stroke changed (not `Canvas` or `QQuickPaintedItem`, 12.2); `src/canvas/MaskEditor.qml`, a
  `LargeViewDialog` whose viewer is `CompareView` in `single` mode with the layer over it. The
  node glue is `MaskNodeView.qml`.
- Input: `imageSource`, `imageSize`, `maskPath` (the stored mask, or empty), `look`. Output:
  `saved(path)` and `canceled()`. It knows nothing of nodes: the glue sets the input's literal,
  one canvas undo step for the whole editing session.
- The buffer is 8-bit at the picture's size. Sizes are in picture pixels, so zooming does not
  change what a stroke covers.
- Brush: a round dab of radius r = size / 2, coverage c = 1 within hardness × r, then a
  smoothstep down to 0 at r; one dab every max(1, 0.1 × size) pixels along the stroke; a tablet's
  pressure scales the size. Paint sets m = max(m, c × opacity) and erase m = min(m, 1 − c ×
  opacity), so a stroke never builds up over itself. Defaults: size 64, hardness 0.8, opacity 1.
- Tools: Brush (B), Eraser (E; X swaps the two), Lasso (L, a freehand outline filled when
  released; Alt erases), Rectangle (R), Invert (I), Clear (Shift+Delete), Fill. Size `[` and
  `]`, hardness Shift+`[` and Shift+`]`; Alt+drag sizes left and right and sets hardness up and
  down, as ComfyUI's editor does. Undo Ctrl+Z, redo Ctrl+Shift+Z or Ctrl+Y: one step per stroke
  or tool use, at least 100 steps, each kept as the 64 px tiles it changed, so memory is the area
  touched and not the picture times the steps. The viewer's own keys and pointer: the wheel
  zooms at the pointer, a middle drag or Space+drag pans, 0 fits, 1 is 100%.
- Look: the mask in the accent color at 50% over the picture; H shows the mask alone, white on
  black. The brush is drawn as its circle (the hardness as an inner ring).
- Bar: the tools, size and hardness, the overlay toggle; then Fit and 100%; then close. Footer:
  Cancel and OK (the primary). Enter is OK and Escape is Cancel, which drops the session's strokes,
  as the crop window does (12.7).
- Saving: OK writes the buffer as an 8-bit gray PNG with `storeImage` (the inputs folder, named
  `<picture stem>-mask-<10 hex>.png`; the same bytes are one file) and the input's literal
  becomes its path. A mask of all 0 empties the literal: no mask.
- Face: the picture slot shows the picture with the mask over it, and a Paint mask button
  (`ViewButton`, 28 px) under it. On Paint Mask a double click on the picture opens the editor,
  as Crop's opens the crop window; on Load Image a double click keeps opening the large view.
- Workflow files: the file a `mask` input names is an input file, as a `file` input's is. Saving
  copies it into `inputs/` (`relocateInputs`), a package carries it (11.5) and a template's is
  taken in (11.10). One predicate says which inputs hold such files, for all three.
- Widget field: an input of widget `mask` names, in `over`, the image input it is painted over
  (6.1); Load Image's paints over its own file.

**Nodes.** Each small; the formula shared with ComfyUI has a parity test against ComfyUI's own
outputs (`tests/nodes/golden/`, made by running ComfyUI's nodes as #29 did; its code is not read
or copied). Mask outputs carry the role.

| Type id | Inputs | Outputs | Notes |
| --- | --- | --- | --- |
| `fugue/load-image` 1.1.0 | adds `mask` (widget `mask`, optional) | adds `mask` | The painted mask; else 1 − alpha when the file has alpha (ComfyUI's Load Image); else all 0. A mask painted on another size is scaled to the picture (bilinear) with `mask_resized`. |
| `fugue/paint-mask` | `image`, `mask` (widget `mask`, over `image`) | `mask` | A mask painted over any picture, a generated one too. Unpainted: all 0. |
| `fugue/composite-masked` | `destination`, `source`, `mask` (optional), `x`, `y`, `resize_source` (advanced) | `image` | ComfyUI's ImageCompositeMasked: the source at x, y over the destination, out = d × (1 − m) + s × m; no mask is all of the source; `resize_source` stretches the source to the destination first; the mask is resized to the source (bilinear). |
| `fugue/feather-mask` | `mask`, `radius` (0 to 512 px, default 16), `direction` (outward, inward, centered; default outward) | `mask` | A soft edge `radius` wide: b = the mask blurred (three box passes, close to a Gaussian of sigma radius / 2); outward max(m, b), inward min(m, b), centered b. Outward keeps all that is painted and fades into the rest; inward never reaches a pixel outside the paint. Not ComfyUI's FeatherMask, which fades from the picture's borders. |
| `fugue/threshold-mask` | `mask`, `value` (default 0.5) | `mask` | ComfyUI's ThresholdMask: 1 where m > value, else 0. |
| `fugue/combine-masks` | `destination`, `source`, `operation` (multiply, add, subtract, and, or, xor), `x`, `y` | `mask` | ComfyUI's MaskComposite. |
| `fugue/fill-masked` | `image`, `mask`, `mode` (gray, blur; default blur) | `image` | What lies under the mask before a redraw below denoise 1 (A1111's masked content "fill", Krita's pre-fill): gray is 0.5; blur spreads the colors around the mask into it (push-pull: averages of the kept pixels, coarse to fine). Where m = 0 nothing changes. Property tests, no parity (no permissive reference). |
| `fugue/overlay-mask` | `image`, `mask`, `color` (default `#ff3b30`), `opacity` (default 0.5) | `image` | The mask drawn over the picture, to check or save what is masked. A mask alone already shows as a gray picture in Preview. |
| `fugue/crop-to-mask` | `image`, `mask`, `padding` (px, default 64), `megapixels` (default 1; 0 keeps the size), `multiple` (advanced, default 16) | `image`, `mask`, `stitch`, `width`, `height` | The box around every pixel with m > 0, grown by `padding` on each side, moved inside the picture where it sticks out and cut where the picture is smaller; then resized to Resize to Megapixels' size for the box (lanczos for the picture, bilinear for the mask), so a small region is redrawn at the model's resolution and a large one is not drawn above it. |
| `fugue/stitch` | `stitch`, `image`, `blend` (px, default 16), `mask` (optional) | `image` | The redrawn crop put back: resized to the box (lanczos) and blended through the handle's mask (or `mask`, at the picture's size) feathered outward by `blend` (Feather Mask's kernel), cut at the box. Outside the box the original's pixels are kept byte for byte. |

**The stitch handle.** A declared data type `fugue/stitch` (a diamond port, 2.2), a descriptor
alone as an Empty Latent is (2.6): `{"data": "fugue/stitch", "hash", "meta": {"image": <the
original's media handle>, "mask": <the mask's, at the original's size>, "box": {"x", "y",
"width", "height"}, "size": {"width", "height"}}}`, the hash that of the canonical `meta`. So a
Stitch cannot be fed a picture by mistake, and it needs no second wire from the original.

**The group.** `fugue/inpaint-crop-stitch`, "Inpaint (crop and stitch)", shipped by the `fugue`
pack as a group (6.1 `runtime.kind: "group"`; in the template's own definition until the
built-in pack can carry one): Crop to Mask, Text Encode (the crop as its reference), VAE Encode
(the crop and its mask), Sampler, VAE Decode, Stitch. Exposed: `model`, `image`, `mask`,
`prompt`, `seed`, `steps`, `guidance`, `denoise` (default 1), `padding`, `megapixels`, `blend`;
out: `image`.

**Generating with a mask, staged (14.3).**

- VAE Encode 1.1.0 takes an optional `mask`. The latent's `meta` carries `mask` (its media
  handle) and `image` (the picture encoded), as a conditioning carries a first frame (#130). An
  all-0 mask is `mask_empty`; an all-1 mask is the warning `mask_full` (that is plain
  image-to-image).
- The Sampler, given such a latent, sends the job inputs `mask` (one channel, scaled nearest to
  the latent's size) and `image` (the picture at that size), with `denoise` as the strength.
  Text Encode does not change.
- Helper (8.10): `sample` and `generate` take `"mask": IN` (job input `mask`), read as one channel
  at the request's size; the three-channel reader used for pictures would break FLUX.1 Fill's
  packing of the mask, and stable-diffusion.cpp's own CLI reads it as one channel. An image
  model's `sample` with a mask also takes `"image"`. For an inpaint model (the library's version
  names `SD 1.x Inpaint`, `SD 2.x Inpaint`, `SDXL Inpaint`, `Flux Fill` and `Flex.2`, from
  `sd_get_model_version_name`) the helper passes the picture as `init_image` and no start latent,
  which the patch refuses for them; for any other image model it passes the start latent and not
  the picture. A video model with a mask is `input_invalid`.
- Patch: one hook where a sample reads its conditioning (`fugue.cond_in`). For an inpaint model
  the conditioning's `c_concat`, the unconditional's and the image-unconditional's are set from
  this call's latents, as a single run sets them (`prepare_image_generation_embeds`), because the
  encode had no picture and mask: stable-diffusion.cpp puts an inpaint model's masked picture in
  the conditioning, not the latent. No change to sampling code.
- What the library does with a mask, read at the pin (532d2c1, `src/pipeline/image.cpp` and
  `diffusion_engine.cpp`): rounds it to 0 or 1, so a soft mask is hard in the latent; scales it
  to the latent by "nearest max", so a latent cell is redrawn when any of its pixels is masked
  (blocks of 8 or 16 pixels); grows it one more cell for an inpaint model; and at each step puts
  the start latent back outside it (`denoised × m + init × (1 − m)`). Every pixel still passes
  through the VAE, so a graph puts the original back with a feathered mask (Composite Masked or
  Stitch). That is where the soft edge is made.
- Models at the pin. With a mask input of their own: FLUX.1 Fill dev, SDXL Inpaint, SD 1.5 and
  2 Inpaint, Flex.2. By the per-step replacement (any other image model): FLUX.2 Klein 4B and 9B
  (with the picture as a reference, as the edit templates do), Qwen-Image Edit, FLUX.1 Kontext,
  FLUX.1 dev, Z-Image, SDXL, SD 1.5. Not: video models; soft masks (differential diffusion) on
  any model.
- `model_shape.cpp` must read an inpaint model's input as it reads Wan's 36 channels: SDXL
  Inpaint's 9 input channels and FLUX.1 Fill's 384 (96 per patch) do not match their VAEs' 4 and
  16, so today Fugue calls them `model_mismatch` and a stage refuses them.
- Guard: staged equals one `generate` run, byte for byte, on an RTX 3090, for FLUX.2 Klein 4B
  (masked sampling, and a VAE that encodes without drawing noise) and for FLUX.1 Fill when its
  file is at hand. The SD-family VAEs draw their latent from the encoder's distribution with the
  request's seed, so SDXL from a VAE Encode (which has no seed) can differ from one `generate`
  run; image-to-image from a latent always could, so this is not a mask matter.

**Generate Image** 1.1.0 takes an optional `mask` (only with `image`): the standard parameter
`mask` of `image.generate` (8.1), an input, an 8-bit gray PNG of the start image's size, white
redrawn. A provider says it takes one with `"mask"` in its `inputs` property; the `omarchy`
provider does when the server's capabilities list `mask_image` (sd-server's native API: one
channel) and sends it as `mask_image`. A provider that does not is `mask_unsupported` on `mask`
before the job starts, never a silent full redraw. The result is put back over the start image
through the mask (Composite Masked's kernel), so what is outside the mask is the start image's.

**Templates** (fugue-workflows, 11.10), both on the FLUX.2 Klein 9B files of the edit templates
(Q8_0, Qwen3-8B, flux2-vae: the same hashes and sources), 4 steps, guidance 1, denoise 1, each
checked on an RTX 3090 before it ships, with a preview and its sample picture:

- `inpaint-region`, "Inpaint: change a region": Load Image (a sample picture and a painted
  sample mask), the group "Inpaint (crop and stitch)", Save Image and Preview. The prompt says
  what should be there.
- `outpaint`, "Outpaint: extend a picture": Load Image, Pad (256 px left and right, feathering
  40), Text Encode (the padded picture as its reference), VAE Encode (the padded picture and the
  pad's mask), Sampler, VAE Decode, Composite Masked (the padded picture, the result, Pad's
  feathered mask), Save Image. The sample picture is sized so the padded one is about 1 megapixel,
  the size the edit templates give Klein.

**Errors** (13.3). Errors: `mask_empty` (a generating node, Crop to Mask or VAE Encode got a mask
with nothing painted; hint: paint the part to change), `mask_unsupported` (the provider or model
cannot take a mask; names the ones that can), `stitch_mismatch` (Stitch got a picture whose shape
differs from the handle's by more than 2%: an edit model returned another shape; hint: keep the
Sampler on the crop's latent). Warnings: `mask_full` (everything is masked: plain
image-to-image), `mask_resized` (a painted mask was scaled to a picture of another size). The
helper's refusals are `input_invalid` and so `node_failed` on the node: a mask with a video model,
a mask of another size than the request, an inpaint model's sample without its picture.

**Automatic masks** are not built in: each brings a model and a runtime, so each is an optional
pack of command nodes (6.1, its own environment, 6.8) whose output is a mask, and Fugue links no
ML runtime. In this order: Remove Background (BiRefNet: MIT code, ONNX weights released by its
author; ONNX Runtime, MIT), Segment by points or a box (SAM 2.1: Apache-2.0 code and checkpoints;
MobileSAM on a CPU), and text to mask (GroundingDINO: Apache-2.0 code, the weights' licence not
stated; CLIPSeg: MIT code, the weights' licence stated two ways). Not BRIA's RMBG (CC BY-NC).
Appendix A has the sources.

**Not in this design:** soft masks in the sampler (differential diffusion: a change to sampling
code, which the patch does not make, 8.10), Color Match (the color shift an outpaint shows at its
seam), and a paint canvas with layers and colors. Each is its own issue.

**Build** (sonnet issues, in order; parallel only where the files are disjoint):

| Issue | Part | After | Beside |
| --- | --- | --- | --- |
| #171 | Mask and composite nodes, `role: mask` in manifests | | #172, #173 |
| #172 | `mask_paint` core and the mask editor component | | #171, #173 |
| #173 | Helper mask support, the `c_concat` hook, VAE Encode and Sampler, inpaint models in `model_shape`, the staged guard | | #171, #172 |
| #174 | The `mask` widget, Load Image 1.1, Paint Mask, the hollow port, masks in saves, packages and templates | #172 | #175 |
| #175 | Generate Image's mask and the `omarchy` provider's `mask_image` | #171, #173 | #174 |
| #176 | Crop to Mask, Stitch, `fugue/stitch`, the group | #171, #173 | |
| #177 | The two templates, masking.md, the screenshots | #174, #175, #176 | |
| #178 | Later: automatic masks as packs | #174 | |
| #179 | Later: soft masks (differential diffusion) | #173 | |
| #180 | Later: Color Match | #171 | |
| #181 | Later: a paint canvas | #174 | |

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
regional prompting, conditioning math, ComfyUI workflow import, a sandbox for
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

The pages are in [docs/nodes](nodes/README.md). An example pack lives in the repository (`examples/packs/io.github.bmaltais.fugue-example`) and is used by the docs and the tests.

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
| Soft masks in the sampler (differential diffusion): a change to sampling code, which the patch's rule excludes (8.10); carry it in the patch anyway, or propose it to stable-diffusion.cpp upstream | soft inpainting, [14.4](#144-masking-and-inpainting-167) | owner decision, #179 |
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
| Workflow tab colors (#98): each tab has one of six colors, the first no open tab has (a seventh shares the least used), shown as a bar along the tab's top (2px, 4px on the current tab) and a light wash; the top corners take the app's corner radius. A right click offers the six; the pick is remembered by the file's path in `tab-colors.json` in the state folder, not in the workflow file: a color is a view preference, so choosing one is no edit, no unsaved mark and no undo step (the issue said the layout; it would have made every recolor a change to a file other people may share). An untitled tab has its color for the session, a Save as keeps it under the new name. The six are the theme's red, yellow, green, cyan, blue and magenta, each lifted to 3:1 against the tab strip and the current tab; a theme with no color gets six fixed hues at the accent's lightness. Browsers' tab groups and VS Code's tab colors are chosen by the person and start with none; here a color is given at once so tabs differ, and can be changed | 12.10 | medium-high |
| More pixel and mask nodes (#29): ComfyUI has no node of its own for Number Math (its Math Expression takes a formula), so Number Math is eight operations on two numbers with Python's rules (`%` takes the sign of b; a zero divisor and a result that is not finite are errors). Shrink is Grow Mask with a negative `expand` (ComfyUI's one node), not a node of its own; `expand` is held to ±256 because each step is a pass over the image. Rotate turns clockwise as ComfyUI's `rot90(dims=[2, 1])` does; Flip says horizontally and vertically in place of ComfyUI's "y-axis: horizontally" and "x-axis: vertically". Sharpen's `alpha` is called `amount`, since alpha is a channel here, and Sharpen and Invert leave an alpha channel alone (ComfyUI's Invert does too; its Sharpen sharpens whatever channels it gets). A mask input takes the first channel of whatever image arrives. Image to Mask of an image with no alpha gives 1 everywhere (ComfyUI raises an index error). Pad's mask is 1 over the new area, the polarity ComfyUI's inpaint nodes take, unlike Load Image's mask, which is 1 - alpha | 14.2 | medium-high |
| Image nodes clamp their output to 0..1 and return float32 RGB or RGBA; gray input becomes RGB | 14.1 | high |
| Crop is ComfyUI's ImageCrop: x and y are held inside the image, the far edge is cut at the image's edge | 14.1 | high |
| Brightness/Contrast (no ComfyUI core node) is clamp((v - 0.5) * contrast + 0.5 + brightness) with alpha untouched: a mid-gray pivot, so contrast does not brighten (spike 03's form did) | 14.1 | medium |
| Blend: `difference` is \|base - top\| (Photoshop, GIMP, CSS) and `subtract` is base - top clamped, which ComfyUI calls difference; opacity is ComfyUI's blend_factor, the top is resized to the base (bicubic, center crop), the result keeps the base's alpha | 14.1 | medium-high |
| Text Concat is ComfyUI's StringConcatenate: a + separator + b | 14.1 | high |
| Images are upright everywhere: an EXIF orientation is applied when an image is probed and read (Fantasi's choice) | 2.4, 14.1 | medium-high |
| A sink reports its resolved inputs as `shown` on node.done, node.cached and in run.json (#62: it was only a sink with no outputs; Compare Images has two outputs and its viewer needs A and B, which Preview's shown inputs already gave a node with none) | 9.4 | high |
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
| A node's face (#47, [canvas-look.md](canvas-look.md) 4): a 7u header, then the outputs, then the inputs in order, one 6u row each (Blender's order, not ComfyUI's side by side), so a value input's widget spans the node; a resource input is a value row too, with the model picker (#48) | 12.2, 3.2, 2.5 | medium-high |
| Port shapes: circle for text, number and boolean, square for image, video and audio, diamond for declared types, a ring for `any`; a list port has a second outline behind it. Colors are the theme's port colors (`theme.portColor`), repaired where two collide and lifted to 3:1 against the node and canvas ([canvas-look.md](canvas-look.md) 6.2); `any` takes the text color and a placeholder's port the muted one | 12.6 | medium |
| Link checks during a drag and on connect: the engine's compatibility (2.3), a node into itself, a cycle, and a node type that is not installed, each with a sentence saying why | 12.6, 2.3 | high |
| Dragging a linked single input picks its link up and carries it from its source (ComfyUI); dragging an unlinked input reaches back to an output | 12.4 | high |
| A new link into a list input adds an item at the end: the first is stored as a plain link, a second turns the value into an array | 2.3, 4.2 | medium |
| Nodes the layout does not place (agent-written files) go in columns by depth, below the placed ones | 4.5, 13.4 | medium |
| Positions are written to `layout.nodes.<id>.pos` as whole numbers, the viewport to `layout.view` (center whole, zoom to three decimals) | 4.5, 4.6 | medium-high |
| Zoom runs 0.1 to 4, 1.1 per wheel step (ComfyUI's step); below 0.5 node text and ports are not drawn, and a press there is on the node, not a port (ComfyUI draws a low-detail node below about 0.6) | 12.1, 12.3 | medium-high |
| A node's port rows are made the first time it is in view at a readable zoom, so a 300-node file opens at the cost of what is on screen | 12.1 | high |
| A touchpad is a wheel event with a scroll phase, never judged by the device type (the owner's mouse reports `touchpad`: angle 120, pixel 15, phase none) or by `pixelDelta`; it pans in both presets; Ctrl+wheel zooms in both; a pinch arrives as a native zoom gesture. A real touchpad on Wayland reports phases (not testable here: offscreen sends synthetic events) | 12.3 | high for mouse, medium for touchpad hardware |
| Space+drag pans with the left button held; Space and a move with no button does nothing (ComfyUI pans on that too) | 12.3 | medium |
| The canvas is never mirrored in right-to-left languages: a graph's direction and its saved positions are geometry | 12.11 | medium-high |
| A click selects a node, Shift or Ctrl+click adds, dragging a selected node moves the selection, and a click on empty canvas clears it (legacy). A marquee selects every node it touches, as ComfyUI's does | 12.3, 12.4 | high |
| Furniture (12.9) in #16 is drawn from the layout only (collapsed nodes, Frames, reroute points); making and editing it waits for the command registry and undo (#17, #21) | 12.9 | medium |
| Node search ranks by name, then keywords, category and description, with letters-in-order as the last resort; every word of the query must match. A dragged link keeps the node types with a compatible port (the engine's rules) and links the first such port; a node dropped from an input goes left of the point so its output faces it | 12.8, 12.4 | medium-high |
| Undo: one `QUndoStack` per workflow, in the canvas model. Each step keeps the whole document before and after it, and undo or redo makes the canvas match by node id, remaking only what differs. A gesture is one step: a drag, a dragged link (one picked up included), an Alt+drag clone with its move, a Shift+drag of links. An edit that leaves the content hash of "unsaved" (11.2) as it was is no step; the viewport and the selection are not edits. Each tab keeps its own stack (12.10) of at most 50 steps, as ComfyUI's change tracker | 12.10, 12.11 | high |
| The clipboard holds a `.fugue` document of the selection (its nodes, their layout and the group definitions they use) as `application/x-fugue` and as plain text, so any workflow text pastes. A paste gets new ids, puts the copy's top left at the pointer (else the view's center) and selects it. A plain paste keeps the links between the pasted nodes; Ctrl+Shift+V also keeps links from nodes still in the workflow that pass the link checks | 12.4 | medium-high |
| Clone keeps no links from outside, as litegraph's node clone does. Ctrl+D puts the copy 20 units right and down; Alt+drag clones the node, or the selection when the node is in it, where it is and drags the copy. Alt+click with no drag takes the node out of the selection | 12.4 | medium |
| Shift or Ctrl+click on a node already selected, with no drag, takes it out of the selection (litegraph's toggle); dragging it still moves the selection | 12.4 | medium-high |
| Bypass, mute and collapse on a mixed selection: every node takes the state, unless all have it already, and then all lose it. That is ComfyUI's rule for bypass and mute; its collapse flips each node, which leaves a mixed selection mixed, so collapse follows the other two here | 12.4 | medium-high |
| Delete removes the nodes, every link into or out of them, and their layout: entries, reroute points into them and their places in Frames | 12.4, 4.5 | high |
| Shift+drag from an output lets go on another output that can feed every one of its inputs to move them all, and anywhere else to leave them be. Ctrl+Alt+click on an input removes its links (every list item), on an output every link out of it | 12.4 | medium |
| Zoom keys step 1.1 at the canvas's center (ComfyUI). Fit (`.`, Home) frames the selection, or every node when none is selected, in three quarters of the canvas, within the zoom range; the run toolbar's box (and the 2u around it) is not part of the canvas for it, so no node ends under the bar. A workflow put in with no saved `view` (opened, a template, a tab switched to) is fitted this way once, before anything is drawn (#162) | 12.4 | medium-high |
| A bypassed node is tinted magenta (body, header and a dashed edge from the palette's magenta, the share stepped down until the text reads at 4.5:1) with a dashed pass-through line in the type color; a muted one fades to 40% with no tint, its outgoing wires too ([canvas-look.md](canvas-look.md) 9). ComfyUI's litegraph draws bypass as magenta `#FF00FF` at alpha 0.2 and mute as alpha 0.4 (read in LGraphCanvas.getNodeModeAlpha); we keep the magenta and the faded mute, but tint rather than fade so the text stays readable | 3.5, 12.6 | high for ComfyUI's look, medium-high for ours |
| A toggle command's menu name says what it will do to the target: "Remove bypass" (ComfyUI: "Remove Bypass"), "Unmute", "Expand", "Unpin" when every selected node already has the state, else the plain verb; the `?` overlay keeps the neutral name | 12.4 | medium-high |
| Node shadows (#47) are painted once per level in C++ and handed to QML as `data:` URLs on `theme.look`, drawn as a 9-slice `BorderImage` per node in a layer of their own, not through an image provider: the same shared texture, with no provider to register in each engine. Measured at 302 nodes: at most 0.45 ms of render time, inside the 1 ms budget, so nodes at rest keep theirs; after #63's larger shadows at most 0.31 ms | 12.1 | high |
| The valid-target ring while dragging a link is one item drawn by the canvas at the port under the pointer, not one per port; hover growth and the widget port marks appear without animation, which would cost objects at 300 nodes | 12.1, 12.6 | medium-high |
| The casing layer of highlighted wires (a selection's and the hovered one) is capped at 64 wires: selecting more draws their wires at rest | 12.1 | medium |
| A collapsed node is its header, as wide as its title plus 24u (kind mark, meta and padding), at most 60u; the mockup's width, where canvas-look.md first said 12u | 12.6 | medium |
| A node's face is made the first time it comes into view (half a screen around the view, any zoom) and kept: off screen a node is two items, so a 300-node file opens at the cost of what is on screen. The open test bounds CPU time, not the wall clock, and counts the items | 12.1 | high |
| The tab bar (no section in canvas-look.md) is a recessed `well` strip, the current tab on the node `body` color with an accent line on top | 12.10 | medium |
| Widgets (#48): the engine decides, the QML only draws. Typed text is parsed and checked with the engine's literal rules (checkLiteral) in C++; a value that fails shows the engine's reason under the field and is not written, and the last valid value typed stays | 6.2, 3.2 | high |
| The engine's reasons stay English for now: a widget's bubble shows checkLiteral's message (`must be at most 12`) as the CLI and agents get it, untranslated, since translating the engine's errors would make their text differ between the window and the JSON the CLI prints; the window's own words, the keymap file's problems included, are translated | 12.11, 6.2 | medium |
| A color widget's value is checked by the engine (#rrggbb or #rrggbbaa, `type_mismatch` otherwise), for literals, defaults and runs alike | 6.2 | high |
| One undo step per field visit: each valid text is in the document as it is typed (so a save meanwhile has it), and Enter, Tab or leaving the field ends the step, as does any other edit that begins meanwhile (a command such as Ctrl+B, an undo), so the two stay separate steps; Escape puts a one-line field back, while in a multiline field (Enter is a new line there) it ends the edit and keeps the text, so a prompt is never lost to a key. A drag (spin scrub, slider) is one step; a chevron, toggle, choice or picked file is one each | 6.2, 12.11 | medium-high |
| Numbers: a dot or a comma as the decimal point (a comma before exactly three digits, as in 1,000, or two separators, as in 1.000,5, is refused as grouping rather than read as another number); empty text unsets an input with a default or an optional one, and is refused for a required number with none. Chevrons and Up/Down step by `step` (1 for whole numbers, else 0.1), held in `min`..`max` and rounded to the step; a drag on a spin scrubs a step per 6 units, a drag on a slider sets the value by position, and a click (no drag) types, as Blender does | 6.2 | medium |
| Text: empty is the empty text, except an optional input with no default (a file, a folder), where it unsets | 6.2 | medium |
| Seed modes: randomize stores `{"auto": "random"}`; increment keeps a number and lists the input in `layout.nodes.<id>.increment`, so the file keeps the choice and a run sees a number; going from randomize to a number takes the seed of the result the node shows (the run's value on the output of the same name, "Seed used"), else draws one. The canvas adds one to each incrementing seed after a run it starts (wrapping to `min` past `max`), as one undo step | 6.2, 4.5 | medium |
| The model picker lists, when it opens, the models every enabled provider instance offers of the input's resource type (for the node type's capabilities, 8.4), with their state; then the input type's files on disk (8.9), by folder (#76). A typed name is stored as a handle served by that name; an optional model input can go back to "Any ready model" (unset, 8.4) | 2.5, 8.4 | medium |
| A linked value input shows its source ("Prompt · Text") in place of the widget. List inputs keep a read-only row (`[n]`); a placeholder's inputs show their value read-only | 3.2, 3.6 | medium-high |
| File and folder widgets store the path the portal returns as it is, or the path typed; the canvas checks no file roots (see the #19 row on canvas runs and roots) | 6.2, 13.6 | medium |
| Text-field focus (12.4): widget fields are text inputs, so while one has focus its keys are the field's; a press on the canvas or on another widget takes the focus from the field first, ending its edit. Menus are modal, so the window's keys sleep while one is open | 12.4 | high |
| Widgets are made with a node's rows (in view at a readable zoom); at rest a widget is a well, a text and a mouse area, and editors, menus and pickers are made on first use | 12.1 | high |
| Canvas runs (#19) run in the window's process (9.8: no daemon) on a thread of their own that owns everything a run touches: its own registry (node types, and providers.json read again at each run's turn, as a worker does), the kernels, the result cache and the Run objects. The UI thread hands over a copy of the document and gets the events back, so compiling, hashing, moving outputs and waiting for providers or the queue never block it. A run goes through `fugue run`'s steps and code: compile (run to here is the compiler's target), run folder, an entry in the shared queue, its turn, the scheduler. A run that does not compile gets no folder, as with `fugue run` | 9.8, 12.1 | high |
| The window's queue entries record `"owner": "window"`, its pid and that process's start time (`/proc/<pid>/stat`, field 22); such an entry is live while that very process lives, whoever looks, and a process never discards its own entries, so the window can look for dead ones while it holds the lock. `fugue runs cancel` of an active window run leaves the file `cancel` in its run folder, which the window looks for every 50 ms; a waiting one is cancelled by taking its entry, as for a worker's | 9.8, 9.6 | medium-high |
| Canvas runs carry no file roots (13.6): the roots guard against agents and scripts writing where nobody looked, while on the canvas a person sets every path on the node face, often through the portal, which can be anywhere (a USB disk, `/mnt`); limiting them would refuse the user's own choice. A workflow opened from elsewhere shows its paths on its nodes before anything runs. Revisit if the canvas ever runs workflows an agent wrote without the user looking (11.8) | 13.6 | medium |
| A node added on the canvas starts its seeds on randomize, as ComfyUI's control-after-generate does by default: Run makes a new picture each time, the loop people come from ComfyUI with. The manifests keep `0` as the default, so `fugue run` and agents stay reproducible without asking, and no manifest default needs `{"auto": "random"}`. Leaving randomize keeps the seed of the result shown, so a liked image can be iterated on downstream from the cache | 6.2, 10.1, 14.1 | medium |
| Per-node state on the canvas (10.3): between runs it is `fugue validate`'s (checkGraph on the run thread, 300 ms after the last edit and when a workflow is shown, only while no run is active); during a run it follows the events. A node's error stays until the workflow is edited or run again; its last values (time, output values, thumbnail) stay through edits, at 50% while it runs again. A node inside a group shows on the group's node, its error with the inner path id (`g7/first: ...`), and every error of the run is listed by path id from the run pill | 10.3, 12.7, 9.5 | medium-high |
| Thumbnails: the run thread writes each image result once as a PNG at most 512 px wide under `~/.cache/fugue/thumbnails/<content hash>-512.png` and the node shows that file (`file://`), not the result: no image provider to register in each engine (as with the shadows), no full-size decode on the canvas, and the cost per node stays one small asynchronous image. `CanvasModel::previewSize(node)` and `previewSizeChanged(node)` give the result's pixel size to node sizing (#49) | 12.7, 12.1 | medium-high |
| One slot under a node's rows shows what it has (a video or audio result is a label zoomed out and a player at full detail, #116): the last image result (Save Image's with the name of the file it wrote in the chip), a Preview's text or number as an excerpt, as long as the slot can show (the box wraps and elides it; only a text over 8000 characters is cut), a video or audio result as a label ("Video · 0:04"; a first frame would need ffmpeg per result), and before any run a Load Image's file (made on the run thread like a result's, cached by path, time and size, asked only once the node's card has made its rows, again after every edit that changes the file, a paste, drop, pick or undo; a missing or unreadable file, or one outside the runs' file roots when they have any, shows "File not found"). Preview and Save Image show a dimmed "No result yet" until a result comes. A run's result takes the slot from the input's picture, and a new file drops the old result | 12.7, 14.1 | medium |
| Each workflow tab keeps what its runs showed and shows it again when it comes back; a run started in one tab never paints another. Progress reaches the canvas at most once per node every 33 ms, through a path that changes one role and lays nothing out | 12.10, 12.1 | medium-high |
| Run first (Ctrl+Shift+Enter) queues ahead of every waiting run, the CLI's included (9.8's class 3), never ahead of the active one. Cancel (Ctrl+Alt+Enter, Escape while a run is waiting or going) takes the window's active run, else its last waiting one | 9.2, 12.4 | medium |
| Node size (#49). ComfyUI (ComfyUI_frontend, litegraph, read at 55d8fd2): `computeSize` gives the minimum; a manual size is clamped to it while dragging and is never pinned (it only grows by `_arrangeWidgets`, nothing shrinks it); a saved size below the minimum is raised on load; the handle is a 15 px square at each of the four corners; there is no double-click on the canvas handle (the Vue nodes have one that fits the height; the context menu's Resize resets to the minimum); an image preview is a growable row with a 220 px minimum, drawn aspect-fit and never upscaled, and the node is not resized when an image arrives; text and DOM widgets have a minimum height and an optional maximum and share the free height. We match: the minimum is the content's (a resize never cuts content off), the layout's `size` is read on load and raised to the minimum, free height goes to the growable item (the preview, else the last multiline input), a corner handle (any of the four since #63). We improve: auto-size is the default and a node follows its content (a preview by its aspect, between half and one times the width (a taller picture is letterboxed; was 1.5 until #163, when the results of the Upscale template grew nodes over their neighbours), a multiline value by its wrapped lines up to 40u, the longest text value up to 120u wide), recomputed only when the content changes, never per frame; a manual size is pinned in `layout.nodes.<id>.size` and a double-click on a corner returns the node to auto; a resize or a return to auto is one undo step; width is held between 40u and 120u; the file may pin only the width (`[w, 0]`) | 4.5, 12 | medium-high |
| `layout.nodes.<id>.size` is written only for a pinned node (`[width, height]`, whole numbers, 0 for the part left automatic) and removed when the node returns to auto; the pinned height is never stored below what the content needs | 4.5 | high |
| A pinned height that is only spare room shrinks back (#49): when the content shrinks below the tallest content the pin has stood over since it was set or read (a thumbnail clears: a result dropped by an edit, a cleared file) and nothing can take the room (no thumbnail, no unlinked multiline input), the node shows its content's height, never less. The pin stays in the layout, unchanged, so the picture gets its room back when it returns, and shrinking changes no document (no unsaved state, no undo step). Blank room the user pinned over the content as it was stays, and a pin read from a file holds until its content first shrinks. ComfyUI never shrinks a node by itself (the blank box stays); a box with nothing in it serves nothing | 4.5, 12 | medium |
| An optional number, slider or seed input with no value and no default shows "auto", dimmed, in its well (the run decides); a missing type's linked input takes the shape and color of the type it is linked to | 6.2, 3.6 | medium-high |
| Images in (#53), research. ComfyUI (ComfyUI_frontend `usePaste.ts`, `app.ts` drop handler and `handleFile`, `useNodeImageUpload.ts`, read at main on 2026-11-19): a paste event looks at the clipboard items in order and takes the first `image/*` before any workflow text; with a Load Image selected it replaces that node's image, else a Load Image is created; a dropped file goes to the node under the pointer if that node handles it, else a workflow found in the file (PNG/WebP/JSON) wins and opens, else an image makes a Load Image node (several images: one node each, joined by a Batch Images node). The image is uploaded to the server's `input/` folder (`POST /upload/image`, the server renames a clash to `name (1).png` and skips an identical file), the widget holds the name, and a failed upload is a toast. There is no paste button; the combo's list shows the folder's files by name with a preview of the selected one, no thumbnails of the others. We match: image paste and drop create or replace a Load Image, a workflow in a dropped file opens as a workflow, one node per image. We improve: the original bytes are kept (a drop is copied byte for byte, a clipboard image keeps its best encoded type; only bare pixels become a lossless PNG), the file is named from the source name with a content hash so the same image is one file whatever it was called, a Paste button sits on the node, the picker lists recent inputs with thumbnails, a file manager's copy (`text/uri-list`) pastes like a drop, one undo step per paste, and a saved workflow carries its images with it | 14.1, 12.4 | medium-high |
| Images in (#53), the rule. Order of the paste command: workflow data (`application/x-fugue`, or workflow JSON as text) first, then an image. An image is, in order: the encoded bytes of the first offered of `image/png`, `image/webp`, `image/avif`, `image/tiff`, `image/jpeg`, `image/gif`, `image/bmp` whose bytes really are an image (a wrong label is ignored), else the files of a `text/uri-list`, else a decoded `QImage` written as lossless PNG (never JPEG). The format and extension come from the bytes (PNG, JPEG `jpg`, WebP, GIF, TIFF `tif`, BMP, AVIF), checked again by libvips reading the header; a file that is not one of them, a damaged one, one over 256 MB or over 256 megapixels is refused with a message in the window banner and nothing changes. With exactly one Load Image selected the image replaces its `file`; otherwise a Load Image is made at the pointer (the view's center when the pointer is outside), selected, and the paste is one undo step. A dropped file is copied byte for byte (EXIF orientation, profile and an embedded workflow stay); it must be a regular file. Several images make one node each, 320 units apart, one undo step. A drop on a Load Image replaces its image even when the picture holds a workflow; elsewhere a picture with an embedded workflow opens as a workflow, and any other image is taken as an image | 14.1, 11.4 | medium-high |
| Images in (#53), where they live. One folder for every workflow: `inputs` under the output folder (`~/Pictures/Fugue/inputs`, from the same lookup as Save nodes, so `XDG_PICTURES_DIR` and a localized folder are honored), chosen by the owner so the images are visible. A file is `<stem>-<first 10 hex of the SHA-256>.<real extension>`; the stem is the dropped file's name (last path component, extension cut, anything but letters, digits, `-`, `_`, `.` made `_`, leading dots dropped, at most 48 characters, `image` if nothing is left) or `pasted-YYYYMMDD-HHMMSS` for the clipboard (the clipboard names no source). A file already in the folder is reused whatever it was called, so the same image is one file, but only a regular file (never a symbolic link) with the same hash part and extension whose whole content is the same bytes: a name and a size prove nothing, since a run's Save Image may write into the folder. A file under the chosen name with other bytes is never replaced; the name takes `-2`, `-3` before the extension. Written atomically (`PartialFile`), the final path checked inside the folder with the file roots. Fugue never deletes from it. Cache keys stay the content hash of the file | 1.4, 14.1, 13.6 | medium-high |
| Images in (#53), file roots. The inputs folder is under the output folder, so the default write roots cover it; it is also a default read root (the pictures folder may be set outside the home), and `fugue run` adds the `inputs` folder beside the workflow file it runs as a read root. Storing an image checks the write against roots of just the inputs folder, never wider. A dropped file the user chose is read without a roots check (an explicit gesture, as the file picker is) but must be a regular file | 13.6 | medium |
| Images in (#53), saving. Save and Save As copy every file the workflow's `file` inputs name under the inputs folder into `inputs` beside the .fugue (same names), repoint those inputs and write, as one step: always, with no question (a question per save would be noise, and a portable file is what a saved workflow should be; the cost is a duplicate file, never a loss). The original stays. If a copy fails the save fails with the reason and nothing is rewritten. The repoint is an undo step ("Move images"). The repointed input holds `inputs/<name>`, relative to the workflow's folder, so the folder can be moved or shared whole; a full path into that `inputs` folder (written by an older save) becomes relative too. Save As to another folder copies the files of the old `inputs` folder along the same way (a different file already there under the name gets `-2`, `-3`), and any other relative path becomes the full path it led to, since it would lead nowhere from the new folder. A shared copy (11.5) keeps a relative path and replaces an absolute one by the bare file name, which then resolves beside the file | 11.2, 11.5, 14.1 | medium |
| Images in (#53), the UI thread. A paste or drop hashes, checks (the libvips header) and writes the image on the UI thread, at most 256 MB: a typical picture takes milliseconds, and keeping it synchronous keeps one undo step per paste with its result known at once; a worker with a busy notice is for when the cap or the formats grow. Save's copies of a workflow's inputs stream a megabyte at a time and the same-bytes check reads in chunks, so no image is held whole | 14.1, 12.4 | medium |
| Undo after Save As to another folder: before the inputs move, the relative `file` paths of the canvas and of every document the undo steps keep become the full paths they name from the old folder (no step of its own), so an undo after the save never resolves an old relative path against the new folder, where it may name another file. The repoint stays its own step ("Move images") | 12.11, 14.1 | medium-high |
| A name typed without an extension gets the real one (`.fugue`, or the image's format for Save Image…) and then never replaces a file already there, since the dialog asked about the name as typed: the save fails with a notice. The dialogs offer the real suffix and filter, so the portal can ask about the full name | 11.2, 12.3 | high |
| Open and drop read only regular files (a pipe, socket or device is refused before it is opened, as opening a pipe waits for a writer), a `.fugue` of at most 64 MB (refused by its size before it is read), and in a PNG or WebP only the chunk headers and the workflow's own chunk | 11.4 | high |
| Recovery owners (11.3): each running Fugue's recovery store holds `owner-<id>.lock` (QLockFile, never stale by age) in the recovery folder from its first copy, and each note names its owner; a copy is offered only when its owner's lock is free or stale (the process is gone), and the start that offers it holds that lock until it ends, so two Fugues never offer, discard or duplicate each other's copies. A copy without an owner (older) is offered. A copy that cannot be read is moved to `unreadable/` in the recovery folder, never deleted. Copies, notes and trust.json are 0600 | 11.3, 6.7 | medium-high |
| Relative `file` paths. A `file` input's relative path (a literal, or text that comes by link) is resolved against the folder of the workflow file being run, never the working directory: on the canvas, by `fugue run` and `fugue validate` (the CLI's compiled-graph file counts as that file), and for the picture a Load Image shows before a run. Trimmed and "~" expanded first, then joined to the folder and walked one component at a time (`file_roots.h` `inputFilePath`, the same walk as the roots' `canonicalPath`), so `..` and symbolic links are resolved before the read roots judge the path; a relative path may lead out of the workflow's folder, and the roots, where a run has them, decide (confining it would protect nothing, as an absolute path can name the same file). An unsaved workflow has no folder: a relative path there is `file_not_found` on the input ("…is relative to the workflow's folder, and the workflow is not saved yet", hint: save first or give the full path), and its picture shows "File not found". `folder` inputs are not changed (Save Image's relative folder is under the output folder). The cache key stays the file's content hash, so a moved folder keeps its cached results. ComfyUI resolves an image name against its server's `input/` folder only, so a workflow and its images cannot travel together | 13.6, 14.1, 11.5 | medium-high |
| Images in (#53), the picker. A Load Image's file input has a Paste button (always shown, takes the clipboard into that node) and its picker glyph opens "Browse files…" followed by the newest 48 images of the inputs folder with thumbnails; the thumbnails are decoded small, only for rows that scroll into view. The list reads the folder's names and times when it opens; picking one is one undo step and only a file inside the inputs folder is accepted | 6.2, 14.1 | medium-high |
| Menus (#50) are built from the command registry: a menu lists command ids (`src/menu/menus.cpp`) and takes each one's name, first key and state from the registry when it opens, so a menu, the `?` overlay and the keymap never disagree. A command with no handler is listed, off and at half opacity; the ticket that owns a command says when it can run (`setEnabledWhen`) and whether a toggle is on | 12.5 | high |
| Six top menus, File, Edit, View, Node, Run and Help, where ComfyUI has one "Comfy" menu (Workflow/File, Edit, View, Help) and keeps node and run commands in context menus and the action bar: every command is reachable from the keyboard and the mouse in one place. Alt+F, E, V, N, R, H open them (none is bound in 12.4); F10 opens the first (`app.menu-bar`, added); Left and Right move along the bar | 12.4, 12.5 | medium-high |
| A narrow window (the titles do not fit, as with a large text size or a long translation) shows one menu button in their place, with the menus as submenus; Alt+letter still opens each one under it | 12.5 | medium |
| While a menu is open the window's shortcuts sleep, so bare letters (Q, L) reach the menu, not quit or links | 12.4 | high |
| Context menus: right-click on a node, a Frame or empty canvas (on release; a right-drag still does nothing); Shift+F10 or the Menu key (`app.context-menu`, added) opens the selection's or the pointer's. A right-click on a node outside the selection selects it first (ComfyUI); inside, the selection stays | 12.3 | high |
| Open Image, Copy Image and Save Image… (`node.open-image`, `node.copy-image`, `node.save-image`) are on a node's context menu and the Node menu, enabled when its picture slot has an image file: its last result, else a Load Image's file. ComfyUI's menu (`useImageMenuOptions`) has the same three for a node with a preview, acting on the shown image: Open Image opens it in a browser tab, Copy Image draws it on a canvas and puts a re-encoded PNG on the clipboard, Save Image downloads it. Matched: the three verbs, shown for image nodes only, a notice when one fails. Improved: all three use the FULL file, not the 512 px thumbnail; Copy puts the file's own bytes under their real MIME type (png, jpeg, webp), plus a PNG of the picture when the file is not one (and `application/x-fugue-image-type` naming the original's type, which a Fugue paste takes first), so another app and a Fugue paste (14.1) get the original; never a decoded QImage, which qtwayland would re-encode for every image/* request; it reads and decodes off the UI thread; Save copies the original bytes (an embedded workflow stays), atomically, through a save dialog that asks before replacing, starting in the output folder; Open hands the default viewer a file with the right extension (when the file's name lacks it, a copy made off the UI thread in `fugue-view` in the runtime folder, 0700, named by the path, size and time), and refuses a file whose bytes are no image. Not in ComfyUI: the mask editor entry | 12.3 | high |
| Large image view and visual Crop (#73). Researched: ComfyUI's core Image Crop (number widgets only, GPL), and the custom nodes that crop visually: ComfyUI-interactive-crop (Ugleh, MIT: a selection drawn while a run waits), ComfyUI-Olm-DragCrop (a draggable box on the node with pixel and percent readouts), ComfyUI-free-drag-crop (drag and resize on the node, aspect lock), ComfyUI-Visual-Image-Crop (in-node rectangle, ratio helpers); the licenses of all but the first were not stated on the pages read, so no code was read or copied. Photoshop's crop tool: eight handles, the outside dimmed, a rule-of-thirds overlay while dragging, Shift keeps the ratio, Alt resizes from the center, Enter applies, Escape cancels, a ratio picked on a frame fits the largest frame of that ratio. Matched: all of those, plus the four numbers staying the node's inputs (so the workflow file and the kernel are unchanged). Improved: the frame is drawn from the kernel's own rule, so it shows what will be cut; one undo step per drag; a drag is computed from where it started and the total offset, so going past an edge and back returns where the pointer is; the geometry (`src/engine/crop_geometry.h`) is plain C++ with its own tests and is shared by the face, the crop window and any later caller (a mask or region picker); the large view is the Compare viewer in a single-picture mode (one zoom and pan code) and a double click opens it for every node with a picture. Not done: drawing a new frame by dragging outside the old one (a drag there pans, as in the large view), and rotating or straightening while cropping. Handles are 8 px drawn with a 24 px press target | 12.7, 14.1 | high |
| Compare Images (#62, `fugue/compare-images`). Researched: rgthree-comfy's Image Comparer (MIT; a Slide mode where the divider follows the pointer, a Click mode, A and B inputs), ComfyUI's core Compare Images (`comfy_extras/nodes_image_compare.py`, GPL, one draggable slider, no modes or labels documented), community packs with slider, side-by-side, toggle and difference modes (Deno Image Compare, ComfyUI-EsesImageCompare), Lightroom's `\` before/after toggle, img-comparison-slider (MIT: hover option, arrow keys move the divider) and a Qt forum QML slider (no license: the clipped top image drawn at a negative offset, which the viewer does too, so B never bleeds through). Matched: the slider is the primary and default mode, and the divider follows the pointer as well as a drag (rgthree's Slide); a flip mode (rgthree's Click, Lightroom's key); the same viewer on the node and in a larger view; A and B labels; arrow keys move the divider (img-comparison-slider). No code was copied: the concepts are used and the viewer is Fugue's own QML, built on the Preview slot (the thumbnail's place on the face, `shown` for the inputs) and Blend's difference formula and fit. Improved: side by side, a horizontal divider and a difference view (|A - B| times a gain, black when identical, made by the node and so saveable); one linked zoom and pan for both pictures, nearest-neighbour past the file's own pixels, 1:1 on a key; a notice with both sizes when they differ (B is cropped to A's shape and resized, as the difference is made); the large view reads the full files, the face the 512 px copies; the view state is the node's and survives the face's items being dropped, and is not an edit; Copy A, Copy B and Copy view; the viewer is made only at full detail. Outputs: the difference and the composite, so they can be saved; the composite is the node's own Mode and Position widgets (a cached result is never made again to flip), while flipping on the face is session state. Keys (when a Compare Images is the selected node): `\` flip, `M` next view, `0` fit, `1` 100% | 14.1, 12.3, 12.7 | medium |
| Node and Frame colors: one command per theme role, `node.color.<role>` for none, accent, muted, red, orange, yellow, green, cyan, blue and magenta, in Edit > Colors and the context menus; the selection's nodes, or the Frame the menu was opened on, take it in one undo step. A swatch shows the theme's color; the node header shows `tint-<role>`. No `error` role (it is the palette's red). ComfyUI's presets are fixed dark colors (red, brown, green, blue, pale blue, cyan, purple, yellow, black) lightened for light themes; theme roles follow every palette. The last row of the Colors menu, Custom… (`node.color.custom`), opens the color picker (#52): a saturation and brightness field with a hue slider (Adobe's layout), a New and Current swatch pair, H S B, R G B and hex fields, and the last 12 colors used (kept in `recent-colors.json` in the state folder, newest first, no repeats); OK, Enter or a click on a recent color gives the selection or Frame a hex color as one undo step, Cancel and Escape change nothing. A hex color is stored as the one string `#rrggbb` in the same `color` field (a role name or a hex value), and the look derives its tint, Frame fill and title from it with the role formulas against the current theme ([canvas-look.md](canvas-look.md) 6.1), rather than storing a light and a dark pair or a hue plus rules in the file: one value, any theme, and a hand-edited file stays readable. The picker is a component with no knowledge of nodes or Frames (docs/ui-ergonomics.md, Color picker), so a node's color widget can use it. Adobe's picker (a saturation and brightness field, a hue slider, number fields, OK and Cancel) is the model; ComfyUI offers only fixed presets and the browser's native color input, which has no recent list | 4.5 | medium |
| A Frame is picked by its title band, not selected: dragging it selects and moves what lies in it, and its color, title and deletion come from its context menu | 5.1 | medium |
| Groups (#26): the canvas keeps the whole workflow and the instances entered; every edit applies to the level shown and is merged back, and undo restores the whole workflow, so an undo after leaving still undoes an edit made inside. Entering is not an edit (as in ComfyUI, where entering a subgraph is navigation) | 5.2, 12.11 | high |
| Make group merges links from one output into inner inputs of one type into one exposed input (ComfyUI's subgraph conversion makes one input per link); a list input fed from both sides cannot be split and is refused | 5.2, 5.4 | medium-high |
| A group's id follows its name while it is `local/…`, so library folders are named after what the user called the group | 5.3 | medium |
| The library is the `local` folder of the first pack path, loaded as a pack of groups only; saving bumps the version by 3.6 from the face; every open window sees a newly saved group at once (#70: the registry is read again, 6.5) | 5.3, 6.5 | medium |
| Live node types (#70): one registry per window stays in place and takes the types read again (`Registry::replaceTypes`); a reload asked for by the window (Packs page, library save) happens at once, one seen on disk (QFileSystemWatcher on the pack paths, pack and node folders, manifests and command entries) after 500 ms of quiet and only when the types differ. The run thread fills its compile registry before its next compile and its run registry before its next run starts. ComfyUI loads custom nodes only at server start (its Manager says "restart required"); Fugue has no server to restart | 6.5, 6.6 | medium-high |
| Run toolbar and queue panel (#60). Researched from ComfyUI's documentation (docs.comfy.org interface overview, shortcuts, job history and settings pages; the frontend source was not read, and it is GPL-3.0-only, so no code was looked at or copied): a floating run bar at the top of the canvas, dragged by a handle and docked back to the top; Run with Run (On Change) and Run (Instant); a batch count (default 1, capped at 100 by a setting); Interrupt, disabled when idle; Ctrl+Enter, Ctrl+Shift+Enter (front), Ctrl+Alt+Enter (interrupt), Q (queue sidebar); a job history panel with status, progress and previews, per-job cancel, Clear queue and Clear history, 64 entries by default. Not found in the docs: whether the bar's place is remembered, reordering or moving a queued job to the front, and clicking a job to show its outputs on the canvas. Matched: the batch count (1 to 99) beside Run, Cancel next to it, the bar floating at the canvas edge and movable, the keys, a queue panel with running, waiting and finished runs. Improved: the panel lists runs started from a terminal as well (one shared queue, 9.8) and cancels them the way `fugue runs cancel` does; move to front for any waiting run; a click on a finished run puts its results back on the nodes; the bar's place and the batch count are remembered; the same Run menu rows behind the arrow. Chosen: the bar moves freely in the window (#120; first it docked at the top or the bottom edge) and the panel follows it; the history is the last 8 runs (ComfyUI keeps 64: ours is a glance, the run folders hold the rest for 7 days); Alt+Q opens the panel because `Q` is Quit here; Run (On Change), Run (Instant), a GPU memory readout and a results strip are not built | 12.13 | medium |
| Side panels (#56). Researched from ComfyUI's documentation (docs.comfy.org interface overview, node library and shortcuts pages; the frontend source is GPL-3.0-only and was not read, so no code was looked at or copied; the documentation is public and only facts about behavior were taken): the left sidebar has tabs for Node Library, Model Library, Workflows, Assets, Apps and (optionally) Job History, each opened and closed by a key (N, M, W, Q; A is Select all); the node library is searchable, has bookmarks and an Essentials tab, and nodes are dragged onto the canvas; Workflows lists saved workflows and recent files. Not found in the docs: whether the sidebar's width or side is remembered. Matched: N and W toggling a left sidebar with one tab open at a time, the searchable node library, a drag onto the canvas, the workflows tab with recent files. Improved: the same search ranking as Add node (a word finds the same nodes in both), a click adds at the middle of the view and a drop where it lands, the open tabs are listed with their color and unsaved mark and can be closed there, the panel can dock on either side, be resized, and its tab, side and width are remembered, the View menu checks the open one. Chosen: one tab at a time (as ComfyUI's); the library is a category tree (closed at first), not a grid with an Essentials tab; the library's list scrolls by the wheel and its scroll bar, not by dragging, so a drag can take a node out; bookmarks, hover previews, a model library (M), assets (A) and a log panel are follow-ups; recent files keep 12 | 12.14 | medium |
| Media viewer for a workflow's results (#133). Researched from public documentation only, no source read (ComfyUI's frontend and digiKam are GPL): ComfyUI's interface page lists Assets ("generated and imported media: preview, reuse, download, delete") and a Job History tab behind a feature flag, and says nothing of grouping, a folder button or dragging back; InvokeAI's gallery page (Apache-2.0 project, docs read): media in boards, a Virtual Boards option grouping by day, preview-size control and resolution badges, an Info button showing the prompt, seed and models stored in the file, and a context menu with Recall (load workflow, remix without the seed, use prompt, use seed), Send to Image to Image / Canvas, star, move to a board, download, open in new tab, delete, recall needing embedded metadata. Not read this session, and recorded from general knowledge of the products, to recheck before quoting: digiKam, Eagle, Lightroom's filmstrip, macOS Photos, Google Photos, XnView, nomacs, Gwenview and Loupe (a grid plus a filmstrip plus a large view, zoom, keyboard stepping, ratings and flags, a metadata panel); Stability Matrix, Automatic1111 history extensions, Fooocus's log and Draw Things (history keyed by prompt and seed, re-run from an image); video review (timeline thumbnails, scrub preview, frame step). Matched: the three modes and the keys of an image browser, the details read from the file's own embedded workflow, grouping by day-like units (here by run), thumbnail cache on disk. Improved: scoped to one workflow and grouped by its runs (ComfyUI and InvokeAI list everything in one flat place), the list comes from the run folders so a file that was never saved by a Save node still shows, one standalone component any host can feed (panel now, node face and large view next), cells exist only for what is in view and the browser only while shown, a single video or sound reuses the player of #116. Chosen: results = outputs of the nodes nothing reads (not inputs or steps); newest 40 runs; one level of grouping (the run); thumbnails 256 px PNG; Open, Show folder and Copy path first, because they need nothing from the canvas; the browser has no actions of its own, the host decides, so a node face can offer different ones; a switch inside the workflows panel rather than a third side panel (the panel already is scoped to workflows, and one panel at a time is kept) | 12.14, 9.7, 11.7 | medium |
| Bars and panels and focus mode (#129). Researched from the products' documentation only (Adobe's Photoshop user guide on the Window menu, panels and workspaces; the Blender manual's interface pages on areas, View menu and Toggle Maximize Area; ComfyUI's documentation on the interface and shortcuts); no source was read, ComfyUI's and Blender's being GPL; Adobe's page answered 403 and Blender's manual page did not load in the session that built it, so the facts below are from the documentation as known, not re-read, and should be rechecked before they are quoted. Photoshop: a Window menu lists every panel and the options bar and tools bar with a check mark while shown; Tab hides all panels, the toolbar and the options bar, Shift+Tab hides the panels but not them, and moving the pointer to the screen edge shows them for a moment; the layout is remembered and a Workspace (Essentials, Reset Essentials) saves and restores a set. Blender: the View menu toggles the toolbar, the sidebar and the header of an area; Ctrl+Space maximizes the area under the pointer and again restores the layout, Ctrl+Alt+Space makes it full screen; a hidden header comes back from a small arrow on the area. ComfyUI: sidebar tabs open and close with keys (N, W, M, Q; 12.14); the documentation read describes no menu of bars and no hide-all key (not verified beyond that). Matched: a menu of every bar with a check (Photoshop's Window menu); one command per bar that the keymap can bind; one key hides all the chrome and the same key restores it (Photoshop's Tab, Blender's Ctrl+Space); the state is remembered. Improved: the restore is exact (what was shown, nothing else; a panel opened during focus mode stays), a hidden menu bar says how it comes back and has three ways (F10, the canvas's menu, Reset), focus mode is never saved so a window cannot open bare, and every bar is a registry entry so a later bar costs one id. Chosen: H, not Tab (Tab enters a group and opens search here, 12.4) and not Ctrl+Space (some input methods take it); the list is a submenu of View (a top Window menu, as Photoshop's, adds four items to the window, which keeps an item budget) with the panels in it; one panel and one bar are one id each; no hover-at-the-edge reveal, no workspaces and no per-panel arrows (follow-ups); the toolbar `view.toggle-run-toolbar` keeps its id | 12.15 | medium |
| Corners (#63): 8 px by default for every canvas surface, the node corner `r` and the popup corner `R` alike, from the `corner_radius` setting (General page: Fixed, 0 to 16 px a step at a time, or Follow Omarchy, which keeps the earlier rule: `R` is Hyprland's rounding and `r` is held to 8). ComfyUI's `ROUND_RADIUS` is 8 and its Vue nodes use 0.75 rem; Omarchy's default rounding is 0, which drew square nodes. The setting sits on the General page as the issue asks; the Theme page shows Hyprland's rounding as "Window rounding" | 12.11 | high |
| Shadows (#63, docs/canvas-look.md 7): a contact and an ambient layer per level, cast downward and growing with the zoom; much stronger on dark themes (black only reads over a near-black canvas when it is deep) plus a 1 px top highlight on the card at 10% of the text color; selected nodes higher (level 2) and the selection being dragged higher still (a new level 4). ComfyUI draws one black 50% shadow at (2, 2) blur 3 for every node, selected or not. The canvas color itself is unchanged (the theme's background); a slightly lighter canvas on dark themes would make the elevation read more and is left for the owner's look review | 12.1 | medium |
| Resize from any of the four corners (#63), as ComfyUI's canvas and Vue nodes (a 15 px square inside each corner; no edges: its `resizeEdgeSize` is declared and unused); a top or left corner moves the node so the opposite corner stays, after the size limits. Each corner sets its diagonal resize cursor before the press. ComfyUI's canvas draws no mark (its Vue nodes show two short diagonals at each corner on hover); ours is one stroke concentric with the node's corner, in all four corners while hovered or selected (the one under the pointer brighter), so the affordance follows round and square corners alike; a port inside a corner's square wins over it | 12, 4.5 | high |
| Advanced sections fold on every node face, not only groups' (6.1 says `advanced` inputs are collapsed); a linked advanced input always shows | 5.4, 6.1 | medium-high |
| Ungroup of a muted instance mutes its nodes; of a bypassed one keeps the inner modes (a bypassed group's pass-through has no node-by-node equivalent) | 5.2, 3.5 | medium |
| The `?` overlay lists the commands that have a key; the rest (colors, exports, help links) are in the menus and appear in the overlay once the keymap file binds them | 12.5 | medium |
| No providers.json: the default instances are this machine and every host omarchy-local-ai's `network.json` says is paired, by host name at `https://<dns>`, the address the omarchy provider already matches a paired host's models by (so a file the settings write later holds the same entries). Offline paired hosts are kept, so their models show as offline and pins stay valid. Read wherever providers.json is read, never written; a file that exists wins entirely | 8.2 | medium-high |
| The model picker lists models by instance for the node type's capabilities, titled by the provider's display name; a model that is not ready stays choosable (a run then says it is not running, 8.4) and shows why and the command that starts it; Up and Down skip the instance headings | 8.4, 2.5 | medium-high |
| A paired host with no model to offer says why (offline, unreachable, or nothing shared running) instead of "not paired" | 8.7 | high |
| Menu items and gaps from ComfyUI: export a share copy and the compiled graph (11.5, #54), Clear workflow, the help links and About are commands; templates are File > Browse templates (#83, 11.10); Clipspace, refresh node definitions, unload models, focus mode, theme switching, node shapes, properties panels and the Manager are not planned (Fugue has no Python node definitions, follows the Omarchy theme, and installs packs per 6.6); rename and pin (#55), the side panels (#56), a node's image actions (#57), align and distribute (#58) and the tab menu (#59) are later tickets | 12.5 | medium |
| Settings panel (#51): six pages (General, Folders, Run, Providers, Keymap, Theme) instead of ComfyUI's category tree, since Fugue has about a dozen settings. Each change is written at once (no Apply), checked in C++; QML only asks | 1.4 | high |
| settings.json keeps keys it does not know; a known key with a bad value is listed as a problem and its default used, the value left as written; a file that is not a JSON object is never overwritten. A value equal to the default leaves the file, so a later default reaches users who never chose | 1.4 | high |
| `new_seed`: random (default, as ComfyUI's control_after_generate) or fixed (a random number that stays) for seed widgets of nodes added on the canvas; increment stays a per-node choice. `embed_workflow` sets what new Save Image nodes start with (11.7) | 6.2, 11.7 | medium |
| `language`: follow Omarchy (default) or a shipped catalog, live. `output_folder` and `inputs_folder` are read afresh by every run and the CLI | 1.4, 12.11 | high |
| Providers page: the effective instances in preference order; with no providers.json the defaults show as "default, not saved", and the first change writes them all (mode 0600, atomically). Keys and secret module fields reach the panel only as set or not set | 8.2, 8.8 | medium |
| ComfyUI settings not taken: custom navigation mix, zoom and pointer tuning, link render modes, node opacity and badges, palettes and background images (the Omarchy theme), auto save (recovery, 11.3), server flags (each provider owns its engine's), accounts. Later: minimap, snap to grid, side panel layout, reduce motion, keys edited in place, pack enable/disable. Full table in the #51 pull request | 12 | medium |
| Command-node previews (#65). ComfyUI (ComfyUI_frontend `app.ts`, `nodeOutputStore.ts`, read at main on 2026-11-20) sends a sampler's latent preview as a binary websocket message (`b_preview_with_metadata`); the frontend revokes the node's last preview and shows the new one in its image slot; nothing clears it when the node finishes or a new run starts, so a KSampler keeps its last preview, and previews are drawn before output images. We match: one slot, the newest preview, full strength while it is this run's, an image result taking its place, and a result with no image leaving the last preview (as a sampler's preview stays on ComfyUI's node). We differ: a node sends a file path, not bytes (the protocol is lines of JSON, and the file stays in the run folder for Open Image); previews queue at most one deep, so a fast node cannot hold up the run thread; a bad preview is a warning, never a failure; the last run's picture is dimmed while the node runs again, until the new preview comes, and a provider's sampling preview (8) follows the same rule, so it is no longer drawn dimmed while it streams | 7.2, 9.4 | medium-high |
| Image Size (#72): ComfyUI's core has no such node (Empty Latent Image takes a width and a height); people install one (Comfyroll's CR Aspect Ratio, resolution-calculator nodes). Built in here, as it is how a size is picked for every image model, and the shipped text-to-image example sets its size with it. Aspects are written `w:h`, landscape and portrait as separate choices so one pick sets both; the width is the nearest multiple to the exact one and the height the nearest to the area left (2:3 at 1 MP is 832 × 1280) | 14.1 | medium-high |
| Context menus offer what applies (#72): a command for a group, a port or an image is left out of a node's menu when it does not apply, and commands not built yet are left out of context menus; the menu bar keeps every command, dimmed when off (40%), so it can be found. As ComfyUI, whose node menu shows a subgraph's entries only on a subgraph and image entries only on a node with an image. Rules: Run to here needs exactly one node, Make unique a definition other instances share, Paste Fugue's nodes, a JSON object or an image, Run and the exports a node | 12.3 | high |
| Renaming a group's port (#72) sets its label, the name on the face, and keeps its `name`, which links and `--set` use, as ComfyUI's Rename Slot sets a slot's label; so outputs gained an optional `label` (an older Fugue keeps it as an unknown key). Reached from the port itself: a double-click on a group card's port or on an inner port's exposed mark, or Rename on its context menu. Node titles (#55) and Frame titles are edited in place, on the header or the Frame (#74, as the owner asked): the title selected, Enter keeps, Esc drops, a click elsewhere keeps; a port keeps the small question, its label not being drawn as one line to edit | 5.4, 12.4 | medium-high |
| Side tab strip (#159). Researched from public documentation (VS Code's user interface page, JetBrains' tool windows page, ComfyUI's interface overview, read 2026-11-21) and, for what those pages do not say, from general knowledge of the products, to recheck before quoting (Obsidian's ribbon, Blender's editor-type and area edge controls, Krita and GIMP docker tabs, Figma and Photoshop panel tabs); no source was read, ComfyUI's, Blender's, Krita's and GIMP's being GPL. Found in the docs: ComfyUI's left sidebar is a vertical icon rail (Job History, Assets, Node Library, Model Library, Workflows, Apps) and its page says nothing of tooltips, keys or one-at-a-time; VS Code's Activity Bar sits on the far left, switches views, carries badges, offers a right-click menu to move it and to show or hide views, views reorder by drag, and the Primary Side Bar moves right while the bar's own place is set separately; JetBrains' stripes sit on the window edges, a click shows or hides the tool window, icons drag to reorder, each tool window has a shortcut (Alt+1 and so on), and Shift+Escape hides the active one. From general knowledge: the icon of the open view is marked (VS Code and JetBrains), a second click collapses it, hover labels carry the key, VS Code's Activity Bar is 48 px wide with 24 px icons, and a narrow window loses the side bar before the bar. Matched: a left vertical rail of icons, one per panel (ComfyUI, VS Code), a click shows and a second click hides (JetBrains, VS Code), the open one marked, a tooltip with the name and the key, the same key as the strip does the same thing. Improved: the strip is built from the panel registry (a new panel adds a tab with no strip change), it is keyboard reachable (Tab, Enter, Space) with a focus ring and screen-reader names with a checked state, and it stays in step with N, W and the View menu through the one toggle (ComfyUI's page does not say); the canvas keeps 160 px when a narrow window cannot hold the panel; the strip hides in focus mode with the rest. Chosen: the left edge, because the panels are navigation (library, workflows) and ComfyUI, VS Code and Obsidian put navigation left; inspectors belong right, and the strip stays left even when a panel is docked right (VS Code does the same); 44 px wide, a 24 px glyph and a 44 px hit area (VS Code's 48/24, above the 36 px floor of the issue); no badges, reordering, right-click menu or right strip (follow-ups); the Lucide glyphs `boxes` (nodes) and `workflow` (workflows), ISC, added to the bundled font by the recipe in `share/fonts/lucide-fugue.txt`; the run queue (a floating panel of the run toolbar) and the media results (a view in the workflows panel) are not docked panels, so they have no tab yet | 12.14, 12.15, 16 | medium |
| Library groups (#74): ComfyUI (ComfyUI_frontend `TitleEditor.vue`, `subgraphStore.ts`, read at main on 2026-11-20) renames a subgraph's definition when its node's title is edited inline, lists saved blueprints in the node library and deletes one after a confirmation; workflows keep their own copy; it cannot rename a blueprint. We match: a group card's title edited in place renames the group; Settings > Packs lists the library's groups and removes one after a yes, workflows keeping their copy. We improve: a group of the library renamed in a workflow offers to rename it in the library too, and the page renames a library group (its folder and id follow, local/<name>); removing from node search is left out, the page being the one place that manages the library. #156 adds the node library panel as the second place (a trash can on a library group's row, a separate Cancel-first question, one shared call); node search stays without it: its row is full and a click on it adds the node, so the panel and the page are the places that manage the library | 5.3, 12.14 | medium-high |
| Wiring far out (#74). ComfyUI keeps its slots usable when zoomed out and connects a wire dropped on a node's body to the first input that fits. We match: ports work wherever they are drawn (the map tier), with the same 10 px reach on screen, and a drop on a body connects to the one port that fits (the one free one when an input is linked). We improve: when several free ports fit, a short list asks which; and a magnifier: far out, a short hover (400 ms; 300 during a drag) shows the node at zoom 1 over itself, its ports live, without moving the view (a setting, Settings > General, `hover_magnifier`, off by default: a node that grows under the pointer surprises, #123); a wire started there leaves it as it travels, and the node it reaches shows its own to drop on | 12.6 | medium-high |
| Selection toolbar (#74): as ComfyUI's selection toolbox (the owner's screenshot: delete, info, color, frame, widgets, help, bypass, "…"), a bar over the selection made of commands, so it is enabled and labelled as the menus are. Fugue's set: Run to here, Bypass, Color, Frame, Enter (a group), Open Image (a node with an image), Delete, "…" (the node's menu); no Info or Help yet, Fugue has neither panel. Node types adding their own buttons (ComfyUI's extensions do, through a hook in its frontend whose exact name and set were not read for this) waits for a manifest field and a protocol message (7.2) | 12.3 | medium |
| Pin (#55): a pinned node stays where it is when a drag or its Frame moves the others, as ComfyUI's pinned nodes | 12.4 | high |
| Model folders (#66): a resource type names its folders under each models root (`folders` in pack.json, else its id's last part), as ComfyUI's `folder_paths.py` maps a category to several folders (`text_encoders` and `clip`); subfolders are searched, as ComfyUI does | 8.9, 6.4 | high |
| Finding a model (#66): by remembered hash, then by hashing the files with the handle's name; never by hashing the whole folder (a ComfyUI folder holds hundreds of GB). A renamed model is found once its hash is remembered | 8.9, 2.5 | medium |
| A derived handle (#66) gets no `path`; the handles inside it (base, each `apply` step) get theirs | 7.2, 2.5 | medium-high |
| A handle without a hash cannot be found on disk: `model_missing` (a provider serves name-only handles) | 2.5, 8.9 | medium-high |
| Picking a model file (#76) hashes it then, one file at a time on a worker thread, and stores the handle when the hash is known (its own undo step); another value set meanwhile wins. A pick belongs to the level it was made at (entering a group meanwhile holds it until that level is shown again), and another document (a tab, a file opened) drops it; the hash is remembered either way. ComfyUI's picker stores a file name; Fugue's handle needs the hash (2.5), and hashing at pick time means a run never waits on it. The hash goes to the shared store, which a process already holding it reads again when another has saved since (the window's run thread lives as long as the window) | 2.5, 8.9 | medium-high |
| `fugue model list` (#76) prints a disk file's handle only when its hash is remembered, and takes `--hash` to hash the rest: a listing that reads every model would take minutes on a large folder, and a handle without its hash names a provider's model (2.5). `--hash` is also the way to hash a whole folder once (8.9) | 13.1, 13.4 | medium-high |
| The `fugue-helper` provider (#78) answers `describe` itself and starts the helper on the first job: a picker describes every provider each time it opens, and that must not start a GPU process. Its stage capabilities offer one model with an empty handle, the models coming as files in the job, so selection needs no new rule and pickers skip it (8.7) | 8.7, 8.11 | medium-high |
| With no providers.json the helper is a default instance when one is installed, after the omarchy ones, so "any ready model" stays the model servers' (#78, owner's choice) | 8.2 | high |
| The CUDA helper is chosen on an NVIDIA card (the kernel driver is loaded: `/proc/driver/nvidia/version`), else the Vulkan one: CUDA was 35% faster on Z-Image's DiT on the 3090 (#77) | 8.7, 8.10 | high |
| The helper keeps a context per set of context options and frees the least recently used ones before a load the device has no room for, by the files' size plus a fifth, as ComfyUI frees models before loading by an estimate; a load or generation out of memory frees the others and is tried once. Fugue records the helper as one resident model of its instance, and dropping it shuts the helper down (#78) | 8.10, 9.3 | medium |
| "Offers to stop Omarchy's model servers, only with confirmation" is, in v0.1, the out_of_memory error's hint (`omarchy-agent-local stop`) after the processes holding GPU memory are named, as 8.4 gives `omarchy-agent-local` commands until Fugue can confirm and run one itself (13.6) (#78) | 8.10 | medium |
| A persistent command program is stopped "when the memory is needed" only if its type declares `needs.gpu`: it is then a resident model, dropped least recently used first (#67); a program without it holds no GPU memory | 7.3, 9.3 | medium-high |
| Free GPU memory is the first discrete Vulkan device's budget minus usage, else the first integrated one's MemAvailable capped at its heap; Vulkan is set up only when a GPU node first asks, so commands that run none load no driver (#78) | 8.10 | medium-high |
| Helper packaging (#77, the owner's choice): Vulkan helper in `fugue`, CUDA helper in a `fugue-helper-cuda` split package, as Arch splits ollama and ollama-cuda; CI builds the Vulkan one, the CUDA toolkit being several GB | 8.10 | high |
| Stage hooks as image options (`fugue.*`) of stable-diffusion.cpp's option API, in place of the spike's environment variables; the extensions' encode-time state crosses through two virtual methods on its extension interface | 8.10 | medium-high |
| No pixel-parity test was built for the first pin (#77): the spike showed identical pixels at this commit (spikes/02), and the owner judged that enough. #77 ran these staged once: Z-Image, FLUX.2 Klein 4B and 9B, FLUX.1, SDXL, SD 1.5, Qwen-Image Edit 2511 (the Qwen-Image 2.1 files at hand did not load, stable-diffusion.cpp failing on them before any stage), image-to-image from a latent, and IP-Adapter. PhotoMaker and PuLID were not run (no ID images or embedding at hand), so their carried state is unverified. A pin move runs only the few checks 8.10 lists, by hand (GPU and model files), as the owner asked: the families are stable-diffusion.cpp's to test | 8.10 | medium-high |
| A paired host's helper (#80): HTTP server inside `fugue-helper` (`--serve-http`, bearer key, started by the host's owner), not ssh and not a change to Omarchy's `sd-server`: stage operations need the patched engine, and the host's `omarchy` provider keeps serving whole-image jobs beside it. Media as data; models as handles resolved on the host (name, size, sha256), so the nodes skip the local file checks for such a provider; the engine id carries the host. Tried on the GB10 (Klein 4B, all stages on the host, and encode on the host with sample and decode here) | 8.7, 8.8, 8.10 | high |
| Sources pinned as GitHub commit archives with SHA-256 (stable-diffusion.cpp, ggml, CUTLASS), not git submodules: verifiable, and a package lists them as its own sources | 8.10 | high |
| Checked on the owner's folders (2026-11-20), after their runs crashed the helper: a file in a quantization stable-diffusion.cpp has no kernels for (ComfyUI's mixed quantization, `comfy_quant`: int8 ConvRot and fp8mixed, both tried and aborting; GPTQ; SVDQuant; AWQ) fits nothing and is `model_unsupported` before the helper; an LLM encoder's family (Qwen3, Qwen3.5, Qwen2.5) must be the one its model reads, so the 4096-wide Qwen3.5-9B no longer fits Klein 9B. LoRAs are judged by every naming they come in (PEFT, kohya, LoKr, LoHa): a native-named LoRA with none of its layers in the model, a UNet's on a transformer, another dominant width, or a block index the model lacks (FLUX.1's on the as-wide Klein 4B) does not fit. The fitting files are one "Fits <model>" group. On the owner's 532 LoRAs: Klein 9B 27 fit, 495 do not, 10 untold; Klein 4B 7, 394, 131 (FLUX.1 LoRAs in other namings, as wide); Z-Image 8, 517, 7 | 2.5, 8.10, 13.3 | medium-high |
| A model path that is one kind's folder is told by its files when the settings do not say (#86, the owner chose both): a path with any type's folder inside keeps ComfyUI's layout, else up to 8 files are read and the kind more than half share is the path's; a mixed folder holds nothing until set. A LoRA fits a model when the layers it changes that the model has are as wide (names matched without prefixes and separators, so kohya's and PEFT's meet); no layer found tells nothing (diffusers names, an SDXL UNet's), so it is not dimmed. What a folder holds is its own key, `model_folder_kinds` by path, not `{"path", "holds"}` entries in `model_paths` as #86 first said: model_paths stays a plain list an older Fugue reads, and a kind for a path no longer listed is ignored. A LoRA's fit is not checked by the canvas (its model comes from a node that has not run then), only by the stage | 8.9, 2.5, 6.4 | medium-high |
| Which text encoder and VAE fit a diffusion model is read from the files' headers (tensor shapes, and the tensor names that tell a T5, CLIP or LLM encoder and an image or video VAE apart), never from a table of file names or hashes: it holds for any finetune, quantization or renamed file with no list to keep (#85, the owner asked whether a table would help). An encoder fits when the model's text width is 1 to 3 times its hidden size (FLUX.2 Klein stacks three Qwen3 layers). Files that do not fit stay pickable, dimmed under "Other": a family the reader does not know must not be hidden. Load Models takes only an LLM encoder, so T5 and CLIP never fit there (FLUX.1 comes later). Headers are remembered in memory per file (size and time), not on disk as hashes are: a read takes 0 to 21 ms (the owner's folder). Known limit: two LLMs of one width (Qwen3-8B, Qwen3.5-9B) both read as fitting. A NodeKernel may say before a run what its inputs would fail on (`check`, the canvas check's warnings) | 2.5, 14.3 | medium-high |
| The component nodes take one `fugue/model`, made by Load Models from the three files, not ComfyUI's separate MODEL, CLIP and VAE wires (#79): stable-diffusion.cpp fails to load a context missing any component ("model metadata validation failed"), so every stage needs all three, and one handle keeps one helper context warm. VAE Decode takes only the latent, which carries the model | 2.6, 14.3, 8.10 | high |
| Text Encode takes the prompt and the negative prompt and gives one conditioning (#79, the owner's choice over ComfyUI's positive and negative wires, which would need a patch change): stable-diffusion.cpp encodes both sets in one call. It always makes the negative set (guidance other than 1 at encode), the Sampler's guidance being unknown to it; sampling at 1 skips it, so the pixels match a single run, at the cost of a second, short encode of the (often empty) negative prompt | 14.3 | high |
| Load Models' text encoder is the model's one LLM encoder (`llm_path`: Qwen3 for Z-Image and FLUX.2 Klein, Qwen2.5-VL for Qwen-Image); families with CLIP and T5 encoders (FLUX.1, SD3) come later. A plain `fugue/model` is a checkpoint (`model_path`, the `checkpoints` folder) | 14.3 | medium-high |
| An Empty Latent is a size in a small file, not a tensor: the helper makes the noise from the seed, as a single run does. The Sampler's guidance defaults to 1 (the distilled models the examples use); sampler and scheduler unset are the model's own. Text Encode sends no image size, so SDXL's size conditioning is the model's default size, as ComfyUI's plain CLIPTextEncode uses a fixed one | 14.3 | medium |

Building M4 made these further choices.

| Choice made in M4 | Section | Confidence |
| --- | --- | --- |
| Enhance Prompt (#87) is a node of its own with styles (three at first, now prompt templates, #140), not a Text Generate preset: `detailed` (a paragraph, for models that read sentences: Z-Image, FLUX, Qwen-Image), `photograph`, and `tags` (SDXL, SD 1.5), plus free instructions. ComfyUI's enhancer nodes (custom nodes on Ollama or LM Studio) work this way; its built-in TextGenerate instead runs the Qwen3 encoder file as an LLM, which stable-diffusion.cpp cannot do (llama.cpp in the helper: not now) | 14.2 | medium-high |
| Text answers lose the model's reasoning ahead of the answer (a leading `<think>` block, or text up to a first `</think>` the chat template opened; `reasoning_content`) on every text node; Enhance Prompt also drops a leading label ("Enhanced prompt:", "Here is …:"), a code fence and quotes around the whole, the chatty filler ComfyUI's enhancers report. An empty answer fails the node (a thinking model can spend all its tokens thinking) | 14.2 | medium-high |
| An image to a text model is sent at most 1024 pixels on a side (as it is when a PNG or JPEG within that), since it travels as base64. It is refused before the job only when the model says it takes no image; a local model says nothing (status.json has no `input`) and is asked | 8.1, 8.7 | medium |
| Generate Video is one node with a first frame as an optional input, not a text node and an image node: the model decides whether it takes one, and the server answers 400 with its own words when it cannot. An end frame, reference images and control frames (`end_image`, `ref_images`, `control_frames` of `vid_gen`) are left out: no model Omarchy ships has them tested (8.7). Split into stages later is #105's | 14.2 | medium-high |
| Generate Song is one node over ace-server's two jobs (`/lm`, `/synth`), as Fantasi does, with `lyrics` out; the two are one step to the user, since the written song goes back to render unchanged and holds nothing useful alone (3.7). `cover`, `repaint` and the base-DiT tasks are not offered: Omarchy ships the turbo DiT only | 14.2, 8.7, 3.7 | medium-high |
| Video defaults are Fantasi's (864 x 480 at about 0.4 megapixel, steps of 32, 24 frames a second, 2 seconds here where Fantasi allows 1 to 10), overridden by what the model reports; the length is `duration` in seconds, from which the frames are made, since a person thinks in seconds and the model rounds the count (MiniMax H3: 17n+5) | 14.2 | medium-high |
| Every LoRA a video server lists is sent with every job (Fantasi's rule; the server applies none, and MiniMax H3's few-step LoRA is how it is meant to run). A user-chosen LoRA for video is not offered: Omarchy's runtime maps none | 8.7 | high |
| Load Video and Load Audio are separate from Load Image, one kernel for the three: an `any` file loader would need a type chosen by content, and compatibility is by type (2.3). ComfyUI also has separate LoadVideo and LoadAudio (core, GPL-3.0: read as documentation only, no code taken; its audio loader also takes video files and decodes whole into memory, its video type is an opaque object; Fugue keeps files and probes). Dropping or pasting a video or sound onto the canvas is not built | 14.2 | medium-high |
| Save Video and Save Audio are two nodes modelled on Save Image, sharing its code for name, folder, embedding and numbering (`nodes/save_files`), not new naming logic. Formats are few: mp4 and webm for video, flac, mp3 and opus for sound, default mp4 and flac. A file already in the format is copied, anything else goes through ffmpeg (already a dependency, run as a program as the probe does). No codec, CRF or `auto` choices, which ComfyUI's SaveVideo has (format auto, mp4, mkv, webm, mov; codec auto, h264, av1, prores; H.264 and AV1 CRF); the defaults are the ones a person would pick. No separate Preview Audio or MP3 and Opus Save nodes (ComfyUI has SaveAudio, SaveAudioMP3, SaveAudioOpus, PreviewAudio): Preview takes any type, and the format is an input | 14.2 | medium-high |
| Research for #116: ComfyUI's SaveVideo (inputs `video`, `filename_prefix` default `video/ComfyUI`, `format`, `codec`; embeds the prompt and the workflow in the file; outputs the video and a UI preview), SaveAudio (`audio`, `filename_prefix` `audio/ComfyUI`) and its audio preview were read as documentation only (docs.comfy.org); ComfyUI is GPL-3.0 and none of its code was read or taken. Kept: the workflow travels in the saved file, so dropping it onto the canvas can restore it; the node passes its file on. Changed: Fugue's `name` is a plain name (no `%date%` formatting), the folder is its own input, a file never overwrites (ComfyUI counts `_00001_`), the output is the file as written, and the player plays on the node face where ComfyUI's preview shows in its sidebar or a widget. Dropping a saved video or sound onto the canvas to restore its workflow is not built (the tag is written; reading it back is a later ticket) | 14.2, 11.7 | high |
| The player uses Qt Multimedia (QML `MediaPlayer`, `AudioOutput`, `VideoOutput`; LGPL-3.0, as the rest of Qt), the FFmpeg backend. This is a new runtime dependency: `qt6-multimedia` and `qt6-multimedia-ffmpeg` in the PKGBUILD's `depends` (CI installs from that list, so it follows). The alternative was no player or a second app opened per file; Qt Multimedia is the one piece of Qt that plays, with the same API on every theme and no process per node. It is a QML plugin loaded when the player is first made, so a window with no video or sound, and the command line, load nothing more. Qt's FFmpeg backend logs a stream dump to stderr for every file; `main.cpp` turns that one category off (`QT_LOGGING_RULES` still wins) | 12.7, 14.2 | medium-high |
| The player is `MediaView.qml`: a source, a kind and a label in; play state, position and signals out; it knows no canvas, node or run. The face and the large view each make one. A video holds a decoder from the moment its source is set (it shows its first frame paused), so the face sets the source only while the node is in view. No volume slider (mute only; the system's volume is the level), no loop, no speed, no fullscreen, no waveform (canvas-look 4.5 sketched one; it needs peaks the engine does not report). A Preview or Save Video of a video with pending trim or crop plays the whole file, not the edit: the edits are applied only in a saved file | 12.7, 14.2 | medium |
| Research for #28: ComfyUI's video nodes (LoadVideo, SaveVideo, CreateVideo, GetVideoComponents, trim) and audio nodes (LoadAudio, PreviewAudio, VAE audio) were read as documentation and in the workbench note `map/research/14-comfyui-media-handling.md`; Fantasi (the Omarchy image app) and omarchy-local-ai, stable-diffusion.cpp `api.md` and acestep.cpp `docs/ARCHITECTURE.md` (MIT) for the servers. Fantasi's code was read for the protocol only; Fugue's provider is written for Fugue | 14.2, 8.7 | high |
| Video stages (#105) reuse the image stage capabilities, the stage file format, the nodes (Text Encode, Sampler) and the provider and transports as they are: the helper picks the video call from the context, a latent's descriptor says it is a video, and only two small nodes are new (Empty Video Latent, VAE Decode Video), because an output port has one type. ComfyUI does the same: one KSampler and CLIPTextEncode for image and video, EmptyHunyuanLatentVideo / Wan22ImageToVideoLatent for the empty latent, VAEDecode giving frames that CreateVideo/SaveVideo assemble | 8.10, 14.3 | high |
| The video leaves the helper as YUV4MPEG2 (`video.y4m`), not an encoded file or PNG frames: no codec in the helper (its build stays the pinned patch), one file for the transport, half the bytes of RGB, and ffmpeg (already required) reads it. Lossless FFV1/Matroska afterwards, as the engine keeps every intermediate video (2.4) | 8.10, 14.3 | medium-high |
| Wan 2.2 14B's two diffusion models are one more optional part of Load Models (`high_noise`), not a second model input on the Sampler: stable-diffusion.cpp loads both in one context and switches at its `moe_boundary` itself, where ComfyUI chains two KSamplerAdvanced nodes by hand. Wan's UMT5 goes in as `t5xxl_path`, told by the file's tensors | 14.3, 8.10 | medium-high |
| Research for #105: stable-diffusion.cpp (MIT) `docs/models/wan.md` and `docs/guides/c_api.md` at the pin (Wan 2.1 and 2.2 T2V, I2V, TI2V, VACE, S2V; `sd_generate_video`; `video_frames`, `fps`, `high_noise_*`); `src/pipeline/video.cpp` read to place the seams, as the image ones were. ComfyUI's Wan example workflows and node docs read for how it splits encode, sample and decode (its code is GPL-3.0 and was not read or copied) | 8.10, 14.3 | high |
| Enhance Prompt's styles are prompt templates, data (#140, the owner's principle: modular parts), one JSON schema read like the model catalog (shipped files compiled in, the person's in the data folder, same id replaces, invalid entries left out and said). Style stays a plain choice input whose list the registry fills at start-up (`Registry::setInputChoices`), so the CLI, the window and the run thread all see it, a workflow stores the id, and a group can narrow it; a live reload on a dropped-in file and picking a template from the connected model's family are not done (a model handle carries no family) | 14.2, 8.9 | medium-high |
| Enhance Prompt extras (#140): kept, a `duration` (seconds, applies to templates that say how, so an image style ignores it) and a `language` (the output language; MiniMax's template keeps spoken words and on-screen text as the idea has them). Not built: a negative-prompt output (MiniMax H3 has no negative field and its guides put exclusions in the description itself; Wan and LTX use one, but no node here takes one: add it with a model that does), keep-original strength (every template already says to keep what the idea gives), a shot-count input (the template adds a cut when the length allows one) | 14.2 | medium |
| Video prompt research (#140), what was read and what it gave, 2026-11-21. MiniMax H3 model card on Hugging Face (MiniMaxAI/MiniMax-H3, read through a mirror, Xdp99/MiniMax-H3) and its `docs/VIDEO_PROMPT_WRITING_GUIDE_base_en.md` (the guide's own page loaded; the card names a second guide for reference-to-video, not read; the weights carry MiniMax's community license that excludes some regions, so the guide is paraphrased in Fugue's own words, never copied). Findings: H3 makes picture and sound (32 kHz stereo) in one pass, 4 to 15 seconds at 24 frames, 768 px short side by default (2K comes from a stage that is not open); the open checkpoints take text plus zero, one or two images (text, first and last frame), and a reference one up to 12 files; the weights are CFG-distilled; MiniMax says to feed it a rewritten prompt (its hosted Context-IR does that) "or build your own context-processing system following the Prompting Guidance", which is what the `minimax-video` template is. The layout it was trained on: three fields in order, `integrated_multimodal_description` (the timeline: looks, actions, shots, speech, singing, sounds on screen), `overall_soundscape` (1 to 4 sentences of ambience, action and non-verbal sounds, never dialogue) and `non_diegetic_music` (1 to 3 sentences in instruments, tempo, rhythm and dynamics, not mood words; N/A for none); shots as `[Shot 2] At 00:04.500, the camera cuts to ...` (shot 1 has no time, times strictly increase, a cut must bring new information, else it is a camera move); speakers `(S1)`, spoken words only in `<d>[Language] words</d>`, verbatim; on-screen text in double quotes, untranslated; camera as natural language (zoom, push in, pull out, pan, truck, tilt, pedestal, arc, tracking, static, shake, POV, roll) inside the sentence, speed or amplitude only when it matters; image-to-video: the first picture is the frame at 0.00 s, so say what it shows once and then the action (first-frame anchor, onset, continuation, result). No length or word limit is stated; MiniMax's platform page for the hosted MiniMax-H3 API allows 7,000 characters, and 4 to 15 second integer durations. Negative prompts: none in H3's guide; Kapwing's H3 guide puts exclusions in a final "Constraints" section as plain prohibitions | 14.2 | high |
| The bracketed camera commands ([Push in], [Pan left], [Tracking shot], [Static shot], up to three at once) belong to Hailuo 01 Director, 02 and 2.3 (fal.ai's Video 01 Director page, platform docs for the legacy routes); H3's own guidance and Kapwing's say to describe the camera in natural language and not to treat the bracket list as H3's syntax. So `minimax-video` forbids bracketed commands. Pages read: platform.minimax.io video generation guide (three examples only, [pan], [zoom], [static]; no list), Kapwing's "How to prompt MiniMax H3" (its seven-part order, timestamped sound cues, constraints as direct prohibitions), HackerNoon's H3 guide (the same three fields, stable speaker ids). Did not load: platform.minimaxi.com's video prompt guide (redirected to platform.minimax.cn, which answered 404), Luma's H3 guide (404), minimax-ai.chat (a fan site, not used). Third-party guides are cited for what they agree with MiniMax's own card on, nothing else | 14.2 | medium |
| Open prompt extenders compared (#140): Wan 2.2's `wan/utils/system_prompt.py` (Alibaba's repo, Apache-2.0; read through a summarising fetch, the file's text not copied): a director's role, keep the subject and action, add shot size, angle, light and time of day, describe motion step by step, 60 to 200 words for text-to-video; for image-to-video, keep the person's motion and camera words, drop what the picture already shows, under 100 words, with worked examples. LTX-Video's `prompt_enhance_utils.py` (Lightricks, Apache-2.0 repo): one flowing paragraph, 150 words at most, in order: the main action, movements and gestures, how things look, the background, camera angle and movement, light and colour, sudden events; for image-to-video the image first. HunyuanVideo's rewrite prompts are under Tencent's community license (regions excluded), so were not read. Borrowed as ideas, not text: the order of the `video` template (action, movement, look, camera, light, change) and its 80 to 150 words come from LTX and Wan; `video-from-image` takes Wan's rule that the prompt is motion only and drops what the picture shows; all instruction text is Fugue's own. ComfyUI's prompt-enhancer nodes (custom nodes over Ollama or LM Studio) report the same chatty-answer failure the node already strips | 14.2 | medium-high |
| Music prompts (#140, `music` for Generate Song's description): ACE-Step's tutorial (docs/en/Tutorial.md of the ace-step repo; its license was not shown on the page) says a caption is genre and style, mood, instruments, timbre, era, production, vocal characteristics and rhythm, specific and not conflicting, and that BPM, key and similar metadata do not belong in it (they have their own fields) while loose words like "mid-tempo" are fine; lyrics carry their own `[Verse]`, `[Chorus]` tags and live in Generate Song's Lyrics input. The template writes only the caption | 14.2 | medium-high |
| Checked live (#140, 2026-11-21): Qwen3.8 27B on the DGX (tensorfold) wrote the `minimax-video`, `video`, `video-from-image` and `music` prompts in 4 to 10 seconds, in the layout asked, with `<d>[English] ...</d>` for given words, (S1) for the speaker, N/A music when there is none; the two worked examples must not share a subject with likely ideas, as the model copied one nearly whole when the idea matched it (the examples were changed for that), and it wrote the clip's length into a prompt ("a four-second loop") until the templates said not to. MiniMax H3 there (`minimax-h3-sdcpp-cuda`) made a 2 s, 416 x 256 clip with sound in 12 s, for both the idea as written and the enhanced prompt, same seed. "A lighthouse keeper climbing to the lamp at dusk": the plain idea showed two figures on a gallery by the lamp (no climb); the enhanced one showed the keeper on the spiral stair, a hand on the rail, the camera tilting up, a sunset sky, and its audio was louder (mean -13.8 dB against -23.4 dB). One pair of clips is an anecdote, not a benchmark; the local H3 does make sound from the layout's soundscape fields | 14.2 | medium |
| Thinking is off unless asked for: Enhance Prompt never thinks, Text Generate has a Thinking switch (Advanced, off). It is sent as the chat template's switch (`chat_template_kwargs.enable_thinking`), not OpenAI's `reasoning_effort`, whose `"none"` a strict server refuses. Checked on the DGX (Qwen3.5 2B, tensorfold, 2026-11-20): thinking spent the whole default answer (4096 tokens, "low" effort too) and wrote nothing; off, a good prompt in 41 tokens. ComfyUI's Qwen3 prompt node has the same switch | 8.1, 14.2 | high |
| A text job waits up to 300 seconds of silence (image jobs: 15 s per request): reading a long prompt or an image, or reasoning, can stream nothing for a while | 8.7 | medium |
| Resize to Megapixels defaults its multiple to 16 (ComfyUI's resolution_steps defaults to 1): sizes then match a latent of the same picture | 14.1 | medium-high |
| Image-to-video (#130) composes the nodes that exist: Text Encode gains two optional inputs, First frame (an image) and Clip (a latent, the Empty Video Latent the Sampler starts from), and no node is new. The first frame needs the size and length because the library makes the model's extra input (Wan 2.2 I2V's `c_concat`: 4 mask channels and the 16 of the frame's latent, over every latent frame) in the encode; ComfyUI's WanImageToVideo does the same in one node that also makes the latent, which is the complex node avoided here. The same Empty Video Latent goes to both nodes, and the Sampler checks that the conditioning was made for its clip (a size or length that differs would otherwise fail inside the model). The picture rides the conditioning like the references (`meta.first_frame`, its hash checked at the Sampler), so Wan 2.2 TI2V (frame in the sampled latent, not the conditioning) also stages, with the cost of one more VAE encode in the sample stage; the helper's job input is `image`, not an option, so a paired host receives it as data. Checked on an RTX 3090 (Wan 2.2 I2V A14B Q4_K_S, both passes, 256 x 256, 5 frames, 4 steps): encode, sample and decode as separate helper processes give a video byte for byte the same as one `generate` run | 8.10, 14.3 | high |
| Text Encode takes an edit's reference pictures and carries them on its conditioning (`meta.references`) to the Sampler, which sends them again (#88). stable-diffusion.cpp's flux2 preset gives references to the model, not the text encoder, but every stage of one generation must send the same options (the guidance sets an edit makes are chosen at encode), so both stages send `ref_images`. ComfyUI attaches reference latents to the conditioning (ReferenceLatent) on the positive and negative branch, after a VAE Encode of each: one wire here does all of it. The Sampler's cache follows the pictures, since the conditioning's descriptor holds them. The encode VAE-encodes them too, unused (a few hundred ms) | 14.3 | high |
| Templates (#83) are a separate repo, `bmaltais/fugue-workflows` (private, like Fugue), bundled as a submodule at a pinned commit: the owner's call (2026-11-20), so templates can be published and contributed apart from the app. ComfyUI ships its templates as a pip package pinned by version | 11.10 | high |
| A template is a folder with no index: ComfyUI's `index.json` must be kept in step with the files by a sync script and CI. A template's models and node versions come from its workflow, not from fields written again (ComfyUI repeats the models in the index and on each node) | 11.10 | high |
| Templates carry sample pictures (`inputs/`), as ComfyUI's repo does (`input/`); opening one takes them into the inputs folder, so an edit template runs as it opens (owner, 2026-11-20). Packs are not carried: a copy would replace the installed version of the pack; side-by-side versions are #90 | 11.10 | high |
| A template made by a newer Fugue is left out, judged by its nodes' versions (3.6), not by `requires.fugue`: a node at the same major and the same or older minor runs as made | 11.10 | medium-high |
| The person's own templates folder is `~/Documents/Fugue/templates`, beside where workflows are suggested; there a plain `.fugue` is a template too. ComfyUI has none (custom nodes' `example_workflows` only) | 11.10 | medium-high |
| A template's sources are exact: each URL's file has the recorded sha256 (Hugging Face's LFS oid). The FLUX.2 Klein 9B templates name unsloth's current Q8 file, not the older build first checked on the 3090, whose bytes only unofficial mirrors still hold (owner, 2026-11-20) | 11.10 | high |
| Model downloads (#89, the owner reversed "no downloads"): any https source, not ComfyUI's allowlist of Hugging Face and Civitai, since the file is kept only when its hash is the handle's; ComfyUI's download list trusts the name and checks no hash by default. http is taken only to this machine, where nothing leaves it | 8.9 | high |
| A cancelled download deletes its part; a break or a quit keeps it, and the next download of that file goes on from it after hashing what is there. A browser's cancel deletes too; a part kept after a cancel would hold many GB nobody asked for | 8.9 | medium-high |
| Downloads run one file after another, not side by side: the first model is usable while the rest arrive, and one bar tells it. The banner shows one button for all of a workflow's missing models ("Download 3 models, 19.4 GB"), not one per model as #89 first said, and a choice of folder only when more than one can take them | 8.9, 12.10 | medium-high |
| `requires.models` may record each model's `size` (#89), for the button's total and an early stop when a source serves another size; the total shows only when every model records one | 4.3, 8.9 | medium-high |
| After a download Fugue says what arrived, or what failed with its hint; after a cancel the banner names what is still missing, with its button again | 12.10 | medium |
| A package to share is a template, zipped (#91): Fugue already reads, checks and opens that folder with its samples, and the person may keep it among their own templates; no second format. ComfyUI shares a workflow as JSON or a PNG with it embedded, and its inputs and models must be found by hand | 11.5, 11.10 | high |
| A package holds no model, only references (hash, source, size): models are many GB and their licences forbid passing many on; #89 downloads them, checked by hash | 11.5, 8.9 | high |
| File > Export for sharing… takes the place of Export share copy…, which the menu listed but nothing ran; the bare share copy stays what an embedded workflow holds (11.7). The window asks only where to write it: prompts and layout are kept, as the person sharing a workflow mostly wants them; the CLI has `--no-prompts`, `--no-layout` | 11.5, 12.5 | medium-high |
| Zips are read and written with libarchive (a dependency of pacman, so on every Arch system); Qt has no public zip API | 11.5 | high |
| A package takes only images Fugue takes in (14.1), not every file a `file` input names: a workflow received or pasted may point an input at any file (`~/.ssh/id_rsa`), and the person exporting would not see it go. The notice names what went in and what did not | 11.5, 13.6 | high |
| A pack's source is written only as an https, ssh or git URL without credentials, with the exact commit: what is shared goes to others, and an install must get the version the workflow was made with | 4.3, 6.6 | high |
| A package has its own extension, `.fuguepack`, not `.zip` (the owner, 2026-11-20): the desktop opens it with Fugue (`share/fugue-mime.xml`: `application/x-fugue-package`, a kind of `application/zip`; `application/x-fugue-workflow` for `.fugue`, a kind of JSON), and a person sees it is a Fugue file. It stays a zip inside, so any unzip tool reads it | 11.5, 1.4 | high |
| Node versions side by side (#90). The registry keys a type by id and major; lookups without a major give the newest, a workflow node asks for its recorded major and falls back to the newest (then it needs review). Search and the library list the newest major only | 3.6, 6.12 | high |
| An older major of a node still installed runs on it and gets the warning `node_newer_major`, not an error; only a major that is not installed is `node_version_mismatch` | 3.6 | high |
| Older majors of built-in nodes live in the built-in pack as `<name>@<major>` node folders with their own kernels (`Kernels::addAt`); no built-in has an older major yet, so the mechanism is tested with fakes | 3.6, 6.4 | medium-high |
| Pack folders `<id>` (first or any) and `<id>@<major>` (pack.json's version major); trust is per pack id and version, so installing, updating or removing one major never forgets another's trust | 6.5, 6.6, 6.7 | medium-high |
| Cache keys carry the pack version of the node's own pack; built-in nodes keep the empty one, so existing keys of built-ins stay valid | 10.1 | high |
| `requires.packs` records one entry per pack id: the version of the pack holding the newest major of its nodes in use. A workflow that mixes two majors of one pack records the newer | 4.3 | medium |
| The offer to move to the newer major is the compiler warning and the CLI; a canvas affordance (a badge and a "Move to 2.0" action) and templates that carry their own pack are follow-ups | 3.6, 11.10 | medium |
| Research (#90): ComfyUI's V3 schema has no per-node version. Its `comfy_api` is versioned as a whole (`comfy_api.latest`, a pinned `comfy_api.v0_0_2`), so an extension chooses the API it was written to, which a node cannot do per node. It renames with `search_aliases` and `NodeReplace` (old node id to new) and marks `is_deprecated`; a custom node is pinned by the Manager and comfy-cli by snapshot of the whole environment (`save-snapshot`, `restore-snapshot`), not per node. Fugue records each node's version in the workflow and keeps older majors installed, which is the part ComfyUI leaves to snapshots. Read as documentation only (docs.comfy.org); ComfyUI, ComfyUI-Manager and comfy-cli are GPL-3.0, so no code was read or taken | 3.6, 6.6 | high |
| Upscaling (#134), the engine: stable-diffusion.cpp's own ESRGAN upscaler in the helper (the `sd_upscaler` API at the pin: ESRGAN RRDBNet and MoSR, `.safetensors`, `.pth` and GGUF, `tile_size` with a quarter overlap, a progress call per tile, CUDA/Vulkan like the rest of the helper). Chosen over realesrgan-ncnn-vulkan (MIT, image-only, needs its own NCNN model conversion, a second runtime) and spandrel/ONNX Runtime (Python, or a second native runtime): the helper already ships, is out of process, on the card's backends, and is where memory is arbitrated (8.10). Verified here on an RTX 3090 (CUDA helper): Real-ESRGAN x4plus (both .safetensors and the original .pth), x4plus anime 6B, 4x-UltraSharp, 4xNomos8kSC, 4xNomosWebPhoto and 4xNomos2 HQ MoSR run; Real-ESRGAN x2plus, 2xHFA2kShallowESRGAN and 2xLexicaRRDBNet do not (their first convolution takes 12 channels, a pixel unshuffle the loader lacks: "tensor conv_first.weight has wrong shape [3,3,12,64]"), so no 2x model is in the catalog and the helper says why | 8.1, 8.10, 14.1 | high |
| Upscaling, what ComfyUI does (docs.comfy.org UpscaleModelLoader and ImageUpscaleWithModel; the node source was read for the numbers): models from `models/upscale_models` (and `extra_model_paths.yaml`), a loader node apart from the upscale node, the image upscaled in tiles to avoid out-of-memory (tile 512, overlap 32, halved and retried on out-of-memory until 128; the docs give no numbers), `ImageScaleBy` with nearest-exact, bilinear, area, bicubic and lanczos. Matched: the folder name, the loader node, the five methods, tiles with overlap, a default tile of 512. Improved: the picker lists the catalog beside the files with the license before any download; progress per tile; Upscale Image is Resize's kernel, so one place does resampling. Not done: halve-and-retry (the helper frees its other models and retries once on out-of-memory; a smaller `tile` is the person's), the Ultimate SD Upscale and SUPIR families (diffusion refinement: later, as groups) | 14.1, 8.9 | medium-high |
| Upscaling, apps read (docs and READMEs, no GPL source: Fooocus' code was not read): Upscayl ships a built-in list of Real-ESRGAN-style models and takes more from a custom-models repo (its docs pages for models returned 404, so its wiki was not read); chaiNNer loads a model file the person has, with OpenModelDB as the place to get them and no built-in download; InvokeAI's Upscale panel has a Spandrel model picker that, when none is installed, offers to open the Model Manager; Fooocus shows only 1.5x and 2x. Chosen: the model list in place (a filterable menu on the input, #84's), downloads from that same menu through the one download line, a license question first (none of them asks) | 8.9, 14.1 | medium |
| Upscale models, formats and licenses (OpenModelDB data and each host's card): `.pth` is a pickle, `.safetensors` is not; the helper's loader reads a `.pth` without running its code, and the catalog prefers `.safetensors` where one exists (Real-ESRGAN x4plus from Comfy-Org's repackaging, the others from their authors' repos), the anime 6B being `.pth` only. Licenses: Real-ESRGAN models BSD-3-Clause (xinntao); the Phhofm Nomos models CC-BY-4.0 (credit the author; two cards say "CC-BY-0.4", a typo, their metadata says CC-BY-4.0); 4x-UltraSharp CC-BY-NC-SA-4.0, listed but marked not for commercial use. Left out (cannot be verified or are not wanted): Remacri (HF license `unknown`, OpenModelDB says CC-BY-NC-SA-4.0, not downloaded), 4x_NMKD-Siax (no stable URL), 4xLSDIR (only a branch-tip raw URL), IllustrationJaNai (Google Drive), AniScale-2 (CC-BY-NC-4.0), and every 2x model for the loader reason above. Every catalog hash and size was taken from the host (Hugging Face's LFS oid, OpenModelDB's sha256) and checked by downloading the file | 8.9 | high |
| Catalog entries carry `commercial` and `attribution` flags beside the SPDX id, so the question can say "not for commercial use" in words (and in the error color) without a person decoding an id; Fugue does not enforce a license, it shows it | 8.9 | medium-high |
| The catalog is a JSON file per resource type, compiled in, with the person's own and updates dropped in `~/.local/share/fugue/catalog/`; a remote update feed is a follow-up. The picker component is the existing model picker (InputWidget, WidgetMenu, WidgetPicker): any resource type with catalog entries gets "Download" and the question, with no change to the type | 8.9 | medium-high |
| Upscale with Model sends RGB and puts the alpha back resized with lanczos (the helper takes three channels); it outputs the model's scale and no "scale to" input: Resize or Upscale Image after it sets the final size (3.7: simple nodes composed) | 14.1 | high |
| Video upscaling research: per-frame image upscalers (Real-ESRGAN and the RRDB models) flicker, because each frame invents its own detail; video models use a temporal window (RealBasicVSR and BasicVSR++: Apache-2.0 code, checkpoints on Dropbox/Drive/OpenMMLab with no stated license; RelayVSR and SeedVR2: Apache-2.0, in stable-diffusion.cpp's `vid_upscale`, heavy, each with its own VAE and diffusion model, SeedVR2 marked experimental there). Video2X (AGPL-3.0) and realesrgan-ncnn-vulkan (MIT) are per-frame ffmpeg pipelines. ComfyUI's video upscaling is a frames node pair (VideoHelperSuite) plus Upscale Image per frame. Chosen: a separate Upscale Video node (an output port has one type) sharing the picker and kernel family; the no-model one is ffmpeg's `scale` on the stream, with no frame round trip; the model one (#138) streams frames through the helper in chunks with the model kept loaded | 14.1 | medium-high |
| Video with a model (#138): the helper keeps its upscaler loaded between jobs and takes a folder of frames per job (`image.upscale`, `frames`), ffmpeg decodes and encodes in chunks, one node (3.7: the value between the parts would be a folder of thousands of frames). Checked on an RTX 3090 with Real-ESRGAN x4plus on a generated 480x270, 10 s, 24 fps clip with audio (chunks of 64): 240 frames in and out, 24 fps, 10.02 s, the audio kept, 1920x1080, frame numbers burned into the source line up across chunk boundaries, 280 s end to end (about 0.9 frames a second, the card shared with a 5 GB desktop load), nothing left in the node's folder after the run, and after a cancel mid-way the folder is empty, the helper is idle and GPU memory is back to what it was. Peak disk is one chunk (frames in and out) plus the segments so far, whatever the length. Flicker is left alone on purpose: a per-frame model makes its own detail in each frame (the test pattern's checkerboard shows it), as Video2X and Upscayl do; a cheap fix (blend each result with a lanczos upscale of the same frame by a `strength`, in the encode pass) trades the added detail for stability and is the owner's call, and the engine's video models (RelayVSR, SeedVR2) stay a separate decision with their own catalog entries. Resume is not built: a cancelled run starts over, as a run folder is not shared between runs | 14.1, 9.2 | medium-high |
| Resize's resampling premultiplies RGBA for every method but nearest (it did for lanczos only, as PIL does): a transparent pixel's color no longer leaks into its neighbours when scaling up or down. ComfyUI resamples RGB only, so there is no reference to differ from | 14.1 | high |
| The video twin encodes H.264 CRF 12 in Matroska, not the engine's FFV1: FFV1 at 4 times the pixels is gigabytes for a short clip, and CRF 12 is visually lossless; Save Video's own encode (CRF 18) follows | 14.1 | medium |
| Agent skill (#30), research: the layout agents read is a folder with `SKILL.md` and `name`/`description` frontmatter; Claude Code scans `~/.claude/skills`, Pi `~/.pi/agent/skills` and `~/.agents/skills`, and a name in both Pi folders shadows. Existing tools ship one the same way: Superwhisper links `~/.claude/skills/superwhisper` to a folder in its own package, and skillpack (this machine's skill manager) copies or links a skill folder into each agent's scan dir. Chosen: one real copy in `~/.agents/skills/fugue` (read by Pi and others), a link from Claude's folder (the owner's "the way Fantasi does"), no copy under `~/.pi` (it would shadow) | 13.5 | high |
| The skill is compiled into the binary (a Qt resource) rather than found beside it, so a packaged, AppImage or built-in-tree `fugue` installs the same text with no path to guess, and `fugue skill install` is the whole update path: a newer Fugue brings a newer skill | 13.5 | high |
| Not a generated skill, but a checked one: the prose is written for agents, and a test fails when a command or option of the table, or an error code of 13.3, is missing from it, when it names a code 13.3 lacks, or when its example workflow does not validate and run. Node manifests are not copied into it (`fugue node describe` is the source), so the part that changes most cannot rot | 13.5 | medium-high |
| Edits are detected with a recorded hash (`.fugue-skill.json`), not timestamps or a version compare: the file is Fugue's to update only while it still matches the bundled text or what a past install wrote; anything else is `skill_modified` and needs `--force`, which keeps the old one as `<name>.bak-<time>`. `skill_modified` is a new code, not a reuse of `usage` or `permission_denied`, because an agent should read it as "ask the person" | 13.3, 13.5 | medium-high |
| `CLAUDE_CONFIG_DIR` moves Claude's skills folder (Claude Code honors it); no flag for an arbitrary folder, `HOME` already moves everything. No MCP server or AGENTS.md snippet is installed: the MCP server is later (13.4) and an edit to a user's AGENTS.md is not Fugue's to make | 13.5 | medium |
| Cache management (#141), research (documentation and the products' own settings, recalled, not re-fetched this session: the fetches of Adobe's and ComfyUI's help pages failed): ComfyUI offers `--cache-classic` (the default), `--cache-lru <N>` (keep N node results) and `--cache-none` (run every node again), all about memory, with no disk size, no per-workflow view and no clear command; InvokeAI has an "empty model cache" and a clear of its image intermediates; Blender has File > Clean Up > Purge Unused Data and a Preferences cache limit for its memory and disk caches; browsers' Clear browsing data lists each kind with its size, a time range and a checkbox, and says in words what is lost; Lightroom Classic has a Camera Raw cache size setting (default 5 GB) with a Purge Cache button, and sets previews apart. Matched: a size limit per cache with least-recently-used eviction (Lightroom's cap, ComfyUI's LRU), a per-kind list with a size and a Clear button and a time range (browsers' dialog; `--older-than`). Improved: the result cache is on disk and survives restarts, so the limit is in GB, with the size per workflow and per node; a dry run; the question names what is lost in the workflow's words (nodes run again, pictures return on the next run, history) before it clears; the CLI and the window use one module; saved outputs and frozen results are never in the list | 10.5, 10.6, 13.1 | medium-high |
| Cache defaults (#141): results 10 GB (the owner's 5.1 GB today; the spec said 20 GiB with no setting; a result is a few MB, a video far more, and a re-run costs minutes of GPU, so the limit is generous), previews 1 GB (261 MB for 619 files today; they are cheap to make again), the newest 20 ended runs kept past 7 days (a run folder is small and holds the history `fugue runs` reads). Sizes in decimal GB, as the app shows them. 0 is no limit | 10.5, 10.6 | medium |
| Clearing and cleaning take the engine lock instead of knowing about runs: a run holds it from its turn to its end, so the cleaner cannot take a file from a running node and a waiting run starts after it. A busy lock skips results and previews with a warning (`cache_busy`); run folders are protected by status (the queue's entries, and any not ended, unless abandoned for a day) | 10.6, 9.8 | medium-high |
| `fugue cache clear` with no `--kind` clears the results, as 13.1 always said; thumbnails and runs need `--kind`. `--workflow` goes with results and runs, `--node` with results only (a preview belongs to no workflow) | 13.1 | high |
| A preview cleared while its picture is on the canvas stays blank until the node's result is shown again (the next run, instant for cached nodes), so the Settings question says so; no per-node reload is built | 12.7, 10.6 | medium |
| Research for masking and inpainting (#167, 2026-11-21). Read: documentation, READMEs, papers, model cards, and permissive code only. ComfyUI (core and frontend), Inpaint Crop and Stitch (lquesada), comfyui-inpaint-nodes (Acly), Krita AI Diffusion and Fooocus are GPL-3.0 and A1111 is AGPL-3.0, so only their docs, READMEs and wikis were read; Differential Diffusion's repository states no licence, so only the paper was read. One fetch of an A1111 source file was attempted by mistake and denied, so nothing of it was read. **ComfyUI** (docs.comfy.org built-in-nodes pages, which say they are AI-written and may be incomplete; docs.comfy.org/interface/maskeditor; comfyanonymous.github.io/ComfyUI_examples/inpaint and /flux): the mask editor opens from Load Image's menu, with a mask pen, a paint pen, an eraser, a paint bucket with tolerance, a color select, brush shape, size 1 to 250, opacity, hardness and step, Alt+right-drag for size and hardness, invert, clear, undo and redo, and mask, paint and image layers; how it stores the mask is not documented (the examples show the alpha as the mask: transparent is painted). VAE Encode (for Inpainting) takes `grow_mask_by` and gray-fills under the mask, so it wants denoise 1 (Acly's and lquesada's READMEs); Set Latent Noise Mask attaches a mask to a latent; InpaintModelConditioning makes an inpaint model's conditioning and latent from pixels and mask; Differential Diffusion patches the model to threshold the mask by timestep, with a strength; ImageCompositeMasked (destination, source, x, y, resize_source, mask), GrowMask, ThresholdMask (1 above the value), MaskComposite, and FeatherMask (fades from the picture's borders, not from the mask). Flux Fill takes the alpha-masked picture from Load Image; Qwen-Image Edit and Kontext take no mask. ComfyUI's Klein 9B "Image Extend" template pads, masks and color-matches (from search snippets). **Inpaint Crop and Stitch** (README): crop to the mask's box grown by a context factor, optional resize to a fixed target size (the model's native one) and a padding multiple, mask expand, blend, invert, fill holes and a high-pass threshold, extend for outpainting by edge replication; stitch reverses the resize and puts the crop back through a blurred blend mask so the unmasked pixels never pass through the VAE. **Acly's inpaint nodes** (README): Fooocus inpaint patch for SDXL (not for Turbo, Lightning or Hyper merges), LaMa and MAT pre-fill, Fill Masked (neutral, telea, navier-stokes), Blur Masked, Expand and Shrink Mask, Stabilize Mask (snaps 0.9999 to 1, which ComfyUI's opacity check otherwise ignores), Color Match (Masked), Denoise to Compositing Mask. **Differential Diffusion** (arXiv 2306.00950): a change map 0 to 1 at latent size; at step t of k the original is noised to t and every point whose map value is below the threshold (k − t) / k is copied from it, the rest kept from the previous step, so low-strength pixels stay pinned longer; a blurred mask is soft inpainting. **Krita AI Diffusion** (docs.interstice.cloud: selections, resolutions, common issues): the selection is the mask; with one, Generate becomes Fill (strength 100%) or Refine (below); modes Fill, Expand, Add Content, Remove Content, Replace Background, Custom (pre-fill none, neutral, blur, border, inpaint model); the context is automatic, the selection's bounds, the whole image or a layer; a denoising mask with a feathered transition (Selection Feather, a percentage of the selection), a small grow, and a separate blend mask for the composite (Selection Blend, pixels); the context is scaled to the checkpoint's native size. **InvokeAI** (Apache-2.0; code read: `invokeai/app/invocations/sd/create_gradient_mask.py`, `invokeai/app/invocations/image.py` `img_paste` and `apply_mask_to_image`; PR 6205): an inpaint mask layer with a noise level and a denoise limit; mask blur before denoising and before the composite; infill (tile, PatchMatch, LaMa, cv2) for outpainting; a coherence pass (Gaussian, Box or Staged, edge size, minimum denoise) built as a gradient denoise mask, and a separate hard mask of the expanded area for the paste back; the docs pages and support articles did not load. **A1111** (wiki Features; PR 14208 text): mask blur, inpaint masked or not masked, masked content fill, original, latent noise or latent nothing, inpaint area whole picture or only masked with "only masked padding, pixels" (the crop at the target resolution, pasted back), an inpainting checkpoint by file name, soft inpainting (per-pixel strength; its defaults were not found). **Fooocus** (README, discussion 414; defaults from fal.ai's and Replicate's API pages, second hand): its own inpaint patch model, Improve Detail and Modify Content, outpaint by direction, inpaint denoising strength 0.5, respective field 0.618 (0 is only the masked region, 1 the whole image), erode or dilate −64 to 64. **Photoshop** (adobe.com learn page; the help pages answered 403): Generative Fill gives three variations as a generative layer with a layer mask the person paints to blend; its resolution limits are reported variously in forums and are unverified. **Models**: FLUX.1 Fill dev (model card: image and mask, guidance 30, 50 steps, FLUX.1 dev non-commercial licence, slight color shift and edge lines noted); SDXL inpainting 0.1 (9 input channels: latent, masked-picture latent, mask; strength 0.99 rather than 1, 15 to 30 steps; Open RAIL++-M); FLUX.1 Kontext dev and Qwen-Image Edit (no mask input); FLUX.2 Klein (docs.bfl.ai: up to 4 references, 4B Apache-2.0, 9B non-commercial; no mask). **stable-diffusion.cpp** (MIT, read at the pin 532d2c1): `mask_image` is one channel, rounded to 0 or 1, scaled to the latent by nearest max, grown one cell for an inpaint model; every image model replaces the prediction outside the mask by the start latent at each step; inpaint models (SD 1.x, 2.x and SDXL Inpaint, Flux Fill, Flex.2) get the masked picture as `c_concat`, which a single run puts in the conditioning; the server's native API takes `mask_image` and lists it as a feature. **Automatic masks**: SAM and SAM 2.1 (Apache-2.0 code and checkpoints), MobileSAM and EfficientSAM (Apache-2.0 code, weights' licence not stated), GroundingDINO (Apache-2.0 code, weights not stated), CLIPSeg (MIT code; its README says the MIT licence does not cover the weights, its Hugging Face card says Apache-2.0), rembg (MIT; models with their own licences), BiRefNet (MIT, ONNX weights by the author), U2-Net and IS-Net (Apache-2.0 code), BRIA RMBG 1.4 and 2.0 (non-commercial); ggml's SAM example runs ViT-B on a CPU only. **What people get wrong**: polarity (transparent is the mask in ComfyUI, and editors zero the color under alpha 0); VAE Encode for Inpainting below denoise 1, which keeps the gray; a small region redrawn at the whole picture's resolution; hard masks giving seams; the colors outside the mask shifting through the VAE; a mask too tight for the model to blend; values just under 1 ignored; edit models moving content inside a crop so it does not stitch; the wrong model (an inpaint model's extra channels, a distilled merge with the Fooocus patch). **Matched**: the editor's tools, keys and Alt-drag; Load Image's mask as 1 − alpha; ComfyUI's composite, threshold and mask composite formulas, with parity tests; crop to the mask with context, resize to the model's size, stitch through a blend mask with the outside byte for byte (lquesada, A1111's only masked, Krita's context); a pre-fill for redraws below denoise 1; the original put back through a feathered mask (Krita's and InvokeAI's blend); outpainting by Pad. **Improved**: the mask is its own file beside the picture, which stays untouched (its hash, and so the cache, unchanged; polarity explicit; carried by saves, packages and templates); mask ports are marked; any node, a pack's too, can have a painted mask through the widget; undo is bounded by the tiles a stroke touched; the crop is sized by megapixels, not a fixed square; the stitch handle is a typed value, so a Stitch cannot be given the wrong picture; the staged path equals one run, checked; a provider that cannot take a mask is an error, not a silent full redraw; Generate Image keeps the outside exactly. **Not done**: soft masks in the sampler, automatic masks, Color Match, a paint canvas with layers (14.4) | 14.4 | medium-high |
| A mask stays an image (2.1, unchanged, medium) and gets `"role": "mask"` on its ports, drawn as a hollow square, rather than a port type of its own: polarity is the users' first mistake, and the mark shows it before wiring, while a `mask` type would retype #29's outputs (a major version of each) and need mask twins of Blur, Resize and Crop. Compatibility stays as it is | 2.1, 6.1, 14.4 | medium |
| The painted mask is a separate 8-bit gray PNG in the inputs folder, named by its bytes, not the picture's alpha as ComfyUI keeps it: the picture's file and hash stay as they were (so its cache and its embedded workflow too), white is plainly the part to change, and saves, packages and templates carry it as they carry pictures. A widget `mask` holds its path, so a pack's node can have a painted mask | 6.2, 11.5, 14.4 | high |
| The brush paints with max and erases with min, sizes in picture pixels, undo as the 64 px tiles a step changed, at least 100 steps; defaults size 64, hardness 0.8 (owner's taste). The editor is the crop window's frame (`LargeViewDialog`, `CompareView` single) with a `QQuickItem` layer, so zoom and pan are the viewer's. Load Image's double click keeps the large view (learned place); Paint Mask's opens the editor, as Crop's opens its window | 12.7, 14.4 | medium-high |
| The mask enters the staged graph at VAE Encode (the latent carries it, as ComfyUI's Set Latent Noise Mask does), not at Text Encode or the Sampler: one place for every model. An inpaint model's masked picture lives in the conditioning in stable-diffusion.cpp, so one patch hook sets `c_concat` from the sample's own latents when it reads a conditioning, as a single run does; the alternative, giving Text Encode the picture and mask as well, would ask for the same wire twice | 8.10, 14.4 | medium-high |
| The helper, not Fugue, decides how an image model uses a mask, by the library's version name (inpaint models: the picture; others: the start latent): it owns stable-diffusion.cpp's behaviour, and the names are checked when the pin moves. Fugue's `model_shape` only learns the inpaint models' wider input so it stops calling them mismatched | 8.10, 14.4 | medium |
| Composite back is a node in staged graphs (Composite Masked, Stitch), and built into Generate Image, the coarse node whose promise is that the outside is unchanged. Soft edges come from Feather Mask and Stitch's blend, in pixels, because the library rounds the mask in the latent | 14.4 | medium |
| Names and defaults the owner may change: Feather Mask (`fugue/feather-mask`, a blur of the mask outward by default; ComfyUI's FeatherMask, which fades from the picture's borders, would convert to another node), Combine Masks for MaskComposite, Crop to Mask's padding 64 px and 1 megapixel, Stitch's blend 16 px, the overlay in the accent at 50% | 14.4 | medium |
| Both templates run FLUX.2 Klein 9B, the files the edit templates already use, by masked sampling with the picture as a reference: the owner has the files and they are checked staged; Klein 4B (Apache-2.0) is the alternative with a licence for commercial use, FLUX.1 Fill the dedicated model once its file is at hand. Whether the reference helps or makes Klein keep the old content is checked on the 3090 when the template is built | 11.10, 14.4 | medium |
| No differential diffusion now: the library rounds the mask, and thresholding it per step is a change to sampling code, which the patch's rule excludes (8.10). It waits for the owner: carry it in the patch, or propose it upstream (17) | 8.10, 14.4, 17 | high |
| Automatic masks are optional packs of command nodes, not built in: each brings a model and a runtime (ONNX Runtime or Python) that Fugue's binary should not link, and their weights' licences vary (BRIA's are non-commercial) | 14.4 | medium-high |

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
| #87 Text Generate, Enhance Prompt | 14.2, 8.1, 8.7 |
| #140 Enhance Prompt styles as prompt templates (video, MiniMax H3, music) | 14.2, 8.9, Appendix A |
| #28 Load Video, Load Audio, Generate Video, Generate Song | 14.2, 8.1, 8.7 |
| #88 Klein 9B edit: references, Resize to Megapixels | 14.1, 14.3 |
| #83 workflow templates, missing models on open | 11.10, 4.3, 13.1 |
| #89 model downloads | 8.9, 4.3, 13.1, 13.3 |
| #91 export for sharing | 11.5, 4.3, 13.1 |
| #90 node versions side by side | 3.6, 6.5, 6.6, 10.1 |
| #29 more pixel and mask nodes | 14.2, 2.1 |
| #167 masking and inpainting: mask editor, mask nodes, staged inpaint, crop and stitch, templates | 14.4, 2.1, 6.1, 6.2, 8.1, 8.10, 13.3, 17, Appendix A |
| #30 agent skill | 13.4, 13.5 |
| #31 providers as modules | 8.8 |
| #32 component-level helper | 8.10, 2.6 |
| #33 component nodes | 14.3, 2.5, 2.6, 8.9 |
| #34 CI recipe | 17 |
| #105 video stages in the helper and nodes | 8.10, 8.1, 2.6, 14.3 |
| #133 media viewer: the media browser and the workflows panel's Results view | 12.14, 9.7, 11.7, Appendix A |
| #159 side tab strip | 12.14, 12.15, Appendix A |
| #141 cache management | 10.5, 10.6, 13.1, 12.7, Appendix A |
| #134 upscaling: Upscale Image and Video, Upscale with Model, the model catalog and picker downloads | 8.1, 8.9, 14.1, Appendix A |
| #138 Upscale Video with Model: frames streamed through the helper's upscaler | 8.1, 9.2, 14.1, Appendix A |
