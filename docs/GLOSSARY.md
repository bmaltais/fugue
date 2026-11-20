# Fugue

Fugue is a node-based workflow app for local AI. Users wire nodes into a graph and run it. These are the words the project uses, in code, docs and issues. The design is in [spec.md](spec.md).

## Language

**Node**:
One placed step in a graph, an instance of a node type, with its own input values.
_Avoid_: block, module, operator

**Node type**:
The definition a node is made from: its id, version, named inputs and outputs, and what it needs to run. Built-in or supplied by a pack.
_Avoid_: node class, plugin

**Widget**:
The control an unwired input shows for editing its value, chosen from a fixed set.
_Avoid_: control type, editor

**Input**:
A named, typed slot on a node that takes either a literal value or a link from another node's output. There is no separate "parameter" kind.
_Avoid_: parameter, socket, pin

**Output**:
A named, typed slot on a node that carries a value it produced or used, such as the seed it actually ran with. May be optional.
_Avoid_: result, return value

**Port type**:
What kind of value an input or output carries. Six are built in (Text, Number, Boolean, Image, Video, Audio); packs can declare more, as resource types or data types.
_Avoid_: socket type, value type

**Resource type**:
A port type declared by a pack whose values are handles to something loadable, such as a model, a LoRA or a ControlNet. Namespaced and versioned.
_Avoid_: model type, custom type

**Resource handle**:
A serializable, hashable value that names a loadable resource (which file, its content hash, what it needs), not the loaded weights.
_Avoid_: model object, loaded model

**Data type**:
A port type declared by a pack whose values are files a node produced, such as a latent or conditioning.
_Avoid_: tensor type, blob type

**Media handle**:
The value an Image, Video or Audio port carries. It can stand for a file, a decoded form, or both; video is always file-backed.
_Avoid_: buffer, blob

**Graph**:
The nodes of a workflow with their inputs and links, containing no layout.
_Avoid_: pipeline, flow

**Compiled graph**:
The flat form the engine runs: groups flattened, bypassed and muted nodes resolved, no layout. The engine and the CLI only ever execute this.
_Avoid_: prompt, API format, flat graph

**Path id**:
The id of a node in a compiled graph: the ids of the groups it sits in, from the outside in, then its own, joined with `/`.
_Avoid_: full id, node path

**Workflow**:
A saved document: a graph, its layout, and what it requires.
_Avoid_: project, scene, patch

**Model folder**:
A folder Fugue finds models in: its own (`~/.local/share/fugue/models`) or one the settings add, in ComfyUI's layout or holding one kind. In code, a *root*. The one downloads go in is the *download folder*.
_Avoid_: model path, models dir

**Part**:
A download not finished yet, `<file>.part` beside where the model goes; it becomes the model only once its hash is the handle's.
_Avoid_: temp file, partial

**Template**:
A ready-to-run workflow listed in the window and by `fugue template list`: a folder of the workflow, its description, a preview and sample pictures. Opening one makes a new workflow from it; the template itself is never edited. Bundled ones come from the fugue-workflows repo; a person keeps their own in ~/Documents/Fugue/templates.
_Avoid_: example, preset, starter

**Layout**:
The positions, sizes, colors, comments and frames of a workflow. Optional; removing it never changes what the graph does.
_Avoid_: canvas state, UI state

**Requirements**:
The node types and versions, model references and Fugue version a workflow needs, recorded in the workflow.
_Avoid_: dependencies, manifest

**Sink**:
A node type marked as a graph's end point (Save, Preview, Export). A run executes every sink and everything upstream of it.
_Avoid_: output node (clashes with Output), terminal

**Run**:
One execution of a graph, or of one node and what it needs.
_Avoid_: job, queue item, prompt

**Run folder**:
Where a run writes the results its nodes make, so they survive and can be cached.
_Avoid_: temp folder, output folder

**Bypass**:
A node passes its input straight through when the types match.
_Avoid_: skip, disable

**Mute**:
A node does not run, and the nodes that depend on it are skipped.
_Avoid_: disable, off

**Frame**:
A purely visual box with a color and a label that moves and copies as one. It has no effect on how a graph runs.
_Avoid_: group (that is a different thing), backdrop, comment box

**Group**:
A reusable unit made from selected nodes that collapses into one node. It is a node type with its own id and version, and its nodes live in its definition.
_Avoid_: subgraph, macro, compound node

**Exposed input**:
An input on a group's face that drives one or more inputs inside it. Keeps the inner input's type, default and constraints.
_Avoid_: promoted widget, proxy input

**Face**:
What a group shows as a node: its exposed inputs and outputs, typed by the inner ports they lead to.
_Avoid_: interface node, proxy

**Level**:
One graph the canvas shows: the workflow's own, or the inside of a group entered. The groups entered, outside in, are the canvas's scope (`scopePath`, the breadcrumb).
_Avoid_: layer, depth

**Section**:
A named run of inputs on a node face, under a header row. The section "Advanced" is folded until opened.
_Avoid_: tab, panel

**Library**:
The groups the user saved from workflows, kept as node types in the reserved `local` pack.
_Avoid_: templates, favorites

**Pack**:
A folder or repository that supplies node types and groups, with its own id and version. Packs are installed and trusted as a unit.
_Avoid_: plugin, extension, custom node folder

**Manifest**:
The JSON file that describes a node type (`node.json`), a pack (`pack.json`) or a provider (`provider.json`).
_Avoid_: schema, descriptor, config

**Command node**:
A node whose type runs as a separate program that speaks JSON over stdio.
_Avoid_: script node, external node

**Share copy**:
An export of a workflow with local paths removed, and optionally prompts and layout.
_Avoid_: clean export, public copy

**Capability**:
What a node asks a provider for, such as generating an image or encoding text, instead of naming a server.
_Avoid_: endpoint, service

**Provider**:
A configured source of capabilities: the Omarchy model servers, Fugue's own helper, a generic HTTP endpoint or an optional ComfyUI server. Local or remote is only its address.
_Avoid_: backend, server

**Result cache**:
The on-disk, content-addressed store of node results, kept apart from models held in memory.
_Avoid_: temp folder, output folder

**Provider instance**:
One configured use of a provider: a name, an address and a key. A node can pin one by name.
_Avoid_: connection, endpoint, server

**Freeze**:
Keep a node's current result so it is reused until unfrozen, whatever changes upstream.
_Avoid_: lock, pin (that is the canvas action that fixes a node's position)

**Command (registry)**:
An action of the window with a string id (`file.save`), registered once with its description, default keys, context and a handler. The menus, the `?` overlay and the settings read the one registry. The user's keymap is a diff against its defaults.
_Avoid_: action id, menu item, shortcut (a shortcut is only one key bound to a command)
