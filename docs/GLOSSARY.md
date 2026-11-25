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

**Mask**:
A one-channel Image where 1 is the part to change and 0 the part to keep. Not its own port type; a mask port is marked with the role `mask` and drawn hollow.
_Avoid_: selection, alpha (a picture's transparency only becomes a mask through Load Image), matte

**Inpaint**:
Redraw the masked part of a picture and keep the rest. Outpaint is inpainting a border Pad added. An inpaint model takes the mask itself; any other model is held to the rest of the picture at every step (masked sampling).
_Avoid_: fill (Fill Masked is the pre-fill under a mask), repaint

**Outpaint**:
Extend a picture past its edges: Pad adds the border and a mask over it, the model draws only the border, and the original is put back over the result through the mask. The Outpaint template does this.
_Avoid_: expand, uncrop

**Crop and stitch**:
Inpainting a small region at the model's full detail: Crop to Mask cuts a box around the mask, the model redraws the box at about a megapixel, and Stitch puts it back through a soft edge. The group Inpaint (crop and stitch), and the Inpaint template, are this.

**Stitch handle**:
The value Crop to Mask gives Stitch: the original picture, its mask, the box that was cut out and the size it was redrawn at. A data type, `fugue/stitch`.
_Avoid_: stitcher, crop info

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
_Avoid_: scene, patch (a *project* is a workspace, below)

**Model folder**:
A folder Fugue finds models in: its own (`~/.local/share/fugue/models`) or one the settings add, in ComfyUI's layout or holding one kind. In code, a *root*. The one downloads go in is the *download folder*.
_Avoid_: model path, models dir

**Part**:
A download not finished yet, `<file>.part` beside where the model goes; it becomes the model only once its hash is the handle's.
_Avoid_: temp file, partial

**App**:
A workflow with an `app` block: a few inputs worth changing and the results, shown as one simple screen (the App view). The graph stays the truth; there is no second file format.
_Avoid_: mini-app, form, wizard

**App block**:
The top-level `app` key of `workflow.fugue` that makes a workflow an app: title, inputs bound to node inputs, and outputs. Outside the cache key, kept by older Fugue.
_Avoid_: app node, app file

**App view**:
The screen of an app: the form, Run, the stage and the takes. A mode of a workflow's tab, a view type of an area, or the whole of an app's own window; Open in Graph shows the same workflow as a graph.
_Avoid_: app mode, simple mode

**App bundle (.fugueapp)**:
One file that shares an app with its packs and sample pictures, models kept as references. Opening it asks once on the Get ready sheet, then the app runs in a window of its own.
_Avoid_: app package, app export

**Take**:
One run of an app, shown newest first in the App view's strip with the form it was run with. A take is a run of the existing History; the app keeps no store of its own.
_Avoid_: result set, attempt, version

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

**Canvas gesture**:
What one press of a pointer button on the canvas does from press to release: a pan, a move, a resize, a link drag or a marquee (`CanvasGesture`, `model.gesture`). It decides by what is under the press and the modifiers, and makes the model edits itself; the window only passes the pointer in and draws.
_Avoid_: drag handler, mouse state

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

**Project** (epic #204, spec 1.4.2; selection, registry and `fugue project` are built, inheriting and the window's switcher are not yet):
An isolated workspace: one root folder (`FUGUE_HOME`) with a `project.json` and its own settings, providers, model sources, packs, recent files and saved results, and a short explicit list (`inherits`) of what it takes from the *Default project*, the ordinary session, which has no `project.json` and the reserved name `default`. A *private* project has a vault (ephemeral or encrypted) as its root and stricter defaults; an ephemeral one is a *temporary project*. A workflow stays the saved document; a project holds the sessions that use workflows.
_Avoid_: workspace, profile, vault (the vault is a project's storage, not the project)
