# Canvas actions: design

Design for [#231](https://github.com/bmaltais/fugue/issues/231), part of epics #206 and #216. It is written
2026-11-22 against `main` at 4855af87, and it is a recommendation, not a survey. Claims marked **[U]** could not be
checked against a source. Every Adobe help page returned 403, so the Photoshop rows rest on search snippets and
are [U]. Related work: [canvas.md](canvas.md), [spec 14.5](spec.md#145-the-canvas-revision-store-206-208),
[canvas-research.md](canvas-research.md), [workspaces-research.md](workspaces-research.md) and
[ui-ergonomics.md](ui-ergonomics.md) (References, Cohesion checklist).

## The design in brief

A **canvas action** is an ordinary workflow file, with Canvas Region, Canvas Write and the model nodes in it, plus a
small top-level `action` block. The block names the inputs the canvas fills in: prompt, strength, seed, model and
the advanced ones. The Canvas view shows a **task bar** under the selection: an action menu, a prompt, a Generate
button and an overflow menu (⋯).

When you press Generate:
1. Fugue copies the workflow and puts the canvas, the selection, the head revision you were looking at and the bar's
   values into the copy.
2. It queues the copy as an ordinary run.
3. Each finished run lands as a layer on the head (#229).

The bundled actions are templates in fugue-workflows. Your own actions are your own templates, per project. **Edit
workflow** opens the same file as a tab in the Graph, so whatever you change and save is the action. A hobbyist never
sees a node. A film team keeps its own actions in the project, versioned and shared as packages.

What this design does not do:
- Make Fugue a paint program. The scope guard of #206 holds.
- Add live mode to the first slices. It is a later, separate toggle: [16](#16-live-link-315).
- Generate nodes dynamically. The Krita maintainer warns this is costly to keep working (discussion #385).
  Fugue's bundled actions are fixed graphs the user can see.

## 1. The action model

### 1.1 Where the binding lives: a block in the workflow file

There were three candidates. The recommendation is the **top-level `action` block**.

| Place | Verdict |
| --- | --- |
| **`action` block in `workflow.fugue`** (chosen) | It travels with every copy: save, Save as, a template, a `.fuguepack`, recovery, the tabs session. **Edit workflow** edits the file the action *is*, so the round trip is free. An older Fugue keeps the block on a round trip, because unknown top-level keys are kept (spec 4.1, `Document::extra`). The block is not in `sourceHash`, so it changes no cache key and no compiled `source`. ComfyUI's App mode works the same way: its builder picks a workflow's inputs and outputs and keeps them as that workflow's interface ([docs.comfy.org/interface/app-mode](https://docs.comfy.org/interface/app-mode.md); whether the interface is stored in the workflow JSON is [U]). |
| Node roles or tags (Krita's `Parameter` nodes) | Rejected. Krita binds by wiring a widget into a `Parameter` node ([docs.interstice.cloud/custom-graph](https://docs.interstice.cloud/custom-graph/)). That adds nodes whose only job is UI, puts presentation into the graph, and makes a plain workflow differ from an action. Fugue does keep one role-like rule: the Region and the Write are found **by node type**, which is safe because only those two node types touch a canvas. |
| `template.json` metadata | Rejected. It exists only for templates, so a workflow saved as a plain file, or opened from a package, would lose its binding. `template.json` keeps only what a template card shows. |

The block reuses the shape of a group's exposed inputs (`groups.<id>.inputs[]`, spec 4.4 and 5.4): `targets`,
`label`, `section`, `default`, `min`, `max`, `choices`. So there is one interface vocabulary, and the editor can
follow the group interface editor (*Node > Group inputs and outputs…*).

A group cannot serve as the action itself: a group's sinks never write when it runs as a node (spec 5.5), and Canvas
Write is a sink.

### 1.2 Minimal schema

```json
"action": {
  "format": 1,
  "name": "Generative fill",
  "description": "Redraws the selection from a prompt.",
  "glyph": "sparkles",
  "selection": "required",
  "region": "region",
  "inputs": [
    {"role": "prompt",   "targets": ["encode.prompt"], "label": "Prompt", "optional": true},
    {"role": "seed",     "targets": ["sample.seed"]},
    {"role": "strength", "targets": ["sample.denoise"], "default": 1, "min": 0.05, "max": 1, "section": "Advanced"},
    {"role": "model",    "targets": ["models.diffusion_model"], "section": "Advanced"},
    {"name": "steps",    "targets": ["sample.steps"], "label": "Steps", "section": "Advanced"}
  ]
}
```

| Field | Meaning |
| --- | --- |
| `format` | 1. A newer format is listed as "Needs a newer Fugue" and cannot be run. |
| `name`, `description`, `glyph` | What the action menu shows. A glyph comes from the Lucide subset; add any that is missing. |
| `selection` | `required` (Fill, Remove, Refine), `optional` (Remove background: the whole layer when nothing is selected) or `bounds` (Expand: the canvas bounds set with the crop tool). |
| `region` | The id of the Canvas Region that receives the selection. It may be left out when the workflow has exactly one Region. |
| `inputs[].role` | One of a fixed list of roles the bar knows: `prompt`, `negative_prompt`, `strength`, `seed`, `model`, `layer` (the current layer's name). An input with no role is a plain one and needs a `name`; it shows only in the Advanced section. |
| `inputs[].targets` | One or more `<node id>.<input>`, all of one type, as in a group. The target may be a top-level node or a group instance's exposed input. |
| `inputs[].choices`, `choice_values` | `choices` are words the bar offers; with `choice_values`, a list of numbers of the same length (slice 4), each word stands for its number and the binding puts the number in the targets, which must take a number. `default` is then one of the words. Refine: `["Light", "Default", "Restyle"]` and `[0.55, 0.7, 0.9]`. A list of another length, a value that is not a number, a repeated word, a default that is none of the words or a target that is not a number is `action_invalid`, in words. |
| `optional` | The prompt may be empty. Photoshop allows Generative Fill with an empty prompt [U]. |

The **count** of variations is not an input. The bar makes the count itself, as N runs; it is hidden when the action
has no `seed` role, because the runs would come out the same.

The `inputs` list is shared with an app's block (spec 4.9, `engine/input_binding.*`): one reader, one check of the targets, one binding of the values. An input with a role and a `name` is keyed by the name (an app with two pictures); an action's inputs have no such need.

Things the block never says, and why:
- **Which canvas.** Fugue fills in the shown canvas.
- **Which revision.** Fugue fills in the head that was shown when you pressed Generate.
- **The rectangle.** It comes from the selection.

Because of this, an action file holds no canvas id. The bundled `generative-fill` template already leaves the id
blank (spec 14.5).

### 1.3 How the task bar finds what it needs

1. It loads the workflow and reads `action`.
2. **Region.** It uses `action.region`, or the one `fugue/canvas-region` node. The action's selection goes into that
   node as `x`, `y`, `width`, `height`, or as `selection` (see 3). Its `canvas` input is set to the shown canvas and
   its `revision` to the head's full hash.
3. **Other Regions.** An action may read a reference or a second layer this way. A Region whose `canvas` is blank gets
   the same canvas and revision, so the run reads one consistent picture. Its rectangle and `layer` are left as
   written, so 0, 0, 0, 0 reads the whole canvas. A Region that names a different canvas is left alone.
4. **Writes.** Every `fugue/canvas-write` must take its `canvas` from a Region of the shown canvas. Otherwise the
   action is invalid and shows "This action writes to another canvas". Each Write lands its own layer.
5. **Inputs.** Each bound input's value is put into its targets in the run's copy of the workflow. The file is never
   changed.
6. **Validation.** The action is listed as broken, with the reason and **Edit workflow**, when a target is missing,
   has the wrong type or is linked, or when the action has no Region, no Write, or two Regions and no `region`. This
   is the same rule as a group's exposed input whose inner node is gone (spec 5.4), so a pro who deletes the Text
   Encode is told at once.

Fugue never guesses a binding when it runs an action. Guessing only happens when the action is authored: **Make
action…** proposes a block by walking back from the Write, which is the walk Canvas Write already does to name its
layer. It proposes the `prompt` of the Text Encode it meets, the Sampler's `seed` and `denoise`, and the model
loader. The person then confirms it in the action inputs editor (slice 9).

### 1.4 Advanced inputs

An input in `section: "Advanced"` shows only in the bar's advanced state (see 4). The bar draws it with the same
`InputWidget` a node face uses. That gives three things for free:
- the model picker, with its **Download** rows and licence question (#134);
- drag-number fields for strength and steps;
- the seed widget: Random, or Fixed.

The default value is the block's `default`, else the workflow's own value. Values the person changes in the bar are
remembered per action and per root (see 7), not written into the file.

## 2. The bundled actions

Three actions need a new feature in Canvas Write or the store. These are the ones that change the canvas size or an
existing layer.

| Action | Selection | Workflow (nodes) | Lands as | What exists today | Missing |
| --- | --- | --- | --- | --- | --- |
| **Generative fill** | required | Region, then Text Encode (refs: the region), VAE Encode (image + mask), Sampler (FLUX.2 Klein, 4 steps), VAE Decode, Write | a layer named from the prompt, with the feathered selection as its layer mask (#226) | template `generative-fill`, verified on the 3090 | the `action` block; Write `candidate` off (#229) |
| **Refine** | required | the same graph, with strength bound to the Sampler's `denoise` (default 0.7, a noise level on a flow model; light 0.55, restyle 0.9; values differ from ComfyUI's `denoise`, a step fraction: see the release note in inpaint-outpaint-research.md 4.7) and no full redraw | a layer | the Fill graph | its own template file. Krita's split of Fill (100%) and Refine (below 100%) is the model here (spec Appendix A, masking research). |
| **Harmonize** | optional (none: the whole picture) | the Refine graph with a seam map built from the selection's edge (Grow Mask out and in, Combine Masks), the seam softened in the reference (Blur, Composite Masked), the Sampler's soft mask on, strength 1 (below 1 the softened seam stays as a frosted ring, verified on the GPU), then Color Match to the picture | a layer "Harmonized" | template `harmonize` and its flat sibling `harmonize-image`, measured once on a made-up collage (canvas.md, Harmonize) | nothing; the seam width is in picture pixels, not relative |
| **Remove** | required | Region (padding 128), Fill Masked (`blur`), VAE Encode with mask, Sampler with a fixed hidden prompt ("the empty background, continued"), Decode, Write | a layer "Remove" | the nodes, Fill Masked included | template. Quality without LaMa or MAT is **[U]** and must be checked on the 3090 (the LaMa variant is `inpaint-workflows/remove-lama`, the Remove Object pack, #245: the action may use it when the pack is installed, Fill Masked otherwise). Fallback: an inpaint-model variant. |
| **Remove background** | optional (none: the current layer) | Region (`layer` = the current layer, padding 0, scale 1, no cap), Remove Background (BiRefNet pack), Write | the subject mask becomes the **layer mask of the current layer**, as Photoshop does [U] | the pack (`examples/packs/io.github.bmaltais.fugue-remove-background`) | Write 1.3 `onto` (write a layer's mask rather than add a layer, a #226 leftover); the `layer` role; the pack-missing flow in the bar |
| **Expand** | `bounds` (the crop tool beyond the canvas) | a canvas resize, then a Fill whose selection is the new border plus 32 px of overlap | a canvas resize, then a fill layer | template `outpaint` (image, not canvas) | a **canvas resize revision** in the store (new size, layers moved and re-tiled; it changes the tree size, so it is not a layer op); then the Fill action unchanged. **Cancelled or failed (#310):** the growth is taken back when the head is still the growth revision (nothing else changed the canvas): the head moves back to the revision before it, the same compare-and-swap as the bar's Undo (`CanvasStore::undo`), so the growth revision stays in the history (immutable) and Redo brings it back; the bar and `canvas act` say "The canvas is back to its size: Expand was cancelled." ("... did not finish." for a failure). A head that moved meanwhile (a Count N variation landed, a person edited) is left alone. A refusal while binding, after the growth, takes it back too. The rule is `canvas::takeBackGrowth` in `engine/canvas_act`, shared by both paths |
| **Upscale** | none | Region (the whole composite, no cap), Upscale with Model (Real-ESRGAN x4), then a new **Canvas New** sink | **a new canvas** beside the source, which stays as it was; the view switches to it | template `image-upscale` (image) | the sink, which makes a canvas from an image with producer `action:upscale`. Resizing every layer of the source in place was rejected: it is costly, it loses detail in the masks, and it is a document step, not a layer. |

The **default model** is FLUX.2 Klein 9B Q8, as the template has today: about 19 GB to download with its encoder.
Klein 9B is non-commercial and 4B is Apache-2.0 (spec Appendix A, masking research). That matters for film teams,
so the default is an open question (see 13).

## 3. The selection path into the Region

The Canvas view **owns** the selection, as Photoshop does. A layer mask is a separate thing, as #230 and #226 already
keep them apart. Today's frame edits a Canvas Region node that the view follows. That stays for the pro loop, where a
Graph and a Canvas sit side by side. The task bar uses the view's own selection.

| Selection | Made by | Into the Region |
| --- | --- | --- |
| Rectangle | the rectangle tool (today's `CropFrame`, owned by the view when no node is followed) | `x`, `y`, `width`, `height` literals |
| Lasso | lasso tool (#216) | mask (below) |
| Quick Mask | Q and the mask brush (#230) | mask |
| A layer's mask as the selection | Ctrl+click the mask thumbnail (Photoshop's load-selection gesture [U]) | mask |
| Subject | a **Select subject** entry (BiRefNet, the same pack) | mask, produced by a run, later |

**How a mask reaches the Region: by hash, not by file.** When you press Generate, the view stores the selection in the
canvas store as one-channel tiles. This is the encoding layer masks already use, so an equal selection is stored once.
The store keeps the selection at `selections/<hex>.json`, named by its canonical hash. The Region gets a new advanced
input, `selection` (Region 1.2, text, `sha256:…`). With it set, the mask is read from the store, and `mask` and the
rectangle are ignored.

Why a hash rather than a PNG fed through a Load Image node:
- **Cache key.** The hash is a literal, so it sits in the cache key and in `compiled.json`. A replay is exact, and
  the same selection on the same revision is a cache hit for the Region.
- **No graph surgery.** The run's copy of the workflow gains no node, and Edit workflow opens the file as it is.
- **Region's own path.** This is the mask path the Region already has: bounds, then padding, then feather.

#230 point 5 plans a "Selection from" input on Canvas Region. It should be this same `selection` input, so that a
workflow run from the Graph and an action read a selection the same way. That needs coordinating with the agent on
#230 (see 12). For a rectangle, the hash is not used: the four literals are enough.

Built with #230 (2026-11-22): Region 1.2's `selection`, `CanvasStore::storeSelection` and `selection`, and Quick Mask's
**Use selection**, which writes the hash and the selection's bounds into the followed Region as one undo step (spec 14.5).

## 4. The task bar

**Placement.** It is a floating bar inside the Canvas view's picture rectangle, in both hosts (the File > Canvas…
dialog and a Canvas area).
- It anchors **under the selection's bounding box**, 2u clear of it. It flips above when there is no room, and stays
  4u inside the picture.
- With no selection it sits centred at the bottom.
- A grip pins it to a place, stored as fractions of the view like the run toolbar's `place` (#120). The ⋯ menu has
  **Reset bar position** and **Hide bar**; View > Bars and panels shows it again. Photoshop's bar has the same three:
  drag handle, pin and hide [U].
- During a pan or zoom it follows the selection, and it never covers the selection.

**Simple state.** The default. From the start of the bar:
- the action menu (glyph and name, a chevron);
- the prompt field, which grows, one line, Enter to generate;
- **Generate**, the bar's one primary and the only accent fill;
- ⋯.

With no selection, an action that needs one shows "Select an area first" in place of the prompt. The menu lists the
actions that can run, with Remove background and Upscale first, as Photoshop offers Select subject and Remove
background when nothing is selected [U]. In an area below 360 px wide the action menu folds to its glyph.

**Advanced state.** A popover from ⋯ or a chevron, hung from the bar. It holds:
- the action's Advanced inputs (1.4);
- **Count**, 1 to 4 in the first build, one constant (see 5);
- **Edit workflow** and **Make action…**;
- the action's licence line when its model is non-commercial.

The popover is transient. Its values stick per action.

**Prompt history.** Up and Down in an empty prompt field step through earlier prompts. A clock button in the field
opens the last 20 prompts for this action, and the search matches in it. History is kept per root in the state folder
(`canvas-prompts.json`, 200 entries per action, atomic write). It is never inherited by a project: prompts can hold
client text. A ⋯ entry, **Clear prompt history…**, asks first, highlighting Keep.

A generated layer can be reused: select it and the bar offers **Use this layer's prompt**. The layer's origin gets an
optional `run` field (the run id). It is in the tree hash, as `time` already is, and from it the bar reads the run's
`workflow.json`.

**Progress.** While an action runs:
- the selection's outline gets a slow shimmer (a moving gradient on the marching ants, made from `look` tokens; with
  reduced motion it stays a static dashed accent);
- the bar shows "Generating… 45%" from the Sampler's `node.progress`;
- with several runs queued it shows "2 of 3".

**Live frame in the selection (built).** While a run goes, `CanvasActions` keeps the newest `node.preview` thumbnail of the
active run (`liveFrame`) with two rectangles in canvas pixels, made when the run starts from the bound Region's selection,
padding and feather: `liveBox`, the padded context box the Region cut (the frame is a model-size picture of it, so it is
drawn stretched onto that box), and `liveClip`, the selection inside it. `CanvasLiveFill.qml` draws it above the picture,
under the region frame, clipped to the selection, with the shimmer dimmed to half. The rule for which node: the newest
preview of the run wins (in practice the Sampler's step frames, the only node of an action that previews while it runs; a
Sampler inside a group needs no path rule). Other runs' events are ignored. At a good end the frame fades over 200 ms
(the layer is already on the canvas); a cancel or an error removes it at once. It uses the Sampler preview setting
(`step_preview`): when it is Off no event comes and only the shimmer shows.

**Cancel.** Generate becomes **Cancel** while this bar's runs are waiting or active. Escape also cancels while the
view has the focus, as `run.cancel` does. Cancel ends all the runs this press started, waiting ones first. A cancelled
run writes nothing.

**Errors in plain words.** One line in the bar, in `look.error`. **Details** opens the run in the queue panel. The
sentences:

| Code | Line |
| --- | --- |
| `mask_empty` | "The selection is empty. Draw one, then Generate." |
| a provider or capability error (8.5) | "No model on this machine can run Generative fill." followed by the download offer (below) |
| out of GPU memory | "Not enough GPU memory. Try a smaller selection, or a smaller model (Advanced)." |
| `stitch_mismatch` | "The action's workflow returned a picture of another shape. Edit workflow to fix it." |
| `canvas_conflict` (size changed during the run) | "The canvas changed size while this ran. The result is kept in History. Run again." |
| a cancelled run | nothing (no error) |
| an invalid action | the validation reason (1.3), with Edit workflow |

**Models missing: the download flow.** When the bar opens on an action, it checks the action workflow's
`requires.models` with `ModelLibrary::missing`, the same check a template card makes. If models are missing:
- the bar shows "Needs 3 models, 19.4 GB" with **Download**, which goes through the existing download line, its
  folder choice and licence (#89, #134);
- Generate stays disabled, with the tip "Download the models first";
- progress shows in the bar ("Downloading 2 of 3, 4.2 of 10.2 GB") and in the window's banner;
- a template's `Download` (`Workflows::downloadTemplate`) is the function to call.

A missing **pack** (Remove background) shows "Needs the Remove Background pack" with **Install…**, which goes to
Settings > Packs through the install and trust questions (6.6, 6.7). Nothing is fetched without asking.

**Cohesion** (ui-ergonomics checklist):
- the bar and popover reuse `ViewButton`, `ThemedTextField`, `InputWidget`, `WidgetMenu` and `PopupSurface`;
- Generate is the only primary; when the candidate banner shows, which after #229 is only for an explicit Keep as
  candidate, Generate turns quiet so two primaries never show at once;
- Cancel sits last in its chunk;
- the words are "Generate", "Cancel", "Edit workflow" (no ellipsis: no dialog follows) and "Make action…";
- the keys: Enter in the prompt generates. Ctrl+Enter keeps its Graph meaning (run.start) and does not generate.
  Whether it should is an open question.

Every control gets a Decisions row in ui-ergonomics.md when it is built.

## 5. Queue and cache behaviour

**One queue.** An action run is a `RunService::start(RunRequest{document, workflow})`:
- `document` is the bound copy and `workflow` is the action's file path;
- it goes into the shared queue, so it shows in the queue panel under the action's name, can be moved to the front,
  is cancelled the same way and follows the one-GPU lock;
- it has no Graph tab: a new small `CanvasActions` object (`canvas/canvas_actions.*`) owns these runs and reads
  their events, as `CanvasRuns` does for tabs;
- `CanvasRuns` ignores them because they are not in its tab map.

**Seeds.** The runner resolves the `seed` role itself, so every run records its seed and the bar can show it:
- Random (the default) is a fresh number per run;
- Fixed `s` with count N gives `s`, `s+1`, …, `s+N-1`.

**Variations (count N).**
- One press makes N runs, all with the same head revision and selection. Their Region, Text Encode and VAE Encode
  are cache hits after the first, so only the Sampler, the Decode and the Write run again.
- Each lands a layer. The first moves the head; the others find the head moved and are placed onto it by #229's
  "layers only add" rebase.
- The layers are named "<prompt> 1" … "<prompt> N". Variations 2 to N land **hidden**: a Write `visible` input
  (advanced, default on), set off by the runner.
- A small strip in the bar, "‹ 2 of 3 ›", shows one and hides the rest. Each pick is one layer edit, so it is one
  undo step, as the eye is.

This is Photoshop's Variations panel [U] made out of Fugue's layers. It matches the owner's "several seeds as separate
layers or a variations strip".

**Generate again on the same selection.** With the same action, an unchanged selection and a head that has only
gained this action's layers since the last press, the runner **pins the Region to the base revision** of the last
press. New results are siblings of the last ones (more variations), not a fill painted over the previous fill. Any
other change, such as a layer edited or a new selection, reads the head as it is.

**The same prompt twice.**
- With a random seed, the Sampler runs again (a new seed), so you get a new layer.
- With a fixed seed and nothing else changed, the whole graph, Write included, is a cache hit. Canvas Write is an
  ordinary cached sink (spec 14.5), so it writes nothing twice.
  - If its layer is still on the head, the bar says "Same prompt, seed and selection as '<layer>'. Change the seed
    to get another." That is correct, not a fault.
  - If the person has undone or deleted that layer, the runner **places the cached revision again**, with the same
    layers-only rule. Generate then always gives a visible result.
  - [U]: whether `accept` takes a revision marked `undone` today (redo does; spec 14.5). Slice 4 tests it.

## 6. The head and a moving head (#229)

- **The head is named when you press.** The runner sets the Region's `revision` to the full hash of the head that
  was **shown** at the press, not to `head`, so what you saw is what is read. With count N, every run shares it.
- **The bar runs on the head only.** When the view shows another revision or a candidate, the bar shows "Go to Head
  to generate" with a Head button, and Generate is off. This is the same rule the layers panel follows. Generating
  from an older revision would fork the history, which the layer edits never do either.
- **While a run is going** you keep working: painting a mask, hiding layers, another Generate. Each makes a revision.
  When the run's Write finds the head moved, #229 places its layer onto the current head as a new revision.
- **Undo** takes back the action's layer like any other step.
- **When the rebase cannot happen** (the canvas changed size, as with an Expand that ran in the meantime), the result
  stays a candidate. The bar shows the `canvas_conflict` line and History shows the candidate.

## 7. Edit workflow and the round trip

- **Edit workflow** opens the action's file as a tab in the Graph and makes the Graph's area active. The window
  always has one Graph area (spec 12.16). In the dialog host the dialog closes; the bar's state is kept for next time.
- **The bar runs what the tab holds.** While the tab is open, Generate uses the tab's current document, unsaved edits
  included, and the action menu marks it with "Unsaved changes". Change the steps in the Graph, press Generate on the
  Canvas, see the result: that is the pro loop. Once the tab is closed, or the edits are dropped, the saved file is
  used again.
- **Saving.**
  - An own action saves in place.
  - A bundled or inherited action saves through `TemplateLibrary::saveOwn`: a copy in this root's own templates
    folder that hides the original by its id (spec 1.4.2). This is how a film team sets **per-project defaults**
    (house model, steps, a LoRA): no new settings layer.
  - **Save as new action** is Save as template under a new id.
  - The library watch (`templatesChanged`) updates the bar's menu.
- **Bindings follow edits.**
  - Deleting a bound node makes the action broken, with the reason (1.3).
  - Renaming a node id updates the targets, as exposed group inputs follow (5.4).
  - Right-click a port, then **Expose to task bar**, adds an input; **Stop exposing** removes it. This mirrors the
    group's Expose on the group.
- **The values used are the run's.** The bar's values are never written into the file. To see what made a layer,
  open the layer's run from the queue history; its `workflow.json` is the bound copy.

## 8. Storage

| Where | What | Notes |
| --- | --- | --- |
| Template library, bundled (`templates/`, fugue-workflows) | the six bundled actions as templates (category `canvas`, `categories.json`) | an action is any template whose workflow has a valid `action` block; nothing else indexes them. They still open from File > Browse templates as plain workflows. `bin/check` validates the block. |
| Own templates of the root (`~/Documents/Fugue/templates`, a project's `<root>/documents/templates`) | own and edited actions | per project; a project that inherits `templates` sees the Default's own actions, read-only (spec 1.4.2) |
| Root state folder | `canvas-prompts.json` (history), `canvas-actions.json` (last action, sticky advanced values per action, count, bar place) | never inherited; atomic writes |
| Canvas store | `selections/<hex>.json` and their tiles | content-addressed, shared like tiles; no GC yet (as for revisions) |
| `.fuguepack` | an action is shared as its workflow's package (File > Export for sharing… from the tab) | opening a package whose workflow has an `action` block adds a banner, **Add to actions**, which calls `saveOwn`. A package never carries a canvas (spec 14.5). |
| `FUGUE_HOME` | everything above follows the root | `tests/cli/test_cli_home_guard.cpp` gains the new paths through `engine/paths.h` |

## 9. Hobbyist and film team

**Hobbyist.**
1. Open File > Canvas… (or a Canvas area) and drag a rectangle.
2. Type "a red umbrella" and press Generate.
3. On first use the bar says what to download and how big it is; one click gets it.
4. The result is a layer you can hide or delete; Ctrl+Z also takes it back.

They see one bar and five or six actions, with nothing about nodes, revisions or candidates.

**Film team.**
- Actions are text files in the project, so they can be versioned, reviewed and diffed.
- A house action swaps the model, adds ControlNet or a LoRA, and is saved per project, where it hides the bundled
  one.
- Every layer names its run, so the exact graph, seed and revision can be replayed (`fugue run compiled.json`).
- Actions are shared as packages, or through a shared project folder. Moving a canvas and its actions between peers
  waits for the mesh (#203).
- A CLI command lets a pipeline script run an action (slice 10):
  `fugue canvas act <canvas> <action> --rect x,y,w,h --prompt … [--count N] [--set node.input=value]`.

The same feature, the same file, only how deep you go differs. This is the product principle of #217.

## 10. Prior art behind the choices

- **InvokeAI** (Apache-2.0, docs opened). A workflow appears in the Canvas's **Run Workflow** list only when its Form
  Builder is on and it has an image input and a `Canvas Output` node. The raster layer goes to the first image input,
  and results arrive in the staging strip, one per Canvas Output
  ([invoke.ai/features/canvas/run-workflow](https://invoke.ai/features/canvas/run-workflow/)). Its Form Builder
  replaced the "Linear View" in 5.7.0 ([release notes](https://invoke.ai/releases/version/v5-7-0...latest/)). That is
  the same shape as here, with one difference: Fugue finds the Region by node type and keeps the form in the file's
  `action` block. Whether 6.13 lands the result as a new layer rather than staging it is [U]: the sources conflict.
- **ComfyUI App mode** (docs opened, frontend 1.41.13 and later): a builder picks inputs and outputs, and the app
  view edits the inputs in a right panel, then Run ([docs](https://docs.comfy.org/interface/app-mode.md)). Subgraph
  widgets can be shown or hidden from the Parameters Panel
  ([subgraph docs](https://docs.comfy.org/interface/features/subgraph.md)). Fugue's equivalent is the `action`
  block, edited by Expose to task bar.
- **Krita AI Diffusion** (GPL: docs only). The Custom Graph's `Krita Canvas`, `Krita Selection` and `Krita Output`
  nodes and its `Parameter` nodes (with `1.` ordering and `Group/Name`) build the docker's UI on import. Results are
  previewed, kept in a history, and applied as a new layer
  ([custom-graph](https://docs.interstice.cloud/custom-graph/), [basics](https://docs.interstice.cloud/basics/)).
  Fugue does not add parameter nodes (1.1), and it lands results directly (#229). The maintainer's remark on the cost
  of dynamic templates is why Fugue's actions are plain fixed graphs
  ([discussion 385](https://github.com/Acly/krita-ai-diffusion/discussions/385)).
- **Photoshop** (all [U], from snippets):
  - the Contextual Task Bar can be dragged by a handle, and its ⋯ menu has Hide, Reset bar position and Pin bar
    position;
  - Generative Fill's variations are chosen in the Properties panel, on a Generative Layer, and pressing Generate
    again adds more;
  - Generate Similar is in the bar's More actions;
  - the bar has a model picker that includes partner models (FLUX.2 pro, Gemini).

  Fugue takes the bar's placement, its pin, hide and reset, the variations-as-a-choice idea, and the model picker,
  which in Fugue is the Advanced model input.

## 11. Migrating the Generative fill template

These changes go in fugue-workflows, in the order the slices need them:
1. #229 already plans Write `candidate: false`. The description must then stop promising "kept as a candidate…
   accept or drop".
2. Slice 1 adds the `action` block (1.2) and moves the template to category `canvas`, keeping the `Canvas` and
   `Inpaint` tags. Opening it from Browse templates still gives the same graph with a blank canvas, so the Graph loop
   of today keeps working.
3. The node versions in the file stay valid: Region 1.0.0 and Write 1.1.0 are older minors. Later slices that need
   Region 1.2 (`selection`) or Write 1.3 (`onto`, `visible`) bump only the templates that use them.
4. Fugue's test for the template (`tests/nodes/test_canvas_nodes.cpp` and the generative-fill test) gains a test that
   the block validates against the graph.

## 12. Testing (offscreen, no real pointer)

- **Pure, no Qt Quick.**
  - The block parser and validator: roles, targets, a missing node, a linked target, two Regions, format 2.
  - The binder: workflow plus context gives a document, with exact JSON expected.
  - Seed series.
  - The "generate again" pin rule.
- **The runner, with `FakeProvider` and a temporary store.**
  - One press gives one run, one layer, and the head moves.
  - N presses give N layers with variations 2 to N hidden.
  - A moved head is rebased (#229).
  - Cancel writes nothing.
  - A cached Write is placed again after an undo.
  - A size change gives a conflict, with the candidate kept.
  - The models-missing state.
- **The window** (`tests/canvas/test_canvas_actions_window.cpp`; QTest events only, `QT_QPA_PLATFORM=offscreen`).
  - Draw a rectangle, type, press Enter, and the bar shows progress, then the layer shows in the layers panel.
  - Escape cancels.
  - An error line appears for `mask_empty`.
  - Edit workflow opens a tab, an edit in the tab changes the next run, and a save hides the bundled action.
  - `FUGUE_SHOT_DIR` pictures, light and dark, narrow area too, beside the run toolbar's.
- **Templates CI.** `bin/check` validates each `action` block; Fugue's tests run the pinned ones through the binder.
- **On the 3090, by hand.** The quality of Remove and Refine, and the time of a Fill with N=3.

## 13. Risks

- **Concurrent work.** #229 (Canvas Write) and #230 (tool strip, Quick Mask, Region "Selection from") are being
  built now in `CanvasView.qml` and `canvas_nodes.*`. The `selection` input (3) must be **one** design with #230's.
  Slice 1 must start after both merge, or touch only new files.
- **Licence of the default model.** Klein 9B is non-commercial. A film team that generates with the bundled action
  may be in breach without noticing. The Advanced section shows the licence line, but the default is the owner's
  call.
- **Download size and VRAM.** About 19 GB, and the 9B model will not fit on an 8 GB card. The first Generate is a
  large download, and it must say so before the click, which the models-missing line does.
- **A silent no-op from the cache.** Mitigated by placing the cached revision again (5). If `accept` refuses `undone`
  revisions, a small store change is needed.
- **Binding drift.** An edited workflow breaks its action. This is caught by validation with Edit workflow, never
  silently.
- **Store growth.** Selections and variations add objects, and nothing is deleted yet (spec 14.5). Each variation is
  only the tiles of its box; a hidden variation still costs its tiles.
- **Two runners.** `CanvasActions` and `CanvasRuns` both read run events. Keep one event path in `RunService`; never
  have both paint the same thing.
- **Unsaved tab drives the bar.** Powerful, and it could surprise someone. The mark in the action menu is the guard.
- **The task bar covering the work.** Anchoring and flipping (4) plus pin and hide; tested at the minimum area size.
- **Photoshop facts are [U].** None of them is load-bearing: each choice also stands on its own reasons.

## 14. Open questions for the owner

1. **Default model of the bundled actions**: Klein 9B (better, non-commercial, about 19 GB) or 4B (Apache-2.0,
   smaller)? Recommendation: 4B by default, with 9B offered in Advanced and its licence shown.
2. **Count default**: 1 (each Generate is one layer, fast and local) or 3, as Photoshop does [U]? Recommendation: 1,
   with Count in Advanced and the strip appearing from 2.
3. **Variations 2 to N land hidden** (needs a Write `visible` input)? Recommendation: yes.
4. **Upscale**: a new canvas (recommended) or the same canvas resized in place?
5. **A cached Write after undo**: place it again (recommended) or say nothing happened?
6. **The bar runs the open tab's unsaved edits** (recommended) or only the saved file?
7. **Ctrl+Enter in the Canvas view**: Generate, or keep it as Run of the Graph's tab? Recommendation: keep Run;
   Enter in the prompt generates.
8. **Prompt history**: 200 per action per root, never inherited, clearable. Is that right for client work, or should
   it be off in projects by default?
9. **Live link** (16): previews are never layers, and Live starts off every session. Both are built as recommended;
   the owner confirms, and says whether a fast "live" setting per action is wanted (16.5).

## 15. Slices, in order

Sizes: S is a day or less, M a few days, L a week or more.

| # | Slice | Size | Needs |
| --- | --- | --- | --- |
| 1 | **Task bar and Generative fill on the head.** The pure `action` block parser and validator. `CanvasActions`, which lists actions from the template roots, binds the canvas, the rectangle, the pinned head and the prompt, then sets a random seed and starts a run. A view-owned rectangle selection (the frame when no node is followed). The bar's simple state: action menu (one action), prompt, Generate/Cancel, progress percent, the shimmer on the frame, plain-word errors, and the models-missing line with Download. The result lands as a layer. The template gets its `action` block. Tests: pure, runner, window. | M | #229 merged; #230 merged or file-disjoint |
| | *Built 2026-11-22 (slice 1):* `engine/canvas_action.*` (parse, check, bind), `canvas/canvas_actions.*` (CanvasActions: list, Generate, cancel, plain errors, the cached Write placed again, prompt history), `canvas/CanvasTaskBar.qml`; the bundled Generative fill is `share/actions/generative-fill.fugue`, compiled in, until slice 2 moves it to fugue-workflows (category `canvas`) and the menu lists template actions. Decisions taken: the bar shows only with a selection; Auto falls back to Generative fill and says what to do for routes not built; the bundled prompt is not optional (an empty prompt changes nothing on Klein, research G8); padding and scale always come from the router. See spec 14.5 and docs/canvas.md. | | |
| 2 | Action menu with several actions; **Edit workflow** (a tab, the tab's document drives the bar, saveOwn hides the bundled one); prompt history | S | 1 |
| | *Built 2026-11-23 (slice 2, first part):* the menu lists every library template with a valid block (own first, then the compiled-in files as fallback; a template of the same id wins); the `generative-fill` template carries its block (Klein 4B, category `canvas`); **Edit workflow** (in `⋯` until the Advanced popover exists) opens the action as an unsaved tab and shows the Graph. Still to do of slice 2: the tab's unsaved edits drive the bar, `saveOwn` on save, Save as new action, bindings that follow edits. | | |
| 3 | Mask selections: the store's `selections/`, Region 1.2 `selection`, lasso, Quick Mask (#230), a layer mask as the selection | M | 1, #230 |
| | *Built 2026-11-23 (slice 3; branch canvas-selections):* the **lasso** (L, `BrushPen` with `selects`; `MaskLayer::selectPolygon`: anti-aliased polygon, replace/add (max)/subtract, one undo step) and a **layer as the selection** (`CanvasViewModel::selectFromLayer`: the layer mask or the layer's alpha; Ctrl+click the mask thumbnail, Shift adds, Alt subtracts; the row menu's Selection from layer / layer mask) both write into the view's one selection mask, the Quick Mask's, which the task bar now uses whenever it is not empty (not only while Quick Mask is on) and a drawn rectangle replaces. Generate, `storeSelection`, the live frame's bounds and Select > Inverse/Deselect/Modify are unchanged: they already took that mask. Decisions (each to confirm): (1) no modifier replaces, Shift adds, Alt subtracts, as Photoshop; (2) the release closes the outline, Enter is not needed; (3) the selection is drawn as the red tint, not marching ants (#232 owns those); (4) Ctrl+Z goes to the selection while the lasso is the tool or right after a selection changed, then back to the canvas; (5) Selection from layer reads only a paint layer's alpha, not a group's. **Not built:** marching ants, a polygonal lasso, Select subject, Select > Load selection from a channel; not seen in a real window; no GPU Generate with a lasso. | | |
| 4 | The Advanced popover (InputWidget for bound inputs, model picker), Count and variations (pinned base, Write `visible`, the strip), generate-again pinning, placing the cached Write again | M | 1 |
| | *Built 2026-11-23 (slice 4, first part; branch canvas-actions-4):* the **Advanced** button and popover (`CanvasTaskAdvanced.qml`): `CanvasActions.advanced(id)` loads the action's workflow, with the remembered values and the block's defaults put in, into a model of its own (`form`, a second `CanvasModel` no tab shows, with the window's downloads) and lists the inputs the bar has no field for; each is the Graph's `InputWidget` over `form.widgets` (`CanvasModel.inputPort` gives it the face's port map), so the model picker with Download, the drag number and the choice are the Graph's, not a copy. An edit (`CanvasModel.edited`) is read back and kept per action in `canvas-actions.json` of the state folder (only what differs from the action's own value; atomic; never inherited), and Generate puts it in the run's copy (the missing-models check and the bar's Download use the picked model). **Edit workflow** moved from `⋯` into the popover. `choice_values` (parsed and checked in `engine/canvas_action.*`, bound in `bindAction`) lets a word stand for a number: the **Refine** template (fugue-workflows) now offers Strength as Light 0.55, Default 0.7, Restyle 0.9. **Count** 1 to 4 (kept per action, shown only with a seed role): Generate binds and queues N runs of the same revision and selection, seeds random each or `s, s+1, …`, each Canvas Write with `suffix` "(n of N)" and, for 2 to N, `visible` off (Canvas Write 1.5.0); one `m_order`, `runNumber` and `runTotal` for the bar's "Generating 2 of 3… 45%"; Cancel ends all, a failed run ends the rest, the last frame wins. Decisions (each to confirm): (1) the button is the text **Advanced** on the bar's row, not a chevron on `⋯`, because a labelled button teaches the place and the row has room (a glyph alone below 90u); (2) Count 1 to 4, not 8; (3) layers are named "<name> (n of N)" for all N, the first included, so a strip can find them later (design 5 said "<prompt> n"); (4) the popover lists every bound input of the block except the prompt, seed and layer, not only `section: "Advanced"`, because the bar has no other place for them; (5) an input with `choice_values` is a row of word buttons, not an `InputWidget`, because the widget would show the number; (6) a value the person set is stored per action, and an action edited later keeps it by key; a stored word that is no longer one of the choices is ignored. **Not built:** the `‹ 2 of 3 ›` strip (the others are shown by their eyes in the Layers panel), "generate again" pinning to the last base revision, the licence line of a non-commercial model, Make action… in the popover. Not seen in a real window; no GPU run of Count.
| 5 | Refine and Remove actions (templates, checked on the 3090) | S | 3 |
| | *Built 2026-11-23 (slices 5 and 7, first part; branch canvas-actions-3):* templates `refine`, `remove` and `expand` (fugue-workflows, category `canvas`, from the #233 benchmark's workflows in `docs/inpaint-workflows`), in the action menu with no code change. **Refine**: Klein 4B, strength 0.7 (a numeric Advanced slider 0.3 to 1, as Harmonize's; the light 0.55 and restyle 0.9 presets are values of that slider until the Advanced popover exists, because a block's `choices` are strings only and nothing maps a word to a number), an optional prompt with the fallback "Refine the details, keeping everything as it is.". **Remove**: no prompt role (the hidden instruction is the Text Encode's literal), Fill Masked `blur` as the reference, padding at least 128 (Generate holds it there even when a typed prompt made the router pick Fill; LaMa is a pack and is only mentioned in the template's description). **Expand**: `selection: "bounds"`; Generate grows the canvas first with `CanvasStore::resize` (one revision, edge-blur pre-fill as the "Expanded area" layer, producer `action:expand`), then routes the grown canvas with `RouteInput::resize` and binds the Region to the new head, the strip plus the 32 px overlap, padding 256. How much to grow: the bar's "Grow the canvas" row (one side of Left, Right, Top or Bottom, 25% or 50% of the picture's side, in multiples of 16), sent as `request.expand = {side, percent}` (or `pixels`). Decisions: (1) one side per run, because the router's fill is the bounds of the grown strips, which for two opposite sides would include the old picture; Fooocus and the research advise one side at a time. (2) A rectangle dragged past the canvas edge is not the way in yet: the frame lives on the picture (CropFrame draws and presses only on it), so the bar's row says how much; the router already routes a selection past the edge, so the frame can feed `request.expand` later without a backend change. (3) The canvas grows before the run, and the growth is taken back when the run is cancelled or fails with nothing else changed (#310, see the Expand row); models are checked before it. (4) Expand's passes (a side grown by over half the picture) are not built: the bar offers 25% and 50%, which is one pass. (5) A larger fill with two or more sides needs a mask selection of the ring: with #230's selection input. Glyph: `maximize`, the subset has no `expand`. Still to do of slice 7: the frame past the canvas and several sides at once; of slice 5: the strength presets in the Advanced popover (slice 4). | | |
| 6 | Remove background: Write 1.3 `onto` (layer mask), the `layer` role, the pack-missing flow, optional selection | M | 1 |
| | *Built 2026-11-23 (slice 6; branch canvas-remove-bg):* Canvas Write **1.6.0** gets `onto` (a layer's name: write that layer's **layer mask** instead of adding a layer) and `layer_mask` (the mask picture); `image` is optional (needed unless `onto`). With a stitch the new mask is placed in its box and mixed into the old mask through the stitch's mask (`old*(1-m)+new*m`), so a selection changes the mask only inside it; without a stitch it is the whole layer. It goes through `CanvasStore::setLayerMask`: one revision, Undo takes it back, a locked layer refuses in words, a head that moved meanwhile writes nothing (`canvas_head_moved`). The template **`remove-background-action`** (fugue-workflows, category `canvas`): Region (`layer`, padding 0, scale 1, no cap, multiple 1) -> Remove Background Mask (the pack's node, BiRefNet) -> Write `onto`; block: `selection: "optional"`, the `layer` role to `region.layer` and `write.onto`, Advanced model and threshold; it carries the pack in its `packs/` as `remove-background` does. The `layer` role is filled from the view's **current layer** (`CanvasViewModel.currentLayer`: the one selected paint layer, "Select a layer first" otherwise; the action keeps Region's own padding and scale). **Pack missing**: the action is listed (not broken) with `packMissing`, `packText` "Needs the Remove Background pack" and `packInstallable`; the bar shows that line, **Install…** (`CanvasActions.installPack` -> `Workflows.requestPackInstall` -> Settings > Packs, which asks the person; nothing installs without it) and keeps Generate off ("Install the pack first"); Generate refuses with `pack_missing`; inputs of the block that point into the missing pack's node wait for it; the other actions are unaffected; a batch refuses it. The bar shows with nothing selected while an optional-selection action is the chosen one. Decisions (each to confirm): (1) the mask **replaces** the layer's old mask (inside the selection), as Photoshop's Remove Background does [U]; (2) no "new layer with the subject cut out" fallback was needed; (3) with no single current layer the action refuses instead of reading the composite (nothing to put a mask on); (4) the button still says Generate; (5) reaching the action with nothing selected still takes one selection first (Auto never picks it); a Layers panel or Layer menu entry is the follow-up. Not verified: no real BiRefNet run (the pack was not installed here), nothing in a real window.
| 7 | Expand: canvas resize revision in the store, the crop tool's bounds, the Fill on the border | M | 3 |
| 8 | Upscale to a new canvas (Canvas New sink) | S–M | 1 |
| | *Built 2026-11-23 (slice 8; branch canvas-upscale, fugue-workflows canvas-upscale):* the node **Canvas New** (`fugue/canvas-new` 1.0.0, a sink that is never cached: a picture becomes a canvas of one layer, named Upscaled, Upscaled 2, ...), the block's `selection: "none"` (the action takes the whole picture: rectangle, stored selection, padding and scale are not bound), `ActionGraph::news`, `CanvasActions::canvasMade(source, made)` (from the node's `canvas` output at run end; the view that showed the source opens the new canvas and says the original is as it was), and templates `upscale` and `upscale-no-model` (category `canvas`): Region (whole composite, no cap) -> Upscale with Model (Real-ESRGAN x4plus; none for the plain one) -> Resize to Region size times Scale (Number Math) -> Canvas New. Scale is Advanced, `choices` 2x/4x with `choice_values` 2/4. Decisions (each to confirm): (1) **new canvas (A), not in place (B)**: `CanvasStore::resize` only moves the canvas rectangle (adds or crops borders, layers keep their pixels), so B would need a new revision that resamples every layer, mask and group, which no store operation does; A needs one small node and no store change, keeps the original, and is design decision 4. (2) The model works at its own factor and Resize brings it to the chosen Scale (a 2x is the 4x result scaled down with Lanczos: sharper than a native 2x run is not claimed, only that it is cheap and robust to any model's factor, 2x or 4x); one more node pair rather than reading the model's factor. (3) The whole composite only (the head, flattened): the current-layer variant is not built (the `layer` role would make a layer mandatory; a person flattens or hides first). (4) The no-model Lanczos variant is a second menu entry, not a choice inside Upscale, because a conditional model input would need a switch node the node set had not (the If/Else Switch of #557, 2026-11-26, now is one: a model only its unchosen side needs no longer blocks paired placement, but Get ready still lists it to download, as it reads the document and not the choice, so this stays two entries); it adds no detail and needs nothing downloaded. Default is the model one, since upscaling without detail is rarely what is wanted and the download is 67 MB with one click. (5) Scale is in the Advanced popover, not on the bar. (6) The source canvas's layers flatten into Layer 1. (7) Over 16384 px a side or 67 MP is refused in words, nothing made. No live frame (no sampler). Verified: Canvas New, the block and bound copies, a real no-model run through the bar's runner, and a real CLI run of the model template with RealESRGAN_x4plus (CUDA helper) at 4x and 2x. Not verified: the window (bar, switch to the new canvas) in a real window, the Download offer for the model in a window.
| 9 | Authoring: **Make action…** (proposed block), action inputs editor, Expose to task bar, the package's **Add to actions** | M | 2 |
| 10 | `fugue canvas act` for scripts and agents | S | 1 |
| | *Built 2026-11-23 (slice 10; branch canvas-act-cli):* `fugue canvas act <canvas>[@rev] <action> [--prompt] [--seed] [--count] [--layer] [--rect x,y,w,h \| --selection mask.png] [--expand side --percent 25\|50 \| --pixels N] [--set input=value]... [--wait]` and `fugue canvas actions` (alias `act --list`). Shared code, not a second copy: `engine/canvas_act.h` now holds the listing (`collectActions`, `ActionEntry`), the checks and binding of Generate (`planAct`: the rectangle held in the canvas, Expand's growth, the router's padding and scale, the layer role, the seeds and hidden variations of Count, the bound copy of each run), the plain words of an error (`plainWords`) and the cached-Write replacement (`placeWriteAgain`); `CanvasActions::refresh()` and `generate()` call them and keep only what is the bar's (auto routing, the settings, the live frame rectangles, the busy state, history); the CLI queues the copies with `startBoundRun`/`waitForRun` (factored out of `canvas batch`). Decisions (each to confirm): (1) **no `--wait` queues and returns** (like `fugue run`), `--wait` blocks: a script that wants the result passes it. (2) **`--set` takes both forms**: `input=value` for a block input (not prompt, seed, layer: they have flags; a choice by its word) and `node.input=value` for a node of the copy. (3) A model or pack missing is refused **before Expand grows the canvas** (the bar's order too), the message names the files and the hint says `fugue model get <action>`; no download is started by `act`. (4) `--selection` is a PNG the canvas size, first channel above 0 selected; its bounds feed the Region; another size is `usage`. (5) `--count` on an action with no seed is `usage` (the bar silently makes one). (6) The action may also be a workflow file with an `action` block, which is how tests use stand-ins with no model. (7) A failed run ends the rest of the Count (as the bar does); the exit is 1 with the bar's plain words. Not done: no prompt history or remembered Advanced values are read or written (the bar's per-root settings stay the bar's), no `--json` event stream (one answer at the end; progress is text on stderr), `--rect` ignores an `auto` route (an action id is required). |
| later | ~~Live preview in the selection (`node.preview`)~~ built (item 2 of the live preview), Select subject as a selection, sharing actions over the mesh (#203) | | |

## 16. Live link (#315)

Written 2026-11-23. A **Live** toggle in the task bar re-runs the chosen action on the current selection when the canvas, the
selection or the prompt changes, like a live preview. It keeps the scope guard of #206 (no paint program, no new
generation path): the preview is the action's own graph, run through `RunService`.

### 16.1 What exists (research)

- **Krita AI Diffusion** (GPL: ideas only) has a *Live* mode [U, from memory; its docs were not reachable]: it re-generates when the
  canvas or selection changes, on a fixed seed, with a fast low-step setup, shows the result as a preview over the canvas, and
  applies it to a layer only when the person presses Apply. So the preview is not a layer.
- **ComfyUI** has *Auto Queue* (on change or instant) [U]: it queues a prompt whenever the graph changes and stays at one pending
  item. It re-runs the whole graph, with no cancel of the run in flight.
- **Photoshop** (Generative Fill, Neural Filters) has no live regeneration for generative fill [U]; Neural Filters preview as you move
  a slider, then apply.
- **Fugue today.** `CanvasActions::generate` builds the request, `canvas::planAct` binds the copy, `RunService::start` queues it as an
  ordinary run, and Canvas Write lands a layer on the head (#229). `RunRequest.target` ("run to here") stops a run at a node.
  The result cache keys every node by its inputs, so a repeat with the same seed re-uses the Region, the Sampler and the Decode.
  `node.preview` gives the sampler's step frames, which the Canvas view already draws inside the selection (live frame).

### 16.2 The design

1. **A preview is never a layer.** A live run is the action's graph run to the node Canvas Write takes its `image` from
   (`RunRequest.target`), so Canvas Write does not run. No layer, no revision, no history entry, no undo step. The picture
   shows in the selection through the existing live frame (the step frames, then the finished decode).
2. **Generate keeps it.** While Live is on, the preview and Generate share one seed (picked when Live goes on; "Another preview" in the
   More menu picks another). Generate is then the same graph and seed, so Region, Sampler and Decode are cache hits and only Canvas Write
   runs: a layer lands with the picture that was shown, in the time it takes to write it. One Generate is one layer, one undo
   step, as today.
3. **No feedback loop.** The preview writes nothing, so it cannot change what triggers it. The layer a Generate lands moves the head;
   that revision is the runner's own (from the Write's event), so noting it adopts it as shown and starts nothing.
4. **What counts as a change** (the state key): canvas, shown revision, action, prompt, selection (rectangle, or the Quick Mask's
   stroke count), the Layers panel's Region layer, the current layer. Advanced values and Count do not trigger it; the next change picks them up.
5. **GPU cost and one queue.** The decisions are `engine/live_link.h` (a state machine with no timer and no clock): a run starts only
   after the state has been still for 600 ms, or half the last run's time if that is longer, at most 5 s (a slow model waits longer, so
   it cannot flood the GPU); at most one preview run exists; a change that makes it stale cancels it at once and the next waits for
   its end; the same state is never run twice; not ready (no selection, a blocker, a Generate running) cancels and starts nothing. A preview is an
   ordinary queued run: it shows in the queue panel, waits for the one GPU and for runs queued before it. Live starts off every
   session and per canvas view, is turned off when the canvas changes, and a failed preview switches it off with the one-line error, so a
   broken action cannot retry on every keystroke.
6. **Eligible actions.** An action whose Canvas Write has an `image` link and a selection (Generative fill, Refine, Remove). Expand
   (the canvas bounds) and Upscale refuse: "cannot preview".
7. **Undo and candidates.** Nothing to undo and no candidates are made. A Generate in the middle of a preview cancels it and its run is
   ignored from then on.
8. **Prompt history.** A preview does not add to it (only Generate does).

### 16.3 Where it lives

- `src/engine/live_link.{h,cpp}`: the decisions, `test_live_link` with a fake clock (the `now` argument).
- `CanvasActions`: `live`, `liveBusy`, `setLive`, `liveNote(key, revision, ready)`, `liveReroll`, the `liveDue` signal, a
  single-shot QTimer that calls the engine class; `generate({... live: true})` starts the preview.
- `CanvasTaskBar.qml`: the toggle, the key, and answering `liveDue` with the same request Generate builds. Nothing else.

### 16.4 Rejected options

- **Live runs that land layers** (re-run on every change, each a layer): fills history in seconds, and each landed layer is itself a
  change, so it loops. Rejected.
- **Live runs as candidates** (revisions kept but not accepted): there is no candidate review in the view any more (#239), they would pile up in
  the store, and they cost disk for pictures nobody asked to keep.
- **A throw-away copy of the canvas** to write to: needs a Canvas Write variant or a second store. A run to the node before the Write
  is simpler and writes nothing.
- **A lower-step "live" variant of the graph made by Fugue**: the step count and sampler are the action's own choices (the Krita
  maintainer's warning about dynamic graphs applies). See 16.5.
- **Re-run on every change with no debounce, or a queue of pending runs**: floods the GPU; only the latest state matters.
- **Cancelling only at the debounce**: the run in flight is wasted GPU while the person is still drawing, so it is cancelled at the first change.
- **On by default, or remembered across sessions**: it uses the GPU without a press, so it is an explicit toggle for each session.
- **A global key shortcut**: no key has been agreed; the toggle is a button until the keymap work (#206) names one.

### 16.5 Not done, and to confirm

- A per-action **fast preview setting** (an optional `live` block in the action file naming lower steps or a turbo model, as Krita's live
  mode uses). Without it a 20 s model is a poor live preview; the longer wait for a slow run is the only protection. Needs an owner
  decision on the block's shape, so it is left out.
- Whether Enter in the prompt should keep the preview (it does: it is Generate), or Enter should re-roll while Live is on.
- Verified only with the fake provider: that a real Generate after a preview is a cache hit through Region, Sampler and Decode (the fake
  test shows the sampler is not run twice; a check with a real model on the 3090 is open).
