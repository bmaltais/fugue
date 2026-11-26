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
12. [The Graph](#12-the-graph)
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
| `~/.config/fugue/settings.json` | Settings, written by the settings panel (`app.settings`, Ctrl+,): `navigation`, `language`, `output_folder`, `inputs_folder`, `model_paths`, `model_folder_kinds`, `download_folder`, `models_setup`, `pack_paths`, `new_seed`, `embed_workflow`, `corner_radius`, `hover_magnifier`, `restore_tabs`, `picture_mode`, `step_preview` (`src/engine/settings.h` has the table). Keys Fugue does not know are kept (the retired `canvas_show_new_results` of #225 is one: ignored, left in the file, #239). |
| `~/.config/fugue/providers.json` | Provider instances and their keys, mode 0600 ([8.2](#82-provider-instances)). |
| `~/.config/fugue/keybindings.json` | Keymap diff ([12.5](#125-command-registry-and-keymap)). |
| `~/.local/share/fugue/packs/<pack-id>/` | Installed packs ([6.5](#65-where-packs-live-and-how-they-load)). |
| `~/.local/share/fugue/providers/<provider-id>/` | Installed provider modules. |
| `~/.local/share/fugue/models/<type>/` | Fugue's models folder, one subfolder per resource type ([8.9](#89-models-on-disk)). |
| `~/.local/share/fugue/canvases/` | The canvas revision store: tile blobs, trees and each canvas's commits and head ([14.5](#145-the-canvas-revision-store-206-208)). Under a project root, the project's own. |
| `~/.local/state/fugue/recovery/` | Recovery copies ([11.3](#113-recovery)). |
| `~/.local/state/fugue/tabs.json` | The saved workflows open when the last session ended, for the next start ([12.10](#1210-several-workflows)). |
| `~/.local/state/fugue/trust.json` | Which packs the user has allowed ([6.7](#67-trust)). |
| `~/.local/state/fugue/workspaces.json` | The window's areas, once they are more than one Graph area ([12.16](#1216-areas-and-view-types-217)); a damaged one is kept as `workspaces.json.bad`. |
| `~/.local/state/fugue/media.json` | What the media browser keeps ([12.14](#1214-side-panels)): the favorites, its mode, the workflows panel's view and the favorites filter. |
| `~/.local/state/fugue/hashes.json` | Remembered file hashes, keyed by path, size and modification time. A project that inherits `models` also reads the Default's for files in the inherited model folders ([1.4.2](#142-projects)). |
| `~/.cache/fugue/results/` | The result cache ([10](#10-result-cache)). |
| `~/.cache/fugue/runs/<run-id>/` | Run folders ([9.7](#97-run-folder)). |
| `~/Documents/Fugue/` | Suggested place for workflows. Not required. |
| `~/Pictures/Fugue/` | Default output folder for Save nodes. A setting. |
| `~/Pictures/Fugue/inputs/` | Images pasted or dropped into Load Image, `<name>-<10 hex of the content hash>.<ext>` ([14.1](#141-m1-the-first-twelve)). Follows the output folder unless set on its own. |

#### 1.4.1 One root: FUGUE_HOME

`FUGUE_HOME=<dir>`, or `--home <dir>` anywhere on the command line (the window and `fugue <command>`; the option beats the variable), makes one folder the home of everything a session writes (epic #204: projects ([1.4.2](#142-projects)) and private mode build on it; a portable install needs it too). The folder must be absolute. Fugue makes it (mode 0700) and, before Qt starts, points these at subfolders of it, so Fugue's own folders (the table above) and every child process, the GPU helper, ffmpeg and the packs, inherit them:

| Variable | Under the root | Table above becomes |
| --- | --- | --- |
| `XDG_CONFIG_HOME` | `config/` | `config/fugue/settings.json`, `providers.json`, `keybindings.json` |
| `XDG_DATA_HOME` | `data/` | `data/fugue/packs/`, `providers/`, `models/` |
| `XDG_STATE_HOME` | `state/` | `state/fugue/recovery/`, `trust.json`, `hashes.json`, the run queue |
| `XDG_CACHE_HOME` | `cache/` | `cache/fugue/results/`, `runs/`, `thumbnails/`, `thumbs/`, Qt's shader and QML caches |
| `XDG_RUNTIME_DIR` | `run/` | copies handed to an external viewer (`run/fugue-view/`) |
| `TMPDIR` | `tmp/` | the helper's work folder, ffmpeg logs, scratch folders |
| `CUDA_CACHE_PATH`, `__GL_SHADER_DISK_CACHE_PATH` | `cache/gpu/` | the NVIDIA kernel and shader caches (not under XDG) |

Fugue makes the root's `documents/` and `outputs/` folders with the root (`setFugueHome`, so a root made before this heals at its next start, and `fugue project new`), so the save dialog never starts on a folder that is not there; saving a workflow into `documents/` makes it if it was removed since, and the dialog's start folder falls back to the nearest existing parent (`nearestExistingFolder`). The other per-root folders are made by the code that writes into them. Four defaults no environment variable can move follow the root by code (`engine/paths.h`): the output folder (`outputs/`, so `outputs/inputs/` too), the folder for documents (`documents/`: the suggested place for workflows, `documents/templates/`, a write root of the file nodes), the media library's thumbnails and the path that messages give for `providers.json`. The Wayland socket, the session bus and the sound server live in the real runtime folder, so `WAYLAND_DISPLAY`, `DBUS_SESSION_BUS_ADDRESS`, `PIPEWIRE_RUNTIME_DIR` and `PULSE_RUNTIME_PATH` are made to name it (unless set already). `HOME` is left as it is: Omarchy's theme and language files are read from it, and a file the user picks is theirs.

With no root nothing changes. What a root does **not** do: the user's own files (a saved workflow, `inputs/` beside it, a folder chosen in settings, `model_paths`) are where the user put them; file nodes may still read under `$HOME` (the read root shrinks only with projects, #204); a pack or a program run by one can write anywhere it chooses (only a sandbox guarantees otherwise, #204); the window title names the open workflow and, in a project, the project ([1.4.2](#142-projects)); the portals, the clipboard, the journal, a core dump and swap are outside Fugue. `tests/cli/test_cli_home_guard.cpp` runs workflows (images; ffmpeg) with an empty `HOME` and asserts nothing was written outside the root, and under `strace -f` where it is available; add any new write path to `engine/paths.h` so that test keeps holding.

#### 1.4.2 Projects

A **project** is a `FUGUE_HOME` root ([1.4.1](#141-one-root-fugue_home)) with a `project.json` in it (epic #204). Everything 1.4.1 says of a root holds; the file names it and says what it takes from the ordinary session. The **Default project** is today's install: no `project.json`, nothing moved; the name `default` is reserved for it. A root with no `project.json` (a bare `--home <dir>`) stays an anonymous project that inherits nothing.

```json
{"format": 1, "name": "Acme", "inherits": ["models", "packs", "preferences"]}
```

- `name`: 1 to 64 characters, no `/`, `\` or `.` (so a name is never taken for a path) and no control character; not `default`, in any case. Names are unique among the registered projects, compared without regard to case.
- `inherits`: words from `models`, `packs`, `templates`, `prompts`, `preferences`, and only the Default project can be inherited from (no chains). A word that is unknown or can never be inherited (providers and keys, recent files, recovery, hashes (but see the model files' hashes under `models`), results, runs, thumbnails, queue, outputs, inputs, workflows, documents, the output, inputs and download folders, the cache limits) is ignored and reported as a `project_inherits_ignored` warning; the project still opens. A new project inherits `models`, `packs` and `preferences`. `models` and `preferences` are layered as below, `packs` ([Packs and trust](#packs-trust-templates-and-prompts-of-a-project)), `templates` and `prompts` after them. Keys this version does not know are kept when it rewrites the file.
- The **registry** `projects.json` is in the base config folder, `~/.config/fugue/projects.json`, also from inside a project: `{"format": 1, "last": "<path>", "projects": ["<absolute path>", ...]}`. It is a list, not a scan, so a project may sit on another drive or in a client's folder. A listed path whose folder or `project.json` is gone lists as `missing` (and stays listed); one whose `project.json` cannot be used lists as `invalid`. The file appears with the first `fugue project new` or `add`; a file that cannot be read is an error and is never replaced by an empty one.
- **Base folders.** Before `setFugueHome` redirects anything it saves the ordinary session's XDG folders once, as `FUGUE_BASE_XDG_CONFIG_HOME`, `FUGUE_BASE_XDG_DATA_HOME`, `FUGUE_BASE_XDG_STATE_HOME` and `FUGUE_BASE_XDG_CACHE_HOME` (never overwritten, so a child process or a second call keeps them); `baseConfigDir()`, `baseDataDir()` and `baseStateDir()` in `engine/paths.h` give the Fugue folders in them. Without a root they are `configDir()` and the others.
- **Location.** A new project's root is `<base data>/projects/<slug>` (`~/.local/share/fugue/projects/acme`; the slug is the name in lowercase letters and digits joined by `-`, with `-2`, `-3` added when the folder is taken), or any folder with `--in <dir>`.

- **Layered settings** (`models` and `preferences`, read from the Default's folders, `baseConfigDir()` and `baseDataDir()`, and only when `inherits` lists the class). Keys by class: `preferences` are `navigation`, `language`, `corner_radius`, `hover_magnifier`, `restore_tabs`, `picture_mode`, `step_preview`, `new_seed`, `embed_workflow` and the keymap `keybindings.json`; `models` are `model_paths`, `model_folder_kinds`, the Default's models folder (`~/.local/share/fugue/models`) and its model catalog (`catalog/*.json`). Every other key (`output_folder`, `inputs_folder`, `download_folder`, `models_setup`, `pack_paths`, the cache limits) is the project's own and never read from the Default, whatever it holds. A project without `inherits` sees nothing of the Default's. A scalar is the project's key, else the Default's, else the built-in value; a list (`model_paths`) is the project's entries and then the Default's that are not already there, each folder once; `model_folder_kinds` takes the Default's entries for the paths the project does not name; the keymap file is one value (a project's own `keybindings.json` replaces the Default's whole). `currentSettings()` is the layered result, `effectiveSettings()` also says where each key's value comes from: `own`, `inherited from Default` or `built-in`. A change (`changeSetting`, the settings panel) only ever writes the project's own `settings.json`; the built-in value is written, not dropped, when it differs from an inherited one, so a project can say no to the Default's. `language` has no built-in value (absent: follow Omarchy), so its unset state is written as `"language": ""` over an inherited language (and dropped when there is none): `""` is the project's own 'follow Omarchy'; giving the key up (removing it) shows the Default's again. The Default's files are never written by a project session.
- **Read-only roots.** The Default's models folder and `model_paths` are model roots after the project's own, searched like them and tagged inherited (`searchPaths` with a source tag, `taggedSearchPaths()`). Nothing is written in an inherited root: deleting or moving a file there is refused with a message that names the Default project as the place to change it, and downloads always go to one of the project's own roots (a `download_folder` that names an inherited root is ignored for that). `fugue settings list` and `fugue model list` show where a value or a model folder comes from. Packs are layered too, see below.
- **Remembered hashes of inherited model files** (slice 7). A project that lists `models` also reads the Default's `hashes.json` (`baseStateDir()`), read-only, so its first run does not hash tens of GB again. Only an entry whose canonical path is under an inherited model root (the Default's models folder or one of its `model_paths`, not a sibling that only starts with the same letters) is used, and under the same rule as the project's own: size and modification time to the nanosecond must match, so a changed file is hashed again. The project's own entry is looked at first. A hash the project makes, or is told (`remember`), goes to the project's own `hashes.json`; what was taken from the Default is not copied there, and the Default's file is never written. A project without `models` (a strict or anonymous one included) reads nothing of the Default's hashes, and no other file's hash (inputs, pictures) is ever inherited, so the Default's list of what it has seen stays its own. The helper's `model-hashes.json` index is not inherited (it is the helper's own). The Default's store is read again when it changes while the project's window is open.
- **One GPU, many roots.** Each root has its own queue and engine lock, so a run also takes a content-free lock file, `fugue-gpu-<uid>.lock`, in the ordinary session's runtime folder (`baseRuntimeDir()`, saved as `FUGUE_BASE_XDG_RUNTIME_DIR` like the other base folders), after its engine lock and until it ends: a run in a second root, or in the Default session, waits for the one going in the first. The system releases it when its holder exits, so a killed run never blocks the next. It is the one file a project session writes outside its root. The helper keeps its models loaded between runs until its idle timeout (`idle_timeout_s`, 10 minutes) or until another load needs the memory, so after a run in one root the helper of that root may still hold the VRAM while the second root's run loads, so a waiting run also leaves a time stamp (`fugue-gpu-<uid>.lock.wait`, refreshed at most every half second while it waits, dropped when its turn comes; never a lock, so a waiter that dies blocks nobody). A helper that has no job and sees a stamp younger than 3 seconds stops at once (`shutdown`, then ended after the grace period; it is checked every half second while the helper idles and as each job ends) instead of staying warm until the idle timeout, so the second run finds the memory free. With no other root waiting nothing changes: the helper stays warm for its idle timeout (`idle_timeout_s`, 600 seconds for the helper and for provider modules; the 300 of 7 is the command nodes' own).

#### Packs, trust, templates and prompts of a project

- **Packs.** A project whose `inherits` lists `packs` also searches, after its own folders, the Default's packs folder (`~/.local/share/fugue/packs`) and the Default's `pack_paths` (`inheritedPackPaths()`), read-only. The project's own `pack_paths` stay its own, and the Default's group library (`local`) is never shared. A pack of the project with the same id and major version hides the inherited one (it is listed as hidden, not loaded, and is not a problem); another major of the id is not hidden. Installs (`pack add`) always go to the project's own packs folder, also over an inherited id. `pack remove` and `pack update <id>` of an inherited pack, and `pack trust` of an inherited version the Default has not trusted, are refused (`usage`) with a message that names the Default project as the place to change it, or says to install an own copy, which hides the inherited one. `pack update` with no id only touches the project's own folder.
- **Trust.** The Default's `trust.json` is read, read-only, for the exact id and version of an inherited pack: a pack the Default trusted at 1.0.0 is trusted at 1.0.0 here, a new version asks again, and the Default's trust of a pack the project does not inherit, or whose id and version the project has itself (the own copy hides the inherited one), counts for nothing. Nothing here writes the Default's file; taking trust back (`TrustStore::revoke`) from trust only the Default's file gives is refused like a removal.
- **Templates and prompts** (opt-in: they may hold client text). `templates` lists the Default's own templates (`~/Documents/Fugue/templates`, read from `user-dirs.dirs` of the ordinary session, not the Fugue bundled ones, which every project has) after the project's own, read-only and tagged inherited. `prompts` reads the Default's `prompts/*.json` between Fugue's own and the project's: an own template with the same id replaces it. It also adds the Default's `wildcards/` folder (Dynamic Prompt, 14.1) after the project's own, read-only; a project that does not inherit `prompts` sees only its own wildcard folder and the packs'. Saving over an inherited or bundled template (`TemplateLibrary::saveOwn`) makes a copy in the project's own templates folder, with its description, preview, samples and carried packs, and the copy hides the original, which is never touched; saving over an own template is in place.
- **Origin.** `fugue pack list` and `fugue template list` say where each entry comes from: `origin` is `own` or `inherited from Default` (templates also `bundled`), `inherited` is a boolean, and the text marks inherited ones `[inherited from Default]`.
- **Live.** The window's registry reloader also watches the project's `project.json` and, when it inherits packs, the Default's `trust.json` and pack folders, so `fugue project inherits` or a hand edit takes effect without a restart.
- **What an inherited pack can still write.** An inherited pack runs from the Default's folder, so the project cannot protect the Default from it (no sandbox before Landlock, see 1.4.1). What lands in the Default's folders: whatever the pack writes in or beside its own folder (`FUGUE_NODE_DIR` is the Default's pack folder: a virtual environment, caches or downloads kept next to its program, state files it keeps there); its setup was made, or not, for the Default, never for the project (`pack add` of an inherited pack is not possible, so a setup an inherited pack still needs must be run in the Default); and, as for every pack, any absolute path or `$HOME` file it chooses. What stays in the project: the run's work folder, its results and caches, and everything written through `XDG_*` and `TMPDIR`, which the pack's process inherits from the project's root. `fugue pack list` says so for each inherited pack and the window's Packs page must show the same words: *Inherited from Default: this pack runs from the Default project's folder, so anything it writes (its setup environment, caches, downloads, state) lands in the Default project's folders, not this project's. Fugue cannot stop that.* A project that must not touch the Default's folders installs its own copy of the pack instead.
- **Never inherited**, whatever `inherits` says: providers and keys (a project's `providers.json` is its own, and missing means Omarchy's defaults with no keys), the group library, recent files, results and runs.

**The window** (slice 6 of epic #204; `settings/project_model.*`, `settings/ProjectsDialog.qml`). File > Projects… (`file.projects`) opens the Projects dialog: the Default project first, then the registered projects, each with its name, its path, **Missing** or **Invalid** when it cannot be used (with why), the words it takes from the Default and a **Current** mark on the one this window runs in. **New…** asks for a name, an optional folder (the default place is shown) and one switch per inheritable word with its description (a new project has `models`, `packs` and `preferences` on); **Create** registers it, **Create and open** (the highlighted one, last at the right after Back and Create) also switches. **Open folder…** registers an existing project folder. **Project settings…** renames the current project and changes what it inherits; in the Default project it only says that the Default is the ordinary Fugue and offers nothing to change. **Remove from list…** (in the danger tint) asks first and says the folder and its files are kept, with **Keep** highlighted and **Remove from list** last; it only unregisters. The window title is `<workflow> — <project> — Fugue` (the Default is unnamed: `<workflow> — Fugue`), and a chip with the project's name sits at the far end of the menu bar (none in the Default: no chip reads as the Default, and File > Projects… is the way in); a press on it, or Enter once Tab reaches it, opens the dialog.

**Switching** starts a new process, `fugue --project <path>` (`--project default` for the Default, so the new window does not reopen the last project), through a launcher the tests replace; the real one is `QProcess::startDetached` with `baseEnvironment()` (`engine/project.h`): the `FUGUE_BASE_XDG_*` folders put back into `XDG_CONFIG_HOME`, `XDG_DATA_HOME`, `XDG_STATE_HOME`, `XDG_CACHE_HOME` and `XDG_RUNTIME_DIR`, the variables a root redirected (`TMPDIR`, the GPU caches) removed, and `FUGUE_HOME` and `FUGUE_PROJECT` removed. When the new process has started this window closes through the quit command. A window with unsaved work does not close or start anything on Switch: the dialog says so and offers **Open in new window**, which starts the other project's window and leaves this one as it is. A project that is missing or invalid cannot be switched to.

**Origins in the panels.** The settings panel tags a setting whose value comes from the Default `Inherited from Default`; once the project sets its own over a value the Default has, the tag gives way to **Use Default's value**, which takes the project's own key out of its `settings.json`. The extra model and pack folders list the Default's after the project's own, read-only, each tagged, and the remove button on one answers with the refusal and changes nothing. The Packs page tags an inherited pack, shows the warning that it runs from the Default's folder, and its Remove, Update and Trust answer with the refusal of 1.4.2's packs rules. The model catalog of a project that inherits `packs` also reads the Default's packs' `catalog/*.json`, before the project's own packs' and its own entries, so a project's entry wins by id.

**Selection**, first match wins: `--home <dir>`, `--project <name|path>`, `FUGUE_HOME`, `FUGUE_PROJECT`, then, for the window only, the project it last ran in (the registry's `last`; gone or unusable means Default), then Default. `--home` together with `--project` is an error (exit code 2, as for any unusable root). The command line never uses the last project, so scripts and agents get the same session every time. A value with a `/` or starting with `.` or `~` is a path (any folder with a `project.json`, registered or not); anything else is a name looked up in the registry. `default` selects the ordinary session, except from inside a project (`FUGUE_HOME` set), where it is refused. A selection that fails (unknown name, missing or invalid project, no `project.json` at the path) is an error with exit code 2 and nothing is started. A child process inherits `FUGUE_HOME` and reads `project.json` again. The window records where it runs as `last` (Default clears it, an anonymous `--home` leaves it); the command line never does.

`fugue project` ([13.1](#131-commands)) works on the registry from any session. It never deletes anything: `remove` only takes a project off the list, its folder and files stay.

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
- **Family and Recommended (#370).** A model file has a family, the model as people name it ("FLUX.2 Klein 9B",
  "Qwen3 8B"), from one table (`engine/model_family.*`, shared with the Templates window's Model family filter, #369): the
  resource type and file name. The pickers of resource types that have families group their files under the family's name (a file of
  no family stays under its folder), and offer a **Family** row at the top when the files make two or more families, which narrows the
  list to one (providers' models and files of no family stay). Among files that are the same weights (one family, the same header
  figures and the same *stem*: the name without quantization and format words, `engine/model_weights.*`; the stem is what tells a
  distilled Klein from its base, or a kv file, which share every shape; a file with no known header or family is never grouped) one is
  marked **Recommended** with a reason, and only marked: a template keeps its file unless the person swaps it, and nothing is picked
  by itself. The rule (`engine/model_recommend.*`): drop what stable-diffusion.cpp cannot load and what does not fit the device's memory
  with the workflow's other models on it (weights of the node's other picked models, plus a fifth, as 8.10); keep the best quality tier left
  (bf16 or fp16, then Q8_0 and INT8 together, fp8, Q6, Q5, Q4; read from the name, a table in code, no hash list; a .safetensors
  file with no quantization word counts as bf16, a GGUF with none has no tier and is never marked); then the fastest in the tier: by the
  measured sampling seconds per step per megapixel on this device when every file left has one, else the backend's default (INT8 on
  CUDA with compute capability 8.0 or more, GGUF on Vulkan and CPU, where INT8 is untested), then the smaller file. A group with one
  loadable file has no mark. The reason reads "Recommended: fastest here at Q8-level quality" (measured), "expected to be fastest" (default)
  or "best quality that fits here" (one file left). A remote helper's numbers are its own device's and are never recorded here.
  **Use what you have** (#605). A workflow that names a model that is not on disk, whose same weights are in another file (another
  quantization, or the same name and quantization from another publisher), offers that file wherever it offers a download. One piece
  decides (`findOnDisk`, engine/model_use.h): the file of one family and one stem that the engine can load, the Recommended one when
  there are several; never a file of another family, one with no readable header, or one the engine cannot load (SVDQuant, NVFP4 and the
  like). When a file of the wanted model's own name is here, a file whose header figures differ from it is not the same weights and is not offered
  (a model that is not on disk has no header, so family and stem decide). A file of the same quantization is said to be from another source,
  whatever the spelling of its name (the bytes are not verified identical). The file's hash is read once on the worker: a second click for the
  same target meanwhile does nothing, that model's own row says it is reading, and a file that cannot be read is said ("Could not read ..."; a template then opens
  without it). In the App view the choice is the App view's alone: once the tab shows its graph, the graph's readiness and run use the
  workflow's own models. With the setting **Ask me** nothing is swapped by itself (the default, **Use my file**, below), and a workflow file is rewritten only when the person saves. The choice is **Use my file** or **Download**, per model, on every surface that offers the download, and nothing happens before the surface's yes.
  In Get ready each matched row has a `Use my file | Download <size>` choice, on Use my file at first (owner, 2026-11-26): the headline, the totals and
  the Destination follow it, the primary is Install while anything downloads or installs and Use my file when only the person's files are left, the
  yes reads the files first (each row says "Reading your file…" with the hash's own percent), then runs the rest, and the sheet closes by itself when
  nothing is left; Cancel discards the choices. A template card that is Ready only through the person's files (its other models are here, no pack
  is needed) opens straight away with them, with no sheet: the banner names the file ("Opened with your file <file> in place of <name> (same model,
  <tier>).") and has Undo; the card says "Ready · uses your <tier> file", and a mixed card says how many the person has and counts only the models a
  download would fetch; the template details' "Download <size>…" opens the template as it is with Get ready on Download. The Needs panel has Use my
  file beside each such row, and its header chip uses every file at once when all are covered (Get ready… when some are; Download all never fetches a
  covered model). The node's yellow missing-model card says it on hover and has the **Use my file** button when kept up (one undo step); its badge is
  reached by the keyboard. In the graph the choice edits the document (one undo step). In the App view it is the app's own: a model field of the
  form that holds the missing model takes the file as its value (kept with the form's values, so Reuse inputs and the takes know it), a model the
  form does not show is kept as an override of the tab, and Run binds it into the copy it runs; the app's workflow is never written, and the line
  under the Needs panel ("Using <file> in place of <name>") has Undo, back to the template's file, with a tip that says it needs a download. The app
  is Ready when every missing model has a chosen file.
  The CLI says it in `model_missing` and `app_not_ready` ("the same model is on disk as <path>; set <node>.<input> to it, or pass
  --use-on-disk") and `fugue run`, `fugue app run` and `fugue validate` take `--use-on-disk`: the same piece, for that command only, each
  substitution printed on stderr.
  **Use my file automatically** (#618, owner 2026-11-26). The setting `use_on_disk` (Settings > General, "When a model is missing but the same model
  is on disk") is `use` (the default) or `ask`. With `use`, wherever the choice would come up it is made: a template or app opens with every covered
  model in place and Get ready opens only for what is really missing (a partly covered template included; a pack to install still opens the sheet,
  for the pack); an opened workflow file, a tab reached later, and a model whose file appears (a folder added, a download ended) use their files once the
  hash is read on the worker ("Reading your file… %" in the banner) and the Get ready sheet of a tab just opened waits for that read, then opens for what is left; a tab that is not shown when its read ends is not a failure: it takes its file when it is shown. Once a tab has been looked at, the Needs panel, the node card and Get ready have nothing covered to offer, except while a file is being read, after Undo, and after a file that could not be read (they offer it as under `ask`). A Get ready the person has open is never applied over, and an input they changed during the read is left as it is. Every automatic swap is said in the banner (naming the file and the tier: "Used your file <file> in place of <name>
  (same model, <tier>)."), with Undo in the graph (one step; the App view's line has its own); an Undo is not undone by the next check, and a file that
  could not be read is said and not tried again for that tab. The rules of #605 hold unchanged (same weights, fits, the best tier; a file of the quantization
  the workflow names, from another source, stands in before a better-ranked other quantization, #624), nothing is swapped silently and a workflow file is
  rewritten only on save. The template details' Download button and a person's opened Get ready are left alone. With `ask` nothing is applied before the
  surface's yes (the sheet's Use my file is preselected). The CLI follows the setting: `fugue run`, `fugue app run` and `fugue validate` behave as if
  `--use-on-disk` were given when it is `use`, `--no-use-on-disk` opts out of that command, `--use-on-disk` forces it under `ask`, and both flags together
  are a usage error.

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
derived handle Load Models makes, `{"base": <diffusion model>, "text_encoder": …, "vae": …}` (and `clip_l` for FLUX.1), with
Apply LoRA's `apply` steps. A latent's `meta` holds the image size, and for a sampled or encoded
latent the model, which VAE Decode decodes with; an Empty Latent's is only the size
(`"empty": true`), the Sampler starting from noise. A video latent (#105) is the same file kind
with `frames` and `fps` in its `meta` too; the tensor has the time axis, tens of MB for a few
seconds, and travels to a paired host like any latent. Wan's 4 n + 1 frames: the model rounds
another count to one. `fugue/stitch` (#176, 14.4) is a data type too: a descriptor alone, where a crop came from.
`fugue/controlnet`, `fugue/upscale-model`, `fugue/seedvr2-model` (folders `seedvr2`, `SEEDVR2`) and
`fugue/seedvr2-vae` (folder `seedvr2_vae`) come with their nodes: a SeedVR2 file is no general
diffusion model or VAE (only the Comfy-Org INT8 ConvRot checkpoints load), so Restore Image's pickers list only
what is in these folders.

---

## 3. Nodes

### 3.1 Node types

A node is an instance of a node type. A node type has:

- an id `namespace/name`. Built-ins are `fugue/...`. A pack's node types are `<pack id>/<name>`,
  for example `io.github.alice.tools/sharpen`. Namespaces are lowercase reverse-DNS or a single
  reserved word (`fugue`, `local`); names match `[a-z][a-z0-9-]*`.
- a semver version ([3.6](#36-versions)).
- a list of named, typed inputs and a list of named, typed outputs.
- flags: `sink`, `deterministic`, and `picture` (whether the node shows a picture, [12.7](#127-inspection)).
- what it needs to run ([6.3](#63-the-needs-block)) and how it runs ([6.1](#61-nodejson)).

The manifest of every node type is available as data (`fugue node describe`), for the canvas, the CLI and
agents alike.

### 3.2 Inputs

There is one list of named inputs per node type and no separate "parameter" kind.

- Each input has a type. `text`, `number`, `boolean` and choice inputs carry a default and
  constraints and show an editable control when unwired; the control is grayed when wired.
- Media and data inputs are wire-only. Resource inputs are a wire or a picked handle ([2.5](#25-resource-handles)).
- An input is required unless it has a default or is marked `optional`.
- An input can be marked **`lazy`** (the "Lazy input" of the [glossary](GLOSSARY.md)): the engine does not start what feeds it until the node asks for it. Once every input that is not lazy has its value, the node's kernel is asked which lazy inputs it needs (`NodeKernel::lazyInputs`, given those values; the default asks for all of them) and only those are fed. A lazy input the node did not ask for is left out of the node's inputs and of its cache key. `lazy` says nothing about whether the input is required: that is `optional` and `default` as before. The If/Else Switch ([14.1](#141-m1-the-first-twelve)) is the first user.
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
- A node that feeds a **lazy input** ([3.2](#32-inputs)) only runs if some node that runs asks for it. A node is *demanded* when it is a sink or the run's target, when a demanded node reads it through an input that is not lazy, or when a node that runs asked for the lazy input that reads it. A node nothing demands is skipped with the reason `not_chosen` (so is what only it needed, up to the nodes something else demands). A node read by both sides of a Switch, or by a demanded node through an input that is not lazy, runs once. **A sink is always a root:** a Save or Preview hanging off a node on the side a Switch did not choose still runs that node and what it needs (ComfyUI's output-node behaviour). A frozen node that is not demanded is skipped, not served. A demanded frozen node with a valid frozen result is served even when a lazy input it would ask for is muted or empty ([10.4](#104-freeze)).

### 3.5 Bypass and mute

- **Bypass**: the node does not run and each output takes the value of the first input, in
  declaration order, with the same type. An output with no matching input is empty.
- **Mute**: the node does not run and every node that depends on it is skipped (`upstream_muted`), except through a **lazy input** ([3.2](#32-inputs)): there a muted source (or one a bypass left empty) only empties that input, and the node is skipped as `upstream_muted` (`upstream_empty` for the bypass) only when it asks for it, so a Switch whose other side is chosen runs. What reads a node skipped that way through a strict input, an optional one included, or through a lazy input it chose (a Switch that chooses another Switch skipped that way), is skipped `upstream_muted` too, as a compile-time mute does; a lazy input it declined is not looked at. A mute is the person's choice, so this is a skip, not an error (#590).

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
| `action` | no | A canvas action's binding block ([14.5](#145-the-canvas-revision-store-206-208), docs/canvas-actions-design.md 1.2). Outside `sourceHash`. |
| `app` | no | An app's binding block ([4.9](#49-app)). Outside `sourceHash`. |
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
  from (a prompt; nothing installs silently), and an optional `name`, the pack's display name for
  the words that ask for it (11.6; an install adds none, a template or a hand edit may). A save writes each pack installed here at its
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
  after each run it starts ([6.2](#62-widgets)), and `picture` (`show` or `hide`: the node's own choice of showing its picture, over the picture mode, [12.7](#127-inspection)).
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
| `nodes` | Keyed by path id. Each has `type` and `version` (never a group), `inputs` (literals, `auto` values, handles, and links by path id), and, when set, `sink`, `provider` (the node's pin or else the workflow's), `frozen` and `lazy_dropped` (lazy inputs left out because what feeds them is muted or came out empty: input name to `upstream_muted` or `upstream_empty`; the node is skipped with that reason when it asks for one, [9.2](#92-scheduling)). `mode` is resolved and never appears. |
| `skipped` | Path ids left out, each with the reason: `muted`, `upstream_muted`, `upstream_empty`, `not_part_of_output`. |
| `target` | For "run to here": the path id the run stops at. |

- Groups are flattened. An inner node gets a **path id**: the instance ids from the outside in,
  joined with `/`, for example `g7/first`, or `g7/inner3/s2` for a nested group.
- Bypassed nodes are removed and their links rewired ([3.5](#35-bypass-and-mute)). A link to a
  bypassed output that has no matching input is removed: an optional input becomes unset, and a
  node with a required input left that way is dropped as `upstream_empty`. Muted nodes and their
  dependents are dropped and listed in `skipped` with the reason.
- Sinks inside a group are dropped ([5.5](#55-sinks-inside-a-group)), except a view sink (Compare Images) that a needed node reads: it stays as a plain node, not as a sink (#623). Links into a dropped sink count as empty.
- Nodes with no sink downstream are dropped, unless the run is "run to here".
- `auto` values are kept; the engine resolves them when the run starts.
- A compiled graph can be run directly: `fugue run compiled.json`. `--set`, `--to` and
  `--provider` change a compile, so with a compiled graph they are `usage` errors.

---

### 4.9 `app`

An **App** (#384, epic #397) is a workflow shown as one simple screen: the inputs worth changing, a Run button, the results.
The graph stays the truth and the app is a list kept beside it, so a fix in the graph is a fix in the app. There is no node
type and no second file format: the list is a top-level `app` block of `workflow.fugue`, the way a canvas action's binding is
(an `action` block). Both are top-level keys, so an older Fugue keeps them on a round trip (4.1), they are outside
`sourceHash` (no cache key changes), and save, save as, a template, a `.fuguepack` and the share copy (11.5) carry them with the document.

```json
"app": {
  "format": 1,
  "title": "Restyle",
  "description": "Paints a picture in a style.",
  "defaultView": "app",
  "inputs": [
    {"role": "image", "name": "subject", "targets": ["load.file"], "label": "Picture"},
    {"role": "prompt", "targets": ["encode.prompt"], "label": "Prompt"},
    {"role": "seed", "targets": ["sample.seed"]},
    {"name": "steps", "targets": ["sample.steps"], "min": 1, "max": 8, "section": "Advanced"}
  ],
  "outputs": [{"node": "save", "label": "Result"}]
}
```

| Field | Meaning |
| --- | --- |
| `format` | 1. A later format is `unsupported_format_version` ("Needs a newer Fugue"). |
| `title`, `description` | What the app card and the app's screen show. A title is required. |
| `defaultView` | `app` (default) or `graph`: what opening it shows first. |
| `inputs[]` | The form, in order (the list's order is the form's order; there is no `order` field). One shared schema with an action's `inputs` (docs/canvas-actions-design.md 1.2): `role`, `name`, `targets`, `label`, `section` (`Advanced` folds), `default`, `min`, `max`, `choices`, `choice_values`, `optional`, `bypass`, `fallback`. `targets` are one or more `<node id>.<input>`, all of one type, a top-level node's input or a group instance's exposed input. `choices` on a target that has its own choices must be a subset of them (5.4). |
| `inputs[].role` | `prompt`, `negative_prompt`, `strength`, `seed`, `model`, `layer`, plus `image` and `mask` (the `file` and `mask` inputs of a Load Image). A role with a `name` is keyed by the name, so an app can take two pictures. An input with no role needs a `name` (a-z, 0-9, `_`) and is plain. The key (the name, else the role) is unique in the list. |
| `inputs[].optional`, `bypass` | On a prompt: it may be empty. On a picture (`image`, `mask`): the picture may be left out (#484). Left empty (nothing given and no `default`, or an empty value given) the run's copy of the workflow has the input's branch **bypassed** (3.5): the nodes in `bypass` (node ids), else the nodes of the input's targets, which is the Load Image or the group instance the picture feeds. Filled, those nodes are put back to normal in the copy, whatever mode the file had them in, and the target gets the picture. The file on disk is never changed. The form starts an optional picture empty (the file its node holds is not a default), marks it "Optional" and needs no sample; `fugue app info` lists it as `optional` and `app run` may leave it out. A required picture left out is the same error as before (`missing_input`). |
| `outputs[]` | `node` (the id of a Save or Preview node) and an optional `label`. The run's results of these nodes are the app's results; the app owns no store (results and History are the existing ones). At least one is needed to be valid. |
| `compare` | Optional, `{"input": "<image input key>", "left": "<number input key>", "top": ..., "right": ..., "bottom": ...}`: where the input picture sits inside the result, for Compare in the App view (12.17). `input` is required and must be an `image` input of the list; each side is the key of a number input that holds the pixels the result gained on that side (a side left out is 0). The App view scales by `resultWidth / (inputWidth + left + right)` (the height when the width is unknown) and puts the input at `(left, top, inputWidth, inputHeight)` of that scale, from the values the take ran with. A name the list does not have is `app_invalid`, in words; deleting the input a name points at drops the whole `compare` with an `app_stale` warning. An older block has none, and the format stays 1 (an optional field, as `optional` and `bypass` were). |

**Deleting a node.** As for a group's exposed port (5.4), a target whose node or input is gone is dropped, an input left with no
target is dropped, and an output whose node is gone is dropped; each is reported as the warning `app_stale`, in words. A node
whose type is not installed (a pack is missing) is not stale: its targets stay, and checking says the pack is missing. What is
still wrong after that (a target linked to another node, of a type the role does not take, an output that is not a sink, no
output) is `app_invalid`, in words. Fugue never guesses a binding. `fugue app info` and `app run` prune the same way and
return what they dropped as `app_stale` warnings (`warnings` in `--json`), so an output the workflow does not have is said and
not listed (a deletion is stale, not invalid; an app left with no output is then `app_invalid`).

**Group instances.** A target that is a group instance's exposed input has the spec of the inner input it stands for (5.4),
however deep the groups nest: `targetInputSpec(document, registry, target)` (`engine/input_binding.*`) is the one resolver.
The checks, `app info`/`app run` (type, parsing of a value) and the App form (number, choice and toggle controls) all use it,
and what the block leaves unsaid (`min`, `max`, `choices` without `choice_values`, a default the workflow holds none of) is
the spec's (`completedBy`).

**Engine** (`engine/app_binding.*`, `engine/input_binding.*`): `appOf`/`parseApp` read the block, `toJson`/`setApp`/`clearApp` write
it, `pruneApp` and `checkApp` are the two passes above, and `bindApp(document, app, values)` returns the run's copy of the
workflow with a form's values in (a missing value keeps the block's default, else the workflow's own; a number is held in
`min`..`max`; a word of `choices` with `choice_values` becomes its number). The file is never changed. The reader of
`inputs[]` is the one `engine/canvas_action.*` uses.

### 4.10 Apps at a glance (#396, epic #397)

This is the map; each part is specified where it lives. An **app** is a workflow that also has an `app` block: the few inputs
worth changing, the results to show. It is never a second document, so the graph stays the truth and Open in Graph is always
there.

| Part | What it is | Where |
| --- | --- | --- |
| The block | The `app` key of `workflow.fugue`: title, description, `defaultView`, `inputs[]`, `outputs[]`, optional pictures; checked and pruned (`app_stale`, `app_invalid`) | [4.9](#49-app) |
| A bundled app | A template whose workflow has a block that reads; the catalog flags it (`app`), the Apps chip and the shelf list it | [11.10](#1110-templates), 12.17 |
| The App view | The form, Run, the stage, the takes strip, Run again Off / On change / Repeat, the Needs panel; a tab mode, or a view type of an area (#450) | [12.17](#1217-the-app-view-385-epic-397) |
| Its own window | `fugue --app <id\|file>`: the App view and nothing around it, a launcher entry for it; a `.fugueapp` opens there | [12.18](#1218-an-app-as-its-own-application-398-epic-397) |
| The builder | File > Make app… (Ctrl+Alt+M) and Edit app…: writes the block, one undo step | [12.19](#1219-make-an-app-the-builder-393-epic-397) |
| The bundle | `.fugueapp`: the app with its packs and sample files, models as references; File > Export for sharing… or `fugue app export` | [11.5](#115-export) |
| The CLI | `fugue app list`, `info`, `run`, `export` | [13](#13-cli-and-agent-interface) |

The app owns no outputs and no store: a run of an app is an ordinary run of a bound copy of the workflow (`bindApp`), so its results,
its History and its **takes** are the existing ones. The form's values live in that copy; the file is never changed by a run.
How to use all this is in [docs/apps.md](apps.md).

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

- A view sink (manifest `view_only`, Compare Images only: it shows and writes nothing) whose output a
  needed node reads, or that is the group's output, is kept as a plain node: its value is needed, and
  it is no sink of the run (#623). One nothing needed reads is dropped without a warning. Any other
  inner sink, a Save Image whose `file` is read for one, is dropped, and what read its outputs sees
  them as empty, as after a bypass: a required input makes the reader `upstream_empty`, a lazy input
  is dropped.
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
| `retired_inputs` | no | Names of inputs the type used to have. A saved value for one is ignored without an error or a notice (3.6). |
| `sink` | no | Default false. |
| `picture` | no | `always`, `result` or `never`: whether the node shows a picture on the canvas ([12.7](#127-inspection)). Absent: a sink is `always`, a type in `Image/Transform`, `Image/Adjust`, `Image/Mask` or `Image/Canvas` is `never`, anything else `result`. |
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
| `role` | `mask` on an `image` input that takes a mask: the port is drawn hollow. Changes no compatibility ([14.4](#144-masking-and-inpainting-167)). The only value is `mask`, and only on an image port; anything else disables the node. |
| `over` | For widget `mask` or `paint`: the image input it is painted over. |
| `shown_when` | A condition on the node's other inputs; the face hides the row while it does not hold ([6.1.1](#611-rows-that-do-not-apply-shown_when)). |

**Output fields**: `name`, `type`, `label`, `description`, `list`, `optional`, `role`, and `carries`, `carries_when` ([6.1.1](#611-rows-that-do-not-apply-shown_when)).

#### 6.1.1 Rows that do not apply (`shown_when`)

An input can say when it applies, so a node face shows only the rows that do something with the values it has (#429;
Blender's panels do the same). The condition is an object; every key must hold, and a key is one of:

| Key | Holds when |
| --- | --- |
| `"<input>": [v, ...]` | the input's value is one of the listed values: `{"fill": ["edge-blur"]}` |
| `"<input>": {"lt": n, "le": n, "gt": n, "ge": n}` | a number input compares so: `{"denoise": {"lt": 1}}` (the keys given all hold) |
| `"linked": "<input>"`, with `"is": true` (default) or `false` | the input has a link, or has none: `{"linked": "mask", "is": false}` |
| `"carries": {"<input>": "<property>"}` | the source linked to the input carries the property, read from the graph without running it: `{"carries": {"latent": "mask"}}` |

An input named `linked`, `is` or `carries` cannot be used in a condition. A condition names other inputs of the same node
(never its own row), or the manifest is refused. What a source carries is declared on its output: `carries` lists
properties (`["mask"]`), `carries_when` (the same kind of condition, on the node's own inputs) says when they hold, so VAE
Encode's `latent` is `"carries": ["mask"], "carries_when": {"linked": "mask"}`. A source with no declaration for the property
does not carry it, but one with no static answer (a node type that is not installed, a group instance, a bypassed node)
gives none, and so does a value with none (a wired input, a seed set to `auto`): **a row with no answer is shown.**

The rules (`engine/input_use.h` is the one place that applies them; the face and the CLI both ask it):
- A hidden row is not drawn and takes no room; the node's height follows, its position and its links' order do not move,
  and a section header goes with the first row of its section that is shown.
- **A linked row is always shown**, so a wire never ends at a hidden row. Only a value row can hide: a port with no widget
  cannot take a wire once hidden, so none of the built-ins puts a condition on one.
- **A hidden row keeps its value** and the engine gets it unchanged: hiding changes what is seen, never what runs.
- A condition on a group's exposed input is read through the group's face: the inner input names become the exposed ones,
  and a condition on an input the group does not expose is dropped (the row always shows).
- `fugue validate` and `fugue workflow describe` list each hidden row as `not used: <input>: Applies only when <why>.`
  (JSON: `not_used`, a list of `{input, why}` on the node).

Built-in manifests that use it: Pad (`blur` only with Fill edge-blur; `feathering` only without a Keep mask), Sampler
(`denoise_mode` only with Denoise below 1; `soft_mask` only when the latent comes from a masked VAE Encode), the song nodes'
`lyrics` unless Instrumental, Canvas Region's `x`, `y`, `width` and `height` without a Mask, and Generate Image's `strength`
with a start image.

### 6.2 Widgets

Widgets come from a fixed set. No pack ships UI code in v0.1, so every node looks consistent.

| `widget` | For | Notes |
| --- | --- | --- |
| `spin` (default for number), `slider` | number | |
| `line` (default for text), `multiline` | text | |
| `toggle` | boolean | |
| `choice` | text with `choices` | Default when `choices` is present. |
| `file`, `folder` | text | Opens the portal picker (`FilePicker`; Qt's own dialog when there is no portal). The value is a path. |
| `color` | text | Value is `#rrggbb` or `#rrggbbaa`. |
| `mask` | text | A painted mask: the value is the path of an 8-bit gray PNG in the inputs folder, empty for none. The face shows it over the picture of the input named in `over` and opens the mask editor ([14.4](#144-masking-and-inpainting-167)). |
| `paint` | text | A painted canvas (#181): the value is the path of an OpenRaster `.ora` file in the inputs folder, empty for none; `over` names the picture input it is painted over. The mask widget's twin: it holds an input file, so a save copies it, a package carries it and its bytes key the cache ([14.4](#144-masking-and-inpainting-167)). Its row opens the paint window (the mask editor's frame with a layers panel, a color swatch and an opacity slider: [14.4](#144-masking-and-inpainting-167)); the row reads "Painted" or "None" and the node's picture slot shows the result. |
| `canvas` | text | A canvas of this root (#211): the value is its id, as for a `line`, so a workflow stays portable. The input is a drop-down of **Active canvas** (first; the empty value, #246) and the root's canvases (name, size, time of the last change; [14.5](#145-the-canvas-revision-store-206-208)) followed by **New canvas…** (the New canvas dialog, [14.5](#145-the-canvas-revision-store-206-208)) **Import picture…** and, on a node that also has a rectangle (Canvas Region), **Pick region…**, which opens the region picker ([14.5](#145-the-canvas-revision-store-206-208)); an id this root has none of is shown as missing, with the list still there. A canvas belongs to the store it was made in (the Default's, or a project's own). It holds no input file, so a package carries the id only. |
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
| `setup` | no | `{"command": ["uv", "sync"], "description": "…"}`: a command, run in the pack's folder, that prepares the pack's private environment ([6.8](#68-per-pack-environments)). A pack may also hold `catalog/*.json`: models to download for its resource types ([8.9](#89-models-on-disk)). |
| `converters` | no | Import and export converters ([11.9](#119-converters)). |
| `settings` | no | Secrets the pack's nodes read (#402), in the shape of a provider module's `settings` ([8.8](#88-provider-modules)): `{"name": "token_1", "type": "text", "secret": true, "label": "Token 1", "description": "…"}`. `name` is lowercase letters, digits and `_`, starting with a letter (at most 40), unique in the pack; `secret` must be `true` (a pack has no other settings yet); `type` is `text` when given; `origin` is `"required"` or `"optional"` (default): a required one must be set with the origin it may be sent to ([6.4.1](#641-secrets)). `label` and `description` are shown on the Packs page. At most 16. A bad entry disables the pack with the reason. The values are never in pack.json: see [6.4.1](#641-secrets). |

#### 6.4.1 Secrets

A pack that calls a service needs a key, and a workflow, a share copy and a pack folder are plain files that get shared. So the key
lives in one place: `pack-secrets.json` in the config folder of the root (the ordinary session, or a project's own, [1.4.2](#142-projects)), next to
`providers.json` and kept the same way, mode 0600 from the first byte, written atomically (the read-modify-write is under a lock), never inside a workflow, a pack
folder, a `.fuguepack` or `.fugueapp`, a run folder or a log:

```json
{"packs": {"io.github.alice.tools|/home/alice/fugue-tools": {"token_1": {"value": "…", "origin": "https://api.example.com"}}}}
```

- **Keyed by pack id and source.** The key is the id and where the pack was installed from (the location in `.fugue-source.json`; the id alone for a pack copied in by hand). An update from the same source keeps
  the values; **a pack from another source that claims the same id gets none of them**, so a look-alike cannot read a key set for the real one. The trust question says "Already set" for names set for this id and source, and, when
  names are set for the id from another source, that this pack does not get them. `pack remove` of the last version forgets the values of every source of the id.
- **Where a key may be sent (origin).** A workflow is data from other people: opening a shared one must not be able to send your key to its author. So the person sets, with the value, the
  **origin** it is for: `scheme://host[:port]`, lower case, no path (`https://api.example.com`). A pack's manifest may mark a secret `"origin": "required"`, and then a value cannot be stored without one (the CLI and the page refuse, and a value stored
  without one anyway is not handed to the node). The origin is shown on the Packs page and in `pack secret list`, never changed in place (Clear and set again). The runner hands the node the origin with the value; **the node must use the value only for a request to exactly that origin**
  (scheme, host and port, compared after placeholders are filled). The HTTP Request node ([6.13](#613-http-request-first-party-pack)) does, before any connection, and refuses a placeholder in the scheme, user, host or port of its URL so the destination cannot depend on an input.
- **Set, clear, list.** Settings > Packs shows each declared secret on the pack's card as *set* or *not set*, with an **Only for** field (the origin), a value field (type it and press Enter, or **Set…**) and **Clear**; the
  value is never shown again. The CLI: `fugue pack secret list [<pack>]` (names, whether set, the origin), `fugue pack secret set <pack> <name> [--origin <origin>]` (the value is read from stdin, so it is on no
  command line and in no shell history; one line with no NUL, at most 4096 bytes; the page and the CLI use the same rules) and `fugue pack secret clear <pack> <name>`. No command prints a value. Only a name the pack declares can be set.
- **Handed over at run time only.** The command runner reads the store when it starts a node and puts, in the request on the node program's stdin, a `secrets` object
  `{"token_1": {"value": "…", "origin": "…"}}` with the names that node's pack declares and that are set ([7.2](#72-one-shot-mode-default)). Never another pack's, never an undeclared name,
  never a node input (so in no workflow, cache key, `run.json` or event), and not in the environment.
- **Masked on the way back.** Whatever a node's program returns (an error's message and hint, the stderr lines of a failure, progress text, text outputs, the strings and keys inside list, JSON and data outputs) has each secret value of 4 characters or more
  replaced by `***` before it is reported, also as it appears percent-encoded (either case, and `+` for a space) or escaped in JSON. The masking comes before a line is cut short, and the stderr kept from a persistent program is dropped at the start of each run. A node
  should still avoid echoing a key; this is the net under it. Not masked: media files a node writes (a value inside an image is not looked for).
- **Isolation between packs is advisory.** A trusted pack's program runs as you ([6.7](#67-trust)): it can read Fugue's config folder, `pack-secrets.json` included, exactly as it can read `providers.json`. Handing each pack only its own values stops an honest
  pack, a mistake and a look-alike node from reaching another's key; it does not stop a pack you trusted from being hostile. The trust question says so. Use a key with the narrowest rights the service offers.
- **Known gaps (filed).** A pack edited in place on disk to add `settings` after it was trusted is not asked about again; the runner's change check covers the node's own files. A node that is deterministic does not have secret values in its cache key.
- **Trust.** The install, trust and Get ready questions list the secrets a pack reads by name ([6.7](#67-trust)).

### 6.5 Where packs live and how they load

- Packs live in `~/.local/share/fugue/packs/<pack-id>/`, plus extra paths set in settings. A pack
  folder is named `<pack-id>`, or `<pack-id>@<major>` (the major of its version) when another major of
  the pack is installed beside it ([6.6](#66-installing)). Several majors of a pack load together;
  the same major twice loads once, from the earlier path. Each node type remembers the pack version
  it came from, which trust and the cache key use.
- A project that inherits `packs` also searches the Default's folders, after its own; an own pack of the same id and major hides the inherited one ([1.4.2](#142-projects)).
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
  they declare ([6.3](#63-the-needs-block)). On a terminal `fugue pack add` asks **one question**
  for the install and the trust together (#379: "Install this pack and trust it?"; a pack with a `setup` shows its exact command in the
  same list, [6.8](#68-per-pack-environments)); a yes does all of it, a no installs nothing. Settings > Packs keeps the install,
  the setup and the trust as separate actions for one-off changes. Without a terminal `pack add` fails with
  `needs_confirmation`.
- **Get ready (#379).** What a template, workflow or package needs (packs, their environments and the models) is one
  **ReadinessPlan** (`engine/readiness.h`), built from the workflow's nodes and `requires`, a template's carried packs and a package's
  contents. The window's "Get ready" sheet ([11.10](#1110-templates)) and `fugue template prepare` / `fugue workflow prepare` show that
  same plan and run it with the same executor: one yes covers each pack's install, its trust, its setup command and the model downloads.
  Nothing happens without that yes, a pack **update** still asks on its own, and the executor refuses a pack that differs from the one
  the list showed (another version, setup command or permissions: `pack_invalid`).
  **Before the yes nothing is fetched or written**: a folder pack (carried, or a local source resolved against the workflow's own
  folder) is copied to a staging folder, read in full (it must load, hold no symbolic link, and be the id and major the workflow
  names), and the yes installs that same copy; a git pack's row shows only what the workflow records (id, version, source, pinned
  ref) and "details after the download". **After the yes** a git pack from a workflow or package is cloned over https only (ssh, git
  and file only when a person types them into `pack add` or Settings > Packs), at most 200 MB, checked against the id and major the
  workflow names, and if it declares permissions (network, files, GPU) that the list could not show it asks once, for that row alone
  (**Allow** or **Skip**). A pack installed but not trusted is removed again, and the list says so. Free space is checked before each
  download. A list with problems (a pack with no source, a folder that is the wrong pack) is not "ready": the problems are reported
  and `prepare` exits non-zero. Folders chosen on the sheet (a picked model folder, **Models go into**) are written to the settings
  at the yes, not before.
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

- Trust is part of the one yes that installs the pack from the Get ready sheet or `fugue pack add` ([6.6](#66-installing)); it is
  still stored per pack id and version, and nothing else changes: an update asks again.
- The first time a node from a pack would run, Fugue shows the pack, its runtime kinds and its
  declared permissions (network, files outside the run folder, GPU), and the user must allow it.
  The decision is stored per pack id and version in `trust.json`; a pack update asks again.
  A project that inherits `packs` also honours the Default's decision for the inherited pack of that exact version, read-only ([1.4.2](#packs-trust-templates-and-prompts-of-a-project)).
- The prompt also lists the secrets the pack reads (`settings`, [6.4.1](#641-secrets)), by name: "Reads secrets: token_1, token_2". A yes covers exactly the
  names shown (a pack that declares others at an update asks again, and a pack fetched after the yes that reads secrets asks once more). Trusting a pack does not set a value; a
  secret is set by the person, with the origin it is for, and only that pack's own nodes ever receive it. The question says plainly that isolation between packs is advisory ([6.4.1](#641-secrets)).
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
environment, so packs cannot conflict with one another. The command is shown in the same list as the install and the trust
([6.6](#66-installing)), so one yes covers it and nothing asks again at the first run (#379). `setup` may record `size` (bytes it
downloads, for example 27 MB of ONNX Runtime); the Get ready list shows it and adds it to the disk space it needs.

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

### 6.13 HTTP Request (first-party pack)

`examples/packs/io.github.bmaltais.fugue-http` is a first-party pack with one command node, `request`, for calls that do not
deserve a pack of their own: a webhook, a REST lookup, an upload (#406, epic #409). It is installed like any pack ([6.6](#66-installing)), and it declares
`needs.network`, so the install and trust question shows `network` ([6.7](#67-trust)). It needs `python3` (standard library only; no setup).
The node is a sink and not deterministic: a run that reaches it always calls, a cached result is never reused, and a validation
or check never runs it (only a run starts a command node).

| Input | Meaning |
| --- | --- |
| `method`, `url` | `GET`, `POST`, `PUT`, `PATCH` or `DELETE`, and the address. |
| `headers` | One `Name: value` per line. A line break in a value after filling is refused, and so are `Host` and `Proxy-*` headers: the URL alone decides where the call goes (a shared edge routes by `Host`, so it could take a key past the origin check). |
| `body_kind`, `body` | `none`; `text` (sent as it is); `json` (the body is parsed as JSON, then the placeholders inside its strings are filled, so a value can never break the JSON). |
| `a`, `b`, `c`, `d` | Text inputs that fill `{a}` to `{d}` in the URL (percent-encoded), the header values and the body. A JSON string that is exactly `{a:json}` becomes the JSON value the input holds (`42`, `true`, `[1,2]`). A value is never expanded again. |
| `picture`, `audio`, `video` | The first one set goes up as `multipart/form-data` in the field `upload_field` (default `file`), with its file name and type. A JSON object body (or a text body, as the field `body`) becomes the other form fields. |
| `path1` to `path3` | A JSON path into an answer that is JSON, filling `field1` to `field3`. |
| `fail_on_error_status`, `timeout_s`, `max_response_mb` | Defaults off, 30 s (1 to 600), 5 MB (1 to 100). |

Outputs: `status` (number), `body` (the answer as text), and `field1` to `field3` (optional).

**JSON path.** The smallest form that reads naturally: an optional leading `$`, keys joined by dots (`data.items`), `[n]` and
`[-n]` for list items (`items[0]`, `items[-1]`; `items.0` too), and `["key"]` or `['key']` for a key with other characters. No wildcards,
filters, slices or recursive descent. A string comes back as it is, a number or boolean as its JSON text, an object or list as
compact JSON, and a missing step, a `null` or an answer that is not JSON leaves the field unset (an optional output). A path that is
not well formed is `bad_input`.

**Rules.**
- Only `https`; `http` is allowed for `localhost`, `127.0.0.1` and `::1` (anything loopback). A user name or password in the URL is refused. This is checked before any connection.
- The whole call (connect, send, read) shares `timeout_s`; the answer is read in pieces and stops at `max_response_mb` (a longer `Content-Length` stops it at once).
- A redirect is not followed: its status and body are the outputs (a redirect to an address that was never checked would break the rules above).
- A status of 400 or more is an output, not an error, because a lookup that finds nothing (404) or a webhook that refuses is often the answer a workflow branches on. `fail_on_error_status` turns it into an error.
- Environment proxies are honoured, except for loopback addresses.
- Errors are command-node errors ([7.2](#72-one-shot-mode-default)); the run reports them as `node_failed` with the node's code in the message and a hint after it: `bad_input` (not https, bad URL, header, body or path), `node_timeout`, `response_too_large`, `http_status`, `network_error` (no connection, TLS), `secret_missing` (a secret that is not set, or stored without its origin) and `bad_input` for a secret that may not go there ([6.4.1](#641-secrets)).
- **Secrets (#402).** The pack declares `token_1`, `token_2` and `token_3`, each `"origin": "required"` ([6.4.1](#641-secrets)). A header value or the path or query of the URL may name one as `{secret:token_1}`, filled at run time from the pack's own stored value (in the URL it is percent-encoded), and **only when the request's origin (scheme, host, port, from the URL as written out) is exactly the one the person set the key for**; otherwise `bad_input` "token_1 is only for https://api.example.com", before any connection. A placeholder (`{a}` to `{d}` or `{secret:…}`) in the scheme, user, host or port of the URL is refused (`bad_input`): the address must be written out. A secret is **refused in the body** (`bad_input`): a body is what a server often echoes back, and a header covers the keys services ask for. A name that is not one of the three is `bad_input`; one that is not set, or stored without an origin, is `secret_missing` with the hint to set it under Settings > Packs or with `fugue pack secret set … --origin`. The text of an input is never expanded: an input holding `{secret:token_1}` sends that text. The value is taken out (`***`, in every form of 6.4.1) of the status line, the body, the fields and every error message the node returns, so a server that echoes the key back does not put it in a run record. Never put a real key in a workflow, which is stored and shared in plain text.

---

## 7. Command-node protocol

A command node is a program that speaks JSON over stdio. Any language works; the protocol needs
no library. All text is UTF-8. Each message is one JSON object on one line.

### 7.1 Process setup

- Working directory: the node's own working folder inside the run folder
  ([9.7](#97-run-folder)).
- Environment: `FUGUE_PROTOCOL=1`, `FUGUE_NODE_DIR` (the folder holding `node.json`),
  `FUGUE_WORK_DIR` (the working folder). Secrets are not in the environment: they come in the request's `secrets` ([7.2](#72-one-shot-mode-default), [6.4.1](#641-secrets)).
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
- `secrets` (#402) is present only when the node's pack declares secrets ([6.4](#64-packjson)) and some are set: an object `{"<name>": "<value>"}` of those, for this run. A persistent program
  gets the current values with every request, so a changed key needs no restart. A program must not write a value to its outputs or logs; what it returns is masked ([6.4.1](#641-secrets)) as a net.
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
| `image.restore` | Restoration with SeedVR2 (#346): one image, already at its final size, redrawn in one diffusion step with the VAE in tiles | M5 |
| `text.encode`, `latent.sample`, `latent.decode`, `latent.encode` | Component-level generation; for a video model (Wan) the same capabilities make and decode a video latent (#105, [8.10](#810-gpu-memory-and-the-helper)) | M5 |

Capability ids are open: a provider module can advertise a new one and a node pack can ask for it.

A provider advertises, per capability: the models it can serve (as resource handles), properties
used for routing (model family, edit support, accepted input kinds; `inputs`, a list of the optional
inputs of the capability that not every provider takes, so far `"mask"` for `image.generate`), limits (maximum reference
images, minimum and maximum size, size multiple), defaults (steps, guidance, size) and status.

Standard parameters for the two M1 capabilities. A provider ignores an optional parameter it
does not support and must report the values it actually used.

| Capability | Parameters | Result |
| --- | --- | --- |
| `image.generate` | `prompt`, `negative_prompt`, `width`, `height`, `steps`, `guidance`, `seed`, `image` (start image), `strength`, `mask` (an input with `image`: an 8-bit gray PNG of its size, white redrawn; only to a provider whose `inputs` property lists `mask`, [14.4](#144-masking-and-inpainting-167)), `model` | `image`; used: `seed`, `width`, `height`, `steps`, `model`, `engine` |
| `image.edit` | `prompt` (the instruction), `images` (references), `width`, `height`, `steps`, `guidance`, `seed`, `model` | same |
| `text.generate` | `prompt`, `system` (optional), `image` (optional, an input), `seed`, `max_tokens`, `temperature`, `thinking` (optional boolean), `model` | `text` (the answer, reasoning left out); used: `seed`, `model`, `engine` |

| `image.upscale` | `model_path` (in `context`: the upscale model's file, or for a paired host a handle `{name, sha256, size}`), `tile_size` (pixels per tile side, tiles overlap by a quarter; an image no bigger than one is done whole), `image` (an input: an 8-bit RGB PNG), or `frames` (an input: a folder of PNGs, a video's chunk, #138) | `image` (the size times the model's scale), or `frames` (a folder `frames-out/` of the same names); `scale` |
| `image.restore` | `model_path`, `vae_path` (in `context`: the SeedVR2 checkpoint, which must carry its own `positive_conditioning`, and its VAE; each a path or for a paired host a handle), `tile_size` (a whole number of pixels, 64 to 16384, default 512), `overlap` (whole pixels two tiles share, default 64, at most half the tile), `seed`, `image` (an input: an 8-bit RGB PNG, the picture at the size the result should have, each side at least 16 and at most 16.8 MP) | `image` (the same size) |
| `video.generate` | `prompt`, `negative_prompt`, `width`, `height`, `video_frames`, `fps`, `steps`, `guidance`, `seed`, `image` (first frame, an input), `model` | `video` (a file); used: `seed`, `fps`, `frames`, `model`, `engine` |
| `audio.generate` | `prompt` (what the song is like), `lyrics` (`[Instrumental]` for none, empty for the model's own), `duration` (seconds), `language`, `seed`, `model`; optional `bpm`, `key`, `time_signature`, `creativity`, `keep_description`; `quality` (mp3-320 or lossless, #258: the default mp3 sends nothing); an input `ref_audio` (a file, #259: a recording whose voice and timbre the song takes); `adapter` (a name from the server's adapters) and `adapter_scale` (#260); or, with `task` (`cover`, `cover-nofsq`, `repaint`) and the input `audio` (a file), `strength`, `start`, `end` (the source song is sent with the job, no `duration`) | `audio` (a file) and `lyrics` (what was sung); used: `seed`, `model`, `engine` |

`image.upscale` is the one helper capability with no sd context. The upscaler (a file of tens of megabytes) stays loaded between jobs, keyed by the model file and the tile size, so the jobs of a video's chunks read it once; it is freed before a diffusion context loads and on out-of-memory, as the contexts are (8.10). For one image the library reports each tile as progress and has no cancel of its own: a cancel ends the helper after the grace period ([9.6](#96-cancel)). `frames` is a folder on the helper's disk (a paired host is refused: the node says so): each PNG goes through one at a time in file-name order, is written beside as `.part` and renamed (a frame is whole or absent), progress is the frames done, and a cancel is heard between frames and answers `cancelled`. The helper runs ESRGAN (RRDBNet, the 4x models) and MoSR; a model that unshuffles pixels to its input (Real-ESRGAN x2plus and the other 2x models) fails to load and is `node_failed` naming it.

`image.restore` has no sd context either: the helper builds a DGL program from the request (the pinned stable-diffusion.cpp has no `vid_upscale` call any more; its `docs/models/seedvr2.md` script is the model for it) and runs it on a DGL context made for the job and freed after it, so the DiT has the whole card (the helper frees what else it holds first). The program pads the picture with zeros at the bottom and right to a multiple of 16, encodes it with the VAE in tiles (the posterior mode), makes one euler step from noise (seeded, `sigmas` 1 to 0) on the encoded latent joined with a ones channel and the checkpoint's own positive embedding, decodes in tiles and crops the padding. The caller scales the picture first (Restore Image resizes with lanczos, which is also the colour fix's reference), so the helper never scales. Only checkpoints that carry the embedding load: the Comfy-Org INT8 ConvRot files (3B, 7B, 7B Sharp) do; the GGUF and the numz fp8 repackagings keep it in a separate file and the engine cannot identify them. The helper reads the two files' tensor names first (a few kilobytes) and answers `model_unsupported`, naming the file, before it frees anything loaded: a Klein or other model, a SeedVR2 file without its embedding, a VAE that is not SeedVR2's. It also refuses, as `input_invalid`, a picture under 16 or over 16.8 MP (the measured limit below; one side over 16384), a tile or overlap that is not a whole number in range, and a path that is not UTF-8. A cancel calls `sd_dgl_ctx_cancel` on the job's context and the job ends as `cancelled` within a step (0.04 s after the request in a real run, during decoding), locally and over HTTP. Progress is one bar that never goes back: loading the weights 0 to 0.2, the tiled encode to 0.4, the step to 0.55 and the tiled decode to 1. Memory on an RTX 3090 (24 GB), 7B INT8, tile 512 and overlap 64: a 258 x 258 picture peaks at about 12 GB of card use, 1536 x 1536 at 13.7 GB, 3072 x 3072 (9.4 MP) at 18.5 GB in 21 s, 4098 x 4098 (16.8 MP) at 22.9 GB in 35 s (all with 3.7 GB used by the desktop, so the model itself is about 8 GB); the sides beyond that were not tried.

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
- **Well-known services (presets).** Settings > Providers > "Add a service" is a closed row under the list; opened it lists the services
  by group, each with a one-line description and an Add chip (a cloud service already on the list says Added, with a Show chip), and the
  page's one search box filters both the list and these rows (names, descriptions, where it runs, "Free tier", and each service's
  model families, `keywords` in the table). Choosing one adds an instance with its name, type and address filled, opens its row, and the
  focus goes to the key (or the address when that is what is missing). The list is one 10u line per instance (glyph for where it runs,
  name, where, status, switch), one row open at a time; docs/ui-ergonomics.md has the decision. The table is one place, `src/settings/provider_presets.{h,cpp}` (`knownProviderPresets()`; the model's `providerPresets()`
  and `addPreset(id)` expose it to QML), a row being id, name, group, type, address, `needsKey`, `needsAddress`, `keyUrl`, a
  description, `keywords`, `freeTier` and, for an off-machine service, a privacy line. A preset is a starting point only: the instance written is the one
  typing it in would write, and adding the same preset again names the second "fal.ai 2". "Get a key" opens `keyUrl` in the browser,
  the only network action of the page. **Cloud services:** fal.ai (`fal`, no address: the type's own `https://queue.fal.run`,
  keys at fal.ai/dashboard/keys), Google Gemini (`gemini`, no address: the type's own `https://generativelanguage.googleapis.com`,
  keys at aistudio.google.com/apikey), OpenAI (`http`, `https://api.openai.com/v1`), Together AI (`http`, `https://api.together.ai/v1`),
  Replicate (`replicate`, no address: the type's own `https://api.replicate.com`, keys at replicate.com/account/api-tokens),
  OpenRouter (`http`, `https://openrouter.ai/api/v1`), Cloudflare Workers AI (`cloudflare`, no address: the type's own `https://api.cloudflare.com`,
  tokens at dash.cloudflare.com/profile/api-tokens). Each says "Sends your pictures and prompts to <name>" under its title.
  **Your own machines:** a local OpenAI-compatible server (`http`, `http://localhost:1234/v1`, no key: LM Studio, llama.cpp, vLLM),
  an Omarchy host (`omarchy`, address to fill), a Fugue helper on another machine (`fugue-helper`, address and key to fill).
  A card of a `fal` instance shows its key, `image_models` and `store_io`; its address, `rest_address`, `timeout_s` and `poll_ms`
  sit under an Advanced disclosure.
  A card of a `gemini` instance shows its key and `image_models`; its address and `timeout_s` sit under Advanced. A card of a `cloudflare` instance shows its token and `account_id` (both are needed); `image_models` and `timeout_s` sit under Advanced.
- **Local or remote is only the address.** It is never a property of a graph.
- Keys live only here. They are never written into a workflow, an export or an embedded graph.
- **Where an instance runs, and consent (#404).** Fugue is local-first: pictures, masks, audio and prompts leave the machine only
  when the person said so. An instance is one of three (`placementOf`, `engine/remote_consent.h`): *this machine* (no address, or
  a loopback one), *an own host* (an `omarchy` instance with an address, a host omarchy-local-ai paired with; or any
  other instance, a `fugue-helper` included (it has no pairing step), at a private (RFC 1918, unique local), link-local or CGNAT
  (Tailscale) address or a `.local`, `.internal` or `.ts.net` name: the test the `http` provider applies to the pictures a
  service may make Fugue fetch, 8.7, plus the tailnet's names, so there is one idea of "local"), or *off-machine* (any
  other type at any other address: a cloud service). The address is the one the provider really uses: an http provider
  module's default `address` (8.8) when the instance has none, `https://queue.fal.run` for a `fal` instance, `https://generativelanguage.googleapis.com` for a `gemini` instance, `https://api.replicate.com` for a `replicate` one and `https://api.cloudflare.com` for a `cloudflare` one. Nothing is asked for the first two: the person typed the
  address in and it is their own network. **The limit:** a stdio provider module is a program that runs here, so it counts as
  this machine even if its program calls a cloud API itself; Fugue cannot see that, and the module's description should say it.
  - **The question.** Before a run whose plan would run a node on an off-machine instance (the nodes that are not cached, found
    by the same check the graph's node states come from), the window asks once per instance, with the Question style of the Get
    ready sheet ([11.10](#1110-templates)): "This run sends data to a service off this machine", then a line per service,
    "Cloud (api.example.com) gets your pictures and prompts." (what leaves is read from the inputs of the nodes routed there:
    pictures, masks, audio, video, and prompts for text that is not a choice), and **No, cancel the run** / **Yes, send it**.
    The question opens with the focus on No, so a stray Enter or Escape never sends anything; Yes takes Enter only after Tab moves
    the focus to it. The run waits (same run id, nothing queued, nothing sent) for the
    answer; No ends it as cancelled. Several services of one run are one question with a row each; a yes covers each of them. The runs of one batch that
    need the same answer share one question (a batch of N asks once; yes starts all N in the order they were made, no cancels
    all). A yes records the services as they were shown (their type and address when the question was made), and a yes that
    cannot be written fails the runs and says so.
  - **Remembered.** A yes is kept in `remote-consent.json` beside `trust.json` in the state folder of the root (so per project,
    0600), `{"instances": {"<name>": "<type>|<address>"}}`. Removing the instance, or changing its address or type, forgets it:
    the file is pruned when providers.json is saved from Settings and before every run is checked, and a changed address no
    longer matches. A removed instance that is added again asks again. Providers are never inherited by a project
    ([1.4.2](#142-projects)), so each root's file depends only on its own providers.json, and Settings of a root prunes that root's
    file; another root's file is pruned by its own save and by its own next run.
  - **Not asked twice.** A pack node that declares `network` (HTTP Request, 6.13) is not routed through an instance: its trust
    question (6.7) already shows `network`, so no second question is asked for it.
  - **The CLI.** `fugue run` refuses with `consent_needed` (exit 1, before a run folder is made), naming each service and what
    leaves, unless the instance is remembered or `--allow-remote <instance>` (repeatable; `fugue app run` takes it too, and `fugue canvas
    act` refuses likewise) is given. The flag allows it for that run alone and keeps nothing: the run folder holds the
    instance's name with its type and address as they were when the flag was given, so a service changed meanwhile is not
    covered. A name that is not an instance is `unknown_provider`. The worker checks again when it reaches the
    node and fails it with `consent_needed` before anything is submitted, so a run started any other way (the canvas batch) cannot
    send either. A cached node sends nothing and is not asked about.
  - **Marks.** A node routed to an off-machine instance shows "Runs on Cloud (api.example.com)" above its face (the graph's
    check, `runsOn` of the node list); the App view says it on its "Runs on" line ([12.17](#1217-the-app-view-385-epic-397)).

### 8.3 Selection

- A node's provider is `auto` by default: the first enabled, ready instance, in the user's
  order, that offers the capability with the required properties and the requested model.
- A node may also state a preference (`Requirement::preferred`, the same shape as the properties): with
  no model chosen, a ready model that has them wins over an earlier one that has not, and none having
  them fails nothing. The selection lists the ready models that have them (`preferredOffers`), so the
  node can name them when the one it got cannot (Generate Image's mask, [14.4](#144-masking-and-inpainting-167)).
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
| `fal` | fal.ai's image models, from a key alone: `image.generate`, `image.edit` and inpainting with a mask ([the `fal` provider](#87-shipped-providers), #407). | M6 |
| `gemini` | Google's Gemini image models (Nano Banana), from a key alone: `image.generate` and `image.edit` with pictures ([the `gemini` provider](#87-shipped-providers), #407). | M6 |
| `replicate` | Replicate's official image models, from a token alone: `image.generate`, `image.edit` and inpainting with a mask ([the `replicate` provider](#87-shipped-providers), #407). | M6 |
| `cloudflare` | Cloudflare Workers AI's image models, from an API token and an account id: `image.generate` and `image.edit` ([the `cloudflare` provider](#87-shipped-providers), #407). | M6 |
| `fugue-helper` | Fugue's own out-of-process generation helper and a Fugue-managed server, started on demand ([8.10](#810-gpu-memory-and-the-helper)). | M5 |
| `comfyui` | Optional, off by default. Talks HTTP and websocket to a ComfyUI server the user installs and runs. Fugue bundles none of its code or node schemas and never starts it. | after v0.1 |

The shipped providers are not special: they sit behind the same interface and registry as a
third-party module ([8.11](#811-the-provider-interface)). `omarchy`, `http`, `fal`, `gemini`, `replicate`, `cloudflare` and `fugue-helper` are
listed as modules with transport `builtin` ([8.8](#88-provider-modules)): they are code compiled into
Fugue, because `omarchy` speaks its servers' own APIs, not the protocol of 8.8, the helper's
program is found by Fugue ([8.10](#810-gpu-memory-and-the-helper)), and `http` needs what a module
is not given (below). `comfyui` is not built yet: a module folder for ComfyUI's own API, written
against 8.8 when its milestone comes, with no change to Fugue.

**The `http` provider** (#401) is a generic endpoint: an instance of type `http` with an `address`
(the service's API root, such as `https://api.openai.com/v1` or `http://localhost:1234/v1`), a `key`
and a few settings (`providers.json`, [8.2](#82-provider-instances)). It serves `image.generate`,
`image.edit` and `text.generate` (with a picture) from two dialects. It is compiled in, not a module
folder as the first plan had it, because a module is not given its instance's key, address or settings
(a `stdio` module gets none, and an `http` one is spoken to in the protocol of 8.8, which no OpenAI
service speaks), and because it shares the omarchy provider's request and result code
(`engine/server_api.h`).

| Setting | Meaning |
| --- | --- |
| `address` | The API root. Required: with none, or one that is not `http(s)://`, describe is `provider_unreachable`. |
| `key` | Sent as `Authorization: Bearer <key>` to the address and nowhere else (never to a redirect, never to a picture the service names by URL). Optional: a local server takes none. |
| `api` | `auto` (default), `openai` or `sdcpp`. Auto asks `<root>/sdcpp/v1/capabilities` (the address without its `/v1`): an answer means the native server, anything else the OpenAI dialect. Another value is `invalid_document`. |
| `image_models`, `text_models` | The served names (a list, or text separated by commas). Without them `GET <address>/models` lists the models: an id that looks like a picture model (`dall-e`, `gpt-image`, `imagen`, `flux`, `stable-diffusion`, `sdxl`, `qwen-image`, `z-image` and the like) is an image model, one that looks like embeddings or speech is none, the rest are text models. A setting replaces that guess. A service with no model list works when the models are named here. |
| `timeout_s` | How long a request may go silent before the job fails (default 300). |

- **OpenAI-compatible.** `image.generate` is `POST <address>/images/generations` (`model`, `prompt`,
  `n` 1 and `size` `WxH` when the node gave both sides); with a start image, and for `image.edit`, it is
  `POST <address>/images/edits` as a multipart form (`image[]` for each picture). The answer is
  one picture, as `b64_json` or as a `url` (fetched without the key). `text.generate` is
  `POST <address>/chat/completions`, streamed, with a picture as a data URL, as the omarchy provider
  does it. Each model is a handle of `fugue/image-model` or `fugue/text-model` with its served name
  and no hash, offered `ready`; every image model is offered for edits too (the service says no in
  its own words when one cannot). A service has no seed, steps or negative prompt, so none is sent or
  reported as used; the width and height used are the picture's own. The engine id is
  `openai@<host>`. A mask is `mask_unsupported` (an OpenAI mask marks by transparency, not by white),
  as is any model of a server that does not list `mask_image`.
- **The stable-diffusion.cpp native server** is the omarchy provider's job API (8.7 below): submit,
  poll, cancel, step previews, the server's defaults and limits, `mask_image` and `ref_images` from
  its capabilities. Its one model is the one it has loaded (`model.name`). Engine id `sdcpp@<host>`.
- **The key.** It lives in `providers.json` only. It is in no message, status, result, run record,
  log or file: a service that repeats it in an error text (they do) is scrubbed before the text is
  used.
- **Errors**, in the codes of [13.3](#133-errors), each with a hint:

| The service | Code | Said |
| --- | --- | --- |
| 401, 403 | `provider_unreachable` | refused the key; the hint names providers.json (or says the service needs one) |
| 404 for a model (`model_not_found`, or a message that names the model) | `model_missing` | no such model |
| 404 otherwise | `node_failed` | nothing at the URL; the hint says the address is the API root, usually ending in `/v1` |
| 429 | `node_failed` | the key is rate limited, with the `Retry-After` it sent ("wait 17 seconds"); `insufficient_quota` says there is no quota left |
| 503 that says it is loading | `model_not_running` | |
| 502, 503, 504 | `provider_unreachable` | not available right now |
| other 5xx | `node_failed` | with the service's message; "out of memory" is `out_of_memory` |
| other 4xx (400 and the like) | `node_failed` | the service's own message |
| no answer in `timeout_s`, a refused connection, a stream cut short | `provider_unreachable` | what happened; the timeout hint names `timeout_s` |
| an answer that is not JSON, or has no picture | `node_failed` | the hint says the address should be the API root |

  There is no new code: a rate limit is `node_failed` ("a later run succeeds") and Fugue does not
  retry it (what a retry would cost on a paid service is the business of #403).
- **Limits.** A picture link a service names is fetched only when it is `http(s)` (a relative one is
  resolved against the address) and not a loopback, link-local or private address, unless the instance
  itself is at one; a redirect of it is checked the same way (3 hops at most), and Fugue follows no redirect
  of an API call: a 301 or 302 is said with the new address. No answer is read past 64 MB (512 MB for a
  picture or a native job), and a native job is given up on (`node_timeout`) after 6 times `timeout_s`;
  its poll is retried a few times, with growing waits, on a 429 or a 502, 503 or 504.
- Not built, and said so here: video and music (`video.generate`, `audio.generate`), a model's
  parameters beyond those above, and any cost or privacy notice (#403, #404). The key goes in clear to
  a plain `http://` address that is not on this machine: use `https://`.
- Checked by hand against: a stable-diffusion.cpp server whose key was not at hand (the 401 above, as
  `fugue run` shows it). Not against a real OpenAI-compatible text or picture service.

**The `fal` provider** (#407, approved by the owner 2026-11-25) gives a person who pastes only their fal.ai key fal's image
models in the model pickers: `{"name": "fal.ai", "type": "fal", "key": "…"}` in `providers.json`. It is compiled in, like `http`, for
the same reason (a module is not given its instance's key). It serves `image.generate`, `image.edit` and, with a mask, inpainting.
It is off-machine ([8.2](#82-provider-instances)): `placementOf` takes the default address `https://queue.fal.run` for an instance
with none, so the consent question of #404 is asked before anything is sent; a run that is allowed sends what the question named.

| Setting | Meaning |
| --- | --- |
| `key` | Sent as `Authorization: Key <key>` to fal's own API hosts only: the queue (`address`) and the storage host (`rest_address`). Never to an upload URL, a result or picture URL, or a redirect (none is followed on an API call). Without a key the models are `unavailable` ("paste your key in Settings > Providers") and a job is refused before anything is sent. |
| `address` | The queue's root, default `https://queue.fal.run`. Tests point it at a local fake. |
| `rest_address` | fal's storage host, default `https://rest.fal.ai` (what fal's own client uses), also when `address` is typed in; only a loopback `address` (a test's fake) serves both. The key goes to this host too, so consent judges both hosts (the more remote wins) and the answer is kept for the pair. |
| `image_models` | More endpoint ids (a list, or text separated by commas), each with a generic mapping: `prompt`, `image_size`, `seed`, `image_url` (so an edit of one takes one picture), `num_images` 1 and `output_format` png. |
| `store_io` | `true` leaves fal's default: it keeps a job's inputs and outputs 90 days. Unset, every job is sent with `X-Fal-Store-IO: 0`, and the pictures sent and the results asked to expire in an hour (below). |
| `timeout_s` | How long a request may go silent before the job fails (default 300). A job is given up on, in any phase (uploads, queue, result, download), after 6 times that (`node_timeout`; the transfer is cut and the job cancelled at fal), so a trickle of bytes cannot hold it. |
| `poll_ms` | The first wait between polls (default 500, growing by half each time to 6 times it, 3 s; back to the first when the state changes). |

- **Models.** A table shipped with Fugue (`share/providers/fal-models.json`, compiled in) lists each endpoint id with its title, its
  published price, its kind (generate, edit, inpaint) and how Fugue's standard parameters ([8.1](#81-capabilities)) become the model's own
  fields: `prompt`; the size as `image_size: {width, height}` when the node gave both sides (a model that follows its picture has none);
  `seed` (sent when it is 0 or more; the seed of the answer is what is reported); steps as `num_inference_steps` (cut to the model's
  limit: schnell takes 4 at most), guidance as `guidance_scale`, a negative prompt where the model has one; the start or edit picture as
  `image_url`; the mask as `mask_url`; and fixed fields: `num_images` 1, `output_format` png, and the model's own safety setting
  (`enable_safety_checker` true, or `safety_tolerance` "2", fal's defaults). A parameter the model has no field for is not sent.
  Each model is a handle of `fugue/image-model` with the endpoint id as its served name and no hash, offered `ready` (the person pays
  per run; nothing is downloaded), titled with its price. The shipped list, with fal.ai's published prices (checked 2026-11-25):

| Endpoint id | Offered for | Price |
| --- | --- | --- |
| `fal-ai/flux/schnell` | `image.generate` | $0.003 per megapixel |
| `fal-ai/flux/dev` | `image.generate` | $0.025 per megapixel |
| `fal-ai/flux-pro/v1.1` | `image.generate` | $0.04 per megapixel |
| `fal-ai/qwen-image` | `image.generate` | $0.02 per megapixel |
| `fal-ai/flux-pro/kontext` | `image.edit` (one picture) | $0.04 per image |
| `fal-ai/flux-pro/v1/fill` | `image.generate` with a picture and a mask (property `inputs: ["mask"]`) | $0.05 per megapixel |

  A text-to-picture model given a start picture is `input_invalid` (the picture would leave the machine for nothing); a mask for a model
  with no mask field is `mask_unsupported`; an edit with no picture, or an inpaint with no mask, is `missing_input`. A picture whose
  safety checker flagged it (`has_nsfw_concepts`, a black picture) is `node_failed`, not a result.
- **The flow.** Pictures first: each is uploaded to fal storage, `POST <rest_address>/storage/upload/initiate?storage_type=fal-cdn-v3`
  (`{content_type, file_name}`, with the key) answers `{file_url, upload_url}`, then the bytes are `PUT` to `upload_url` (no key, it is
  presigned), and `file_url` goes into the input. The link is kept per content hash for the session (30 minutes at most, of the hour the
  file is asked to live, so a picture sent twice is sent once). Then `POST <address>/<endpoint id>` with the input; the answer's `status_url`, `response_url` and
  `cancel_url` are the only URLs used afterwards (building them from the endpoint id is a 405) and must be on the origin of the queue or
  the storage host, or the job fails before the key goes there. `GET status_url` is polled (answered 202 while the job waits or runs, 200 when it is done; `IN_QUEUE` with its position, `IN_PROGRESS`:
  both shown as the job's status text; `COMPLETED`, with an `error` when the job failed), then `GET response_url`, then the first
  picture is downloaded from its URL **without the key**, under the `http` provider's rules: `http(s)` only, no loopback, link-local or
  private address (unless the instance is at one), redirects checked, 3 hops, 512 MB. Cancel is at once: the job is `cancelled`, polling
  stops and `PUT cancel_url` is sent (a cancel made while the job is being sent is sent when its URLs arrive); fal may still finish a
  job already running (202 `CANCELLATION_REQUESTED`), whose result Fugue drops. The engine id of the description is `fal@<host>`; the
  `engine` used (in the run record) is `fal@<endpoint id>`, and the cache key holds the endpoint through the model handle.
- **What still leaves the machine.** The prompt and the other fields, and every picture and mask of the node, go to fal.ai (and the
  pictures to fal's storage CDN, where a link is readable by anyone who has it until it expires). With `store_io` unset, the requests
  carry `X-Fal-Store-IO: 0` (fal then keeps no copy of the job's inputs and outputs), `X-Fal-Object-Lifecycle-Preference` and the
  uploads `X-Fal-Object-Lifecycle` with `expiration_duration_seconds` 3600. Fal still sees the request while it runs, and its terms let it
  derive anonymised usage data from them; ask fal for more. The key is in no message, status, result, run record or log: the error
  texts are scrubbed of it (a service that repeats it is `***`).
- **Errors**, in the codes of [13.3](#133-errors), each with a hint:

| The service | Code | Said |
| --- | --- | --- |
| no key | `provider_unreachable` | the provider has no key; the hint names Settings > Providers |
| 401, 403 | `provider_unreachable` | refused the key, with the service's words ("Exhausted balance"); the hint names Settings > Providers |
| 404 on submit | `model_missing` | no such model (the endpoint id) |
| 404, 410 later | `node_failed` | no longer has the job |
| 422 | `node_failed` | the service's `detail` message (`loc: msg`); `content_policy_violation` and `no_media_generated` say so |
| 429 | `node_failed` | rate limited, with the `Retry-After` it sent (fal's concurrency limit starts at 2 jobs) |
| 502, 503, 504 | `provider_unreachable` | not available right now |
| other 5xx | `node_failed` | the service's message; "out of memory" is `out_of_memory` |
| `COMPLETED` with an `error` | `node_failed` | the service's message (a `timeout` type is `provider_unreachable`) |
| other 4xx | `node_failed` | the service's own message |
| no answer in `timeout_s`, a refused connection | `provider_unreachable` | what happened; the hint names `timeout_s` |

  A poll that gets no answer, a 429 (its `Retry-After`, up to 30 s, is waited) or a 5xx is tried again up to 5 times in a row before it fails the job. A key is never sent over plain `http://` to a host that is not loopback: the provider is `provider_unreachable` with a hint to use https. There is no retry of a
  submit (fal bills a job that ran; #403 is the place for what a retry costs).
- **Wire details** confirmed against fal's own client (fal-js, 2026-11-25): the key header is `Key <key>`, sent only to fal hosts;
  the storage host is `https://rest.fal.ai`; the upload is `initiate` then a `PUT` of the bytes to `upload_url` with the file's
  `Content-Type` and no key; the `X-Fal-Object-Lifecycle` (uploads) and `X-Fal-Object-Lifecycle-Preference` (jobs) headers carry
  `{"expiration_duration_seconds": n}`; the client builds the status and cancel URLs from `owner/alias` only, so the URLs the queue
  returns must be used. Not confirmed anywhere: whether a white mask means "redraw" for `flux-pro/v1/fill` (Fugue sends white as redrawn,
  [14.4](#144-masking-and-inpainting-167)); the result of a rate limit on the queue (`429` body); whether `num_images`,
  `output_format` and the safety fields are accepted by every model of the table (taken from each model's published input list).
- **Decisions** (the builder's, each with its confidence; overturn any): the model offers are `unavailable` without a key rather than
  `ready` (high: a pick that can only fail is worse); an inpaint model is offered for `image.generate` with the `mask` input, as 8.1
  says, not as an edit (high); `Description.engine` is `fal@<host>` because selection reads one engine per instance, the endpoint being
  in the model handle and in `used.engine` (high); uploads and results expire in one hour, the session's reuse window 30 minutes
  (medium: the header is in fal's client, its effect was not observed); a flagged (black) picture fails the node instead of being
  returned (medium-high); a cancel answers `cancelled` at once, without waiting for fal's reply to the PUT (medium-high); a job
  that names URLs on another origin than the queue's or the storage host's is refused, since the key travels with them (high).
- **Not built**, and said so here: fal's cost estimate before a run, discovery of any endpoint's inputs from its OpenAPI schema,
  multi-image Kontext, upscale, video, webhooks and the synchronous `fal.run` API.
- **Not checked against the real service**: no fal key was at hand when this was built. The tests use a fake queue, storage and file
  server on the loopback, whose answers follow fal's documentation and client; a real run with a key is pending.

**The `gemini` provider** (#407, approved by the owner 2026-11-25) gives a person who pastes only a Google Gemini API key the Nano
Banana image models in the model pickers: `{"name": "Google Gemini", "type": "gemini", "key": "…"}` in `providers.json`. Compiled in,
like `fal`. It serves `image.generate` and `image.edit`; every model takes the prompt and the node's pictures (up to 14) in one
request, so a picture is an edit or a reference. A mask is `mask_unsupported`: the API has none. It is off-machine
([8.2](#82-provider-instances)): `placementOf` takes the default address `https://generativelanguage.googleapis.com`, so the consent
question of #404 is asked before anything is sent.

| Setting | Meaning |
| --- | --- |
| `key` | Sent as the `x-goog-api-key` header to the origin of `address` and nowhere else; never in a URL (not `?key=`) and never to a redirect (none is followed). Without a key the models are `unavailable` and a job is refused before anything is sent. |
| `address` | The API's root, default `https://generativelanguage.googleapis.com`. Tests point it at a local fake. |
| `image_models` | More model ids (a list, or text separated by commas), with the generic mapping: `aspectRatio` from the node's size, no `imageSize`, up to 14 pictures. |
| `timeout_s` | The job's one deadline (default 300): the call is cut and the job fails with `node_timeout` after it. |

- **The flow.** One synchronous call: `POST <address>/v1beta/models/<model>:generateContent`, body `contents[0].parts` = the prompt
  as `text`, then each picture as `inline_data` (`mime_type`, base64 `data`), and `generationConfig`: `responseModalities`
  `["TEXT", "IMAGE"]`, `imageConfig.aspectRatio` (the nearest supported ratio to the node's width and height), `imageConfig.imageSize`
  (`512`, `1K`, `2K`, `4K`: the nearest to the larger side, for the models that take one; the others make about 1 K) and `seed` (modulo 2^31: Google's is an int32) when
  the node sets one. The result is the first `inlineData` image part of the first candidate that is not a `thought` (Gemini 3 Pro
  shows interim pictures as thoughts), written to the output folder. The values used are the picture's own size (PNG) or the size
  asked for, the seed sent, the model and `gemini@<model id>`. A cancel drops the call (Google has no cancel; a started call is billed).
  The whole request, inline pictures included, may be 20 MB (Google's limit, [image understanding](https://ai.google.dev/gemini-api/docs/image-understanding)):
  more is `input_invalid` before anything is sent; the Files API is not used.
- **Models.** A table shipped with Fugue (`share/providers/gemini-models.json`, compiled in) lists each model id with its
  `aspects`, `sizes` and price; the price is in the title. Checked 2026-11-25 against Google's pages
  ([models](https://ai.google.dev/gemini-api/docs/models), [pricing](https://ai.google.dev/gemini-api/docs/pricing),
  [image generation](https://ai.google.dev/gemini-api/docs/image-generation)):

| Model id | Name | Price |
| --- | --- | --- |
| `gemini-nano-banana-2.1` | Nano Banana 2.1 | $0.0336 per 1K image ($0.113 per 4K) |
| `gemini-3.1-flash-image` | Nano Banana 2 | $0.067 per 1K image ($0.151 per 4K) |
| `gemini-3.1-flash-lite-image` | Nano Banana 2 Lite | $0.0336 per 1K image |
| `gemini-3-pro-image` | Nano Banana Pro | $0.134 per 1K or 2K image ($0.24 per 4K) |
| `gemini-2.5-flash-image` | Nano Banana | $0.039 per image (up to 1024 px); Google shuts it down 2027-03-15 |

  Imagen is not offered: Google lists `imagen-4.0-generate` as shut down.
- **No free quota.** Google's pricing page lists "Free tier: Not available" for every image model above, and the free API tier
  does not cover them: a key without billing is expected to be refused or answered 429 (not observed; no key was used). The preset's description cannot promise a free try. The 429 error
  says so and its hint points to turning billing on for the key's project in Google AI Studio.
- **What Google does with the inputs** ([terms](https://ai.google.dev/gemini-api/terms)). On the *unpaid* (free) services Google uses the
  prompts, pictures and outputs to "provide, improve, and develop" its products, and human reviewers may read them. On the *paid*
  services it does not use them to improve its products. The image models are paid-only today, so a key that works for them is on
  the paid terms; a person who also uses the key for free-tier models (text) is on the unpaid terms for those. The preset's privacy
  line says only that prompts and pictures go to Google; the consent question names what is sent.
- **Errors**, in the codes of [13.3](#133-errors), each with a hint. Google's body is `{"error": {"code", "message", "status", "details"}}`:

| The service | Code | Said |
| --- | --- | --- |
| no key | `provider_unreachable` | the provider has no key; the hint names Settings > Providers |
| 401, 403, or 400 with `API_KEY_INVALID` | `provider_unreachable` | refused the key (Google answers 400, not 401, for an unknown key) |
| 404 | `model_missing` | no such model |
| 429 | `node_failed` | no quota for this key, with the `Retry-After` it sent; the hint says the image models have no free quota |
| 400 `FAILED_PRECONDITION` | `node_failed` | the key cannot be served here (billing, region) |
| other 400 | `node_failed` | the service's message |
| 502, 503, 504 | `provider_unreachable` | not available right now |
| other 5xx | `node_failed` | the service's message |
| a prompt block (`promptFeedback.blockReason`), or a `finishReason` of `SAFETY`, `IMAGE_SAFETY`, `PROHIBITED_CONTENT`, `IMAGE_PROHIBITED_CONTENT`, `BLOCKLIST`, `SPII`, `RECITATION` and no picture | `node_failed` | held back under Google's safety rules; change the prompt or the pictures |
| an answer with text and no picture (`NO_IMAGE` too) | `node_failed` | made no picture, with what the model said |
| a mask | `mask_unsupported` | the API has none |
| no answer in `timeout_s`, a refused connection | `provider_unreachable` / `node_timeout` | what happened; the hint names `timeout_s` |

  A key is never sent over plain `http://` to a host that is not loopback. Every text from the service is scrubbed of the key. There
  is no retry.
- **Decisions** (the builder's, each with its confidence; overturn any): `generateContent` on `v1beta`, not the newer Interactions
  API Google calls the default for new work (medium-high: `generateContent` is documented as supported, answers in one call and
  has the one stable shape; the Interactions API would need a second request shape for no gain here); `responseModalities`
  `["TEXT","IMAGE"]` as in Google's examples (medium-high); the key in the header only (high; the docs allow both); Imagen left out,
  being shut down (high); `max_images` 14 for every model, as Google's page says "up to 14 reference images in total" (medium: the
  older Nano Banana was advised up to 3, Google answers an error if it refuses); the negative prompt, steps and guidance are not sent,
  the API has none (high); a thought picture is skipped (high).
- **Not built**: `text.generate` (the same key could serve Gemini text models later, through `http_provider`'s `text.generate` and
  Google's OpenAI-compatible endpoint; not built now), the Files API for large inputs, the Interactions API, search grounding, thinking
  levels, multi-turn editing, a mask, a cost estimate before a run.
- **Not checked against the real service**: no Gemini key was used and no call was made to Google (documentation only). The tests use
  a fake API on the loopback whose answers follow Google's documentation.

**The `replicate` provider** (#407, approved by the owner 2026-11-25) gives a person who pastes only their Replicate API token
Replicate's official image models in the model pickers: `{"name": "Replicate", "type": "replicate", "key": "r8_…"}` in `providers.json`.
Compiled in like `fal`, for the same reason. It serves `image.generate`, `image.edit` and, with a mask, inpainting. It is off-machine
([8.2](#82-provider-instances)): `placementOf` takes the default address `https://api.replicate.com` for an instance with none, so the
consent question of #404 is asked before anything is sent. Unlike fal there is one host, so the consent fingerprint is the address alone.

| Setting | Meaning |
| --- | --- |
| `key` | Sent as `Authorization: Bearer <key>` to the origin of `address` only. Never to a picture URL or a redirect (none is followed on an API call); the one exception is an output file at `replicate.delivery` that refuses the first, keyless fetch (below). Without a key the models are `unavailable` and a job is refused before anything is sent. |
| `address` | The API's root, default `https://api.replicate.com`. Tests point it at a local fake. |
| `image_models` | More official model ids, `owner/name` (a list, or text separated by commas), with a generic mapping: `prompt`, `seed`, `image`. An id that is not `owner/name` is ignored. Only official models can be run by name (a community model needs a version id: not built). |
| `timeout_s` | How long a request may go silent before the job fails (default 300). A job is given up on, in any phase, after 6 times that (`node_timeout`; the transfer is cut and the prediction cancelled). |
| `poll_ms` | The first wait between polls (default 500, growing by half each time to 6 times it; back to the first when the state changes). |

- **Models.** A table shipped with Fugue (`share/providers/replicate-models.json`, compiled in) lists each model with its title, published
  price, kind (generate, edit, inpaint) and how Fugue's standard parameters ([8.1](#81-capabilities)) become the model's own `input` fields,
  read from each model's published input schema. Replicate's models take an aspect ratio and a megapixel class, not a width and height:
  the node's size becomes the nearest `aspect_ratio` the model lists (on a log scale) and, where the model has one, the nearest
  megapixel value; a model that follows its picture (Kontext, Qwen-Image Edit, Klein with a picture: `match_input_image`) gets no size.
  `seed`, steps (cut to the model's limit), guidance, a negative prompt where the model has one, the picture (`input_image`, `image` or
  a list `images`) and the mask (`mask`; white is redrawn, as Fugue sends it); fixed: `output_format` png. Prices checked 2026-11-25:

| Model | Offered for | Price |
| --- | --- | --- |
| `black-forest-labs/flux-schnell` | `image.generate` | $0.003 per image |
| `black-forest-labs/flux-2-klein-4b` | `image.generate`, and `image.edit` (up to 5 pictures: the table's `images_max`) | $0.001 per megapixel |
| `qwen/qwen-image` | `image.generate` | $0.025 per image |
| `black-forest-labs/flux-kontext-dev` | `image.edit` (one picture) | $0.025 per image |
| `qwen/qwen-image-edit` | `image.edit` (one picture) | $0.03 per image |
| `black-forest-labs/flux-fill-dev` | `image.generate` with a picture and a mask (`inputs: ["mask"]`) | $0.04 per image |

  The refusals are fal's: a start picture for a text-only model is `input_invalid`, a mask for a model with none `mask_unsupported`, an
  edit with no picture or an inpaint with no mask `missing_input`.
- **The flow.** `POST <address>/v1/models/<owner>/<name>/predictions` (`{"input": {...}}`, header `Cancel-After` set to the job's deadline,
  so Replicate stops a prediction nobody waits for) answers 201 with `urls.get` and `urls.cancel`; these are the only URLs used afterwards
  and must be on the API's origin, or the job fails before the key goes there. `GET urls.get` is polled (`starting`, shown as queued;
  `processing`; then `succeeded` with `output`, or `failed` with `error`, `canceled`, `aborted`). The first output URL (`replicate.delivery`)
  is downloaded **without the key**, under the `http` provider's URL rules (3 hops, 512 MB). Replicate's own clients send the token to
  the delivery host, and its output-files page says to; so when that first fetch is answered 401 or 403 it is repeated **once** with the key,
  and only when all hold: the instance's address is the default `https://api.replicate.com` (an overridden or loopback address was agreed to
  for that address only), the URL is `https` on port 443, its host (case-folded) is `replicate.delivery` or ends in `.replicate.delivery`,
  it is the first hop (never on or after a redirect; a redirect drops the key), and there is a key (`replicateMayKeyDelivery`). Cancel is at once: the job is `cancelled`,
  polling stops and `POST urls.cancel` is sent (a cancel made while the job is being sent is sent when its URLs arrive). `Prefer: wait`
  is not used: one polling path serves a 1 s model and a 60 s one alike.
- **Pictures.** Up to 256 KB (the HTTP reference's limit for data URLs) goes in the input as a `data:` URI, which Replicate does not store; a larger
  one is uploaded with the files API (`POST /v1/files`, multipart `content`, 100 MB at most, with the key) and its `urls.get` (checked to be
  on the API origin) goes in the input. The files API keeps a copy on Replicate for its own lifetime, which is why the small path is a
  data URI.
- **What still leaves the machine.** The prompt and other fields and every picture and mask go to Replicate. For predictions made
  through the API Replicate removes inputs, outputs and logs after one hour (its documentation), and output files expire after an hour
  too; there is no setting to ask for more or less. The key is in no message, status, result, run record or log: error texts are
  scrubbed of it.
- **Errors**, as fal's table (13.3), with these differences: 402 (no credit) is `provider_unreachable` with a hint to add credit at
  replicate.com/account/billing; the body is `{title, detail, status}` or, on a 422, `{detail, invalid_fields: [{field, description}]}`;
  a `failed` prediction is `node_failed` with its `error` ("NSFW content detected" says content policy; E1001 or "out of memory" is
  `out_of_memory`; E6716, E9243 and E8765 are `provider_unreachable`). Polls are retried as fal's. No retry of a submit.
- **Wire details** confirmed from Replicate's HTTP API reference, its OpenAPI schema and its Python client (2026-11-25): the key header is
  `Bearer`, the official-model endpoint, `Prefer: wait` 1 to 60 s, `Cancel-After` (5 s minimum), the status values, the files API's
  multipart `content` field and 201 answer with `urls.get`, the rate limits (600 predictions and 3000 other calls per minute; 429 with
  a `detail`).
- **Decisions** (the builder's, each with its confidence; overturn any): a data URI up to 256 KB and the files API above it (medium-high:
  the HTTP reference says 256 KB, the input-files guide 1 MB, no hard limit is published); seeds are reduced modulo 2^31 (the models
  publish no seed range; Fugue's go to 2^53-1; the table's `seed_max` overrides) and the value sent is the one reported; no `Prefer: wait`, polling only (high); the output is fetched without
  the key first and with it once on a 401 or 403 under the conditions above (medium-high, owner's decision after review); the seed reported is the one sent, as Replicate returns none (high); `image_models` takes official ids
  only (high).
- **Not checked against the real service**: no Replicate token was at hand. Whether `replicate.delivery` really answers 401 or 403
  without the key is not observed: the retry costs nothing if it does not. A refusal after the retry is `node_failed` ("would not serve
  the picture"), without a hint of expiry.
  Not built: a cost estimate, community models by version id, several outputs, upscale, video, webhooks and `Prefer: wait`.

**The `cloudflare` provider** (#407, approved by the owner 2026-11-25) gives a person who pastes a Cloudflare API token and their
account id Workers AI's image models in the model pickers: `{"name": "Cloudflare Workers AI", "type": "cloudflare", "key": "<token>",
"account_id": "…"}` in `providers.json`. It is compiled in, like `fal`, for `image.generate` and, for the models that take reference
pictures, `image.edit`. It is off-machine ([8.2](#82-provider-instances)): `placementOf` takes the default address
`https://api.cloudflare.com`, so the first use asks for consent, naming Cloudflare.

| Setting | Meaning |
| --- | --- |
| `key` | The API token (it needs the Workers AI permission), sent as `Authorization: Bearer <token>` to the origin of `address` only; no redirect is followed. Without it, or without `account_id`, the models are `unavailable` ("paste your API token / account id in Settings > Providers") and a job is refused before anything is sent. |
| `account_id` | Required: the account the calls are made for (dash.cloudflare.com overview). Letters, digits, `-` and `_` only, as it is part of the URL. |
| `address` | The API's root, default `https://api.cloudflare.com`. Tests point it at a local fake. |
| `image_models` | More `@cf/...` model ids (a list, or text separated by commas), with a generic JSON mapping: `prompt`, `width`, `height`, `seed`. Ids that are not of the form `@xx/owner/name` are ignored. |
| `timeout_s` | How long the call may go silent before the job fails (default 120). The job is given up on after 6 times that (`node_timeout`), so a trickle of bytes cannot hold it. |

- **Models.** A table shipped with Fugue (`share/providers/cloudflare-models.json`, compiled in) lists each model with its title, its
  published price, how the request is sent and which of Fugue's standard parameters the model takes (a parameter with no field is
  not sent). Each is a `fugue/image-model` handle whose `name` is the model id, no hash. The list, with Cloudflare's published
  neuron prices (developers.cloudflare.com/workers-ai/platform/pricing, checked 2026-11-25; $0.011 per 1,000 neurons above the free
  allocation), the title showing the price of a 1024x1024 picture (4 tiles of 512x512):

| Model id | Offers | Published price | One 1 MP picture |
| --- | --- | --- | --- |
| `@cf/black-forest-labs/flux-1-schnell` | `image.generate` (JSON: `prompt`, `steps` up to 8, default 4; no seed: the service answers 400 "Additional or unevaluated properties '/seed'", seen on the owner's first real run 2026-11-25; no size field is documented) | 4.80 neurons per 512x512 tile, 9.60 per step | 57.6 neurons, about $0.0006 |
| `@cf/black-forest-labs/flux-2-klein-4b` | `image.generate`, `image.edit` (up to 4 reference pictures) | 26.05 neurons per output tile, 5.37 per input tile | 104 neurons, about $0.0012 |
| `@cf/black-forest-labs/flux-2-klein-9b` | `image.generate`, `image.edit` (up to 4) | 1363.64 neurons per first MP, 181.82 per further or input MP | 1364 neurons, $0.015 |
| `@cf/black-forest-labs/flux-2-dev` | `image.generate`, `image.edit` (up to 4) | 37.50 neurons per output tile per step, 18.75 per input tile per step | 150 neurons a step, about $0.0016 a step |

- **Free plan.** The Workers Free plan has 10,000 neurons a day (reset 00:00 UTC). None of these models is on Cloudflare's list of
  models that need Workers Paid (the pricing page and the July 2026 changelog name only text models), so they run on it: about 170
  pictures a day of 1 MP with schnell, 95 with klein 4B, 7 with klein 9B and 2 with dev at 25 steps. When the allocation is used up
  the answer says so and the error's hint names the reset time.
- **The flow.** One synchronous call: `POST <address>/client/v4/accounts/<account_id>/ai/run/<model id>`. schnell takes JSON
  (`prompt`, `steps`, `seed`); the FLUX.2 models take `multipart/form-data` even for a prompt (`prompt`, `width` and `height` clamped
  to 256..1920, `guidance`, `seed`, `steps` for dev only, and each reference picture as `input_image_0`..`input_image_3`; the
  documentation asks for pictures under 512x512, so each is scaled down with libvips to fit 511x511, keeping its aspect and never
  enlarging, and sent as a PNG; more pictures than the model takes are refused with `input_invalid`). The answer is Cloudflare's envelope
  `{"result": {"image": "<base64>"}, "success": true, "errors": [], "messages": []}`; a bare `{"image": ...}` and a raw `image/*` body
  are read too. The picture is saved from the base64 at once: nothing is downloaded. Cancel aborts the request and answers
  `cancelled` at once. The engine id is `cloudflare@<host>`; the `engine` used is `cloudflare@<model id>`.
- **What still leaves the machine.** The prompt, the fields and the reference pictures go to Cloudflare. The FLUX.2 models are
  partner models served with zero data retention according to their model pages. The token is in no message, status, result, run
  record or log (error texts are scrubbed of it) and is never sent over plain `http://` to a host that is not loopback.
- **Errors**, in the codes of [13.3](#133-errors), each with a hint:

| The service | Code | Said |
| --- | --- | --- |
| no token, no account id | `provider_unreachable` | what is missing; the hint names Settings > Providers |
| 401, 403 | `provider_unreachable` | refused the token, with the service's words; the hint names the Workers AI permission and the account id |
| 404, code 5007 | `model_missing` | no such model |
| other 404 | `provider_unreachable` | does not know the account or model; the hint names the account id |
| 413 | `input_invalid` | too large; use smaller pictures |
| 429, or a message about neurons | `node_failed` | rate limited, or the daily allocation used up (resets 00:00 UTC) |
| a message about NSFW, flagged or policy | `node_failed` | content policy |
| 502, 503, 504 | `provider_unreachable` | not available right now |
| other 5xx | `node_failed` | the service's message; "out of memory" is `out_of_memory` |
| other 4xx | `node_failed` | the service's own message (`errors[].message`) |
| no answer in `timeout_s`, a refused connection | `provider_unreachable` | what happened; the hint names `timeout_s` |

- **Decisions** (the builder's, each with its confidence; overturn any): FLUX.1 [schnell], FLUX.2 [klein] 4B and 9B and FLUX.2 [dev]
  are the table, the others (Leonardo, Stable Diffusion models, some being deprecated on 2026-11-20) are left to `image_models`
  (medium-high: their request fields were not read); reference pictures are scaled to fit 511x511 (medium-high: the limit is in the FLUX.2 [dev] changelog, not in every model page); a seed is reduced modulo 2^31-1 (medium: no range is documented) and the one sent is what the run record holds; no retry of a call (high: neurons are billed per run).
- **Not checked against the real service**: no Cloudflare token was at hand. The tests use a fake API on the loopback whose answers
  follow Cloudflare's documentation; the exact REST envelope for the image models, the error codes (5007, the neuron-limit message),
  and whether the partner FLUX.2 models draw on the free allocation are unconfirmed (schnell's `seed` is settled: refused); a real run with a token is pending.

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
- **Where a node runs when its model is not here (#395).** A node that takes its model files in the job (the stage nodes) and whose
  files are not in this machine's model folders (checked as 8.9 does, reading no file) asks `selectProvider` with
  `Requirement::modelsElsewhere`: only an instance that does not share this machine's filesystem, a paired helper, can serve it, and
  the first one that answers does (the user's order among them). A model that is here keeps the ordinary choice, so a paired helper
  never takes over a run that can be local. When no paired helper answers, the node fails with `provider_unreachable`, "The model
  files are not on this machine, and the paired host gx10 is not answering", with the host's reason, instead of the `model_missing` of
  a machine that never had the files; with no paired helper configured at all nothing changes. A pin still wins. `describe` on a
  paired host gives up after 10 s (a black-holed address is "Operation timed out" in 10 s, a name that does not resolve at once),
  not after the 120 s transfer timeout. `run.json` records `"remote": true` on the node's provider entry; `fugue app run` says
  "Ran on gx10." on stderr and has `"ran_on": ["gx10"]` in its JSON; `fugue app info` says "Runs on gx10" and has `"runs_on"` (and `"runs_on_nodes"`, node to host) when
  models are missing here and the paired helpers that answered take every node that needs them; a helper that did not answer is
  `"silent_host"` ("gx10 is not answering"), never the host (below, "One verdict"). The app is runnable either way: `app run` does not
  stop at the missing models, and the host answers `model_missing` itself when it does not have them either. Not the same as the omarchy
  hosts' models
  (8.7), which are chosen by the model's name.
- **One verdict for the CLI and the window (#491, #581).**
  - *Paired placement* (`engine/paired_placement.*`, `decidePairedPlacement`) reads the document the way the scheduler decides, per node,
    and returns three outcomes. The nodes are the compiled graph's (`compile`): a node inside a group or a group instance is scanned under
    its path id (`g7/inner3`) and takes the instance's pin, a muted node or one no output needs is not, and a document that does not
    compile places nothing. Each node's requirement is the kernel's, read with the inputs the node holds (literals, handles, the spec's
    defaults; what a link brings is not known before the run), so its capability and `properties` are the scheduler's. The host is chosen
    by `selectProvider` itself on what the hosts answered (`modelsElsewhere` unless the node names its model), so only a host that does not
    share this machine's filesystem takes a node, and a host whose model lacks the properties does not; `silentPairedHelpers` is the
    scheduler's own list of the hosts that do not answer. It applies only when only models are missing (no pack; the problems no more than `model_missing` or
    `download_failed`) and a paired helper is enabled; otherwise nothing is placed. A node needs the model when one of its own Resource
    inputs holds the missing handle, or is linked to a node that holds it through a Resource output (a picture decoded from it does not
    carry it); only a provider node counts.
  - **Blocked**: `model_missing` per node, naming it, when it is pinned (its own pin, else the workflow's) to an instance that runs on this
    machine ("Node enc is pinned to desk, which runs on this machine, and the model m.safetensors it needs is not here") or disabled, or when
    every paired helper that could take it (the one it is pinned to, else all) does not offer the node's capability ("... the paired host
    gx10 does not offer text.encode"; what the host said live; for one that does not answer, what it last said, the scheduler's `neverOffered`). The hint says to get the
    model here (`prepare`, Get ready) or to unpin the node.
  - **RunsOn**: per node, the first helper in the user's order that answered live, offers the node's capability and respects the pin (the
    scheduler's own choice). The line lists the distinct hosts. A host that answered without the capability is not the host.
  - **Silent**: the only helper(s) that could take a node did not answer. A silent host is never named as the host. The window says
    "<host> is not answering" and keeps the model rows; `app info` leaves out `runs_on` and has `silent_host`; `app run` goes ahead (a cached
    re-run can still work; otherwise it fails with `provider_unreachable`).
  - *Order:* the pins decide first and block without asking anyone, so an app that pins already block never waits for the probe. Every
    helper that passes the pins is then asked (`describe`, 10 s at most, in parallel; each answer refreshes what is kept in
    `<state>/paired_helpers.json`, by instance name and address). A helper that answered decides by its live answer. For one that did not,
    what it last said only refuses (the scheduler's `neverOffered`): a node whose every candidate is live-without the capability or
    silent and remembered without it is Blocked; a node with a silent candidate that has no memory, or remembered offering it, is Silent.
    What was kept never places a node and never overrules a live answer, so a host that gained a capability is not refused for good.
  - *Callers:* `fugue app run`/`info` wait for the probe (once per command: the answer is kept for the second look `--input` makes); the
    App view makes the verdict again, asynchronously, whenever the plan or the document changes, reusing an answer of the last 30 s
    (`keptPairedProbe`). The probe is the seam (`PairedProbe`): the live one is `probePairedHelpers`, a test gives a fake
    (`tests/nodes/test_paired_placement.cpp`). `fugue app info` has the blocked lines as `host_problems`. With no paired helper,
    nothing changes. The Needs panel uses this verdict (`planUnder`), never the "Runs on" line, which may be naming a service off this
    machine (#404).
  - *The host down, and the node could never go there:* when the host does not answer but it never offered the node's capability (what it
    last said), selection fails at once with `model_missing` (the files are not here and the host did not offer it), not
    `provider_unreachable` and no local job is started to fail late; a host never heard from is still named.
  - *The host down, and the result already made there:* a result made on the host is in the cache under its engine id (`sdcpp@gx10`) and
    model, which only its `describe` tells. With the host not answering, the scheduler asks for a stand-in selection from what it last said
    (engine id, whole model handles), only for a node that wants no property and one named model or a host that offered one model
    (memory never picks a model the live host might not) and looks the key up in the cache: a hit is served (`node.cached`, nothing runs, `run.json` records no
    "remote"); a miss is the `provider_unreachable` of the host being gone. Nothing ever runs from the remembered description.
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
- A mask (#175, [14.4](#144-masking-and-inpainting-167)): a model's `image.generate` offer has the
  property `inputs: ["mask"]` when its server's capabilities say it takes one, that is
  `features_by_mode.img_gen.mask_image` is true (else the deprecated top-level `features.mask_image`
  when there is no mode-aware object; a features list that names it counts too). Read at stable-diffusion.cpp
  532d2c15's api.md: `mask_image` is one of `features_by_mode.img_gen`'s flags and on by default
  (`mask_image` is a default-enabled recipe feature), a one-channel image of the start image's size. The provider
  sends the job input `mask` as `mask_image` (base64 of the 8-bit gray PNG, white redrawn) beside
  `init_image` and `strength`, and only then: a job that has a mask for a model without the property is
  `mask_unsupported` before anything is posted, never sent without it (the server would redraw the whole
  picture). Not run against a live server (no sd-server here); the tests use a fake one.
- Video (#28): each `kind: video` row is a `fugue/video-model` of `video.generate`, probed with
  `GET /sdcpp/v1/capabilities` (defaults from `defaults_by_mode.vid_gen`: size, steps, guidance, `fps`).
  A job is `POST /sdcpp/v1/vid_gen` and the same job API as images; it asks for WebM with the VAE
  tiled, sends `init_image` for a first frame and every listed LoRA, and the finished job's whole
  container (base64) is written to `video.webm` at once (8.6). No previews are asked for.
- Modes (image modes): a row's `kind` is what its recipe is for, not all a server can do. When the
  capabilities of a ready `image` or `video` row list `supported_modes`, the model is also offered for
  the other mode the server serves: a video row whose server lists `img_gen` is a model of
  `image.generate` (and of `image.edit` when `features_by_mode.img_gen.ref_images` is true; the
  row's own `edit` flag decides for an image row), with the `img_gen` defaults; an image row that
  lists `vid_gen` is a model of `video.generate`. An image job goes to `img_gen` with `init_image`
  and `strength` (image to image) or `ref_images` (edit), whatever the row's kind. Observed on
  2026-11-21: MiniMax H3 (`minimax-h3-sdcpp-cuda`, kind `video`, `edit` false, input `["text"]`)
  reports `supported_modes: ["vid_gen"]` only, so it is offered for Generate Video alone; its image
  variant (P-H3-Image) is not an Omarchy recipe (it runs on ComfyUI).
- Music (#28): each `kind: music` row is a `fugue/music-model` of `audio.generate`, probed with
  `GET /health` (ace-server has no capabilities call; 10 to 600 seconds is its documented range), then `GET /props`, whose `adapters` (the files of the server's `--adapters` folder, as names or `{name}` objects) are offered as models of the capability `audio.adapter`, each a `fugue/music-adapter` (a server that has no `/props`, or no adapters, offers none; the music model stays the only model of `audio.generate`). A job that names an adapter the server does not list is `model_missing` before the song is written; one it lists goes to `/synth` as `adapter` and `adapter_scale` (the DiT is loaded with the adapter merged in, so the first song with a new adapter makes the server reload it) and none sends neither. A song is
  two ace-server jobs in turn: `POST /lm` writes it out (lyrics, tempo, key, audio codes), `POST /synth`
  renders what `/lm` wrote, as an mp3 (128 kb/s, the server's own), an mp3 at 320 kb/s (`mp3_bitrate`) or a 24-bit WAV (`output_format` `wav24`) by the node's Quality (#258; the file is saved as `audio.mp3` or `audio.wav`, and Save Audio re-encodes it as it does any other audio); `GET /job?id=N` is `running`, `done`, `failed` or `cancelled`,
  `GET /job?id=N&result=1` the result (a JSON array, then `multipart/mixed` whose first part is the audio),
  `POST /job?id=N&cancel=1` cancels. A job with files (a song to remix or repaint, a style reference, #259) posts /synth as a multipart form (`request` JSON, `audio`, `ref_audio`, each part with a file name); one without stays JSON. The style reference is read by the renderer only: the LM that writes a new song is not told of it. There is no queue state, step counter or preview, so progress is
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
| `id` | Letters, digits and `. - _`. The folder is named after it. A module cannot use the id of a provider that comes with Fugue (`omarchy`, `http`, `fugue-helper`). |
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
- A resource type's folder names include those other tools use, so their installs are read as the layout they are
  (ComfyUI's `a1111:` section maps the same): `fugue/model` also `Stable-diffusion`, `Stable-Diffusion` and `StableDiffusion`;
  `fugue/lora` `Lora` and `LyCORIS`; `fugue/vae` `VAE`; `fugue/text-encoder` `TextEncoders`; `fugue/diffusion-model`
  `DiffusionModels`; `fugue/upscale-model` `RealESRGAN`. They come after the type's own names, so Fugue's own folders are
  unchanged (#619). An Automatic1111, Forge, SwarmUI or Stability Matrix `models/` folder is therefore one layout root. These other
  tools' names make a layout (`ModelLibrary::layoutFromFolders`, used by `kindOf` and by the finder) only when two different kinds
  have a folder (`Lora/` and `LyCORIS/` are one kind), so an existing folder of LoRAs with `LyCORIS/` and `SDXL/` inside, or of
  checkpoints with `VAE/`, `SD15/` and `SDXL/`, stays a folder of one kind found by its files; ComfyUI's names decide as they always
  did (one is enough).
- **Finding the model folders a person already has** (#619; `engine/model_folder_finder.h`). Folder names and file sizes only: no
  model file is opened, read or hashed, and nothing is written. A folder is a candidate when at least two of the resource types'
  folder names are in it (the registry's, so a pack's count) and, for a tool that sorts models its own way, by what stands beside
  it (InvokeAI: `invokeai.yaml` beside `models/`). It is told by its neighbours: ComfyUI (`custom_nodes/` beside `models/`),
  ComfyUI portable (a `python_embeded/` above), ComfyUI Desktop (under `ComfyUI-Shared` or `ComfyUI-Installs`), Forge
  (`modules_forge/`), Automatic1111 (`webui.py`), SwarmUI (`launch-linux.sh` or `launch-windows.bat`), Stability Matrix
  (`Packages/` beside it, or a parent named `Data`), InvokeAI, else "Model folder" (one with no model in it is a coincidence of
  names and is left out). The places are a list the person ticks: the usual install folders that exist (`~/ComfyUI/models`,
  `~/ComfyUI-Shared`, `~/ComfyUI-Installs/*/models`, `~/StabilityMatrix/Data/Models`, `~/StabilityMatrix/Models`,
  `~/stable-diffusion-webui/models`, `~/stable-diffusion-webui-forge/models`, `~/SwarmUI/Models`, `~/invokeai/models`), `~` (4
  levels deep), and each drive under `/mnt`, `/media/$USER` and `/run/media/$USER` (5 levels deep). **A place that is not ticked
  is never touched**: no stat and no listing, whatever the others hold. The walk skips hidden folders, `node_modules`,
  `site-packages`, `venv`, `__pycache__`, the Trash, Windows' system folders, Fugue's own data folder, any folder of more than
  5,000 entries, and anything below a candidate. Caps: 3 s for finding the folders in all; counting one folder (the model files
  under its type folders, by stat size) 2 s or 20,000 entries, after which the count is a lower bound (`countCapped`, shown "N+
  models" and "Counting stopped: slow drive"). Network file systems (`nfs`, `nfs4`, `cifs`, `smb3`, `fuse.sshfs`, `fuse.rclone`,
  `davfs`, `9p`, read from `/proc/self/mountinfo`) are never walked; an exact install folder on one is listed and not counted. A
  folder reached by two paths (a link) is one candidate with `alsoReached`; one whose every type folder resolves inside another
  (Stability Matrix linked into ComfyUI) is that one's alias, and its files are counted once. `fugue model find-folders [--json]`
  prints the candidates and writes nothing.
  **Links, automounts and workers.** A link is resolved by reading its text (`readlink`), and every step is checked against the
  places left off before anything is stat'd or listed, so a link from a ticked place into an unticked one, at the top or nested,
  or from one install folder to another that was left off, is never followed; each type folder and each folder counted is
  resolved the same way, and one that leads onto a network drive is not entered. An autofs mount point (and anything under it)
  counts as a network drive and is not even listed as a place: looking inside it would mount the share. An autofs point with a real file system mounted on it (systemd's `x-systemd.automount` of an NTFS drive) is not one: it is searched like any drive, and only a network file system on top blocks it. The places left off are kept as written and as resolved, so a home behind a link (`/home` -> `/var/home`) keeps them. The registry's folder
  names and extensions are copied on the GUI thread (`FinderTypes`) and the worker reads only that copy. The worker is never
  waited for: it holds its own cancel flag, answers to the window through a weak pointer, and a stalled stat leaves it behind
  without holding the window or the quit. Time and cancellation are checked at every entry (listing and stat), counting all
  folders has a total cap, a count that stops early is `countCapped`, and a search a cap ended says so (`capped`; the sheet offers
  Search again, with big places switched off, or Add a folder). A folder holding `model_index.json` (a diffusers repo) is skipped.
- **The first-run setup** (#619; `settings/model_setup.h`, `ModelSetupSheet.qml`). It opens by itself, once, as the main window
  shows, when the session is the Default or a root with no `project.json`, the layered `model_paths` is empty, Fugue's own models
  folder holds no model file and `models_setup` is unset; never in a project. Two screens: the **consent screen** says why and
  that only folder names and file sizes are read, lists every place with a switch each (all on), and offers **Search**, **Add a
  folder…** (a manual pick that goes to the results with no search) and **Skip**; only Search searches. The **results** list what
  was found, a switch each (on for a folder with models, off for InvokeAI and for an install with none; "In use" folders first, on),
  **New downloads** (the picked writable folder in ComfyUI's layout with the most model bytes, never a network or removable drive unless the person picks it, else Fugue's own; a rerun starts from the `download_folder` the person has; the choice
  lists Fugue's own and each such folder with its free space and keeps a short disk's choice, saying "Only %1 free there." under
  20 GB) and **Use N folders**. Nothing is written before the yes; the yes is one write of this session's own settings.json:
  `model_paths` and `model_folder_kinds` (`layout`, pinned so a guess never flips), `download_folder` and `models_setup: "done"`
  (`SettingsModel::applyModelSetup`). InvokeAI, when ticked, adds the folders under its `models/` (2 levels) whose files' headers
  agree on one kind, as single-kind roots (told by a job of its own that starts when the row is ticked; searching opens no model file,
  and a folder in use is counted by its folder names, only where the places allow); the others are left out and the notice counts them. A folder that is gone, or already a
  root, is not added. Skip, Escape or closing writes `models_setup: "skipped"` and nothing else; Done with nothing found writes
  `done`. **Settings > Folders > Extra model folders > Find model folders…** and the Get ready "Change model folder…" menu run it
  again (the consent screen first); a rerun writes no `models_setup`, and in a project writes only the project's own file, with the
  Default's roots listed "Inherited from Default", without a switch, never a download target. After a yes the templates and the
  banner read the model folders again: a template whose model file name is now on disk is ready.
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
  (`src/catalog/*.json`, compiled in; then the catalogs of installed packs, `<pack folder>/catalog/*.json` under the pack
  search paths (#178), so a pack that declares a resource type brings the downloads for it with their licences; then the
  person's own, and updates, files in `~/.local/share/fugue/catalog/*.json`; each read after the one before, an entry
  of the same `id` replacing it).
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
- **Where downloads go, and the one yes (#379).** The "Get ready" sheet ([11.10](#1110-templates)) and `fugue template prepare` /
  `fugue workflow prepare` list the missing models with the packs and environments, each with file, licence (when the catalog has it),
  size, source host and destination folder, then ask once. The sheet has a **Models go into** row with the current
  `download_folder` (Fugue's own folder when unset), the free space at that folder against the download size, and **Change…**, which
  offers the model folders (`model_paths` and Fugue's own) or **Pick a folder…**: a picked folder is added to `model_paths` as a
  ComfyUI-layout root (Fugue downloads only into folders it searches) and becomes the new `download_folder`. Files still go in the
  type's subfolder. A destination with too little free space says so before the yes and offers another folder. Settings > Folders has
  the same setting, "Download models into". Models still download one after another; the first failure stops the rest.
- **Speed measurements (#370).** `HelperProvider` times the sampling steps of a job at guidance 1 with no frames from its progress
  lines (the first step's time and the model's load are left out; at least three steps between the first line and the last) and
  records the seconds per step per megapixel for the model file (name and size) on the device `<backend>:<card name>`: the last five, the
  median used, in `model_speed.json` of the state folder (shared by every process; a record reads the file again before it writes).
  Nothing benchmarks by itself: the numbers come from real runs. Measured on the owner's RTX 3090 (workbench klein-da notes,
  2026-11-23, 1024x1024, 6 steps): Klein 9B INT8 ConvRot about 3.0 s sampling against about 5.6 s for Q8_0 GGUF, warm.
- **No Models page in Settings** (removed 2026-11-24, owner): it only listed the files and their Recommended mark, which the
  pickers and template cards already show, and a list of files is not a setting. A Model library panel beside the canvas is its own issue.

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
- On CUDA the helper adds `"sage_attn": true` to a context that does not set it: the diffusion model
  runs SageAttention where it beats flash attention (no mask, a head of 64 or 128, no K/V range
  scaling: FLUX.2 yes, Z-Image no), about a third less attention time; the pictures are 33-40 dB PSNR
  from flash attention's and look the same (#382). `"sage_attn": false` keeps flash attention.
- stable-diffusion.cpp loads a model as one context of all its files and checks at load that
  every component is there, so every stage job names all of them, even the ones it does not run:
  one `fugue/model` gives the same context to every stage, and the helper keeps that one loaded
  for the whole graph.
- Out of memory is a typed error on the node (`out_of_memory`). A helper crash kills only the helper. When another process holds the GPU's memory (a second helper: the two clips of two roots, an
  idle helper of another window), stable-diffusion.cpp does not return an error for a failed device allocation: it asserts
  and aborts the helper (seen on the RTX 3090 with two helpers, #130: `cudaMalloc failed: out of memory`, then
  `GGML_ASSERT(!chunk_hidden_states.empty())`, SIGABRT, in the text encode; a decode allocates its VAE buffer on top of the
  resident weights and fails the same way). A helper that dies with such a line at the end of its log is `out_of_memory`
  like one that said so (the library's wording: `out of memory`, `ErrorOutOfDeviceMemory`), so the scheduler frees a
  resident model and tries again and, with nothing left to free, names the processes holding the memory (above); the
  next job starts a new helper.
- **Step previews (#297).** A Sampler on the local helper shows the picture changing while it samples. The
  `latent.sample` submit carries `preview` (a key of the message, not a param, so it is not in the cache key:
  `proj`, the library's cheap latent-to-RGB projection; `tae`, a tiny autoencoder, which needs `taesd_path` in the
  context and else falls back to `proj`; or `vae`, the full decoder) from settings.json's `step_preview` (`off`: the key
  is left out, `fast`: `proj`, the default; `tae`; `vae`). The helper registers the library's preview callback for that
  job only, every Nth step so that about 8 pictures come, writes each to `<output_dir>/preview.png` (beside it, then
  renamed over it) and says so in a status line, `"preview": PATH`. `HelperProvider` reads the file at once into
  `JobStatus.preview` and the scheduler shows it as `node.preview` like any provider's (an image faster than the
  canvas shows is dropped, the newest kept, 9.4). A video model has no step previews (a clip's frames are not worth
  their cost), nor has a helper over HTTP (nothing would fetch the file). Both ends ignore keys they do not know, so
  an older helper or Fugue works as before without pictures. A group's card shows the frame of a node inside it
  as its own picture while that runs (the card is the group's result slot), until the group's result replaces
  it; a new run starts from the last result again. The frame shows whatever the picture mode is: where the
  card has a picture slot the frame is in it; where the mode hides the picture it is an overlay on the canvas, just
  under the card, as wide as it, above the other cards, that fades out in 200 ms when the node stops running (a
  live frame never changes layout: the card's rect and the wires stay put). A node whose own Hide picture is set
  shows no frame; preview thumbnails are made for hidden pictures too. A canvas action's run (the task bar's
  Generate) has no card: its newest frame is drawn on the Canvas view inside the selection instead
  (`docs/canvas-actions-design.md`, Progress).
  With the task bar's Live on, a run to the node before Canvas Write (`RunRequest.target`) draws the same frame as a preview and lands no
  layer (`docs/canvas-actions-design.md` 16).
- The component-level helper is based on stable-diffusion.cpp with separate encode, sample and
  decode steps, C++ only. The M0 spike showed the split works
  ([spikes/02](spikes/02-sdcpp-split.md)).
- Fugue carries the split in its own fork of stable-diffusion.cpp (upstream plus Fugue commits), pinned by commit; it does
  not depend on upstream adopting it. Upstream generates by rendering a Diffusion Graph Language
  program; the fork cuts that program at named values (`src/api/fugue_stage.cpp`, helper/README.md
  "The seam"). To keep that cheap: the fork's changes stay small: the cut, one hook per generation call
  and the stage file format, no change to sampling or model code, except opt-in hunks that leave
  upstream behaviour byte-identical when off, each listed in helper/README.md with its upstream PR and checked at every
  pin move (today: `fugue.soft_mask`, #179, and `fugue.init_video`, #130); the pin moves only on
  purpose; and every move of the pin is checked where the fork can break, not model by model
  (stable-diffusion.cpp tests its families): staged runs of Z-Image (LLM conditioning), of SDXL
  with a negative prompt (CLIP conditioning, two sets) and of image-to-image from a latent, the
  Z-Image one giving the same pixels as a single run of the same build. IP-Adapter, PhotoMaker and
  PuLID are DGL-only upstream since 3b12781: the generation API refuses their options and no node sends them.
- The helper's source is `helper/`: the pin (stable-diffusion.cpp, its ggml and CUTLASS commits,
  with their hashes), Fugue's fork of it (`bmaltais/stable-diffusion.cpp`, branch `fugue`), and `fugue-helper`. `bin/build-helper cuda`
  (or `vulkan`, `cpu`) builds it; `helper/README.md` says how, and how to move the pin. The fork adds image options `fugue.*` that name the files a
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
  the conditioning sets (a file from an older helper that holds PhotoMaker, PuLID or IP-Adapter
  state is refused). A file
  is read only by a model of the version it was written for. Every call VAE-encodes the request's
  images (init, reference, control), so `sample` takes the reference images as `encode` does, and
  a stage with images loads the VAE beside its own component. An inpaint model cannot start from
  a latent alone (its extra input is made from the init image).
- **Masks (#167, #173):** `sample` and `generate` take a one-channel `mask` (`stbi_load` with one
  channel, checked against the request's width and height before any model loads), and an image
  model's `sample` with a mask takes the picture too (`image`): an inpaint model (the version names
  `SD 1.x Inpaint`, `SD 2.x Inpaint`, `SDXL Inpaint`, `Flux Fill`, `Flex.2`, from
  `sd_get_model_version_name`) starts from the picture as `init_image` and ignores a start latent,
  another image model from its start latent (`init_latent`) with the mask applied at every step, and
  ignores the picture. A mask on a video model, an inpaint model without its picture and a mask with
  neither picture nor latent are `input_invalid`. An inpaint model's masked picture is a predictor input
  (`concat`) that the sample's program builds from the call's own picture and mask, as a single run
  does, because the encode had neither; no sampling or model code changes. The design and what the library does with a mask are in
  [14.4](#144-masking-and-inpainting-167).
- **Video (#105)** is the same stages on the same capabilities, no parallel path. A context that
  makes video (`sd_ctx_supports_video_generation`: Wan 2.1 and 2.2, as the pin supports them) is run
  with the library's video options and `sd_generate_video`; the fork gives the `fugue.*` options to
  video too (`cond_in`/`_out`, `latent_in`/`_out`, `stop_after` `cond` or `sample`; a video has no
  `init` stage), shares the file code with the image path (`src/api/fugue_stage.cpp`), and
  cuts the video program at the same three seams: after the text encode, after the sampling
  (both Wan 2.2 passes, high noise then low), and before the VAE decode. The conditioning file is
  the same (`c_vector` carries Wan 2.1 I2V's CLIP-vision output; the sample rebuilds the
  image-to-video `concat` from its own picture); the latent file's descriptor gains `frames` and `fps`. `encode` and `sample` take the generation
  options of the video (`video_frames`, `fps`, `width`, `height`, `sample.*`; a Wan 2.2 14B
  context also `high_noise_diffusion_model_path`, its UMT5 as `t5xxl_path`); `decode` writes
  `video.y4m` instead of `image.png`, a YUV4MPEG2 file (4:2:0, one `FRAME` per frame) that every
  ffmpeg reads and the helper needs no codec for; over HTTP its format is `y4m`. Fugue's VAE Decode
  Video turns it into the engine's lossless intermediate (FFV1 in Matroska, [2.4](#24-media-handles)).
  An image-to-video model (Wan 2.2 I2V; Wan 2.2 TI2V from its first frame) is staged with a start
  picture, the job input `image` of `text.encode` and `latent.sample` (the library's `init_image`;
  an image model refuses it with `input_invalid`): the sample turns it into the model's `concat`
  at the clip's width, height and length, so those are the options of both stages, and the
  encode and the sample send the same picture so the stages make one generation (TI2V starts its
  sampling from the frame, which is not in the conditioning). Wan 2.1 I2V's CLIP-vision output is made in the same
  encode.
  Video to video (#130) is the same stages again: `vae_encode` of a video model takes a clip
  (`video`, a YUV4MPEG2 file of the layout `decode` writes) and writes its latent (the opt-in
  `fugue.init_video`, `read_y4m` in the fork); the clip's size, rate and length are the
  file's. A `sample` takes that latent as `init_latent` with `strength`: the video program's `base` is
  replaced by it, and the first `(1 - strength)` of the schedule's steps are dropped, the high-noise
  pass of a Wan 2.2 14B first (only the low-noise model runs when `strength` leaves its steps
  alone), as image to image trims its sigmas; the sampler noises the start latent to the first
  sigma as it does for an image. One `generate` of the same clip (`init_video`) gives the same
  frames as the staged run. Not staged yet: S2V and the audio models, LTX/MiniMax (audio and hires
  latent upscale in the same call) and a latent upscale; the helper says so (`input_invalid`).
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
  messages, whether describe is asked of the process). The registry lists `omarchy`, `http` and
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
- pinned providers exist (the configured instances, as `providers.json` lists them);
- a node of a pack the user has not allowed ([6.7](#67-trust)) is refused.

One step does all of it, with the provider setup and the trust a run uses (`compileForRun`): the
providers are registered first, a library or pack group named only by its type id is embedded, and
the provider problems come before the compiler's. `fugue compile`, `validate` and `run`, the
window's runs and File > Export the compiled graph all go through it, so the exported graph is
the one a run would run, and an untrusted pack's node is refused by the export as by a run.

It is also where a canvas's `head` is named ([14.5](#145-the-canvas-revision-store-206-208)): after
the compile, each Canvas Region's `revision` (`head` when unset) becomes the revision's full hash,
once, so the hash is a literal input of the compiled graph, in its cache key and in `compiled.json`.
The setup's `resolveCanvas` callback does the lookup (the session's canvas store in the app, a fake
in a test); a canvas or revision that does not exist is a compile error on the node.

`fugue validate` is this step with nothing executed. It also reports warnings: nodes that are
not part of any output, missing models, untrusted packs, a frozen result that is gone.

It also asks each kernel what it can tell from the node's written values and selections alone
(`NodeKernel::check` and `ProviderKernel::check`, on the scheduler's thread, after every committed edit and
before a provider is chosen), and reports the answer on the node (a check error, or a warning where the kernel says one or the model is missing, see below), with the code, input and message the run
would give. A check is cheap by contract: no model loaded or hashed, no network, no provider, at most a few KB read from
a file, and a run's errors that need what an upstream node makes (a picture's size, a list's length) or a provider are
not among them. An input that is wired or not run yet is absent from a check's inputs, and a rule that needs it is not
judged. Each node's row in [14](#14-built-in-nodes) says what its check reports.

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
- **Demand** ([3.4](#34-what-runs)): a node waits until it is demanded and its inputs that are not lazy are in. When they are, its kernel names the lazy inputs it needs (`NodeKernel::lazyInputs`, on the scheduler thread), the nodes feeding those are demanded, and the node waits for them. A node a consumer let go of is skipped as `not_chosen` once no other node can still demand it, and the same goes upstream. A node let go because its reader could not ask (the reader failed upstream, or a required input of it came back empty, so nothing is asked of its lazy inputs) is `not_chosen` too: nothing upstream of it failed, and only the reader and what reads it are `upstream_failed` or `upstream_empty`. A node that declines a lazy input stops counting as a reader of what feeds it at once, so the decoded images of a node a Switch declined but another node still reads are let go when that other node finishes, not when the Switch does. A run's last pass skips whatever is still waiting with nothing left to ask for it (`upstream_failed` when a node failed before it could ask, else `not_chosen`). Under validate ([10.3](#103-per-node-state)) every lazy input is taken as needed: both sides show as they would run, never skipped. When every input of the node that is not lazy is known (cached or literal), the hook is still asked and the inputs it declines stay out of the key, as in a run, so the node and what reads it show `cached` after a run; while one is unknown (not run yet, a random seed) nothing is declined and the node is `stale`.
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
| `workflow.json` | The workflow document the run started from (for an App view run, with the `app_take` record of its form, [12.17](#1217-the-app-view-385-epic-397)). |
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
  the advisory lock `~/.local/state/fugue/engine.lock` (and, across project roots and the Default session, the cross-root GPU lock of [1.4.2](#142-projects)). "Run first" writes an entry that sorts
  ahead of the waiting ones; removing a queued run deletes its entry. An entry whose process is
  gone is discarded, and its run is ended as `failed`.
- `fugue run --wait` that has been queued behind another run for about a second says so once: on stderr
  `Waiting for another Fugue run to finish (pid N)…`, and with `--json` a `{"event":"waiting","run":…,"pid":N}` line on
  stdout instead (the line is not written to the run's `events.jsonl`). `N` is the active run's process, else the holder of the engine or
  GPU lock (read from `/proc/locks`); without one the message ends after `finish…`, and the event has no `pid`.
  `runs events --follow` does not say it. `fugue validate` runs nothing and takes neither lock, so it never waits for one.
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

Loops, batches and several runs in parallel. The engine must not rule out dynamic graph expansion later. Conditionals are in: lazy inputs ([3.2](#32-inputs)) and the If/Else Switch ([14.1](#141-m1-the-first-twelve)), which run only the side that is chosen. Node expansion (a node that returns a sub-graph, ComfyUI's loops) is not.

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
- a lazy input the node did not ask for is not in the key, nor is the node that feeds it: a Switch's key is its condition and the chosen value ([3.2](#32-inputs));
- for provider nodes, the engine id ([8.11](#811-the-provider-interface)) and the identity of
  the model the provider resolved;
- for a node with `deterministic: false`, the run id;
- for a Canvas Region, its `canvas` is the id of the Active canvas when it was empty (#246) and its `revision` the full hash that `head` was at when the workflow was compiled ([14.5](#145-the-canvas-revision-store-206-208)), so it is in the key like any literal;
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
`{"state": "skipped", "reason": "muted"}`. A node a Switch did not choose is `skipped` with the reason `not_chosen` in a run (never under validate, which does not know the condition and shows both sides as `stale` or `cached`); it never ran, so it has no key. Without `--json` it prints one line per node. A size view shows what the cache holds per
workflow and per node.

**A node's own check errors (#600).** A kernel's `check()` (what a run of the node's own values would fail on, told with no run, no provider call and no model: Expression's formula that does not parse, a Text Template or Dynamic Prompt template that does not) shows on the canvas as the node's error, the same chip a run's error makes, without a run: the canvas check that follows an edit (300 ms after the last one: a widget committed with Enter or on leaving the field, a wire added or removed, an undo) asks, and the node goes back to normal at the next check that no longer reports it (the value fixed). It is also listed with the run's errors (`errors`, with the `input` it names) and counted by the run pill, and a group holding the node shows it with the inner path id, as a run's does. Only what `check()` itself reports counts: an input fed by a link, an `auto` value or a node not run is absent from what `check()` sees, so it says nothing of it. A node no output needs (`not_part_of_output`) is checked too, from its literal values, so a formula typed into a node not yet wired is reported at once; a run never reaches it. Its file and folder literals are read as a run reads them (trimmed, `~` expanded, a relative file the saved workflow's folder's, one function for both), so a pasted picture kept as `inputs/x.png` is found; a relative file in a workflow not saved has no folder, so that input is left out and the run's own error stands. Where a finding goes is one rule for wired and unwired nodes: a finding with severity error (the default) is a check error shown on the node; a `model_missing`, and any finding the kernel itself marks `Severity::Warning` (one a run may never meet), is a warning, the node's yellow badge, as a missing model always was. Only what `check()` reports is listed with the run's errors and counted by the pill (the check carries those findings apart from the dry run's own errors, such as `permission_denied` or a relative file in an unsaved workflow, which keep their earlier display). A node a run failed on keeps the run's error: a run's start clears a check's error and the saved check findings listed with the errors (the next check lists what is still wrong), and a run's error is never taken for a check's, so a later check does not erase it. Each wildcard name of a Dynamic Prompt is looked up once, and the strongest severity of its occurrences stands (`{a|__x__} __x__` is an error, not a warning). A header-only read of a LUT (the check) reserves no table. `fugue validate` still lists these as warnings (`GraphCheck::checkErrors` are written to its `warnings`), so its exit code follows the compile only. The input the error names (`formula`) is not marked on the node's row: a run's error is not either (decision 2026-11-26, medium).

### 10.4 Freeze

Freezing a node keeps its current result: it is kept, never evicted, and never invalidated by
upstream changes until unfrozen. The node stores the frozen key (`frozen` in [4.2](#42-graph))
and shows the frozen state; the cache holds a marker for that key, removed on unfreeze. The
cache view lists frozen results so one left by a deleted workflow can be released. If the frozen result is missing (the file moved to another machine)
validation warns and the node runs normally. A valid frozen result is served whatever the inputs are now: a lazy input of the node (a Switch side) that came back muted or empty does not skip it.

### 10.5 Eviction and garbage collection

- A size limit, default 10 GB (`cache_results_gb`, 0 for none), configurable. Over the limit, the least recently used entries
  are evicted, when a run ends and when the cache is cleaned ([10.6](#106-cache-management)). Frozen entries are never evicted.
- Clear all, per workflow or per node (`fugue cache clear`, and Settings > Cache, whose Clear all clears every kind, [10.6](#106-cache-management)).
- Garbage is collected after a grace period of 7 days: run folders whose run has ended, blobs no
  key record refers to, and key records whose blobs are gone. A cached result is otherwise kept
  until it is evicted for size or cleared; it does not expire with age.

### 10.6 Cache management

Fugue keeps three kinds of copy under `~/.cache/fugue`, and each can be measured, cleared and
kept within a limit (#141):

| Kind | Folder | Holds | If it is gone |
| --- | --- | --- | --- |
| `results` | `results/` | The result cache ([10.2](#102-store)). | The node runs again. |
| `thumbnails` | `thumbnails/` and `thumbs/` | The 512 px pictures nodes show ([12.7](#127-inspection)), one per image by content hash, and the media browser's 256 px ones ([12.14](#1214-side-panels)), one per file, size and time. The two folders are one kind: one size, one limit, one Clear. | Made again when the result is shown (a cached node shows its result at once on the next run) or the browser shows the file. |
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
  `cache_thumbnails_gb` (default 1; 0 for none, for both thumbnail folders together) and `runs_kept` (default 20). Results over the
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
  run; runs: the history), and the limits. One **Clear all…** beside the total "N in all" does
  what `--kind all` does (all three kinds in one go; frozen results, saved outputs and a run
  going or queued stay; while a run is going results and previews are left alone and the page
  says so, as `cache_busy`). It asks first with the same Clear and Cancel, saying in words what
  is lost (nodes run again, previews return on the next run, the run history) and the total
  size, and it is off while measuring or clearing and when nothing can be cleared. File menu > Clear image preview cache opens that
  question for previews.

---

## 11. Workflow files

### 11.1 Format and place

- One format: the `.fugue` document of [section 4](#4-the-workflow-document).
- Workflows are ordinary files wherever the user saves them. File dialogs go through the desktop
  portal, never `QFileDialog`: every picker is `FilePicker` (`src/menu/FilePicker.qml`), which asks
  `org.freedesktop.portal.FileChooser` (OpenFile, SaveFile; `src/menu/portal_file_dialog.*`) over
  D-Bus, with the name filters, the current folder and a suggested name, and takes the `file://`
  URIs of the Response (a cancel is Response 1). Qt's `FileDialog` is no stand-in for the portal:
  Omarchy's platform theme (qtengine) has no native file dialog, so it would always be Qt's own QML
  one. That one is only the fallback, when the portal is not on the bus or its call fails, shown
  inside the window. A save adds the default suffix to a name typed without one. Fugue keeps a
  recent list and suggests `~/Documents/Fugue`.
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
- A **canvas to share** (#249, Canvas > Share…, `fugue canvas export <id> <file>.fuguepack`): the same `.fuguepack` zip carrying a canvas of the revision store (14.5) instead of a workflow: the head with its layers and no history, or with its history. Opening one (File > Open, Import picture…, `fugue canvas import`) imports a new canvas and never replaces one; the commands table (13.1) has the layout and the checks.
- A **package to share** (#91, File > Export for sharing…, `fugue workflow export --share`): one
  `.fuguepack` file, a zip (MIME type `application/x-fugue-package`, which the desktop entry
  opens with Fugue), holding a template (11.10): `<id>/template.json` (the workflow's name), `workflow.fugue`
  (the share copy, holding the definition of every group it uses, a library's or a pack's too,
  and its `requires` naming the packs it uses and where to get them, 4.3),
  `inputs/` (each image a `file` input names that Fugue takes in, 14.1, and each paint canvas
  a `paint` input names, 14.4: by its bytes, at most 256 MB; renamed to letters, digits and `._-`; the input then names `inputs/<name>`) and
  `preview.<ext>` (the newest picture a Save Image or Preview shows). Only such images go in,
  so a package never carries another file a node names (a key, a document): that input keeps
  its bare name, as one whose file is gone does, and the export's notice names each, and the
  images it put in. At most 3 GB of images. No model is in it: models stay references by hash,
  source and size, which the person downloads (8.9). Never written over a file.
- An **app bundle** (#394, epic #397; File > Export for sharing… of a workflow with an `app` block, `fugue app export <id|file> <file.fugueapp>`): the
  package above with one more extension, `.fugueapp` (MIME type `application/x-fugue-app`, a kind of `application/zip`, which
  `share/fugue-app.desktop` opens with `fugue --app %f`: the app in a window of its own, 12.18; a `.fuguepack` still opens in the
  editor), and one reader with the same safety rules. The export menu writes `.fugueapp` for an app and `.fuguepack` for any
  other workflow; a name the person typed ending in either is kept. A shared app holds everything but the models:
  - **its packs**: `<id>/packs/<pack id>/` is the exact folder of each pack the app's nodes use (found as installed here, or in the
    template the app came from), less what an install leaves around: dot files and folders (git, a `.venv`, the source record),
    `venv`, `__pycache__`, `node_modules` and links; a file over 256 MB or more than 5000 files cannot be carried. The
    workflow's `requires.packs` records its version, source and ref as before (4.3) and, new, its `commit`, so the receiver's copy can be updated (6.7);
  - the executable bit of a pack's files (its `run.sh`) is stored in the zip (0755 against 0644) and restored on unpacking, from the archive only
    (never setuid, no other mode); a bundle made without it still installs: the install makes the file each command node's `runtime.entry` names
    executable. A pack unpacked from a bundle records the bundle file as its source (`.fugue-source.json` kind `bundle`, #520), not the scratch
    folder it was unpacked into; `fugue pack update` says plainly that it has no source to update from (open a newer bundle instead);
  - **its media**: `inputs/` holds, besides images and paint files, any file (a video, a song, a document) that a node an app input is bound to
    names, at most 256 MB each (3 GB in all); a file another node names is left out as before, the input keeping the bare name;
  - **models stay references** (hash, source, size, licence by the catalog), never in the file.
  The export checks first and names each item: it is **refused** (`app_not_shareable`, one error per item) when a pack can be
  neither carried nor fetched (not found here, or too big, and no `source` in `requires.packs`) or a model has no download source,
  because whoever opens the app would be stuck. It is only a **warning**, the bundle written, when the receiver can still get the item:
  a pack that is not carried but has a source, or a sample file that is gone or too big (its input stays empty). Decided: refuse, not
  warn, for what cannot be got at all (the owner's "never stuck"); no override flag.
  Opening one (a double-click, `fugue --app x.fugueapp`, File > Open, a drop) is a template's opening, by content, so a `.fugueapp`
  renamed `.fuguepack` or `.zip` (or the reverse) opens the same: the package is unpacked under the cache folder and, when it
  carries packs, kept there while its tab is open, so the one **Get ready** sheet (6.6, #379) lists the carried packs (install and trust),
  their environments and every model to download with the folder it goes in; one yes, then the App view is ready to run. Its sample files are
  taken into the inputs folder, whatever their type. The first open of a bundle in a window of its own offers **Keep this app** once it is
  ready: it installs the app into the person's apps (the own templates folder, as a template with its `packs/` and `inputs/`) and adds its launcher
  entry (12.18), and leaves the bundle where it was. A package made before app bundles opens as it always did, as a workflow, with no
  pack carried.
- Opening a package (File > Open, a drop, the desktop; told by its first bytes, so one renamed
  `.zip` opens too) unpacks it and opens it as
  its template opens: a new unsaved workflow, its images taken into the inputs folder, the
  banner naming what it lacks with the one **Get ready…** button (#379, [11.10](#1110-templates)): its packs
  at the source and commit it records, their environments and its missing models, asked once on one sheet.
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

**A missing pack in words (#264).** A pack is named for a person by its display name: the `name` in
its `pack.json` when a template carries it (`packs/<id>/`), else the `name` its `requires.packs`
entry records, else the last part of the id with a leading `fugue-` dropped, hyphens as spaces and
each word capitalized (`io.github.bmaltais.fugue-remove-background` is "Remove Background").
- A run that reaches a node whose type is of a missing pack fails it with `unknown_node_type`
  and `pack`. The node's card does not show that text (cut off, technical, naming an inner node):
  its error chip says "Needs the Remove Background pack", on a plain node and on a group
  instance whose inner node needs the pack (the card's error shows on every group holding the
  node, 5.6), and the whole text, with the inner node's path id and the hint, is the chip's tooltip.
- When the pack's source is known (the template carries it, or `requires.packs` records one and no
  version is installed) the chip has an **Install…** button that does what the banner's does: it
  opens Settings > Packs and starts the install, which asks before it installs and before the pack
  is trusted (6.6). It stays after the banner is dismissed. Without a source the chip has no button.

### 11.7 Embedding in outputs

- Save Image embeds the workflow in PNG and WebP by default, so dragging an output back onto
  Fugue reloads its graph.
- Absolute local paths are never embedded: they are replaced as in a share copy, including a path
  typed into a node whose output reaches a file or folder input through links. Layout is
  embedded, so the graph comes back as it looked. A `run` section is added with the values used
  per node (seed, model, engine, and the picked text of a Dynamic Prompt), so a randomized result can be reproduced.
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

- A template is a folder: `template.json` (title, description, category, tags, and optionally `source`), `workflow.fugue`,
  a preview (`preview.webp`, `.png` or `.jpg`) and `inputs/`, the sample pictures its Load Image
  nodes name as `inputs/<name>`. A root of templates may hold `categories.json`, the order and
  titles of its categories. Nothing indexes the templates: a root's folders are its templates.
- A template may have a **moving preview** (docs/canvas-look.md 20.19): `template.json`'s `preview` names a file of the folder, a still
  (`preview.webp`, `.png`, `.jpg`) or a WebM (`"preview": "preview.webm"`, VP9, no audio, a loop of 3 to 4 seconds, 640 by 640,
  under 600 KB), and for a WebM `poster` names its still (`"poster": "poster.png"`; else `poster.png`, else the conventional
  `preview.webp|png|jpg`, which the bundled templates keep as the fallback). Without the key the conventional stills are found, and
  a `preview.webm` beside them is the moving preview. A name that is not a plain file of the folder is ignored. The still (the
  poster) is `preview` in `Workflows::templates()` and everywhere a picture is wanted (frosted ink reads it); the clip is `video`.
  The apps shelf's card and the App view's header play it muted and looped only while visible (card hovered or focused;
  the open app's header) and the window is focused, stop otherwise, and under reduced motion (`motion` look parameter) show the
  poster only. `bin/check` of fugue-workflows reads both keys and limits each file to 600 KB.
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
- **Source and model family (#369).** `template.json` may say where a workflow comes from: `"source": {"name",
  "author", "url", "note"}`, each optional text; a `url` must be `http://` or `https://`. A field that is not as
  said, or an unknown one, is left out and is a warning of `fugue template list` (the template stays listed). A
  card with a name or an author says "Based on <name> by <author>"; with a `url` the line is a link that opens the
  browser (a click on it does not open the template), and its tooltip shows the note and the address. A template's
  **model families** are read from its Load Models: each diffusion, SeedVR2, upscale or BiRefNet model's resource
  type and file name give a family from the one table in `engine/model_family` ("FLUX.2 Klein 9B", "FLUX.2 Klein 4B",
  "Z-Image", "Qwen-Image", "Wan 2.2", "SeedVR2", ...), the same names the model pickers' family filter uses
  (#370); nothing is typed in `template.json`, and a text encoder or a VAE is no family. Two drop-downs
  beside the search, **Source** (by `source.name`) and **Model family**, list each value with the count a choice would
  leave under the other filters; each is hidden while no template has a source or a family. They reset when the
  dialog opens.
- **Apps (#387, epic #397).** A template is an **app** when its workflow has an `app` block that reads (4.9). The block is the
  binding: its inputs, outputs and title are what the App view shows. `template.json` carries only the catalog's side:
  `"app": true`, or `"app": {"title", "summary", "kind"}` (each optional text: the card's name, its one line, and a short
  word for what it makes such as "image"; the card's name and line fall back to the block's `title` and `description`,
  the line then to the template's own `description`). `"app": false` keeps a workflow that has a block out of the
  Apps (it still runs by name with `fugue app run <id>`). With no `app` key (a loose `.fugue` file in the person's folder
  has no `template.json`) the block alone decides. What does not agree is a problem of the catalog check (the bundled
  templates' CI) and a warning of `fugue template list`, never a reason to hide the template: an `app` flag with no block, a
  block with no flag in a folder's `template.json`, a block that does not read (the template is then no app and opens as an
  ordinary graph), an `app` object with a field that is not text or an unknown field. An app card shows the app's name and
  one line; the Templates window has an **Apps** chip beside Ready to run (`templatesApps`, hidden while no template is an
  app, reset when the dialog opens) that keeps only the apps. Opening an app template opens its tab as any template does, and
  the tab opens in the view the block's `defaultView` names (12.17): the App view for an app, with **Open in Graph** giving
  the same workflow as an ordinary tab. Samples in `inputs/` are the inputs' defaults, as for any template.
  `fugue template list` gives `app` for each: `false`, or `{"title", "summary", "kind"}`; `fugue app list` lists the templates
  with this flag (one definition of "an app" for the window and the command).
- **Search (#369).** Every word typed must match at the start of a word (a letter or digit after a space,
  hyphen or dot) of the title, description, category, tags, id, source name and author, or families: "da" finds
  "DA workflow" and no longer "update". `tag:`, `source:` (name or author) and `family:` limit a word to one field; a
  value with spaces is quoted, `family:"klein 9b"`. `fugue template list` gives `source` and `families` for each.
- The dialog shows categories, a search over title, description and tags, and cards with the
  preview and what this machine lacks (models, packs; a check, reading no model file). A click
  on a card, or Enter on it, opens it; a small "i" at the picture's bottom right corner (I or Space on the
  focused card) shows its details first (Open and Cancel at the foot, Escape closes them, then the dialog).
  Opening one makes a new unsaved tab named after it, its samples taken into the inputs folder as a
  pasted image is, so it runs as it opens and a save keeps it portable. Every file it ships is taken in, a song, a clip or a
  LUT as well as a picture: one left in the template's folder would be named by a path an AppImage mounts elsewhere at each start. The CLI writes the
  workflow with its samples copied into `inputs/` beside it, never replacing a file: a sample
  whose name is taken by another file takes `-2`, `-3`..., and the workflow names that. A sample
  must be a file inside its template's folder; one that leads out, by `..` or a link, is not read.
  The person's own template wins over a bundled one of the same id, even when it is broken.
- **Get ready (#379).** A template that is not ready (a carried pack not installed or not trusted, its environment, missing
  models) is made ready with one question. The banner of the opened template keeps its line and has one button, **Get ready…**
  (it replaces the separate Install pack, Download and "Into …" controls); it opens **one sheet** with the headline ("Fugue will
  install and trust 1 pack, set up 1 environment and download 1 model, 1.2 GB"), a row per pack and per model that opens to the
  full detail the separate prompts gave (the pack, version, source, runtime kinds and declared permissions, the setup command and its
  size; the model's licence, size, host and folder), the **Models go into** row ([8.9](#89-models-on-disk)), and **Install**
  (one word whenever there is anything to do: packs, environments or models) or Cancel. It is one click: a model whose resource
  type is declared by a pack in the same list (the Segment pack's GroundingDINO model) is a row like the others, marked "type from
  <pack>", in the totals and the free-space check; it cannot be planned before that pack is in, so the run plans it after the pack
  rows, with the types the installed packs declare, and downloads it in the same run. If the pack, once installed, does not
  declare the type, that is a `pack_invalid` error saying so. Then the sheet shows one list with progress per row and **Stop**; the
  first failure stops the run, keeps the sheet open, says what is left, and **Try again** lists only that. When every row is done
  the sheet closes by itself and the template it was opened for is the tab shown, ready to run; no "done" sheet is left.
  Nothing happens before the yes; a pack update, or a pack that declares more than was shown,
  still asks on its own ([6.6](#66-installing)). A package ([11.5](#115-export)) opens the same way: its `requires` name the packs
  and models, and it carries no pack. `fugue template prepare <id>` and `fugue workflow prepare <file>` do the same on a terminal, one question for the packs and every model.
- **Download for a template that needs packs.** A card's Download for a template whose packs are not installed
  does not start a direct download (it would refuse a model whose type a pack declares): it opens the template, and the
  Get ready sheet plans the packs, then the models (`Workflows::downloadTemplate` returns true; the Templates window closes).
  A template that needs only plain models still downloads from the card.
- **Where its models are (#164).** A card says, with a small badge and a tooltip, where each of
  its models can be had: `monitor`, "On this machine" (a file in the model folders, or a model a
  server on this machine serves) and `server`, "Shared by <host>" (a model a paired host serves
  through the omarchy provider, [8.7](#87-shipped-providers)); a template whose models are in
  both places shows both. A model in neither place is the existing "Models missing" line and
  Download offer (#85, #89). A host that lists a model but is not answering is shown
  dimmed with "Host offline: <host>", and is not a place: it never makes a template ready. A
  **Ready to run** chip filters to the templates whose every model is somewhere (a template with
  no model files is ready), and within a category the ready ones come first. Availability is what
  the disk and the providers report, never guessed: a model is the same model by hash, else by
  resource type and name (`sameModel`, [8.3](#83-selection)), so a host's model counts
  for a template's only when it reports that hash. One pure mapping, `engine/model_places.h`
  (`placesOf`, `summarizePlaces`, `placeGlyphs`), serves the cards and the model pickers
  ([2.5](#25-resource-handles)); the hosts' models are read when the dialog opens
  (`Workflows::loadHostModels`, then `templatesChanged`), so the cards of a slow host fill in
  after the dialog shows.

---

## 12. The Graph

The look and feel of the prototype is approved. This section fixes behavior.

### 12.1 Targets

- Pan and zoom stay smooth (about 60 frames per second) with 300 nodes and 600 wires.
- The window is up in well under half a second.
- No CPU use at idle.
- No costly effects on nodes (shadows, animation) at scale.

### 12.2 Construction

Our own graph view, no library: node bodies as QML items in a Repeater under one transformed item,
wires as shape paths. Not `Canvas`, not `QQuickPaintedItem`, not Qt Canvas Painter (GPL or
commercial). Each wire is its own `Shape` with the default geometry renderer, not `CurveRenderer`
([spikes/05](spikes/05-canvas.md): same cost as one Shape, 1.5 ms per frame at 300 nodes;
CurveRenderer drops to 15 fps when dragging at 1000 nodes).

### 12.3 Navigation

Two presets, chosen in settings (`navigation`, applied at once):

| | Legacy (default, ComfyUI's default) | Standard |
| --- | --- | --- |
| Left-drag on empty graph | pans | selects |
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
| `node.search` | Add a node by search | double-click empty graph, Tab, Shift+A | keys added |
| `frame.make` | Frame the selection | Ctrl+G | same |
| `group.make` | Make a group | Ctrl+Shift+E | same |
| `group.ungroup` | Ungroup | Ctrl+Shift+G | added |
| `group.enter` | Enter the selected group | Tab, double-click the group's body | Tab added |
| `group.leave` | Leave the group | Escape, the breadcrumb | same |
| `view.fit` | Fit the view | `.`, Home (Home fits every picture view too, #228) | Home added |
| `view.zoom-in`, `view.zoom-out` | Zoom | Alt+=, Alt+-, Ctrl+=, Ctrl+-, `+`, `-` | Ctrl added; `+` and `-` added (#228: the keys of every picture view, one keymap) |
| `view.toggle-links` | Hide or show links | L | same |
| `view.picture-smart`, `view.picture-sinks`, `view.picture-all` | The picture mode: Smart, Sinks and inputs only, All (View > Picture, checked; writes `picture_mode`, [12.7](#127-inspection)) | none | added (#263) |
| `node.picture-show`, `node.picture-hide`, `node.picture-default` | A node's own picture choice, over the mode: Show picture, Hide picture, Default picture (the Node menu and the node's menu, left out where they would change nothing; one undo step) | none | added (#263) |
| `panel.nodes`, `panel.workflows` | Toggle the node library, the workflows panel | N, W | same |
| `panel.queue` | Toggle the run queue panel | Alt+Q | added (ComfyUI's `q` is Quit here) |
| `view.toggle-menu-bar`, `view.toggle-tabs`, `view.toggle-run-toolbar`, `view.toggle-selection-toolbar`, `view.toggle-breadcrumb`, `view.toggle-run-status`, `view.toggle-zoom-readout` | Show or hide each bar | none (View > Bars and panels) | added |
| `view.focus-mode` | Hide all bars and panels; the same key brings them back as they were | H | added |
| `view.reset-bars` | Every bar back, the panels closed | none (View > Bars and panels) | added |
| `view.toggle-app-inputs` | Show or hide the App view's inputs panel; kept between runs, focus mode hides it (12.17) | none (Window > App inputs) | added |
| `view.toggle-app` | Switch this workflow between its App view and the graph; on a plain graph it opens the app builder, on the shelf it is Start blank, on the empty graph Start blank gave it brings the shelf back (12.17, #397) | Ctrl+Alt+A (Window and Graph View menus, the window bar's App \| Graph switch) | added |
| `app.show-all` | The shelf of apps over the window, on the current app's card, to open another (12.17, #397) | Ctrl+Shift+A (File > All apps…, the rail's Apps item, Fugue's mark in the window bar) | added |
| `app.make` | Make an app from this workflow, or edit the one it has: the builder (12.19, #393) | Ctrl+Alt+M (File > Make app…) | added |
| `app.edit` | The same builder on the workflow's app block; off for a workflow with none (12.19, #393) | none (File > Edit app…, shown only with a block) | added |
| `app.menu-bar` | Open the menu bar | F10 | added |
| `app.context-menu` | Open the context menu | Shift+F10, Menu | added |
| `area.split-row`, `area.split-column` | Split the active area side by side, top and bottom (12.16) | `Ctrl+\`, `Ctrl+Alt+\` | added |
| `area.close` | Close the active area (12.16) | Ctrl+Alt+W | added |
| `area.view-graph`, `area.view-canvas` | The active area shows the Graph, a Canvas (12.16) | Ctrl+Alt+G, Ctrl+Alt+C | added |
| `canvas.new` | New canvas…: the New canvas dialog, anywhere (14.5, #232) | Ctrl+Alt+N (File) | added |
| `canvas.paste-new` | Paste as new canvas: the clipboard's picture as a canvas, no dialog; off while the clipboard holds no picture (14.5, #232) | none (File) | added |
| `view.reset-layout` | One area showing the Graph again (12.16) | none (Window menu) | added |
| `canvas-layers.visible` | Hide or show the selected layers of a Canvas view (14.5, #226) | H, a click on the eye (Alt+click: that layer alone) | added |
| `canvas-layers.lock` | Lock or unlock the selected layers: a locked layer cannot be deleted, merged, transformed or have its layer mask changed (14.5, #226) | Ctrl+/, a click on the lock | added (Photoshop's key) |
| `canvas-layers.use-in-region` | Generate (the task bar's run, Canvas Region's Layer input) reads this layer's own pixels instead of the whole picture; again, or the banner's x, goes back; a view state, not a revision (14.5, #226) | none (the layer's menu) | added |
| `canvas-layers.rename` | Rename the layer | F2, double-click its name | added |
| `canvas-layers.duplicate` | Duplicate the selected layers | Ctrl+D | added |
| `canvas-layers.raise`, `canvas-layers.lower` | Move the selected layers up, down | `]`, `[`, drag a layer | added |
| `canvas-layers.delete` | Delete the selected layers | X, Delete | added |
| `canvas-layers.mask` | Turn the layer mask off or on | none (Shift+click the mask) | added |
| `canvas-layers.undo`, `canvas-layers.redo` | Undo, redo the last change to the canvas (the head back and forward) | Ctrl+Z; Ctrl+Shift+Z, Ctrl+Y | added |
| `canvas-view.tool-hand`, `canvas-view.tool-zoom`, `canvas-view.tool-region` | The Canvas view's Hand, Zoom and Rectangle region tools (14.5, #230) | H (Space held: a moment's Hand), Z, M or R | added (Photoshop's keys) |
| `canvas-view.tool-move`, `canvas-view.free-transform` | The Move tool (a drag moves the current layer, the arrow keys nudge it) and Free Transform (the box with eight handles, turn, numbers; Enter keeps it as one revision, Escape leaves it) (#236) | V, Ctrl+T | added (Photoshop's keys) |
| `canvas-view.tool-lasso` | The Lasso tool: a freehand drag makes the selection (Shift adds, Alt subtracts, a new one replaces); its area shows live in the selection's red while it is dragged, as the rectangle's does (#230, #380) | L | added (Photoshop's key) |
| `canvas-view.tool-selection-brush` | The Selection Brush (#380): paints the selection directly, next to the lasso; the covered area shows live as translucent red (about 40%) on every move and the marching ants take over on release; Alt at the press subtracts, `[` and `]` size it; a stroke is one selection undo step | A | added (Photoshop Elements' key and tool) |
| `canvas-view.tool-brush`, `canvas-view.tool-eraser`, `canvas-view.brush-swap` | The mask brush and eraser (they paint the selected layer's mask only, with their own live trace in the mask's tint, even with the Overlay off; with Quick Mask on they paint the selection), and swap them | B, E, X | added (Photoshop's keys) |
| `canvas-view.brush-smaller`, `canvas-view.brush-larger` | The brush's size, by a quarter | `[`, `]` (Alt+drag on the picture) | added |
| `canvas-view.quick-mask` | Quick Mask: any brush paints the selection, and the whole selection shows as the red overlay, on whenever Q is turned on (`\` hides it) | Q | added (Photoshop's key) |
| `canvas-view.add-mask` | Add a layer mask to the selected layer (one revision; the brush takes it) | Ctrl+Shift+M | added |
| `canvas-view.mask-overlay`, `canvas-view.mask-invert` | Show or hide the red overlay of the mask; invert the mask or the selection | `\`, Ctrl+I | added (Photoshop's keys) |
| `canvas-view.undo`, `canvas-view.redo` | Undo, redo in the Canvas view: a Quick Mask stroke in Quick Mask, a lasso or a selection from a layer while the lasso is the tool or right after it changed, the canvas while a layer mask is painted, else the document's (the region) | Ctrl+Z; Ctrl+Shift+Z, Ctrl+Y | added |
| `canvas-view.select-all`, `canvas-view.deselect`, `canvas-view.invert-selection` | Select all, Deselect (the free rectangle, the Region node's area set to 0, 0, 0, 0 as one undo step, the painted selection; Escape does it too when nothing is dragging or running), Inverse of the painted selection (#232) | Ctrl+A, Ctrl+D (Escape), Ctrl+Shift+I | Ctrl+A added: the view's own, over the Graph's select all |
| `canvas-view.selection-expand`, `.selection-contract`, `.selection-feather`, `.selection-border`, `.selection-smooth`, `.selection-transform` | Select > Modify (each asks one pixel amount) and Transform selection (scale %, rotate, move): on the painted selection, or on the rectangle painted into it first; one step of the selection's undo (#251) | none | added |
| `canvas-view.new`, `.open`, `.import`, `.export`, `.rename`, `.duplicate`, `.clear`, `.delete`, `.close` | The Canvas view's File menu: a canvas made, chosen, imported, exported (.png, .jpg, .webp, .ora), renamed, copied with its history, emptied (one undoable step), deleted for good, closed (#232) | none | added |
| `canvas-view.new-layer`, `.merge-down`, `.flatten`, `.group`, `.ungroup`, `.layer-properties` | The Layer menu's operations on the layers panel's selection (#252): an empty layer above the current one; the layer into the one under it; every shown layer made one; the selected layers in a new group, or the groups taken apart; the Layer properties dialog (name, opacity, blend, mask). One revision and one undo step each | Ctrl+Shift+N, Ctrl+E, none, Ctrl+G, Ctrl+Shift+G, none (Photoshop's keys) | added |
| `canvas-view.copy`, `.copy-merged`, `.copy-mask` | Edit > Copy (the selected pixels of the current layer as a picture on the clipboard, the selection as its alpha, cut to its bounds; Ctrl+C while the Canvas view is the one in use, the Graph's Ctrl+C still copies nodes), Copy merged (every shown layer flattened) and Copy selection as mask (a gray picture of the canvas's size, white selected); nothing is stored; no selection is the whole layer (#327) | Ctrl+C; Ctrl+Shift+C; none | added (Photoshop's Copy and Copy Merged; Save selection as channel) |
| `canvas-view.paste-layer`, `.paste-canvas`, `.place-picture` | Edit > Paste as new layer (the clipboard's picture as a layer above the current one, centered in the view, one revision; Ctrl+V while the Canvas view is the one in use, the Graph's Ctrl+V still pastes nodes), Paste as new canvas, and Canvas > Place picture… (a file as a layer); a picture file dropped on the picture does the same (#236) | Ctrl+V; none; none | added (Photoshop's Paste and Place) |
| `canvas-view.flip-horizontal`, `.flip-vertical`, `.turn-clockwise`, `.turn-counterclockwise` | Layer > Transform: the current paint layer flipped about its center or turned a quarter turn, each one revision (#236) | none | added |
| `canvas-view.use-selection`, `.mask-reveal`, `.mask-hide`, `.fit`, `.actual-size`, `.zoom-in`, `.zoom-out`, `.history`, `.layers`, `.compare`, `.details`, `.snap` | The rest of the view's own menu bar: what its top bar and options bar had as buttons (#232). Fit, 100%, Zoom keep their keys by hand (Home or 0, 1, + and -), shown in the menu as text | none | added |

Commands with no default key (the colors `node.color.<role>`, `edit.clear`, the exports, the help
links, the `view.toggle-...` bars, `view.reset-bars`, `view.reset-layout`) are in the menus, and the keymap file can bind any of them.

Align and distribute (#58): `node.align-left`, `-right`, `-top`, `-bottom` put the selected nodes
on the selection's outermost edge; `node.distribute-horizontal` and `-vertical` even out the gaps
between three or more, in the order they start, never closer than 24 units: nodes that do not fit
the span get that gap and the run grows past it (ComfyUI's Align Selected To and Distribute Nodes).
They have no default keys; they are in the Node menu, and in the node's menu when enough nodes are
selected (two to align, three to distribute). A pinned node stays where it is. One undo step each.

Tab enters a group when exactly one group is selected and opens node search otherwise, inside a
group too. A command states the context it applies in, and the more specific context owns the key.
The `canvas-layers` commands (#226) are in the most specific context, `canvas-layers`, which is on
while the pointer is over a Canvas view's layers panel or the panel's list has the focus (Blender's
rule: the keys go to the editor under the pointer), so over the panel H, X, Delete, Ctrl+D, F2 and
Ctrl+Z are the canvas's and elsewhere the Graph's; the region drawn on the picture keeps the
document's Ctrl+Z. The `canvas-view` commands (#230) are in the context just below it, `canvas-view`, on while the
pointer is over a Canvas view or its picture has the focus: there H, Z, M, R, B, E, X, `[`, `]`, Q, `\`, Ctrl+I,
Ctrl+Shift+M, Ctrl+Z and Ctrl+Shift+Z are the view's tools (Q, Ctrl+Z, `\` and M win over `app.quit`, `edit.undo` and
Compare Images' keys there), and over its layers panel the panel's own keys still win (X deletes a layer there and swaps
the brush on the picture). A view in a modal popup binds them itself, as the panel does. The task bar's prompt (#231) is
a text field, so the bare letters are its own while it has the focus; there Enter generates (it is not a command: the
field takes it), Ctrl+Enter is still `run.start`, Up and Down step through the prompt history, and Escape cancels the
bar's runs while they wait or go (also when the view has the focus), else it is the window's. Their handlers ask the panel under the pointer (`windowRequested`); a panel in a
modal popup (the File > Canvas… dialog, whose window shortcuts sleep) binds the same keys itself. A Canvas view
that has the focus (a Canvas area pressed or tabbed into) keeps the Graph's own keys from the Graph behind it: its
`ShortcutOverride` takes any key whose owner `CommandRegistry::actsOnGraph` (the `canvas`, group and compare contexts
but the window's panels and bars), so Home, `+` and `-` are its picture's and Delete, Ctrl+A or Tab do nothing there;
the `app` keys stay the window's (Ctrl+Z, Ctrl+S, Ctrl+Q) except the bare Q, which the view keeps for a quick mask
(#228; tests/workspace/test_area_split_window.cpp `aFocusedCanvasAreaKeepsTheGraphsKeysAndQ`).

Mouse:

- Double-click on empty graph opens node search.
- Releasing a dragged link on empty graph opens search filtered to nodes with a compatible port.
- Shift+drag an output moves all its links. Ctrl+Alt+click on a port disconnects it.
- Shift or Ctrl+click adds to the selection.

Text fields: while one has focus, bare-letter and Shift-only keys, and Ctrl+A, C, V, X, Z, Y,
belong to the field. Hence the Ctrl and F1 alternatives for quit and the overlay.

### 12.5 Command registry and keymap

- Every action is a command with a string id in one registry. Menus, the `?` overlay, the
  settings panel and the CLI's listing all read the same registry.
- Each key has one owner at a time in a given context (canvas, a selected group, a text field,
  a run in progress). Contexts belong to commands, not to the keymap file.
- **The window's menus and the areas' menus (#262, Blender's model).** The window bar holds what is global: File, Edit (document
  Undo and Redo, Copy, Paste, Clear, Settings), Run, Window (every bar and panel with a check, focus mode and the way back,
  then the areas' split, close, view and reset) and Help. Each area has a header with its view-type menu, then its view's own
  menus, then split and close. The Graph's are View (fit, zoom, links), Select (select all), Add (node search, paste connected)
  and Node (rename, colors, clone, delete, bypass, mute, collapse, pin, align, distribute, images, frame, groups); the Canvas
  view's are Canvas, Edit, Select, Layer and View (in the header of the area that shows it; in the File > Canvas… dialog, which
  has no header, the view keeps its own bar of the same menus). Which menus a view type's header holds is `menus::viewMenus`.
  A command is listed at one level only. The Graph's header is also above the graph while the window has one area; in a
  header too narrow for the titles they fold into one Menu button. Only the window bar's menus have Alt letters (F, E, R, W,
  H); the headers have none. **The workflow tabs (12.10) are in the window bar's row**, after its menus and before the
  project chip and Close, as Blender's workspace tabs are in its top bar: a tab takes up to 36u and shrinks to 16u as they
  crowd, then the strip scrolls and keeps the current tab whole (`menus::tabWidth`, `menus::tabScroll`). Hiding the bar
  (12.15) hides them with it.
- The menu bar and the context menus list command ids; each item's name, key and enabled
  state come from the registry when the menu opens, and a command not built yet shows as
  unavailable. Alt+letter opens a top menu where 12.4 binds nothing to it.
- A close button (`closeApp`, #218) at the bar's right end runs `app.quit`, so the unsaved-content
  question applies; its tooltip shows the command's key. Always shown.
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

**Node pictures** (#263). A chain of nodes that each repeat the picture before them is noise, so which
nodes show a picture is a choice at three levels. The **picture mode** (`picture_mode` in Settings > General,
a `preferences` key a project inherits; also View > Picture in the Graph's header) is `smart` (the default:
nodes that make or load a picture, and sinks), `sinks` (Sinks and inputs only: the sinks and the Load nodes)
or `all` (every node that has a picture, as before). A node type says what it is with the manifest key
`picture` ([3.1](#31-node-types)): `always` (Preview, Save, Compare Images: shown in every mode but a hide), `result`
(Load Image, Generate Image, Sampler, the decodes, Upscale with a model, and Crop and Paint Mask, whose
frame and strokes are drawn on the picture: shown by Smart and All, and by Sinks only for a Load node, a
type with a file input and no media input) or `never` (a transform that only changes the picture before it:
Resize, Blur, Flip, Color Match, the mask nodes: shown by All only). Without the key the type's category
decides. The node's **override**, `layout.nodes.<id>.picture` = `show` or `hide` (Show picture, Hide picture and
Default picture in the Node menu and the node's right-click menu), wins over the mode. A group instance
is a `result` node: it shows the image its group makes after a run (its first image output's inner node), and before any run, the image its face's file input names, as a Load Image does (a group that exposes a Load node's file is an input node for Sinks and inputs only). Where the picture is hidden the node
has no picture slot and is shorter, and its thumbnail is not made (the run is told which nodes to skip,
`RunRequest::noPictures`; a node that a shown Crop or Paint Mask draws over still gets one). A picture
shown later than the run that made the results waits for the next run: a cached node gives it at once.

### 12.8 Node search

Search-first node creation. Results show category, ports and version, and are filtered by port
type when opened from a dragged link. Groups appear in search like any node type. With nothing typed the
list is by category, then name; opened from a link dragged out of an output, the sinks (Preview, the Save
nodes) come after every other node, so the nodes that carry the work on lead the list (owner, 2026-11-25:
Preview led it only because "Basic" sorts first). Typed words rank as before.

### 12.9 Furniture

v0.1 has collapse, hide unused ports, reroute points and Frames. A minimap and snap-to-grid are later.

### 12.10 Several workflows

Tabs inside one window. Each tab tracks its own unsaved state and its own undo stack.

**Reopening the tabs (#212).** A start with no file named opens again the saved workflows that were
open when the last session ended, in their order, with the same tab shown. The setting
`restore_tabs` (Settings > General, "Reopen tabs at start"; on by default; a `preferences` key, so a
project may inherit it from the Default and say its own) turns it off, live. The tabs are kept in
`tabs.json` in the state folder of the root (`~/.local/state/fugue/tabs.json` for the Default session,
the project's own `state/fugue/tabs.json` in a project), one small file written whole or not at all
(`{"format": 1, "tabs": [path, ...], "active": path}`). It is rewritten a short moment after the tabs
change (open, close, switch, save as, rename: a burst is one write, so a crash loses nothing) and at
quit; a project never reads or writes the Default's. Turning the setting off removes the file, so
turning it on later never brings back an old session. Rules:
- Only a tab with a file is kept. A workflow never saved (new, from a template, pasted) and the
  unsaved changes of a saved one are not kept here: a quit asks about them as before, and after a
  crash the recovery copies of [11.3](#113-recovery) offer them as before, on top of the reopened tabs.
- A file that is gone, is not a workflow, or cannot be read is skipped, and one line in the banner
  says how many and names them; the kept session then drops them. If the shown tab's file is among
  them, the last tab is shown.
- A start with a file named (`fugue path.fugue`) opens that file and nothing else, and neither reads
  nor replaces the kept session, so looking at one file never loses a project's tabs. A project switch
  starts the new window without files, and the old one writes its tabs as it quits.
- Two windows on one root keep whichever wrote last. Each tab's zoom and centre are those saved in
  its file ([11.2](#112-saving)), not kept separately.

### 12.11 Undo, look and language

- Every edit is a command on one undo stack per workflow, group edits included.
- The window follows the Omarchy theme live and the desktop text size. The corners
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

A toolbar floats over the graph (#60, M3), as ComfyUI's action bar, and a panel hangs from it. Both
read and write the queue of [9.8](#98-runs-across-processes) and the commands of [12.4](#124-default-keymap);
they add no queue of their own.

- **Toolbar** (`RunToolbar.qml`): a grip, the batch count, **Run** with a menu behind it (the Run menu's
  rows: Run, Run first, Run to here, Cancel), **Cancel**, and the **queue** button with the number
  of runs going and waiting. It floats anywhere over the graph (#120): dragging the grip moves it
  across as well as up and down, and the arrow keys on the grip move it by 2u (Shift: 10u). It stays
  inside the graph, 4u from the edges, also when the window is resized, and a drag is pulled
  lightly (within 3u) to the edges and the middle. It starts centered at the top. The place is
  remembered with the batch count in `~/.local/state/fugue/run-bar.json` as fractions of the room
  the bar has (`place`: x and y, 0 to 1), so a resized window keeps it where it was relative to the
  edges; an older file's `dock` ("top" or "bottom") is read as the centered top or bottom place. `view.toggle-run-toolbar`
  (View > Bars and panels) hides and shows it. A thin bar under it shows the progress of the node going.
- **Batch count** (1 to 99): one Run makes that many runs, queued together in order, each taking the
  document as it is then, so a seed set to increment gives each its own number (ComfyUI's batch
  count). It applies to Run, Run first and Run to here.
- **Queue panel** (`RunQueuePanel.qml`, `panel.queue`, Alt+Q, or the toolbar's queue button): hung from
  the bar wherever it is, centered on it (held inside the graph), under it when there is room, else
  on the side with more room, and kept shorter to fit there. *Running and
  waiting* lists the run going first, then the waiting ones in queue order, **whoever started them**:
  this window, or `fugue run` from a terminal. A row has the workflow's name, the node going with its
  progress or "Waiting", where it came from, the time since it began (or was queued), **move to front**
  (waiting runs but the first) and **cancel**. *Finished* lists the last eight runs that ended, newest
  first: a **thumbnail of the first output the run made** (a frame of a video, a waveform of a sound; none when it
  made nothing), what to call it (the first Text node's text, else the first output's file name, else
  the workflow's name, since many runs are called "workflow"), how it ended, how long it took, when, and
  the workflow's name; a click on the row (or its results button) **opens the media browser on that
  run** (#133: the workflows panel's Results view, scoped to the one run, with an *All runs* button to
  widen it to the workflow), and it also shows the results on the nodes when it is the workflow of the
  tab shown (a run's node ids mean something only there); a button opens its run folder. The lead and
  label are `RunIdentity` (`media/media_items.h`, `runIdentityOf`), read once per ended run. A
  Preview's or Compare's `shown` values count as results beside `outputs` (they have no outputs). Cancel of a run of this window goes through the run thread; of any
  other run it does what `fugue runs cancel` does, so both ways end a run the same way. Move to
  front renames a waiting entry into class 3 ahead of the other waiting ones (9.8); it never goes
  ahead of the run going.
- The panel and the toolbar are made only when shown, and the window keeps its item budget.
  The panel reads the queue folder (a file watch, nothing at idle); only while it is open does it
  read the going run's events once a second and list the history.

### 12.14 Side panels

A dockable panel beside the graph (#56), as ComfyUI's sidebar, with two tabs: the **node library**
(`panel.nodes`, N) and the **workflows panel** (`panel.workflows`, W). One is open at a time: its key
opens it, the same key closes it, the other key replaces it. Each is a checked item in the View menu's Bars and panels submenu
while it is the open one. The graph gives up the panel's width; the panel is made only while open,
and the window keeps its item budget.

- **Tab strip** (#159, `SideTabStrip.qml`): a slim vertical strip on the window's **left edge**, under the menu bar
  (the workflow tabs are in its row) and beside the graph, with one icon per registered panel (a table in `panels/side_panels.cpp`:
  tab id, registry id, command, Lucide glyph, name; a new panel is one row and gets a tab). A click opens the panel,
  a second click on the same tab closes it, a click on the other tab replaces it, exactly as the keys do (one path:
  `SidePanels::toggle`, so the strip, N and W and the View menu's checks always agree). The open tab is marked (accent
  glyph, a wash, a bar on the edge); the tooltip says the name and its key, "Node library (N)"; every tab takes Tab
  focus and Enter or Space; the strip is 44 px wide with 44 px tabs and 24 px glyphs. It is always there, also with
  no panel open (it is the way in), except in focus mode (12.15), which hides it like every other bar (registry id
  `side-tabs`, no toggle of its own; a panel asked for in focus mode still shows). When the window is narrow the
  panel gives way, never the strip: the graph keeps at least 160 px. The strip stays on the left when a panel is
  docked on the right. **Action tabs** (#192): a row of kind `action` launches a command instead of toggling a panel and is never marked open or
  checked: Templates (`file.templates`, third in the top group directly under Workflows) and Settings (`app.settings`, Ctrl+,,
  alone at the lower end past a hairline). Which end a row sits at is its `edge` field in the table, not QML. The run queue (`panel.queue`) is a floating panel of the run toolbar, not a docked side
  panel, and the media results are a view inside the workflows panel; neither has a tab (follow-ups: a queue and a
  results tab once they are docked panels).
- **Container** (`SidePanel.qml`): a title bar with a button that moves the panel to the other side
  and one that closes it, the content below, and a grip on the graph-facing edge (drag, or Left and
  Right on it with the focus) that sets the width, 200 to 640 px. It knows nothing of the graph or
  of what it holds. Which panel is open, its side and its width are remembered in
  `~/.local/state/fugue/side-panels.json`; a window starts with what it closed with.
  The sides are the window's, left and right: the graph never mirrors, and neither does the side.
- **Node library** (`NodeLibraryPanel.qml`, `search/node_library.h`): a search field over every node
  type in a tree by category (the `/` parts of `category`; closed at first, a click, Enter, Left and
  Right open and close one, with the count of nodes under it). The filter is node search's own
  ranking ([12.8](#128-node-search)), so a word finds the same nodes in both, and a query opens every
  category on the way to a match. A **click or Enter adds the node** in the middle of the view; **dragging
  a row onto the graph** adds it where it is let go (a drop over the panel adds nothing); one undo step
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
  action asked (`actionRequested`, `activated`); it knows nothing of the graph, files or runs. Modes: *Grid*
  (rows under a heading per run, newest first), *Filmstrip* (the current item large over a strip), *Single*
  (one item large; a video or sound in `MediaView`). Keys: Left/Right (PageUp/PageDown) step, Up/Down move a
  grid row, Home/End, Enter opens the item large, Escape goes back to the mode it came from and then asks to
  close, I shows the details. Actions (the host gives them; the Results view gives these): *Favorite* (F, a
  toggle, a star on the cell), *Open*, *Re-run*, *Open workflow*, *Send to Load Image*, *Show folder*, *Copy path*,
  *Move to trash* (Delete). The core is plain C++
  (`media/media_items.h`): a run's results are the media outputs of the nodes nothing else reads (from
  `workflow.json`), `mediaOfWorkflow` takes the newest 40 runs whose `run.json` names the workflow's path
  (an unsaved workflow has none), a file in several runs stays with the newest, a missing file is left out;
  `embeddedSummary` reads prompt, seed and model from a PNG or WebP's embedded workflow (11.7). Thumbnails
  (`media/thumbnail_cache.h`) are PNGs of at most 256 px in `~/.cache/fugue/thumbs/`, named by a hash of path,
  size, modification time and box, made off the main thread by the `image://fuguemedia/` provider the first
  time and read back after. A video's thumbnail is a frame 0.5 s in (from the start when the clip is shorter)
  and a sound's its waveform (`showwavespic`), both taken by the `ffmpeg` the engine already runs
  (`engine/ffmpeg.h`, killed after 15 s); a file that cannot be read, or a machine without ffmpeg, gives
  an image load error and the cell keeps its glyph and name. The queue's finished rows use them too. The panel refreshes the list when Results opens, the workflow changes, a run ends
  or the refresh button is pressed.
- **Favorites, trash, re-run, send, zoom, compare, memory** (#133, `media/media_state.h`, `MediaLibrary`):
  - *Favorites* are paths kept in `~/.local/state/fugue/media.json` (`{"favorites": [...], "mode", "view",
    "only_favorites"}`; a file that is gone from the disk stays until `prune`). A row carries `favorite`; the
    *Favorites* toggle beside Refresh narrows the Results view to them (`MediaLibrary.onlyFavorites`).
  - *Move to trash* asks first, in the actions' place ("Move <file> to the trash?", *Move to trash* and *Cancel*;
    Escape cancels), then `QFile::moveToTrash` (the desktop's trash, never a delete); a failure changes nothing.
    The run folder's record is left as it is; the file drops out of the list because it is gone.
  - *Open workflow* opens the workflow embedded in a PNG or WebP (11.7) as a new tab (`Workflows::openPath`);
    *Re-run* does that and runs it (`run.start`). The embedded workflow holds the seed it was made with, and
    only new nodes get a fresh seed, so the same seed runs again. Both are off for a file with no workflow.
  - *Send to Load Image* (`ImageInputs::sendFile`) takes the picture into the inputs folder as a dropped
    file is (byte for byte, named by its content, 14.1) and points the one selected Load Image at it, else makes
    a Load Image node at the pointer; one undo step; the notice says which. Images only.
  - *Zoom*: in Single and Filmstrip an image of known size shows in `CompareView`'s single mode: the wheel
    zooms about the pointer, a drag pans, `+`/`-` zoom, `0` fits, `1` is 100% (Fit and 100% buttons).
  - *Compare*: C (or the Compare button) pins the current image as A; the image chosen next is B and the two
    show in `CompareView` (Slider, Slider horizontal, Side by side, Flip; no Difference picture is made
    here). C or Escape leaves it. A change of the list ends it.
  - *Remembered*: the browser's mode, whether the workflows panel was on Files or Results, and the favorites filter
    are kept in `media.json` and restored with the next panel.
- **Not built** (follow-ups of #133): hosting the browser on a node's face and in the large view, a Media
  Browser node, Preview showing a batch as a filmstrip; the details of a video or sound (container tag,
  11.7); saved files that are in no run folder (a scan of the output folder grouped by embedded workflow);
  drag back onto the graph; zoom and pan of a video; fine-grained compare of a pair with a difference picture.
- **Not built** (follow-ups): a model library tab (M), assets, a log or terminal panel at the bottom,
  bookmarks and a preview of a node in the library.

### 12.15 Bars and panels: the menu and focus mode

Every bar and panel can be shown and hidden (#129), as Photoshop's Window menu does. One small registry
keeps which are shown (`panels/chrome_visibility.h`: plain C++, no QML, tested alone); each bar
registers an id with it and the window shows the bar while the id is shown. The commands, the menus,
focus mode and the file all read that one place.

| Id | Command | What it is |
| --- | --- | --- |
| `menu-bar` | `view.toggle-menu-bar` | the menu bar; not shown, it **auto-hides** (below), never disappears |
| `workflow-tabs` | `view.toggle-tabs` | the workflow tabs (12.10); still only from the second workflow on |
| `run-toolbar` | `view.toggle-run-toolbar` | the run toolbar (12.13); the queue panel hangs from it and goes with it |
| `selection-toolbar` | `view.toggle-selection-toolbar` | the toolbar above a selection |
| `breadcrumb` | `view.toggle-breadcrumb` | the group breadcrumb |
| `run-status` | `view.toggle-run-status` | the run pill at the top right |
| `zoom-readout` | `view.toggle-zoom-readout` | the zoom pill at the bottom left |
| `app-inputs` | `view.toggle-app-inputs` | the App view's inputs panel (shut, the strip) |
| `side-tabs` | none | the side tab strip (12.14): shown by default, hidden by focus mode, no toggle of its own |
| `node-library`, `workflows-panel` | `panel.nodes`, `panel.workflows` | the side panels (12.14), one at a time (an exclusive group of the registry) |

- **Bars and panels** (a submenu at the end of the View menu, after fit, zoom and links, with **Focus mode** under it): one row per bar with a check while it is shown, then the
  panels (`panel.nodes`, `panel.workflows`, `panel.queue`), then **Focus mode** and **Reset bars and
  panels**. The same menu is a submenu of the graph's context menu, so a right click finds it even
  with the menu bar hidden.
- **Focus mode** (`view.focus-mode`, **H** on the graph): hides every bar and panel; H again brings
  back exactly what was shown, no more and no less. Tab is not used: it already enters a group and opens
  node search (12.4). While it is on, what is asked for shows alone: a panel opened with N or W, the
  menu bar slid in with F10, which goes out again by itself (below). Focus mode is not
  remembered: a window always opens with its bars, so a window closed in focus mode loses nothing. A line
  at the bottom says how to leave it for four seconds.
- **Auto-hide menu bar** (#160, `panels/AutoHide.qml`). Hiding the menu bar (View > Bars and panels > Menu bar,
  unchecked) does not remove it: it collapses to a slim hot edge, 4 px tall across the window's top edge, and a
  **handle** (a menu glyph, 36 x 28 px, hanging under the edge at the left after the side strip and panel; at the
  right in a right-to-left language) that stay drawn, so the way back needs no hidden knowledge. The bar waits just
  above the window and slides down **over** the graph (the bar overlays: nothing under it moves or resizes while it
  slides, and the graph is pushed only while the bar is *shown*, pinned). The slide moves the bar's `y`, not the layout:
  170 ms, ease out (`Easing.OutCubic`).
  - **In**: the pointer on the edge or the handle for 120 ms (a pointer passing is not a request), a press on the handle
    at once, **F10** (it slides in and opens the first menu, as ever) and an **Alt mnemonic** (Alt+F and the rest keep
    working while it is out: they slide it in and open the menu).
  - **Out**: the pointer off the bar, edge and handle for 400 ms; a command chosen; the menu closed (with the pointer
    not on the bar); **Escape** when no menu is open (the graph's and a field's own Escape come first). While a menu of
    the bar is open the bar stays. After a key or a command the pointer may still rest on the edge: it must leave and
    come back before the bar slides in again, so Escape is never undone at once. A touch screen has no hover: a tap on
    the edge or handle brings it in and it stays 4 s (or until a command or a menu closes).
  - The group breadcrumb moves right to clear the handle while it is drawn.
  - The container is one component: **`AutoHide.qml`** takes any top bar as its child (`pinned`, `edgeShown`,
    `holdOpen`, `reveal()`, `conceal()`); it is used for the menu bar only; see the follow-ups below.
  - Focus mode (H) still hides everything, **the edge and handle too**; a bar asked for in focus mode (F10, an Alt
    key) slides in alone over the graph and out again, and leaving focus mode restores what it was (shown, or
    auto-hide). F10 does not change the remembered mode.
  - Screen readers: the handle is a button named "Show the menu bar" with a press action; the bar's own items are
    not exposed while it is out (F10 and the Alt keys are the keyboard way, as in every desktop menu bar).
- **Nothing is lost.** The menu bar holds the commands that bring the others back, and it is never gone: the handle
  and the edge stay (except in focus mode, where H and the line at the bottom are the way out), F10 and the Alt keys
  slide it in, the graph's context menu has the same Bars and panels submenu, and **Reset bars and panels** (a key
  if the keymap file binds `view.reset-bars`) shows every bar, the menu bar pinned, and closes the panels. No notice
  is needed when hiding it. A newly hidden bar keeps its command in the keymap: any `view.toggle-...` can be bound in
  the keymap file (12.5).
- **Remembered** in `~/.local/state/fugue/chrome.json`: `{"visible": {"<id>": true|false}}`, an entry per
  choice made; the menu bar's entry is a mode, `"shown"` or `"auto-hide"` (written as that word since #160; a file
  that says `false` for it, as before, reads as auto-hide and `true` as shown; Reset writes `"shown"`). An id this version does not know is kept as it is; a file that cannot be read gives the
  defaults. The run toolbar's older `visible` in `run-bar.json` and the side panels' older `tab` in
  `side-panels.json` are read once, when `chrome.json` has nothing for them; those files keep the toolbar's
  place and batch count and the panel's side and width.
- The queue panel (`panel.queue`) is open or not for the window's life, and is not an id of its own:
  it hangs from the run toolbar, so hiding the toolbar or entering focus mode hides it, and it comes
  back with the toolbar.
- Not built: workspaces (a named set of shown bars, Photoshop's Essentials), a minimap (12.9, later), a
  status bar and a bottom panel (none exist). Follow-ups of #160: auto-hide for the workflow tabs (done as a side effect of
  #262: they are in the menu bar's row, so one reveal brings both)
  and for a collapsed run toolbar (a floating toolbar has its own grip and place; an edge reveal does not fit it);
  a setting for the delays; an animation-off setting (the slide then jumps).

### 12.16 Areas and view types (#217)

The window's body, the place under the menu bar (the workflow tabs are in its row) beside the side strip and panel, is a tree of
**areas**, each showing one **view**, as Blender's screen is areas each showing an editor (epic #217; the research and
the order of slices are [workspaces-research.md](workspaces-research.md)). Slice 0 (#220) built the container; slice 1
splits, resizes, heads each area with a view-type menu and adds the **Canvas** view. Fugue serves hobbyists and film
teams: the **default window stays one area showing the Graph**, item for item what it was before areas, with no
header, border or file; areas are there for whoever asks for them (View > Areas, or a key).

- **Names.** A *view type* is what an area can show. The node graph is the **Graph** view (`graph`; the code's older
  names keep "canvas" for it: `FugueCanvas`, `App.canvasModel`); the picture of a canvas (#216, #221) is the **Canvas**
  view (`canvas`). A new name never calls the node graph "canvas".
- **The area tree** (`workspace/area_tree.h`, plain C++, tested alone). A *leaf* is an area: an id, its view type and
  a state per view type it has shown (switching an area to another view and back finds the first one's state again,
  as Blender's `spacedata`). A *branch* lays two or more children along one axis, each with a **share**: fractions
  that sum to 1, so a window resize scales every area in proportion. The axis is `row` (from the start edge to the end
  edge, mirrored in a right-to-left language) or `column` (top to bottom): never left and right. **Split** gives a new
  area a share of an area's place, after or before it, as a sibling inside a branch of the same axis or as a new
  branch across it; **close** gives an area's share to the child beside it in its branch (the one before it, or after
  it when it is the first; a branch left with one child becomes that child); **join** merges two leaves side by side;
  **swap** trades what two areas show (view and states) and keeps their places; **resize** moves the border between
  two children by pixels and keeps each at its views' **minimum size**; **equalize** gives a border's two children
  equal shares. Along a branch the minimum is the sum of its children's, across it the largest; laid out, no area
  goes under its minimum while the minimums fit (when they do not, each shrinks in proportion to its minimum), and
  every border falls on a whole pixel. At most 64 areas, 16 levels deep.
- **Saved form**, format 1: `{"format": 1, "root": NODE}`, a NODE being `{"type": "leaf", "area": 1, "view": "graph",
  "state": {"graph": {...}}}` or `{"type": "branch", "axis": "row", "children": [{"size": 0.62, "node": NODE}, ...]}`.
  Keys this version does not know are kept and written back, and so is a view type no view registers. A tree that does
  not validate (another format, a node that is neither a leaf nor a branch, an area id twice, a size that is not a
  positive number, too deep, too many areas) gives the default tree; redundant shapes are normalized (a branch of one
  child, a row inside a row, shares that do not sum to 1).
- **The view registry** (`workspace/view_registry.h`): one row per view type, as the side panels' table, so a new view
  is one row: id, name (translated), glyph, the QML component, minimum size (the area's, header included),
  multiplicity (`many`, `one-per-target` for an editor of one node or canvas, `one-per-window`).

  | Id | Name | Glyph | Component | Minimum | Multiplicity |
  | --- | --- | --- | --- | --- | --- |
  | `graph` | Graph | `workflow` | `canvas/FugueCanvas.qml` (declared by the window, adopted) | 160 x 120 px | one per window |
  | `canvas` | Canvas | `image` | `workspace/CanvasArea.qml` | 240 x 180 px | many |
  | `app` | App | `layout-template` | `apps/AppArea.qml` | 800 x 360 px | one per window |

  A component other than the Graph is an `AreaView` (`workspace/AreaView.qml`): made with its area's id and the
  state its type has there, and it says what to keep (`areaStateEdited`) only on a change of its own. A type no row
  registers is drawn as a placeholder that names it and keeps it in the tree (`AreaPlaceholder.qml`, 160 x 120 px);
  the header's menu picks another view for it.
- **The Graph always has exactly one area** (slice 1). It is one per window until a second Graph has its own pointer,
  link drag and zoom requests (research 5.5, risk 2), and the run toolbar, node search, the context menu and the
  window's dialogs hang from it, so it never leaves the window: the area showing it cannot switch to another view;
  choosing Graph in another area **moves it there** (the two areas trade what they show, the other area's canvas
  too); closing its area gives the Graph to the area that takes the place. A **split's new area** shows the view of
  the area split when that view may be shown twice, else the first registered view that may (so splitting the Graph
  gives a Canvas). A split is refused when the tree would not fit the body (`AreaTree::fits`), and the window starts
  no split from a body too small.
- **The layout** (`workspace/workspace.h`, `App.workspace`): the tree, the **active area** (the one last pressed or
  given the keyboard focus; the commands act on it, and the area that a split makes or a close leaves becomes it),
  and the commands. Kept in **`workspaces.json`** in the root's state folder (`~/.local/state/fugue/`, or the
  project's own), the saved form above, written whole (`engine/atomic_file.h`) 400 ms after a change, so a border
  drag is one write, and at quit. **A layout of one Graph area is never written**, and coming back to it (closing
  down to one area, Reset layout) removes the file: the default window reads nothing and writes nothing. At start the
  file is read once; one that does not read (not JSON, larger than 1 MiB, not format 1, a tree that does not
  validate) gives the default window, is kept beside as **`workspaces.json.bad`** (replacing an older one), and the
  window's notice line says so once. A layout without the Graph gets it in its first area; a second Graph area shows a
  Canvas. The active area is not kept (the Graph's area is active at start), nor focus mode.
- **Commands** (context `app`; in **View > Areas**, a submenu after Focus mode):

  | Id | Menu | Key | What it does |
  | --- | --- | --- | --- |
  | `area.split-row` | Split side by side | `Ctrl+\` | the active area gets a new area after it, side by side (VS Code's split key) |
  | `area.split-column` | Split top and bottom | `Ctrl+Alt+\` | the same, one over the other |
  | `area.close` | Close area | Ctrl+Alt+W | the area beside it takes its place; off with one area |
  | `area.view-graph` | Graph | Ctrl+Alt+G | the active area shows the Graph (it moves there); checked where it is |
  | `area.view-canvas` | Canvas | Ctrl+Alt+C | the active area shows a Canvas; off in the Graph's area |
  | `view.reset-layout` | Reset layout | none | one area showing the Graph again; off with one area |

  The keys are free in every context and none is a text field's (12.4); a keymap file can rebind them (12.5).
- **Drawing** (`workspace/AreaNode.qml`, recursive: a branch makes one per child). An area is not an item: every view
  goes straight into a *host* item at its rectangle, so an area adds no item, and a resize moves views without
  reparenting them. A view the window already has is *adopted* (Main.qml declares the Graph; its area takes that item,
  and never makes, moves out of the host or destroys it); another is made from the registry's component, or is the
  placeholder. New sizes in the same shape move the same views; another shape makes the nodes again, and a view
  already made for the same area and type is **kept** and moved (a split or a close leaves the other views as they
  were). Views sit at z 0, border handles at 1, headers at 2. Tested offscreen with a split tree
  (`tests/workspace/test_area_node.cpp`).
- **The window.** With one area (`Main.qml`: one `AreaNode`, `objectName: "areas"`), the canvas is where the window's
  column puts it, with its `objectName`, its margins and everything hung from it, and there is no host: the window has
  the same items and item names as before areas (`tests/workspace/test_area_window.cpp` against
  `tests/workspace/data/default-window.txt`) and writes no file. **On the first split** the column gets a host
  (`AreaHost`, `objectName: "areaHost"`, a C++ item that sees each press in it before the item under the point does,
  without taking it, to make that area active) in the canvas's place with the canvas's margins, and the canvas moves
  into it as its area's view; what was placed from the canvas's `x`, `y` and height (the side strip, the side panel,
  the auto-hide menu bar's handle) is placed from the body (the host, or the canvas with one area), so it does not
  move. The run toolbar and queue panel, node search, the context menu and the dialogs stay hung from the canvas and
  so stay inside the Graph's area. Back to one area, the canvas returns to the column, the host and what it held go,
  and the window is item for item the default one again.
- **Area header** (`workspace/AreaHeader.qml`, `areaHeader-<id>`, one per area, only while there are two or more; 7u =
  28 px tall, `popup` color, a hairline foot): the **view-type menu** at the start (`areaType-<id>`: the view's glyph,
  its name, a chevron; a click opens the list of view types, the current one checked, Graph marked "moves here" in
  another area and Canvas off in the Graph's area with "the Graph keeps an area"), and at the end **split side by
  side** (`areaSplitRow-<id>`, the `columns-2` glyph), **split top and bottom** (`areaSplitColumn-<id>`, `rows-2`), a 2u gap, then **close**
  (`areaClose-<id>`, the `x` glyph, a danger tint and a red glyph on hover or keyboard focus), each 6u = 24 px square, with its key in the tooltip. A
  press in the header makes its area active; the active area's header is tinted a little more (no accent foot: it read as a drag line, owner 2026-11-25) and has its name and glyph in
  the text and accent colors. The ends keep 3u clear of the border handles, and the header at the body's top start
  keeps its menu clear of the auto-hide menu bar's handle. **Focus mode (H) hides the headers** with the other bars
  (12.15); the views take their place and the borders stay.
- **Borders** (`workspace/AreaHandle.qml`, `areaHandle`, one per border while there are two or more areas): a press
  target 24 px (6u) across the border and as long as the branch, drawn as a 1 px hairline, 2 px in the accent while
  hovered, pressed or focused; the split cursor. A **drag** puts the border under the pointer, each side kept at its
  minimum; a **double click** gives the two sides equal shares; with the keyboard focus (Tab, or a press) the
  **arrow keys** along the axis move it 2u (Shift: 10u) and **Enter** shares equally. Mirrored for a row in a
  right-to-left language: the keys move it the way they point on the screen. The shares are what is kept.
- **The Canvas view in an area** (`workspace/CanvasArea.qml`, `canvasArea`): `CanvasView.qml` with the context the
  File > Canvas… dialog gives it: the session's store, and as the follow target the one Canvas Region selected in the
  Graph (which also moves the view to that node's canvas). The canvas it shows is the area's state,
  `{"canvasId": ...}`, kept with the layout, so a window opens on the canvas each area showed; a kept canvas that is
  gone gives the newest. A run's layer lands on the latest version and is followed, as in the dialog (14.5).
  Its own keys (0, 1, + and -) work while it has the focus, which a press in it gives. File >
  Canvas… stays: the dialog is the quick look, the area the one that stays open beside the Graph.
- **Not built yet** (later slices of #217): workspaces and presets (named layouts, tabs), drag to split or swap from a
  corner, join from the header, maximize, the Queue, Results, History, Viewer, Mask editor and Properties views, the
  active area's own keys and undo (every key still belongs to the window: `canvas` stays active while a Canvas area
  is), a view following the selection or pinned beyond the Canvas's region, a new window, and the Corner Handles and
  Border Width settings.

### 12.17 The App view (#385, epic #397)

A workflow whose document has an `app` block (4.9) opens as one simple screen instead of the graph when its `defaultView` is
`app`. The screen has no graph or node vocabulary in it.

- **A tab mode in one area, a view type in a split (#450).** With one area the App view is a mode of the workflow's tab: the
  window's body shows it in the canvas's place (`Main.qml`, `appPageItem`, made only while shown). With several areas, `app` is a
  view type (12.16): an area shows `apps/AppArea.qml`, which holds `AppPage.qml` over the same `AppView` store (one store, a
  form, runs and takes per tab), beside the Graph or a Canvas; the page is made only while the shown workflow is an app (else
  the area says "This workflow is not an app."), and no page exists while no area shows the App. In a split `AppView.shown`
  is whether the **active area** shows the App (`AppView::setAreas`), so Ctrl+Enter, Ctrl+Alt+Enter, Ctrl+V and the bare keys
  act on the App in its area and on the graph in the Graph's; the tab's own mode is kept for the one-area window. `view.toggle-app`,
  the App | Graph switch and the rail's Graph item ask the areas (`Workspace::toggleApp`): the active area shows the other view;
  an area showing the App takes the Graph (the two areas trade, 12.16); the Graph's area takes the App from the area that shows it,
  or with none gives it to the next area; any other area shows the App. Asking for the App view (Apply and open, a chosen app)
  brings it to the active area unless one shows it (`requestApp`). Splitting from the one-area App view shows the graph and a
  Canvas (the App is one pick away in the new area's type menu). A window
  with no app tab has exactly the items it had (`tests/workspace/data/default-window.txt`). A block that does not read
  (`app_invalid`, a later `format`) or a document with none opens as an ordinary workflow; for a block that does not read the
  window says so once per tab, in the notice line other load problems use: "This workflow's app block cannot be used, so it
  opens as a graph: <why>", or "...was made by a newer Fugue..." for a later format (#387).
- **The screen** (`apps/AppPage.qml`, `appPage`), in the layout of ComfyUI App Mode (its docs and screenshots only: layout and
  behaviour, no code, CSS, icons or branding; GPL-3.0 and AGPL-3.0). Three parts side by side, mirrored in right-to-left (a
  picture never is):
  - the three parts are rounded cards (the kit group's corner, a hairline edge) 2u apart on the window's background, in the look of the Drift kit (docs/canvas-look.md section 20, epic #456); the window's own side strip and panels are not shown in the App view;
  - the **rail** (`AppRail.qml`, `appRail`, 14u wide, at the start edge): each item an icon over a short word, with tooltips and Tab: **Apps**
    (`appRailApps`, first, "All apps (Ctrl+Shift+A)": `app.show-all`, the apps shelf's grid in `AppsDialog` over the page, opened on the current app's card; choosing a card opens that app as a new tab, and in an
    app's own window switches that window to it in place, 12.18), **Results and pictures** (`appRailResults`, `AppLibrary.qml`:
    a popover with search and a grid of this app's takes, which show in the stage, and of the recent pictures, which become the
    picture input) and **Graph** (`appOpenInGraph`, Open in Graph; not in an app's own window);
  - the **stage** (`AppStage.qml`, `appStage`, the wide middle): a thin progress line at its top; a toolbar; the result; under it
    "1392 × 752 · 100%" (the size and the zoom, quiet) with **Fit** and **100%**; the takes strip and the shown take's inputs
    in one quiet line (below);
  - the **panel** (`appPanel`, at the end, a quarter of the window, at least 80u and at most 40%, until its border `appPanelHandle` is dragged: then that width, kept between runs, and Enter or a double click on the border gives the automatic one back; dragged under half its least width, or **Hide the inputs** (`appHideInputs`, in the hero's corner), or Window > App inputs (`view.toggle-app-inputs`), it slides shut to a 9u strip with **Show the inputs** (`appShowInputs`) and a round **Run** (`appStripRun`)): a `KitHero` (`appTitle`) with the app's preview picture (the Dusk gradient without one), its serif title and one-line description (whole in a tooltip), and in its corner **Add to launcher** (`appLauncherToggle`, a frosted round pin) and, in the app's own window, **Open in Fugue** (`appOpenInFugue`); the form scrolling (`appForm`: each input
    of `inputs[]` in the block's order, `AppInput.qml`, `appInput-<key>`: a small dim label over a compact control; the
    `Advanced` ones folded under a quiet **Advanced** row, `appAdvanced`; an optional picture or mask (4.9, #484) starts empty and carries the quiet hint "Optional" under its label, and left empty its branch is out of the run); and **Run** (`appRun`, the one primary control, full
    width, 9u tall, pinned at the panel's foot, "Run (Ctrl+Enter)" in its tooltip). A "Number of runs" stepper is not offered:
    a tab holds one run at a time, and queuing several of one bound workflow is the run service's.
  The **toolbar** (`appToolbar`) is, while a run waits or goes, **Cancel this run** (`appCancel`, a normal button: danger red
  is for deletion) and where the run is ("Waiting for its turn…", "Working… 40%"); otherwise **Run again** (`appRunAgain`: this
  take's inputs, run), **Reuse inputs** (`appReuseInputs`: this take's inputs back in the form), **Save as…** (`appSaveAs`: a copy
  of the file, `AppView::saveResult`), **Clear** (`appClear`, shown once there is something to clear: the stage back to its
  empty state and the takes strip emptied, owner 2026-11-25; the files stay on disk and in the results store, and the newest run
  cleared is recorded in `app-takes.json` beside the runs root, so a restart does not bring the cleared takes back) and
  **…** (`appMore`: Open large, Show in folder, Use as input, Compare with the input). The **result** is a picture in
  `CompareView` (single mode, the large view's viewer): the wheel zooms about the pointer, a drag pans once zoomed, a double
  click fits (at Fit it opens `LargeViewDialog`), **Z** toggles 100%, **C** compares with the input picture (slider mode over
  the form's first picture), Home or 0 fits, 1 is 100%, + and - zoom. When the block has a `compare` (4.9; Expand), the
  result is A shown whole and the input is B drawn at its place in it (`CompareView.placeB`, from `AppView::inputPlacement`,
  computed from the shown take's own values, not the form's): an original square picture is a square, the room the app added
  above and below it shows as the strip around it, and the slider wipes between the two as an A/B. Placement applies in the
  slider modes and flip; side by side and difference keep fitting B to A's frame (no shared frame to place B in). Without a
  `compare`, or when a size or the take's picture file is unknown, B is fitted to A as before. The graph's Compare node and its
  dialogs never place. A video or sound plays, a text is text; with more than
  one output a small row names them by `label`, else "Result". While a run goes the Sampler's step picture (`node.preview`,
  `AppView::liveFrame`) shows over the result, else the previous result stays; the empty stage says in one line what to do.
  A failed run shows one line in the error color (the engine's own sentence) and a quiet **Details** (`appDetails`) with the
  rest and the hints; a cancelled run says nothing.
- **Takes** (#389, `AppView::takes`, `apps/app_takes.*`, `AppTakes.qml`, `appTakes`): every run of this app is a take, newest first,
  at most 30: a thumbnail (a 14u square) with the shown one outlined, a turning badge while it waits or goes and a mark
  when it failed; a run cancelled before it made anything leaves none. A take keeps the form it was run with (a quiet line:
  "a red fox · Steps 3 · seed 12"). The stage shows the newest take that made something, or the one the person picked
  (`selectTake`; picking the newest follows new runs again); a new run is shown once it has made a result, the earlier one stays
  until then. **Takes survive a restart**: the app owns no outputs, so a take is a run of the existing run history (9.7). The App
  view stamps the run's bound copy (`workflow.json`) with the `app_take` record in the document's `extra` (4.1; outside the
  source hash, so no cache key changes): `{"app": <key>, "values": {...}, "modes": {...}}`, the form as bound. The key is the
  app's workflow file, the saved file or, for an app opened from the catalog (a tab with no file of its own, whose runs carry no
  workflow path in History), the template's own `workflow.fugue`; an app with neither keeps session takes only. Showing the app
  reads the run folders newest first (at most 400 looked at), keeps the ended runs (ok or failed) whose record has this key, and
  lists them below the session's; so takes follow the app across projects and roots as History does (the run folders are one
  per-user store, not per project) and last as long as History does (run folders go 7 days after the run ends, 10.5). A take read
  back has no pictures yet: its results come from `RunService::replay` (the queue panel's "show results"; the same thumbnails a
  live run makes, from the content-addressed cache, so a run whose result is cached shows at once), asked for the newest take
  when the app is shown and for each other take only when the strip draws it (`AppView::ensureTake`), so a long history costs
  nothing until it is looked at. **Random seeds are recorded**: a seed left to Random is drawn by the form (in the input's
  range, as the engine would) and bound as a plain number, so the record, the take's line ("seed 4291") and the run all hold
  the number the picture came from; the form's own mode stays Random for the next run. `reuseInputs` and `runAgain` put a take's
  values back (that seed, in Fixed), so Run again with a take's inputs makes the same picture, and with the cache the same
  result at once. Keys in the strip: Left and Right move, Enter or Space shows the take, **R** runs again with the inputs of the
  take that has the place (the toolbar's Run again acts on the shown one).
- **Live and Repeat** (#471, `AppView::setLiveMode`, the "Run again" chooser at the panel's foot above Run: Off, On change, Repeat, `appLiveMode`;
  the canvas Live toggle's wording, #315): off at first, per tab, for the session only. **Live**: a change
  of an input made by the person (typing, a slider, the dice, a seed mode, a picture or model chosen; never a value the store
  moves itself, such as the seed Increment advances after a run) arms a 600 ms debounce, restarted by each change, and when the
  form has been still it queues one run. A run of the tab that still waits for its turn is cancelled first and leaves no take (it
  was superseded); a run that is working is never cut: the owed run starts when it ends. A tab not shown keeps the run owed until
  it is shown. **Repeat**: only for an app with a seed (`canRepeat`); switching it on starts a run at once when none goes, and
  each run that ends well starts the next while a seed is Random or Increment (a new picture after another). With every seed
  Fixed the next run would be the same picture (from the cache, at once: the owner's strip of identical takes, 2026-11-25), so
  Repeat waits and says so (`appRepeatWaits`) until a seed varies; a failure ends the chain (the mode stays on for the next manual run) and
  **Cancel turns Live and Repeat off**, since the person said stop. One mode at a time. Both run through the one queue and
  keep one run per tab in it, so the queue never grows; the window losing the focus changes nothing (the queue is the cap).
  Nothing is armed or polled while both are off (no timer runs). Both switches are Tab stops and Space or Enter flips them;
  they are hidden while the app has a problem (Run cannot).
- **Controls per kind** (#386, `AppInput.qml` picks, no control for an input the block does not bind): *text*, always a
  wrapping well that grows with its text up to 8 lines, then scrolls (#567; a long default reads whole, never as one line
  that scrolls sideways): the prompt roles start three lines tall, any other text one line tall. Enter is a new line only for
  the prompt roles and a target whose widget is `multiline`; in any other text Enter types nothing and a pasted line break
  becomes a space. Tab leaves it, Ctrl+Enter runs; *number*, a drag field with
  its range (`min` and `max`) as a slider with the value in a small editable field at its end (`AppSlider.qml`: a press or drag sets
  it, arrows step, a double click puts back the default), else a typed field (empty means the workflow's own); *toggle*, a switch (Space or Enter);
  *choice*, a drop-down; *seed*, one row of the height of the other fields: the number, a small dice for a new
  one and its mode as a small drop-down as a node's seed has it: Fixed, Random (the engine's own `{"auto": "random"}`; the
  field then reads "New each run"), Increment (the run uses the number shown, the next the next one, wrapping at `max`); a
  number typed makes it Fixed. Fixed and Increment always show a number (the workflow's, else a new one) and every run uses
  it; going from Random to Fixed keeps the number the field had, or picks a new one (a run's own random seed is not known
  to the form). *image* (`AppImageInput.qml`): a row of a dropdown and a folder button (the file picker) over a dashed well that shows the
  picture fitted, says "Drop, paste or browse" while empty, takes a drop, and has a remove (x) button while the pointer is over
  it. The dropdown lists, in groups with a thumbnail each and a search field, **Samples** (the template's `inputs/` pictures,
  its own sample first: the block's default, else the picture the workflow holds), **Recent** (the inputs folder's newest) and
  **This app's results** (so a result can be fed back in: it is copied into the inputs folder), and **Use sample** whenever
  the template has one. An app opens on its sample, so Run works at once; a picture from elsewhere is copied into the inputs folder as Load Image takes one (the same
  bytes are one file). Ctrl+V (and Edit > Paste) with a picture on the clipboard puts it into the focused picture input,
  else the form's first, in the full window's App view and in an app's own window (a focused text field keeps Ctrl+V for
  text); a picture dropped anywhere on the page goes to the same input. *mask*: Paint mask opens the mask editor (14.4) over the form's first picture input, "Painted" or
  "None", Clear; with no picture chosen it says so. *model*: the model menu of a node's picker (the providers' models, the
  files in the model folders with the Recommended one marked, #370, the catalog's models under Download, which asks first
  with the license, size, source and folder and fetches through the window's one download line); a file whose hash is not
  known is read first. Image from the canvas document (#206) is not a source yet.
- **Keys.** Every control takes Tab; Enter or Space opens or flips it; Ctrl+Enter (Run) works from any text field, a prompt
  included, and Ctrl+Shift+Enter, Run ahead of the queue, with it.
- **Run** binds the form's values into a copy of the workflow (`bindApp`, 4.9) and queues it through RunService like any run:
  one queue, one GPU, the queue panel lists it, `fugue runs` sees it. The tab's document and file are never changed. Ctrl+Enter
  (also ahead of the queue) and Ctrl+Alt+Enter act on the app while it is shown and on the graph otherwise. The app owns no
  outputs: a run's results are the existing run history's, which the takes strip reads back (above).
- **Open in Graph** (the rail's Graph item, which runs `view.toggle-app`) switches this tab to the graph, the same workflow as an ordinary tab: the whole window, as for any
  workflow (window bar, tabs, the graph's header and its menus, the side panels), with the nodes fitted in the canvas as it is
  now, also when the window was resized or tiled while the shelf or the App view hid the canvas. **App | Graph** is the way back
  and forth: two segments at the window bar's far end (`appViewSwitch`, before the project chip), always shown (a
  bar under 56u wide waits), the view in force marked, labels "App | Graph" in every state; pressing the chosen segment does
  nothing and the other does what the tab needs: an app workflow, its other view; the apps shelf (App chosen), Start blank; a
  plain graph with nodes (Graph chosen), the app builder (Make app, Ctrl+Alt+M; cancelling leaves Graph chosen); the empty graph
  Start blank gave (Graph chosen), the shelf again (`AppShelf.showAgain`). The same switch is `view.toggle-app` (Ctrl+Alt+A,
  Window > App view, checked on the shelf too, Graph > View > App view). It brings the form back rebuilt from the workflow as the graph
  left it (values kept for inputs that still exist). A workflow with no app block has no switch and the command is off. The choice is per tab and kept for the session; a tab opens in the view its
  `defaultView` names. A run started from the App view does not paint its results on the graph's nodes.
- **The apps shelf** (#388, `apps/app_shelf.h`, `AppsHome.qml`, `AppsDialog.qml`). Fugue has no start screen; a window with nothing
  open (the shown tab is empty, untitled and unsaved: also a new tab made over an app) shows the shelf in the canvas's place, and File > All apps… (`app.show-all`, Ctrl+Shift+A, also Fugue's mark at the window bar's start) shows it
  as a dialog over open tabs. It lists the templates that are apps, the same list the Templates window reads
  (`Workflows::templates()`, 11.10), never a second catalog: a card has the app's name, its one line (`summary`), its
  preview, the models-where badges (#164) and one line of readiness, a check and "Ready" when its models are on this machine
  or on a paired host and no pack is missing, else what is missing in few words ("Needs 2.1 GB and 1 pack": the models'
  download size when known, else their number; and the packs not installed). Ready apps come first. Choosing a card opens the
  template as any template opens (`openTemplateById`, 11.10): a ready app lands in the App view; an app that is not ready
  opens the same tab with the one Get ready sheet (6.6, 6.7, 8.9, #379) over it, and once the sheet's one yes has run the
  tab is shown ready to Run. Nothing is installed, trusted or downloaded before that yes. **Start blank** ends the shelf
  for the session (an empty graph). A window that is given a workflow or reopens its tabs never makes the shelf: the
  catalog is read only when the shelf is shown (`AppShelf::refresh`). With no apps it says so and offers **Browse
  templates…**.
  In the Drift kit (#463): a Dusk panel (`AppShelfView.qml`, one component for the page and the dialog) with a `KitHero` header (the
  frosted Browse templates and Start blank buttons), a search field (`KitSearch`, a recessed well) and kit cards (`KitCard`,
  raised) over `KitPicture`s that move (the app's WebM, 11.10) while in view and the window is focused. **Search, pins, copies,
  names** (#470): the search narrows the cards live by every word in the title, summary or kind (`AppShelf.query`, `shown`);
  a pinned app (the pin on its picture, Ctrl+B) comes first and stays pinned for the person (`app-bookmarks.json` in the state
  folder, ids only); **Duplicate** makes a whole copy as the person's own app (`<id>-copy` in the own templates folder, titled
  "<name> copy", the original untouched) and opens it in the App view; **Rename** changes the title of one of the person's own
  apps only (`template.json` and the workflow's app block), never a bundled or inherited one. Keys: the search has the focus on
  open; Down or Tab go to the cards, the arrows move, Enter or Space open, F2 renames, Ctrl+D duplicates, Ctrl+B pins, Menu or
  Shift+F10 opens the card's menu; Escape closes the dialog.
- **Code** (`apps/app_view.h`): `AppView` holds the logic and no QML (`isApp`, `shown`, `fields`, `setValue`, `setSeedMode`, `setImageFiles`, `pasteImage`, `useImage`, `maskPicture`, `models`, `pickModel`, `run`, `cancel`,
  `status`, `progress`, `error`, `details`, `results`, `takes`, `takeIndex`, `shownResults`, `selectTake`, `reuseInputs`, `runAgain`, `saveResult`, `useResultAsInput`, `useSample`, `liveFrame`, `openInGraph`, `showApp`); it takes the run as a function, so a test feeds
  it the events a RunService sends. `apps/app_window.*` ties it to the open tabs, the runs and the run commands.

**Needs panel (#469).** While the shown app is not ready, or a run failed for a missing model or pack, a quiet persistent block at the head of
the input panel (not a toast, in the app's own window too) lists each missing item with its size and licence: a Moss header line ("This app
needs 2 models (6.0 GB)"), one row per item, **Download** for one model, **Download all** for the lot, and for builders **Show in graph**
(switches to the graph and centres and selects the node that holds it; not in the app's own window). The rows are the Get ready plan
(`engine/readiness.h`, asked again whenever the workflow, its models or packs, or a run's outcome change; `apps/needs_model.h` is the
store). Download all goes through the Get ready path: a plan of models only starts at once (the click is the yes, nothing to approve);
a plan with a pack opens the sheet for the one yes, as does a pack row's button. One model downloads on the window's download line. The
panel clears by itself when a refresh finds the app ready. Keys: Tab reaches Download all first, then each row's buttons.

**Runs on a host (#395, #581, epic #397).** When the app's models are not on this machine and the paired verdict (8.7, "One verdict")
says the helpers that answered take every node that needs them, the form shows a quiet line above Run, "Runs on gx10" (`appRunsOn`;
several hosts are comma separated; nothing for a local run), and the Needs panel marks the model rows as the host's to have (below). The
verdict is made again whenever the plan or the document changes, asynchronously, reusing a host answer less than 30 s old (`describe`, 10 s at
most); a host that does not answer leaves the Needs rows as they were and the line says "gx10 is not answering" (the i18n string
`%1 is not answering`). The run goes where the scheduler puts it (8.7, "Where a node runs when its model is not here"): a host that has gone,
with no copy of the models here, ends the run with the error "The model files are not on this machine, and the paired host gx10 is not
answering." in the form's error line and Details, like any run error; with a copy here the run is local. Choosing a host or a mesh peer is #203's, not this.
The helper cannot say which models it has (`describe` lists no model of a helper that takes its files in the job, and the protocol has no
other question for it), so the Needs panel keeps every model row and marks each "on gx10 if it has it" (the Download still works here); a
host that does not have the model answers `model_missing` at the run (#493). When a node of the app cannot go anywhere (pinned to a
provider on this machine, or no helper offers the node's capability, 8.7), the form does not say "Runs on", and the panel names the
node and why as a line without an action (#491), pins first, even when no host answered.

The same line says where an app runs when a node is routed to a service off this machine (#404, 8.2): "Runs on Cloud (api.example.com)"
(several services are listed, comma separated), from the graph's check, and it takes the place of a paired host's name. Running such an
app asks the one question of 8.2 first.

### 12.18 An app as its own application (#398, epic #397)

A `.fugueapp` (an app bundle, 11.5, #394) is the same window's file type: the desktop opens it with `fugue --app <file>` and a `.fuguepack` with `fugue <file>`;
`--app` and `--edit` take a package by its bytes, not its name.

`fugue --app <id|file|folder>` opens one app in a window of its own: the App view (12.17) and nothing else. No menu bar, tabs,
graph, node library or panels; the title is the app's (the block's `title`) and the icon its template's `preview.*`, when
there is one. `<id|file|folder>` is a template's id, a workflow file, a template folder or a `.fuguepack` (the words of
`fugue app info`; a path that exists wins over an id). It is the same `fugue` binary, the same registry, run queue and helper;
a second `--app` is a second process (no single-instance plumbing yet), which starts with the helper and the model cache the
first one warmed. A name that is no app is a message on stderr and exit code 2; a file that has no usable app block opens the
window with "This is not an app." and an Open in Fugue button.

- **The window** (`apps/AppWindow.qml`, `appWindow`): the page (`appPage`, with `standalone`: no Open in Graph) and a notice line
  at the foot that is the full window's (`workflows.notice`) with **Get ready…**, which opens the Get ready sheet (6.6, #379)
  inside this window: a not-ready app (packs, trust, environments, models) goes through it at the first start, once. Ctrl+Enter
  runs the app, `?` shows the keys. The window keeps no tab session and no recovery copies of its own (they go to a scratch
  folder), does not record its project as the last one, and closing it, or Ctrl+Q, quits at once and never asks about unsaved
  work. Closing it touches no other window's, no CLI run and not the shared helper.
- **Add to launcher / Remove from launcher** (`appLauncherToggle`, in the App view of the full window and of the app's window;
  `apps/app_launcher.h` for the shown app, `apps/launcher_entries.h` for the files): writes
  `$XDG_DATA_HOME/applications/fugue-app-<key>.desktop` (`Type=Application`, `Name` the app's title, `Comment` its description,
  `Exec=fugue --app <ref>` quoted as the Desktop Entry spec asks, the AppImage's own path in place of `fugue` when run from one,
  `Icon` the copy of the preview in `$XDG_DATA_HOME/fugue/app-icons/` or `fugue`, `StartupWMClass=fugue-app-<key>`,
  `X-Fugue-App=true`). `<ref>` is the template's id when the catalog has that folder, else an absolute path; `<key>` is the id, or
  a path's file stem with a short hash of the path. The app's window sets its application id (Wayland app_id, desktop file name) to
  `fugue-app-<key>`, which ties the window to its entry in the launcher and the task bar. Remove deletes the entry and its icon
  copy and touches nothing else. The buttons are absent for a tab that has no file and no template folder (a new workflow: nothing
  a launcher could start).
- **Keep this app** (`appKeep`, a small round button beside the launcher pin, #394): shown only for an app opened from a package
  (`AppLauncher::keepable`: the shown app is a package and nothing is missing any more, so after the one Get ready sheet). It unpacks the package into the person's
  templates folder under its folder name (`-2` on a clash) as a template, writes its launcher entry for that template's id (so the entry starts it by name,
  `fugue --app <id>`), and then offers the pin to remove it as for any app. The bundle is not moved; the packs it carried were installed by the sheet.
- **Apps** (the rail's, 12.17) in an app's own window: choosing another app opens its tab in this window and shows it as the App view (`bindOwnAppWindow`), so the title (the view's), the window icon (that app's preview), the launcher entry and Open in Fugue become that app's, in the same process; the first tab stays open behind it.
- **Open in Fugue** (`appOpenInFugue`, the app's own window): starts `fugue --edit <ref>`, the full window with that app's
  workflow as a tab, shown as the graph. A template is opened as a new unsaved workflow (as the Templates window does), a file
  as that file. `--edit` also works from a terminal.

### 12.19 Make an app: the builder (#393, epic #397)

File > Make app… (`app.make`, Ctrl+Alt+M) turns the workflow on the canvas into an app (4.9) with no JSON by hand; File > Edit app…
(`app.edit`, shown only while the workflow has a block) is the same dialog on the block there. Both work from the graph and from the
App view, and an app block that cannot be read starts an empty builder that replaces it on Apply. Inside an entered group the builder
refuses and says to leave the group first: an app is made of the whole workflow. The logic is `apps/app_builder.*` (`AppBuilder`, the
canvas model's `appBuilder`); `apps/AppBuilderDialog.qml` only draws it.

- **The draft.** The builder works on a copy of the block (`app::App`, 4.9) and never touches the document until **Apply**, which writes it
  with `app::setApp` as ONE undo step on the document ("Make app", "Edit app"; Remove app is "Remove app"), so Ctrl+Z takes the block out
  again, and the App view reads it at once. Cancel, Escape and closing throw the draft away. Applying a draft equal to the block pushes
  no step. An input or output of a block loaded for editing whose node is gone from the workflow is left out with a sentence (as
  `pruneApp` does everywhere).
- **What it holds** (the group interface editor's rules, 5.4, where they fit): the **title** (required), **description** and **Opens in**
  (`defaultView`: App or Graph); the **inputs** in the form's order, each with a label (the placeholder is the name the form would
  say), **Advanced** (the one section the form folds), a range for a number (the placeholder is the target's own), a default, the inner
  inputs it sets and **Also set…** (one input drives several of one type, never of two types), Move up and Move down (Alt+Up and
  Alt+Down in a row) and a button to stop asking for it; the **outputs**, each with an optional label and the same ordering. Not in the
  builder (kept as the block has them): choices and their numbers, `optional`, `fallback`, other section names. The role is guessed
  from the input the way the templates name it: a Load Image's `file` is `image`, its `mask` is `mask`, a seed-widget input `seed`, a
  model picker `model`, a text input named `prompt` or `negative_prompt` that role, anything else a plain input keyed by its name (a
  second picture keeps its role under a name of its own).
- **Adding.** By clicking, or from a list, so a keyboard needs no pointer. **Pick on the canvas** closes the dialog and puts the canvas in
  click-to-expose mode (`canvas/AppPickLayer.qml`, over the canvas while `appBuilder.picking`): the input row or node under the pointer
  is outlined, a click on an input row adds it (a second click takes it out), a click on a Save or Preview node adds its result as an
  output (a second takes it out), a click on any other node says in the banner that it has no result; what the draft already has is
  outlined in the accent. The wheel and the middle button still pan and zoom the canvas. **Done** in the banner or Escape ends picking
  and the dialog comes back (from the App view the window goes to the graph for the picking and back after). **Add an input…** lists
  every unlinked value input of the workflow (node title, then the input) and **Add an output…** every Save and Preview node. An input
  that is linked, only takes a connection (an image wire), or is in the app already is refused in one sentence.
- **Checking.** `problems` (the red line above the buttons) says in words what Apply refuses: no title, nothing to show, no output
  ("Pick at least one output: the Save or Preview node whose result the app shows"), and every `app_invalid` that `app::checkApp` gives
  (an output that is not a sink, a target that is linked or of mixed types); Apply and Apply and open are off while any stands. The
  model can add an output that is not a sink (it is shown in the error tint and named in the problems); a click on the canvas never does.
- **Preview.** The right pane is the App view's own form: `AppView::fields` of a second, idle `AppView` given the workflow with the draft's
  block, drawn with `AppInput` and nothing in it editable, so a label, a range, Advanced or a new input shows as the form will show
  it, with the title, the description and what the app shows.
- **After Apply.** **Apply** leaves the window where it was (the graph stays the graph); **Apply and open App view** (the primary) shows
  the App view (`AppView::showApp`); the key and the switch in the window bar (`view.toggle-app`) are there after either.
- **Keys.** Tab goes through the title, description, Opens in, Pick on the canvas, Add an input, each row (label, Advanced, Move up, Move
  down, remove, its range, Also set…), Add an output, its rows, then Remove app (when editing), Cancel, Apply, Apply and open. Space,
  Enter or Down opens a list, the arrows choose and Enter picks; **Enter in a text field applies**, Escape cancels.
  `test_app_builder_window` drives it with keys only.
- **Words** are the group editor's where the thing is the same (Move up, Move down, the x that stops exposing, Advanced), and the form's
  own where it is not ("Pick on the canvas", "Add an input…", "Sets <node>").

---

## 13. CLI and agent interface

Fugue must be usable by agents. The design is one command table: every operation exists once and
is exposed through the CLI now, and later through an MCP server and a live-session channel. There
are never three implementations.

### 13.1 Commands

Each command works with no display, never opens a dialog, and takes `--json`. For `node`,
`pack`, `provider`, `model` and `template` the plural is an alias (`fugue nodes list` is `fugue node list`).
`fugue run` starts a run and `fugue runs` manages runs; those two are different commands.
`--help` or `-h` after a command (`fugue run --help`, `fugue canvas diff -h`) prints that command's usage lines and runs nothing; `fugue --help` lists them all.

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
| `fugue pack list`, `add <folder\|git-url> [--ref <tag\|commit>]` (one question: install, setup and trust), `remove <id>`, `update [id] [--ref <tag\|commit>]`, `trust <id>` (an `<id>@<major>` where majors of a pack sit side by side) | Packs ([6.6](#66-installing)); `list` says where each comes from (`own`, or `inherited from Default` in a project that inherits `packs`, with the warning of 1.4.2 about what an inherited pack writes). All but `list` need a human ([13.6](#136-safety)). |
| `fugue provider list` | Instances, their capabilities and status. |
| `fugue template prepare <id>`, `fugue workflow prepare <file>` | Get a template or workflow ready with one question (#379, [6.6](#66-installing)): lists the packs to install and trust, each environment's setup command and size, and the missing models (file, licence, size, host, folder) with the totals and the free space where they go, asks once on a terminal, then runs the list (install, trust, setup, downloads) and stops at the first failure, naming what is left; a rerun lists only that. Nothing is ready to ask: `ready: true`. With no terminal it fails with `needs_confirmation` and prints the list; no flag answers for the person. Reply: `{plan, ready, done?}`. |
| `fugue template list` | The templates, each with the models it names (`here` or not, and the folder a missing one goes in), their `families`, the packs it lacks, whether it is an `app` (`false`, or `{title, summary, kind}`, 11.10), its `source` (name, author, url, note, as `template.json` gives it, `{}` when none), and where it comes from (`own`, `inherited from Default`, `bundled`); what is wrong with one is a warning. |
| `fugue model get <file\|template id> [--into <model folder>]` | Download the models a workflow or template names that are not here and have a `source` (8.9, #89), into the model folder downloads go in (`--into` names another, for this call; an inherited folder is refused as read-only); progress on stderr. Each failure is an error; what arrived stays. |
| `fugue model find-folders [--only <place,...>]` | The model folders the person already has (8.9, #619), by folder names and file sizes: the same finder as the first-run setup. Read-only (no model file is opened or hashed, nothing is written). `--only` names the places to search (a path or id; the others are never touched). `--json` data is `{folders: [{tool, path, alsoReached, models, size, counts, countCapped, network, layout}], searched, capped, elapsedMs}`. |
| `fugue model list [--type <resource type>] [--family <name>] [--hash]` | Models found on disk and offered by providers; a file has `family` (null when none) and `recommended` (with `reason` when true, #370), and `--family` (case and punctuation aside, "flux.2 klein 9b") lists only the files of that family; each file says which root it is in and whether that is the project's own or inherited from Default (`origin`), and `roots` lists the model folders searched. A file on disk has a `handle` only once its hash is known (listing reads no model file); `--hash` hashes the others, once each, so every file gets a full handle; a file it cannot read is a `media_unreadable` warning. |
| `fugue cache status` | What Fugue keeps under `~/.cache/fugue` ([10.6](#106-cache-management), #141): per kind (results, thumbnails including the media browser's, runs) the bytes, files, oldest and limit, and the results per workflow and per node. |
| `fugue cache clear [--kind results\|thumbnails\|runs\|all] [--workflow <file>] [--node <id>] [--older-than <age>] [--dry-run]` | Clear one kind (results unless `--kind` says), narrowed by workflow (results and runs), node (results) and age (`30d`, `12h`, `2w`); `--dry-run` says what would go and changes nothing. Frozen results, saved outputs and a run going or queued are never removed; while a run is going its results and previews are left alone (the `cache_busy` warning). |
| `fugue canvas new [--name <text>] [--size <w>x<h>] [--background transparent\|white\|black\|#rrggbb]` | Make a canvas ([14.5](#145-the-canvas-revision-store-206-208)) with one paint layer, default 1024 x 1024 (at most 16384 a side and 67 megapixels), empty (`Layer 1`) or, for a background other than `transparent`, filled opaque (`Background`, #232); prints its id and first revision. |
| `fugue canvas import <file.ora\|.png\|.jpg\|.webp> [--name <text>]` | Make a canvas from an OpenRaster file (the paint file format, [14.4](#144-masking-and-inpainting-167)) or from a plain picture: a PNG, JPEG or WebP becomes a one-layer canvas of its size, the layer named after the file (by its content, not its extension). Anything else is `media_unreadable` and names the kinds supported. Its first revision has the producer `import`. A `.fuguepack` that carries a canvas (found by its `canvas.json`, not its name) is imported as a **new** canvas with its revisions as shared (head only or history), never over one: the packaged id is kept when free, else the canvas gets a new id and `data.renamed` is true with `data.packagedId`. The package is untrusted: every file is read by a hash checked with `isSha256`, never by the entry's own name; a tree must hash to its name, a revision to its id, a tile's PNG header must give the size its place gives and its pixels hash to its name (`canvas_corrupt`); entries outside the folder, links, a file over 64 MB, more than 200000 entries or 4 GB unpacked are `invalid_document`; unpacking is inside the store's root and a failed import leaves no canvas. |
| `fugue canvas export <id> <file>.fuguepack [--history]` | A package to share (#249, [11.5](#115-export)): the canvas's head with its layers and no history (a new first revision), or with every revision it came from (`--history`; candidates are left out). The same `.fuguepack` zip as a workflow's: `<name>/canvas.json` (format, kind `canvas`, id, name, mode, revisions), `trees/<hex>.json` and `tiles/<hex>.png`. Never over a file; combined with a flag of the picture export (`--layers`, `--format`, `--quality`, `--lossless`, `--scale`, `--size`, `--width`, `--height`, `--background`, `--estimate`, `--only-visible`, `--no-masks`, `--prefix`) or with a revision (`@<revision>`) it is `usage`, as is `--history` with a target that is not a `.fuguepack`; `canvas_not_found`, `canvas_corrupt`, `value_out_of_range` (over 3 GB or 5000 revisions). `fugue canvas import <file>.fuguepack` reads it back (below). |
| `fugue canvas export <id>[@<revision>] <file>` | Write a revision (the head by default) by the file extension (#214): `.ora` is the layered file (the same revision gives the same bytes); `.png`, `.jpg`/`.jpeg` and `.webp` are the flattened picture, made by `PaintDocument::flattenToPicture`, the flatten the history window shows (PNG is the very bytes of `flattenToPng`). JPEG (quality 92) has no alpha and is flattened onto white, and `data.background` is `white` with a note in the text; WebP is lossless with alpha, at most 16383 px a side. Any other extension is a `usage` error; a file already there is never replaced. A picture answers `data.format` too. Options (#247, #248; `engine/canvas_export.h`, shared with the Canvas view's Export with quality and scale… and Export layers as files… dialogs, `canvas/canvas_export_model.h`): `--format png|jpg|webp|ora` (else the extension; a file name with another known extension is `usage`), `--quality 1-100` (jpg default 92; webp lossy at that quality unless `--lossless`; `usage` for png and ora), a size by exactly one of `--scale <n>%|<factor>`, `--size <w>x<h>` or `--width`/`--height` (one of them keeps the proportions; resampled with Lanczos on premultiplied alpha; `input_invalid` past the canvas size limit), `--background transparent|white|black|#rrggbb` (laid under the picture; jpg is white by default and `input_invalid` for transparent; `data.background` is answered unless transparent), `--estimate` (encodes, prints `data.bytes`, `width`, `height`, writes nothing, a file is optional with `--format`); `data.width`, `height` and `bytes` are always answered. `--layers` (positional 2 is a folder, made when missing) writes one file per paint and picture layer, bottom first, named `[<prefix>-]<NN>-<layer name>.<png|webp>` (NN the position from the bottom, 1-based, at least two digits; names made safe by `safeFileName`), the layer's own pixels (opacity and blend are not applied; the layer mask is, when it has one on, unless `--no-masks`); `--only-visible`, `--prefix`, `--format png|webp`, `--quality`/`--lossless`; mask layers are left out (`data.skipped`); `data.files` lists `{position, layer, file, bytes}`; every name is checked before the first file is written, and any taken is `permission_denied` with nothing written. |
| `fugue canvas batch <action> <in-folder> <out-folder> [--prompt <text>] [--seed <n>] [--keep] [picture options]` | A canvas action over a folder of pictures (#250; `engine/canvas_batch.h`, the code the Canvas view's Automate > Batch… dialog runs through the window's own runs). `<action>` is a file (an existing path) or the id of an action in the bundled actions (`share/actions`). Every picture directly in `<in-folder>` (`.png`, `.jpg`, `.jpeg`, `.webp`, `.bmp`, `.tif`, `.tiff`, `.ora`, in name order) is imported as a temporary canvas (producer `batch`), the action is bound to it as the Canvas view's Generate binds it with the **whole canvas** as the selection (padding and scale from the router), its workflow is run like `fugue run` (a queue entry and a detached worker, so it waits its turn for the GPU), and the head the run left is exported with the picture options of `canvas export` (`--format`, `--quality`, `--lossless`, `--scale`, `--size`, `--width`, `--height`, `--background`; PNG when none) to `<out-folder>/<input name>.<ext>`, made when missing. A name that exists, or that an earlier picture of the batch took, becomes `<name>-2`, `-3`, ...: nothing is replaced. The temporary canvas is removed afterwards unless `--keep`. `--prompt` fills the action's prompt input (`input_invalid` when the action needs one and none is given), `--seed` fixes the seed of every picture (else each gets a random one). A picture that cannot be read, whose run fails or whose file cannot be written is reported `failed` with its code and message and the batch goes on; SIGINT or SIGTERM cancels the run under way and the pictures not started are `cancelled`; the exit code is 1 when any is not `ok`. Output is a line per picture; with `--json` one JSON object per line: `batch.started` {total}, `item.started` {index, total, file}, `item.done` {index, total, file, status, output?, code?, error?}, `batch.done` {ok, failed, cancelled} and the last line `batch.report` {report: {folder, total, ok, failed, cancelled, items}}. Nothing starts (an ordinary error envelope) for `file_not_found` (no such action or folder), `input_invalid` (no pictures, a missing prompt), `action_invalid` (the file is not an action, it is broken, or its `selection` is `bounds`: Expand needs a crop frame), `usage`. |
| `fugue canvas actions` (alias `canvas act --list`) | The canvas actions the Canvas view's task bar lists (#231 slice 10; `engine/canvas_act.h` `collectActions`, the listing the bar's `CanvasActions` uses): the template library's workflows with a valid `action` block first, then the files compiled into Fugue (`share/actions`), the first of an id winning. Per action: `id`, `name`, `description`, `selection`, `needs_prompt`, `has_seed`, `has_layer`, `needs` {`models`, `models_missing` (by the model library, nothing hashed beyond what is remembered), `pack` (the name of a pack its nodes need that is not installed, else empty)}, `broken` (+ `reason`) and `inputs` [{key, label, advanced, choices?, default?}]. |
| `fugue canvas act <id>[@<revision>] <action> [--prompt <text>] [--seed <n>] [--count 1-4] [--layer <name\|position>] [--rect x,y,w,h \| --selection <mask.png>] [--expand left\|right\|top\|bottom --percent 25\|50 \| --pixels <n>] [--set <input>=<value>]... [--wait]` | A canvas action run as the task bar's Generate runs it (#231 slice 10): the checks, Expand's growth of the canvas, the router's padding and scale, the layer role, Count N and the bound copy of each run are `engine/canvas_act.h` `planAct`, which `CanvasActions::generate` calls too; this command only reads the arguments, queues each run like `fugue run` (`startBoundRun`) and, with `--wait`, waits (`waitForRun`; SIGINT or SIGTERM cancels the run under way and the rest). `<action>` is an id of `canvas actions` or a workflow file with an `action` block; `--rect` is in canvas pixels (held inside the canvas); `--selection` is a PNG the size of the canvas whose first channel is the selection (above 0 selects), stored by content with `CanvasStore::storeSelection` as the lasso's is and bounded by the painted area (`mask_empty` when nothing is, `usage` for another size); `--rect` and `--selection` are left out, with a note on stderr, by an action whose `selection` is `bounds` or `none`. `--count` is for an action with a seed (`usage` otherwise); the first layer is visible and 2..N hidden with ` (n of N)` after the name, seeds `--seed`+n (random each without). `--layer` is the layer a layer-role action works on (Remove background), else the one layer Canvas Region reads (a position, 0 the bottom, or a unique name). `--percent`/`--pixels` apply to `bounds` actions only. `--set <input>=<value>` sets an Advanced input of the block (not prompt, seed or layer; a choice input takes one of its words, case-insensitive; else JSON when it parses, as `fugue run --set`) and `--set <node>.<input>=<value>` an input of a node in the run's copy. Without `--wait` it answers at once `{canvas, action, previous, runs: [{run, seed?}], expanded?, status: "queued"}`. With `--wait` the answer is `{canvas, action, previous, head, status: "ok", layers: [{name, visible}] (those new on the head), canvases? (Canvas New's: Upscale makes a new canvas and leaves this one as it was), expanded?: {revision, width, height}, runs, notice?}` and a failed or cancelled run is an error (`cancelled`, or the run's code with the bar's plain words, `(n of N run(s) finished.)` appended) with exit 1; the runs not started are cancelled; a fully cached Write after an undo is placed on the head again when its run ended ok, and a failed or cancelled run places nothing of its own (#337): how a Generate ended is `engine/canvas_act_outcome.h` `ActOutcome`, which the bar reads too (design decision 5). A cached Write that cannot be placed is an error with exit 1. Progress is on stderr (`node ...`, `node 40%`, `node done`/`(cached)`, prefixed `[n of N]` for Count). Refused before anything is changed: `file_not_found` (no such action: `valid` lists the ids), `canvas_not_found`, `action_invalid`, `pack_missing`, `model_missing` (the message names the missing files, the hint says `fugue model get <action>`; this check comes before Expand's growth), `act_refused` (select an area, type a prompt, pick a side, name a layer), `mask_empty`, `usage`. |
| `fugue app export <id\|file> <file.fugueapp>` | The app as a bundle to share (#394, [11.5](#115-export)): the template or workflow file's `app` block with its packs and sample files carried and its models as references; `.fugueapp` is added to a name that ends in neither it nor `.fuguepack`; never over a file (`permission_denied`). Data: `file`, `app`, `packs` (ids carried), `files` (sample files put in); warnings: a pack fetched rather than carried, a sample file left out. `no_app` (no block), `file_not_found` (no such app), `usage` (the file is missing), `app_not_shareable` (refused, one error per pack with no folder here and no source, and per model with no download source: nothing is written), `value_out_of_range` (over 10000 files). |
| `fugue app list` | The apps of this machine (#392, epic #397): the templates the catalog flags as apps (`app` in `fugue template list`, 11.10: a readable `app` block, 4.9), the person's own first, by the template's id. Per app `id`, `title`, `description`, `file`, `inputs` and `outputs` (counts); a template that cannot be read is a warning and left out. |
| `fugue app info <id\|file>` | What an app takes and gives: `id`, `file`, `title`, `description`, `default_view`, `inputs` [{`key` (the name, else the role), `role`, `label`, `type` (`image`, `mask`, `text`, `number`, `bool`, `model`, `choice`, ...), `required` (a picture or mask with none in the workflow, a prompt that is not optional and has no fallback), `advanced`, `default` (the block's, else the workflow's own value, a seed left to the engine as `{"auto": "random"}`, else the input's default), `min`, `max`, `choices` (the block's, else the target's, through a group instance to its inner input)}], `outputs` [{`node`, `label`}], `ready` and, when not ready, `needs` (the `prepare` plan, 6.6); what the workflow no longer has (a target, an input, an output) is dropped and reported as `app_stale` warnings. `<id\|file>` is a template id, or a workflow file (a path that exists wins over an id). Errors: `file_not_found` (no such app: `valid` lists the ids), `no_app` (the workflow or template has no `app` block), `app_invalid` (the block does not fit its graph; checked only when the app is ready, as a missing pack leaves its nodes unknown), `unsupported_format_version`. |
| `fugue app run <id\|file> [--input <key>=<value>\|<key>=@<file>]... [--out <dir>]` | Run an app without the window (#392): `app::bindApp` puts the values in a copy of the workflow (an input left out keeps its block default, else the workflow's own value; a seed is never made random), the copy runs through the queue like `fugue run --wait` (`startBoundRun`, `waitForRun`; progress on stderr, Ctrl+C cancels), and the files the app's output nodes made are printed, one per line (`--json`: `{app, run, status: "ok", outputs: [{node, label, files}], files}`); results are the run's own (`fugue runs result <run>`, History), the app owns no store. `--input image=<file>` is a picture or mask path (it must exist, and a run may read it only under the home folder, 13.6); `<key>=@<file>` is that file's text, for a prompt; a number input takes a number, a choice input one of its words, a model input a model file's name (as `canvas act --set model=`) or a whole handle as JSON. `--out <dir>` (made when missing) sets the `folder` of every output node that writes files (Save Image, Save Video, Save Audio); a Preview has none. Refused before anything runs, in this order: `file_not_found`, `no_app`, `unknown_input` (`valid` lists the keys), `missing_input` (one error per input a run cannot do without), `usage` (a value of the wrong kind), `app_not_ready` (a pack to install or trust, a model to get; the hint says `fugue template prepare <id>` or `fugue workflow prepare <file>`), `app_invalid`; then `cancelled`, or the run's own code (`node_failed`, `out_of_memory`, ...), exit 1. |
| `fugue --app <id\|file\|folder>` | The window, with one app only (#398, 12.18): the App view and nothing around it, titled and iconed as the app, for a desktop launcher entry. Not a command: it is the window's option, so it needs a display and may stand with `--home` and `--project`. `fugue --edit <id\|file\|folder>` opens that app's workflow in the full window. `fugue app open <id>`, the same as `--app`, is not there yet (it belongs in `src/cli/app.cpp`). |
| `fugue canvas log <id>[@<revision>] [--limit <n>]` | The revisions from the head (or the one named) back along their parents, newest first, 20 by default: revision, time, producer. |
| `fugue canvas show <id>[@<revision>]` | One revision: its tree hash, parent, producer, time and layers (position, name, kind, visibility, opacity, blend, layer mask, origin, tile count). |
| `fugue canvas list` | The canvases of the store, newest first: id, creation time, name, head. |
| `fugue canvas candidates <id> [--all]` | The candidates of a canvas ([14.5](#145-the-canvas-revision-store-206-208), #210), newest first: revisions that are not in the head's history and not accepted or dropped, each with whether it was made from the head; `--all` adds the dropped ones. |
| `fugue canvas accept <id>[@<revision>]` | Accept a candidate into the head: the head moves to it when it was made from the head; when it was made from an older revision and only added layers, a new revision with those layers on the head becomes the head (`rebased`); else `canvas_conflict`. With no `@revision`, the one open candidate (`usage` when there are none or several). |
| `fugue canvas drop <id>[@<revision>]` | Hide a candidate from the list (nothing is deleted; `accept` can still take it). A revision of the head's history or an accepted one is `canvas_not_candidate`. |
| `fugue canvas diff <id>[@<revision>] [<@revision>\|<id>@<revision>]` | What differs: with one revision, against its parent; with two, the first to the second (of one canvas or two). Per layer: added, removed or changed, the tiles that differ (counted whole: a tile is the unit of storage) and the rectangle of the pixels that differ (read from the changed tiles' pixels, so a 552 px box is reported as 552 px, not as the 1024 px of the tiles it touches; a layer whose tiles cannot be read, or a canvas that changed size, reports the whole tiles' rectangle), changed properties; and a size change. |
| `fugue canvas layer list <id>[@<revision>]` | The layers of a revision (#226), top first as a layers panel shows them, each with its position (0 is the bottom), name, kind, visibility, opacity, blend (`normal`, `multiply`, ...; an op Fugue does not draw is shown as it is stored), layer mask (enabled, tiles) and origin (producer, time); a group has kind `group` and its `children` (names, bottom first). |
| `fugue canvas layer set <id> <layer>... [--visible true\|false] [--opacity <0-1>] [--blend <mode>] [--mask on\|off\|delete] [--lock true\|false]` | Edit layers of the head ([14.5](#145-the-canvas-revision-store-206-208), `CanvasStore::editLayers`): each property given is one new revision whose parent is the head (in the order visible, opacity, blend, mask, lock), producer `canvas-cli:<op>`, pixels and tiles untouched. A `<layer>` is a position or a name (a name two layers share is `usage`). An edit that changes nothing makes no revision. `@revision` other than `head` is `usage`: only the head is edited. |
| `fugue canvas layer rename <id> <layer> <name>` | Rename one layer (1 to 200 characters) as one revision. |
| `fugue canvas layer rm <id> <layer>...` | Delete layers (with their layer masks) as one revision; `undo` brings them back. |
| `fugue canvas layer dup <id> <layer>...` | Duplicate layers: each copy (`<name> copy`, `copy 2`, ...) goes right above its layer, sharing its tiles; one revision. |
| `fugue canvas layer move <id> <layer>... --to <position>` | Move the layers as a block (their order kept) so the lowest of them ends at `--to` among the others (0 the bottom; past the top is the top); one revision. |
| `fugue canvas layer new <id> [<layer>] [--name <text>]` | An empty transparent paint layer (#252), right above `<layer>` or on top; named `--name` or `Layer N`. One revision. |
| `fugue canvas layer merge <id> <layer>` | Merge down (#252): the layer is drawn into the one under it, which keeps its place, name, blend and origin and gets the picture of both (its opacity and layer mask are baked in, so it has opacity 1 and no mask). Both must be shown paint layers or groups (`input_invalid` for a hidden, mask or picture layer, or the bottom one). One revision, producer `canvas-cli:merge-down`. |
| `fugue canvas layer flatten <id>` | Every shown paint layer and group becomes ONE layer, `Background`; hidden layers are dropped, a mask layer stays above it; a canvas that is one plain layer already changes nothing. `input_invalid` with a picture layer. One revision. |
| `fugue canvas layer group <id> <layer>... [--name <text>]` | Puts the layers into a new group (#252, 14.5) at the place of the highest of them, in their order; named `--name` or `Group N`. A group is one layer of the revision that composites its children and has its own opacity, blend and mask. One revision. |
| `fugue canvas layer ungroup <id> <layer>...` | Replaces each group by its children. `input_invalid` for a layer that is no group, or a group that is hidden or has its own opacity, blend or layer mask (it would lose them). One revision. |
| `fugue canvas resize <id> --to <x>,<y>,<w>,<h> [--fill edge-blur\|edge\|gray\|transparent] [--blur <px>]` | Grow, shift or cut the canvas (#240, `CanvasStore::resize`, `engine/canvas_resize.h`): ONE new revision of the head (producer `canvas-cli:resize`) whose canvas is the rectangle `x,y,w,h` of the old one, in the old canvas's pixels (x and y may be negative to grow left or up; the rectangle may reach past any side; it must hold part of the old canvas and be within the canvas size limit, else `input_invalid`). Paint and mask layers and layer masks are cut and re-placed (the picture does not move on screen; a layer mask is white in a new area), a picture layer stays a placeholder. When it grows and `--fill` is not `transparent`, a new bottom layer `Expanded area` (producer `canvas-resize`) holds the pre-fill of the new area only, made from the flattened visible layers by `engine/edge_fill.h`: `edge-blur` (default; the nearest picture pixel stretched into the area, then blurred by `--blur` px, 24 by default, 0 to 512), `edge` (stretched only) or `gray` (0.5). The old picture lies over that layer unchanged. `undo` takes it back. |
| `fugue canvas undo <id>` | Move the head to its parent (`canvas_no_step` for the first revision) and mark the revision left `undone`, so it is not a candidate. Any head can be undone, a Canvas Write's included. |
| `fugue canvas redo <id>` | Move the head forward to the newest undone revision whose parent is the head (`canvas_no_step` when there is none). |
| `fugue project list` | The registered projects ([1.4.2](#142-projects)): name, folder, state (`ok`, `missing`, `invalid`), what each inherits, and the one this session is in; the Default project first. |
| `fugue project show [<name\|path>]` | One project: with no argument, the one this session is in (`default` with none). |
| `fugue project new <name> [--in <dir>] [--inherits a,b]` | Make a project (its `project.json`) and register it. `--in` is its root (made if needed; it may not hold a project already), else `<base data>/projects/<slug>`; `--inherits` is a list of `models`, `packs`, `templates`, `prompts`, `preferences` or `none`, default `models,packs,preferences`. A word that cannot be inherited is a `project_inherits_ignored` warning. |
| `fugue project add <dir>` | Register the project already in `<dir>` (again is fine). |
| `fugue project remove <name\|path>` | Take a project off the list; its folder and files are never touched. |
| `fugue project inherits <name\|path> <items\|none>` | Replace what the project inherits (`models`, `packs`, `templates`, `prompts` and `preferences` take effect, see 1.4.2). |
| `fugue settings list` | Every setting in effect in this session with its value and where it comes from (`own`, `inherited from Default`, `built-in`); the entries of `model_paths` that are inherited are named. A bad value in either settings file is a warning. |
| `fugue command list` | The command registry and current key bindings. |
| `fugue skill install [--target agents\|claude\|all] [--dry-run] [--force]`, `fugue skill uninstall` (same options) | Install, update or remove the bundled agent skill ([13.5](#135-agent-skill)). |

Exit codes: 0 for success, 1 for any failure. A run that ends `failed` or `cancelled` under
`--wait` exits 1. 2 is a session that could not start because of its root or project selection
(`--home` and `--project` together, an unknown or unusable project, a root that cannot be made); nothing ran.

`--home <dir>` and `--project <name|path>` may stand anywhere on the line and take no part in a command's own options ([1.4.1](#141-one-root-fugue_home), [1.4.2](#142-projects)).

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
| `pack` | On `unknown_node_type` for a type of a pack (not a built-in or `local/` type): the pack's id. The window words the error from it (11.6). |

Codes in v0.1:

| Area | Codes |
| --- | --- |
| Files | `file_not_found`, `invalid_json`, `invalid_document`, `unsupported_format_version`, `permission_denied`, `media_unreadable`, `download_failed` (no source, a source or name Fugue does not take, an error status, a break it gave up on, no room, a file at the name), `download_mismatch` (the file is not the handle's, by hash or size; deleted) |
| Graph | `unknown_node_type`, `node_version_mismatch`, `node_newer_major` (a warning), `unknown_input`, `missing_input`, `unknown_link_source`, `unknown_output`, `type_mismatch`, `value_out_of_range`, `value_not_in_choices`, `cycle`, `group_recursion`, `unknown_provider` |
| Warnings | `not_part_of_output`, `model_missing` (an error when a run reaches the node, 8.5), `model_mismatch` (a text encoder, VAE or LoRA that does not fit its diffusion model, by the files' headers, #85, #86; an error when a run reaches a stage), `model_unsupported` (a model file in a format the helper cannot load, such as ComfyUI's mixed quantization; likewise), `frozen_result_missing`, `pack_update_available`, `workflow_not_embedded`, `node_bad_preview` |
| Trust | `pack_untrusted`, `needs_confirmation`, `consent_needed` (a run would send data to an off-machine provider instance the person has not allowed, 8.2, #404; `fugue run --allow-remote <instance>` allows it for that run) |
| Packs | `pack_invalid`, `pack_exists`, `unknown_pack`, `fetch_failed`, `setup_failed` |
| Groups | `group_invalid` (an edit a group cannot take: a list input fed from both sides of the selection, a name taken or not valid, a port exposed already) |
| Providers | `capability_unavailable`, `model_not_running`, `provider_unreachable` |
| Canvas (#208) | `canvas_not_found` (no such canvas, revision or stored tree; or a canvas with no revision asked for its head), `canvas_ambiguous` (a revision prefix that starts more than one), `canvas_corrupt` (a stored object is not the content it is named for, or is not well formed); #210: `canvas_not_candidate` (accept or drop of a revision that is in the head's history or already accepted), `canvas_conflict` (a candidate made from an older revision that changed more than it added layers cannot be placed on the head), `canvas_head_moved` as an error of `fugue canvas accept` (the head moved while it ran; nothing changed); #226: `canvas_head_moved` also for a layer edit, `undo` or `redo` whose head moved, `canvas_no_step` (`undo` of the first revision, `redo` with nothing undone) ([14.5](#145-the-canvas-revision-store-206-208)) |
| Canvas nodes (#209) | `padding_raised` (a warning, Canvas Region 1.3.0, #242: Padding was below Feather + 16, so Canvas Write's blend would end in a hard edge at the region's box; it was raised to that), `canvas_head_moved` (Canvas Write, an error since 1.4.0, #239: the canvas changed size while the workflow ran, so the run's layer could not be placed on the head; the run says so in plain words and writes nothing visible; when it can be placed, it is, with no warning); `input_invalid` on `revision` when Canvas Region got a revision that was not named in full at compile; a canvas or revision that is not there fails the compile with `canvas_not_found` on the node's `revision`; a canvas input left blank is the Active canvas ([14.5](#145-the-canvas-revision-store-206-208), #246): it compiles to the active canvas's id, and only with no canvas in the root at all `input_invalid` on `canvas` ("Choose a canvas") is raised when the node runs |
| Canvas actions (#231) | `action_invalid` (a workflow's `action` block is not well formed, or does not fit its graph: a target node or input that is missing, linked or of the wrong type, no Canvas Region or Canvas Write, two Regions and no `region`, a Write onto another canvas; the message says which); a block of a later format is `unsupported_format_version`; `app_invalid` (the same for an `app` block, 4.9) and `app_stale` (a warning: a target or output of the app's block was dropped because its node is gone); `pack_missing` (the action's nodes are of a pack that is not installed, `fugue canvas act`, #231 slice 10), `act_refused` (`fugue canvas act`: the request is not one the action can run, in the words the task bar shows: select an area, type a prompt, pick a side, select a layer) ([14.5](#145-the-canvas-revision-store-206-208), docs/canvas-actions-design.md) |
| Apps (#392) | `no_app` (`fugue app info\|run`: the workflow or template has no `app` block), `app_not_ready` (`fugue app run`: the app's packs, trust or models are not here; the message says what, the hint names the `prepare` command); `app_not_shareable` (`fugue app export`, Export for sharing, #394: a pack the bundle can neither carry nor fetch, or a model with no download source; one error per item, nothing is written); a missing or unknown input is `missing_input` and `unknown_input`, a block that does not fit its graph `app_invalid` (4.9) |
| Masks (#167) | `mask_empty`, `mask_unsupported`, `stitch_mismatch`; warnings `mask_full`, `mask_resized` (Paint too) ([14.4](#144-masking-and-inpainting-167)) |
| Running | `node_failed`, `node_bad_output`, `node_timeout`, `out_of_memory`, `cancelled`, `node_test_failed` (`fugue node test`: an output is not what the test expects) |
| Cache | `cache_busy` (a warning: `fugue cache clear` left the results and previews alone because a run is going) |
| Projects | `project_invalid` (a name or `project.json` that cannot be used, or a folder that holds none), `project_exists` (a registered project has the name, or the folder holds one), `unknown_project`, `project_inherits_ignored` (a warning: a word of `inherits` that is unknown or can never be inherited) |
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
  a pack's `setup`, `template prepare`, `workflow prepare` (one question for the whole list), adding a provider or a key, starting or stopping a model server. From the CLI
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
  `fugue app run --out <dir>` is judged by the same roots before the folder is made, so an `--out` outside them is refused with nothing left behind.
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
| `fugue/load-image` | `file` (file picker) | `image`, `width`, `height` | Keyed by the file's content hash. The image is upright: an EXIF orientation is applied, and `width`, `height` are the size it is seen at. **Also reported before a run** (the kernel's `check`, a warning on the node under validate, [9.1](#91-compile)): a `file` that is not there (`file_not_found` on `file`), and a painted `mask` file that is not there (on `mask`). |
| `fugue/save-image` 1.1.0 | `image`, `name`, `format` (png, webp), `embed` (full, no-prompts, none), `folder` (optional) | `file` | Sink. Writes to the output folder. `name` may hold `{date}` (yyyy-MM-dd), `{time}` (hh-mm-ss), both local time at the save, and `{model}` (the file name without its extension of the diffusion model used by the nearest upstream node that has one: the model a provider node selected (also when its result is frozen), or the `diffusion_model` a Load Models node holds; never its VAE, text encoder, LoRA or an upscaler; made safe: folders in it are dropped, control characters removed and other characters than letters, digits and `._-+() ` become `_`; `unknown` when no such model is upstream), and `/` to make subfolders: `{date}/{model}_{time}`. The tokens are not part of the node's cache key (the typed name is), so a repeat run of an unchanged Save is a cache hit and writes no second file. Subfolders stay inside the folder saved to: `..`, a part starting with a dot, an empty part (so an absolute name), a backslash, a control character, or a link below the folder that leads out is an error on `name`. The folder is then opened once, folder by folder from the root without following a link (a missing one is made), and the file is written and renamed through that open folder (#360), so a link swapped in for the folder or any folder above it after these checks is refused (`permission_denied`) or has no effect: the file never lands outside. The folders are walked with `O_PATH` descriptors (a folder needs only the search permission of those above it), a part that is `.`, `..` or empty is refused, and the partial file is made exclusively without following a link. Save Video and Save Audio hand ffmpeg `/proc/<pid>/fd/<n>`, the open partial file itself, so this needs `/proc` and a process that may read its own `/proc` entries (not `PR_SET_DUMPABLE 0`; see docs/private-mode-research.md). Save Video and Save Audio take the same `name` (one shared code; 1.1.0 added the tokens and subfolders, #344, so a template that uses them is not listed by an older Fugue). Never overwrites a file: `<name>.png`, else `<name>-2.png`, `-3`, and so on (the end of a name that is 255 bytes long gives way to the number), also when several Save nodes write one name at once. [11.7](#117-embedding-in-outputs). **Also reported before a run** (the kernel's `check`, a warning on the node under validate, [9.1](#91-compile)): a `name` that cannot be a file name (`node_failed` on `name`; `{model}` counts as a word) and a relative `folder` that leaves the output folder (`permission_denied` on `folder`). Write roots and names already taken are a run's. |
| `fugue/preview` | `value` (any) | none | Sink. Shows the value on the node; from the CLI it is in the run result. |
| `fugue/generate-image` | `prompt`, `negative_prompt`, `model` (optional), `width` (optional), `height` (optional), `steps` (optional), `guidance` (optional), `seed`, `image` (optional start image), `strength`, `mask` (optional, 1.1.0: [14.4](#144-masking-and-inpainting-167)) | `image`, `seed`, `width`, `height`, `model` | Capability `image.generate`. Unset optional inputs use the model's defaults; an unset size is the start image's, else the model's default; a default of 0 (sd-server's "the model's own") is no default, so the server picks. |
| `fugue/edit-image` | `instruction`, `images` (list, 1 to the model's limit, else 4), `model` (optional), `steps` (optional), `seed` | `image`, `seed`, `width`, `height`, `model` | Capability `image.edit`; `instruction` is sent as its `prompt`. |
| `fugue/resize` | `image`, `width`, `height`, `fit` (stretch, contain, cover), `method` (nearest, bilinear, bicubic, lanczos, area) | `image` | ComfyUI's resampling. `contain` fits inside the box without padding, so the result can be smaller than the box on one side. |
| `fugue/resize-megapixels` | `image`, `megapixels` (default 1), `multiple` (advanced, default 16), `method` (as Resize, default lanczos) | `image`, `width`, `height` | ComfyUI's ImageScaleToTotalPixels: each side is `round(side * sqrt(megapixels * 1024 * 1024 / (W * H)) / multiple) * multiple`, rounded half to even, then stretched with Resize's resampling. No crop, so the shape can drift by the rounding. A side that rounds to 0 becomes `multiple`. `width` and `height` can feed an Empty Latent. |
| `fugue/upscale-image` | `image`, `scale` (default 2, 0.1 to 16), `method` (as Resize, default lanczos) | `image`, `width`, `height` | #134. No model: Resize's resampling by a factor (ComfyUI's ImageScaleBy), each side `round(side * scale)` half to even. The same kernel as Resize, so the same pixels for the same size. To fit a size use Resize; for added detail, Upscale with Model. |
| `fugue/upscale-with-model` | `image`, `model` (`fugue/upscale-model`), `tile` (advanced, default 512) | `image`, `width`, `height` | #134. Provider capability `image.upscale` ([8.1](#81-capabilities)). The size grows by the model's own scale. An alpha channel is resized with lanczos and put back (the helper takes RGB); a gray or 16-bit image is sent as 8-bit sRGB. A missing model file is `model_missing` on `model`. |
| `fugue/restore-image` | `image`, `model` (`fugue/seedvr2-model`), `vae` (`fugue/seedvr2-vae`), `factor` (default 3), `color_fix` (`lab`, `wavelet`, `none`; default `lab`), `seed`, `tile` (advanced, default 512), `overlap` (advanced, default 64) | `image`, `width`, `height` | #346. Provider capability `image.restore` ([8.1](#81-capabilities)). One node, not ComfyUI's scale, preprocess, conditioning, sampler and postprocess: the pieces between them are half-finished pictures and padded latents. The image is resized by the factor with lanczos (Resize's kernel, so the pixels are PIL's), written as an 8-bit PNG and restored by the helper; the colour fix then runs on the result against that same resized picture (`nodes/restore_color.h`, pure functions). `wavelet`: a 5-level à-trous split (kernel [1 2 1] / 4 on each axis, dilation 2^level, the edge repeated) keeps the result's high frequencies and the reference's low ones. `lab` adds, in CIELAB (sRGB, D65), a and b histogram-matched fully to the reference and L = 0.8 content + 0.2 matched. An alpha channel is resized with the picture and put back. Refused in the node before the image is decoded or resampled, as `value_out_of_range`: a `factor` that is not a finite number from 1 to 8; a `tile` outside 64 to 16384; an `overlap` above half the tile; an image whose result would be under 16 pixels on a side (on `image`, naming the minimum); a result over 16.8 MP (4098 x 4098, what a 24 GB card finishes with the 7B model) or 16384 on a side (on `factor`, with the largest factor and image size that fit, or on `image` when even factor 1 is too big). A missing file is `model_missing` on `model` or `vae`; a file of another model, a SeedVR2 file without its stored embeddings, or a VAE that is not SeedVR2's is `model_unsupported` on `model` or `vae` (read from the file's header on this machine; a paired host's helper checks its own). `model` is a `fugue/seedvr2-model` and `vae` a `fugue/seedvr2-vae`. Needs a helper (CUDA for the 7B). **Also reported before a run** (the kernel's `check`, a warning on the node under validate, [9.1](#91-compile)): an `overlap` above half the `tile` (`value_out_of_range` on `overlap`), and a `model` or `vae` that is not a SeedVR2 file (`model_unsupported`, from the tensor names in its header, read once and remembered; a file that is not on this machine is the scheduler's `model_missing`). |
| `fugue/upscale-video-with-model` | `video`, `model` (`fugue/upscale-model`), `tile` (advanced, default 512) | `video` | #138. Provider capability `image.upscale`, with `frames`. One node, not decode / upscale / encode nodes: the value between the parts would be a folder of thousands of frames, half-finished state that costs disk and is useless alone (3.7). ffmpeg decodes a chunk of frames to PNGs (64, fewer as the frames grow: about 30 frames of 1080p, at least 4), one helper job upscales the folder with the model kept loaded, ffmpeg encodes the result to a segment (H.264 CRF 12, Matroska) and the chunk's frames are deleted; at the end the segments are joined as they are with the source's first audio track copied. So memory is one frame and disk is one chunk in and out plus the result so far. The frame rate is the source's exact fraction and chunks are cut by frame number, so the frame count, rate, length and sound are the source's (a variable-rate video comes out at its average rate). The size grows by the model's scale; Upscale Video after it sets a final size. Progress is the frames done over the frames the length says; cancel stops ffmpeg and the helper job and removes the chunk folders and segments. Needs a helper on this machine. Not done: resume of a cancelled run (a new run starts over), a temporal option (see Appendix A). |
| `fugue/load-upscale-model` | `model` (`fugue/upscale-model`, the picker) | `model` | #134. The handle its picker holds, passed on, so one choice feeds several upscalers. Upscale with Model has the same picker on its own input. |
| `fugue/upscale-video` | `video`, `scale` (default 2, 0.1 to 8), `method` (as Resize, default lanczos) | `video` | #134. The video twin of Upscale Image: ffmpeg's `scale` filter on the streamed frames (never decoded here), each side rounded down to even; the frame rate, length and audio track (copied) are the input's; H.264 CRF 12 in Matroska (close to lossless, far smaller than FFV1 at the new size, so Save Video's encode is the only visible one). Progress is the output time over the length, and cancel kills ffmpeg. A pending trim or crop on the input is applied first. |
| `fugue/crop` | `image`, `x`, `y`, `width`, `height` | `image` | Visual (#73): its face shows the picture arriving at `image` (the last run's, or a Load Image's file feeding it directly; "No picture yet" before) with the frame over it and the outside dimmed. Dragging inside the frame moves it, a corner or edge handle resizes it (Shift keeps the ratio, Alt resizes from the center); each drag writes `x`, `y`, `width` and `height` as one undo step, and typing the numbers moves the frame. A double click opens the crop window: the same frame at full size, the four numbers, an aspect lock (Free, Original, 1:1, 4:3, 3:2, 16:9, Turn), a rule-of-thirds grid while dragging; OK or Enter writes, Cancel or Escape leaves the node as it was. With one of the four linked the frame is not drawn. The frame is what the kernel cuts (the origin held inside, the size stopping at the edge). |
| `fugue/brightness-contrast` | `image`, `brightness`, `contrast` | `image` | |
| `fugue/blend` | `base`, `top`, `mode`, `opacity` | `image` | Modes at least: normal, multiply, screen, overlay, soft light, difference. Difference is \|base - top\| as in Photoshop, GIMP and CSS; subtract is base - top clamped, which ComfyUI calls difference. |
| `fugue/compare-images` | `a`, `b`, `mode`, `position`, `gain` | `difference`, `composite` | A sink (#62). The node face and the large view show A and B in a viewer (docs/ui-ergonomics.md, Compare view); the outputs are for saving: `difference` is \|A - B\| times `gain` (Blend's difference formula; identical images give black), `composite` is the picture of `mode` (slider, slider_horizontal, side_by_side, a, b, difference) at `position`. B is fitted to A as Blend fits its top; alpha is left off. It writes no file. |
| `fugue/text-concat` | `a`, `b`, `separator` | `text` | |
| `fugue/text-template` | `template` (multiline, default `{a}` and `{b}` on two lines), `a` to `f` (multiline, default empty), `trim` (advanced, default off) | `text` | Fills the template: `{a}` to `{f}` become those inputs' text (an input with nothing gives nothing), `{{` and `}}` are a literal `{` and `}`. Any other name between braces, a lone `}` or an unclosed `{` is a `template_invalid` error on `template` that names it, also under validate (the kernel's `check`), never a silent hole in a prompt. Dynamic syntax typed into `template` (`{a|b}`, `__name__`, `${v}`) is such an error too, since `{a|b}` is an unknown placeholder: put dynamic syntax in a slot input (inserted text is never read again) and Dynamic Prompt after the Text Template, or Dynamic Prompt first and its `text` into a slot. The result is not trimmed unless `trim` is on; the inserted text is never read again for placeholders. Replaces chains of Text Concat for building a prompt from parts. Decision (2026-11-23): there is no variable-input mechanism (a `list` input takes many links into one unnamed port, and manifests are fixed), and a template-driven port set (B) would make ports appear and links vanish as one types, so six fixed text slots named `a` to `f` (A). The names are fixed by the manifest, so a template says `{a}` where one might wish `{setting}`; put a Text node with a title in front of the slot, or rename nothing, rather than hide behaviour. Six, not more: a node taller than that is better split in two. |
| `fugue/expression` | `formula` (line, default `a + b`), `a` to `f` (type `any`, optional, linked only) | `number`, `boolean` | #557. Works out a formula over the six values. Language (`nodes/expression_ops`, a recursive-descent parser, no eval, parsed once and evaluated over an AST): numbers (`12`, `.5`, `1e3`), the names `a` to `f`, `true`, `false`, `pi`; operators tightest first `**` (right to left; `-2 ** 2` is -4, `2 ** -1` is 0.5), unary `+ -`, `* / %` (`%` takes the sign of the divisor, as in Python), `+ -`, comparisons `== != < > <= >=` (chained as in Python: `a < b < c`), `not`, `and`, `or`, `x if c else y`; functions `round` (halves to even, as Python; an optional second argument of 0 to 15 digits) `floor ceil abs min max` (one or more values) `clamp(x, lo, hi)` `sqrt pow log log2 log10 sin cos tan`. A boolean is 1 or 0 in arithmetic and comparisons and a number is true unless it is 0; `and`, `or` and `not` always give a boolean (Python gives an operand); `and`, `or` and `x if c else y` evaluate only what they need, so `1 / a if a else 0` is safe. `number` is the result (1 or 0 for a boolean result) and `boolean` is true unless it is 0; a whole result prints without a decimal point because every value is a double. An input that is not connected is an error only when the formula reaches it (`input a has no value`); an input holding text, a picture or a list is `value_wrong_type` on that input. A formula that does not parse is `formula_invalid` on `formula` with `Column N:` (1-based) first in the message, also under validate and on the canvas after an edit, with no run (the kernel's `check`, which parses and does not evaluate; 10.3); so is a failure while evaluating (division or modulo by zero, a square root of a negative value, a log of 0 or less, a negative number to a fractional power, 0 to a negative power, `clamp` with its limits reversed, an exponent beyond 1024, any result beyond 1e15 or not finite), naming the operator's or function's column. A formula is at most 2000 characters and 64 levels deep; there is no loop, variable or other call. Decision (2026-11-26): input type `any` so a number or a boolean can be linked, which means no literal widget on a to f (a Number node feeds one; constants go in the formula). |
| `fugue/not` | `value` (boolean, default off) | `value` | #557. The flipped boolean. |
| `fugue/and` | `values` (a list of boolean, 1 or more) | `value` | #557. True when every item is true. |
| `fugue/or` | `values` (a list of boolean, 1 or more) | `value` | #557. True when at least one item is true. |
| `fugue/dynamic-prompt` | `template` (multiline, default `a photo of a {red|green|blue} {cat|dog|fox}`), `seed` (the seed widget: fixed, randomize or increment; default 0) | `text` | A template turned into one prompt with the syntax of adieyal's `dynamicprompts` library (MIT; the engine of A1111's sd-dynamic-prompts), native C++ in `engine/dynamic_prompt.*` (#556); parity with its SYNTAX.md (read 2026-11-25) is the floor. **Syntax.** Variants `{a|b|c}`, weights `{0.5::a|0.2::b}` (unweighted is 1), several picks without repeats `{2$$a|b|c}` joined by `, `, a custom separator `{2$$ and $$a|b|c}`, ranges `{1-3$$…}` with an omitted bound (`{-2$$…}` is `{1-2$$…}`, `{1-$$…}` is up to all options; more than the options clamps to them), and a variant whose only option is a wildcard picks among the wildcard's lines (`{2$$__colours__}`, no duplicates). Wildcards `__name__` read `name.txt` (one value per line, a line that starts with `#` is a comment, `N::text` weights a line), and the lists of `.yaml`/`.yml`/`.json` files by their key path (`__artists/finnish__`; a name above the lists takes all below it; numbers and nulls are ignored; a string is a one-item list), in nested folders (`__people/europe__`), globbed (`__colours*__`, `*` within one folder level) and recursively (`__people/**__`); wildcard lines may hold variants, wildcards and variables, nested. Variables `${v=…}` (evaluated at each use), `${v=!…}` (once, now), `${v}`, `${v:default}`, `${v?=…}` (only when unset), and parameters `__name(v=value)__` (literal text, set while that wildcard expands and then restored). Wrap `%{wrapper ... $$prompt}` (`...`, `…` or `⋯` marks the place). Sampler marks `~` random, `!` combinatorial, `@` cyclical on a variant (`{@a|b}`) or wildcard (`__@name__`). Whitespace and comments: a newline and its indentation read as one space, the result is trimmed, `#` starts a comment at the start of a line or before a space (so `#python` and `#ff0000` mid-line stay), `\#`, `\~`, `\!`, `\@`, `\{`, `\}`, `\|`, `\$`, `\%`, `\_` and `\\` give the character. **Seed.** A `std::mt19937_64` seeded with `seed` and our own draws: the same template, seed and wildcard files give the same text on every machine. `@` parts take their place from the seed (seed 0, 1, 2, … step through them in step with each other), so Increment cycles them. **Wildcard folders** (`dynprompt::wildcardFolders()`): `<data dir>/wildcards/`, the Default's `wildcards/` for a project that inherits `prompts` (1.4.2), and `<pack>/wildcards/` of every pack, as a pack's `catalog/` is found, then last the **built-in starter collection** (`share/wildcards/*.txt` compiled into the resources at `:/fugue/wildcards`: `places`, `eras`, `weather`, `time-of-day`, `art-styles`, `moods`, `colour-palettes`, `creatures`; original short lists, family-friendly), so a user's or a pack's file of the same name always wins; the first folder that has the name in any format (`.txt`, else `.yaml`/`.json` lists; with a glob, all of those in that folder) wins and lists of the same name in later folders are not merged, so a pack's `colours.txt` never beats the user's `colours.yaml`; Settings > Folders > Wildcards shows the folder. Downloaded collections drop in as they are. **Security (templates are untrusted).** A name resolves only to files inside those folders: `..`, `.` and empty folder names, an absolute path, a backslash, a colon (drive letter) and control characters are refused, and the file's canonical path (symlinks resolved) must lie inside the canonical folder or it is not read. Caps with clear errors: a wildcard file over 4 MB, nesting deeper than 32 (braces, and wildcards or variables inside each other: the chain of names is in the message, which is how a loop shows), a prompt over 64 KB, 200 000 template steps (a variable that expands into itself many times over), 5 000 wildcard files in the folders (each listed once per run, the same listing the cache signature hashes), a YAML nesting over 32, a pick count times options over the step limit; a pick count over the options clamps. YAML outside the subset (nested names ending in lists of text, `|` and `>` blocks, quoted and plain strings, comments) is an error naming the file and line, and one unreadable collection file never blocks the others (it is named when a wildcard is not found). **Errors** are `template_invalid` on `template` that name the place (`line L, column C of the template`, or the wildcard file and line): an unclosed `{`, `${`, `%{` or `__`, a stray `}`, a backwards range, a missing wildcard (naming the folders searched), a loop, a variable that is not set, weights that are all 0. The kernel's `check` finds the syntax errors, wildcards no file has and variables never set, so they show under validate. A syntax error, and a missing wildcard or unset variable outside any variant, are errors on the node (a run fails on each); one inside a variant's option (`{cat|__animals/missing__}`) is a warning, since a run evaluates only the option it picks and may never meet it. Never a silent hole in a prompt. **Cache.** `NodeKernel::keyExtra` puts a signature of the wildcard files (path, size, modification time) in the cache key when the template has `__`, so editing a wildcard file makes a new result. **Metadata.** The saved workflow holds the template and `run.nodes.<id>.auto.seed` (the concrete seed), which with the wildcard files reproduce the prompt; the picked text is also written into the embedded `run` section as `run.nodes.<id>.prompt` for a Dynamic Prompt upstream of the Save node, and left out when the Save node embeds without prompts. **Text Template** uses `{a}` to `{f}`, so dynamic syntax typed into its `template` is a `template_invalid` error: put dynamic syntax in a slot input (inserted text is not read again) and Dynamic Prompt after Text Template, or Dynamic Prompt first. **Every combination:** `dynprompt::expandAll(template, options, cap)` lists all prompts (the last choice changes fastest; `{2$$a|b|c}` gives its combinations; `~` and `@` parts are not enumerated: when none of their options holds a variant, wildcard, variable or wrap, `@` is drawn from the prompt's place and `~` at random again for every prompt; else (e.g. `{@{a|b}|c}`) the first pick is kept for the whole walk so the choices inside it enumerate, and `c` is not reached; capped, with `capped` set); the node has no `mode` input yet, the grid sweep (#552) wires it as an axis. **Deviations from dynamicprompts (all recorded here):** Python's `random` sequence is not reproduced (a template gives different text for the same seed there); a stray `}` is an error, written `\}` for a literal one (the library reads it as text); a `__` that closes nothing stays text, as in the library (`cute ^__^ face`); the `@` cyclical sampler takes its place from the seed instead of a counter across a batch; a recursive wildcard that does not end is an error at depth 32 (so expandAll cannot walk a self-referencing wildcard); the weighted YAML form is `N::text` on a list item (SYNTAX.md's example for it is not valid YAML); a symlink in the wildcard folders that leaves them is refused, not followed; the built-in resource folder is read without the canonical-path check (compiled in, read-only, no symlinks, and a `:/` path has no canonical form to compare; only that exact root is trusted, names still cannot leave it); plain YAML items with `: ` inside are read as text. Out of scope (the issue): Magic Prompt, I'm feeling lucky, the attention grabber, Jinja2. |

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
| `fugue/load-video` | `file` | `video`, `width`, `height`, `fps`, `duration` | Probed with ffprobe, never decoded; keyed by the file's content hash. **Also reported before a run** (the kernel's `check`, a warning on the node under validate, [9.1](#91-compile)): a `file` that is not there (`file_not_found` on `file`). |
| `fugue/load-audio` | `file` | `audio`, `duration`, `sample_rate` | As Load Video. **Also reported before a run** (the kernel's `check`, a warning on the node under validate, [9.1](#91-compile)): a `file` that is not there (`file_not_found` on `file`). |
| `fugue/generate-video` | `prompt`, `negative_prompt`, `model` (optional), `image` (first frame, optional), `duration` (seconds, default 2), `width`, `height` (optional, steps of 32), `fps`, `steps`, `guidance` (advanced, optional), `seed` | `video`, `seed`, `width`, `height`, `fps`, `duration`, `model` | Capability `video.generate`. Unset size: a first frame's shape at about 0.4 megapixel in steps of 32, else the model's default, else 864 x 480; the frame goes to the model as a PNG of exactly that size (scaled to cover, cut from the middle, as Generate Image's start image). Unset rate: the model's, else 24. The size, rate and length out are the file's. |
| `fugue/generate-song` | `prompt`, `lyrics` (empty: the model writes them), `instrumental`, `model` (optional), `duration` (seconds, 10 to 600, default 30), `language` (advanced, default `en`; `pt-BR` is sent as `pt`), `seed`; advanced since 1.1.0 (sent only when set, else the model chooses): `bpm` (0 = model's), `key` ("F# minor"), `time_signature` (2, 3, 4, 6), `creativity` (0 to 2), `keep_description` (the model does not rewrite the description); `quality` since 1.2.0 (advanced; `mp3`, `mp3-320`, `lossless`); `reference` since 1.3.0 (an optional `audio`, the Style reference); `adapter` (a `fugue/music-adapter`, optional, advanced) and `adapter_scale` (0 to 2, default 1, advanced) since 1.4.0 | `audio`, `lyrics`, `seed`, `duration`, `model` | Capability `audio.generate`. `lyrics` out is what was sung (or the model's own words), so it can feed Text nodes. |
| `fugue/remix-song` | `audio` (the song), `prompt` (the new style), `lyrics`, `instrumental`, `model` (optional), `strength` (0 to 1, default 0.7), `stay_close` (keep the tune and timing), `seed`; `quality` since 1.1.0, `reference` since 1.2.0 and `adapter`, `adapter_scale` since 1.3.0 (as Generate Song) | `audio`, `seed`, `duration`, `model` | Capability `audio.generate` with `task` cover (or cover-nofsq with Stay close): one ace-server `/synth` job, multipart with the song, the LM skipped; caption and lyrics reach the renderer as they are. The length is the source's. Docs: ace-step-research.md. |
| `fugue/repaint-song` | `audio`, `prompt`, `lyrics`, `instrumental`, `model` (optional), `start`, `end` (seconds; 0 = the end), `seed`; `quality` since 1.1.0, `reference` since 1.2.0 and `adapter`, `adapter_scale` since 1.3.0 (as Generate Song) | `audio`, `seed`, `duration`, `model` | As Remix Song with `task` repaint: redraws `start` to `end` and keeps the rest. The end must follow the start (`value_out_of_range`). **Also reported before a run** (the kernel's `check`, a warning on the node under validate, [9.1](#91-compile)): an `end` above 0 that is not after `start` (`value_out_of_range` on `end`); an `end` of 0 is the song's end, not known yet. |
| `fugue/extend-song` | `audio`, `prompt`, `lyrics`, `instrumental`, `model` (optional), `before`, `after` (seconds to add, 0 to 300; defaults 0 and 10), `seed`; `quality`, `reference`, `adapter`, `adapter_scale` (as Generate Song) | `audio`, `seed`, `duration`, `model` | As Repaint Song with a region outside the song: `before` sends `repainting_start` of minus `before` and an end of 0, `after` sends a start at the song's length and an end `after` seconds beyond it. The output is the song with the new part, longer by that much. One side at a time (both would redraw the song itself): both set is `value_out_of_range` with a hint to chain two nodes, as is none. Docs: ace-step-research.md. **Also reported before a run** (the kernel's `check`, a warning on the node under validate, [9.1](#91-compile)): `before` and `after` both 0, or both above 0 (`value_out_of_range`). |
| `fugue/describe-song` | `audio` (the song), `model` (optional) | `caption`, `lyrics`, `bpm`, `key`, `time_signature`, `language`, `model` | Capability `audio.generate` with `task` understand: one ace-server `/understand` job (multipart, the song as `audio`; the answer is the first part, one request as JSON), no `/lm` and no `/synth`. `caption` is a prompt a song node takes, `lyrics` and the metadata are what the model hears (a guess where there is no singing). Category Text. Docs: ace-step-research.md. |
| `fugue/save-video` 1.1.0 | `video`, `name`, `format` (mp4, webm; default mp4), `embed` (full, no-prompts, none), `folder` (optional) | `file` (video) | Sink, as Save Image: the same `name`, `folder`, `embed` inputs, naming and never-overwrite rule (one shared code, `nodes/save_files`), roots check and `workflow_not_embedded` warning. A video already in the chosen container is copied as it is, with no new encoding; otherwise ffmpeg encodes it (mp4: H.264 CRF 18, yuv420p, AAC 192k, faststart; webm: VP9 CRF 30, Opus 128k). Pending trim and crop are applied first (7.2). The workflow goes in a container tag `fugue` ([11.7](#117-embedding-in-outputs)). **Also reported before a run** (the kernel's `check`, a warning on the node under validate, [9.1](#91-compile)): a `name` that cannot be a file name (`node_failed` on `name`; `{model}` counts as a word) and a relative `folder` that leaves the output folder (`permission_denied` on `folder`). Write roots and names already taken are a run's. |
| `fugue/save-audio` 1.1.0 | `audio`, `name`, `format` (flac, mp3, opus; default flac), `embed`, `folder` (optional) | `file` (audio) | As Save Video. A flac or mp3 file saved in its own format is copied as it is; mp3 is VBR about 190 kb/s (libmp3lame -q:a 2), opus is 128 kb/s. **Also reported before a run** (the kernel's `check`, a warning on the node under validate, [9.1](#91-compile)): a `name` that cannot be a file name (`node_failed` on `name`; `{model}` counts as a word) and a relative `folder` that leaves the output folder (`permission_denied` on `folder`). Write roots and names already taken are a run's. |

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
  an `id` (lower case words joined by hyphens), a `label`, a `family` (`image`, `video` or `audio` for Enhance Prompt, `describe` for Describe Image), a
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
- Shipped (#194): Describe Image (`fugue/describe-image`: an image, a style, more instructions, in
  Advanced a language, model, seed; outputs the answer as `text`, with `seed` and `model`), a thin node
  on `text.generate` with the image (8.1), the same kernel as Enhance Prompt: the style is a prompt
  template of the family `describe` (`src/prompts/describe.json`: `image-prompt`, `caption`,
  `detailed-description`, `tag-list` and `song-prompt` (the music that suits a picture, for Generate Song); a file in the data folder's prompts folder with `"family":
  "describe"` adds one), the question sent with the picture is fixed ("Describe this picture."), the
  answer is stripped to the bare text as Enhance Prompt's is, and it never thinks. Enhance Prompt lists the
  families image, video and audio, Describe Image only `describe`; a style of the other node's family
  fails with `value_not_in_choices`. A model that says it takes no image fails with `capability_unavailable`
  (an omarchy model does not say, and is tried).
- Shipped (#29), each with a parity test against ComfyUI's own outputs (`tests/nodes/golden/`):

| Type id | Inputs | Outputs | Notes |
| --- | --- | --- | --- |
| `fugue/pad` 1.2.0 | `image`, `left`, `top`, `right`, `bottom`, `target_width`, `target_height` (advanced, 0 = unset), `mask` (optional keep mask), `fill` (edge-blur, edge, gray, transparent; default edge-blur), `feathering` (advanced, default 40), `blur` (advanced, 0 to 512, default 24) | `image`, `mask` | ComfyUI's Pad Image for Outpainting with a choice of what the new area holds (#243; `engine/edge_fill.*`, the code the canvas resize uses, 14.5): `gray` is 0.5 (alpha 1) as ComfyUI (the parity tests run it), `edge` the nearest picture pixel stretched outward (a corner takes the corner pixel), `edge-blur` that blurred by `blur` px (the benchmark's best outpaint start, docs/inpaint-outpaint-research.md 4.6), `transparent` alpha 0 (an RGB picture gets an alpha channel). The picture itself is never blurred. The mask is 1 in the new area and 0 over the original; feathering fades it inward as ((f - d) / f)^2 and is ignored when twice it is not smaller than the image's sides. The default changed from gray (1.0.1) to edge-blur in 1.1.0, so a saved Pad node without `fill` now pads with the edge; set `fill` to gray for the old result. 1.2.0 (#343, Klein DA 4): **target size** `target_width` and `target_height` (both or neither, else `input_invalid`; not together with a non-zero side, `input_invalid`): the picture shrinks (area resampling) by `min(tw/W, th/H)` when that is below 1, never enlarges, and is centered with `pad_top = (th - h) // 2` (the rest goes to the bottom; the same for left and right), the sides being worked out from it. **Keep mask** `mask`: marks the area to keep (the picture's size, else `input_invalid`; it shrinks with the picture); it is padded with 0 and inverted, so the output mask is 1 - keep over the picture and 1 in the new area, and feathering is not applied. With none of the three set the output is as in 1.1.0. Rows (#429): `blur` shows only with Fill edge-blur, `feathering` only while no Keep mask is linked (6.1.1); a hidden value is kept. **Also reported before a run** (the kernel's `check`, a warning on the node under validate, [9.1](#91-compile)): a target width without a target height or the other way round, and a target size with a nonzero `left`, `top`, `right` or `bottom` (`input_invalid`), judged from the values written; one that is wired is not. |
| `fugue/extend-edges` 1.0.0 | `image`, `mask`, `mode` (edge, edge-blur; default edge-blur), `blur` (advanced, 0 to 512, default 24) | `image` | The part under the mask filled from the nearest picture, stretched outward and (edge-blur) blurred: for a hole open on one side (an outpaint strip, a removal at the edge), where Fill Masked's blur pulls the far side in. "Nearest" is the nearest pixel the mask leaves (m = 0), found by two sweeps of the 8 neighbours; where m = 0 the pixel is byte-identical, elsewhere out = image x (1 - m) + fill x m, as Fill Masked. A picture with nothing outside the mask is gray. Property tests, no parity (no permissive reference). |
| `fugue/rotate` | `image`, `rotation` (none, 90 degrees, 180 degrees, 270 degrees) | `image` | Clockwise. |
| `fugue/flip` | `image`, `direction` (horizontally, vertically) | `image` | |
| `fugue/blur` | `image`, `radius` (1 to 31), `sigma` | `image` | ComfyUI's Blur Image, alpha included. |
| `fugue/sharpen` | `image`, `radius`, `sigma`, `amount` (advanced) | `image` | ComfyUI's Sharpen Image (`amount` is its `alpha`); alpha is left alone. |
| `fugue/invert` | `image` | `image` | Alpha is left alone. |
| `fugue/grow-mask` | `mask`, `expand` (negative shrinks), `tapered_corners` (advanced) | `mask` | ComfyUI's Grow Mask. |
| `fugue/invert-mask` | `mask` | `mask` | |
| `fugue/image-to-mask` | `image`, `channel` (red, green, blue, alpha) | `mask` | ComfyUI's Convert Image to Mask. |
| `fugue/mask-to-alpha` 1.0.0 | `image`, `mask` | `image` | Apply Mask as Alpha: RGBA, the colors kept, alpha = the image's own alpha (1 if it has none) x the mask, the mask stretched to the image. Not Composite Masked onto a transparent picture: that mixes the destination's color into a soft edge. |
| `fugue/solid-mask` | `value`, `width`, `height` | `mask` | |
| `fugue/number-math` | `a`, `operation` (add, subtract, multiply, divide, modulo, power, min, max), `b` | `value` | Python's arithmetic. **Also reported before a run** (the kernel's `check`, a warning on the node under validate, [9.1](#91-compile)): a `divide` or `modulo` with a `b` of 0 (`value_out_of_range` on `b`, also when `a` is wired), and a result that is not a finite number when `a`, `b` and `operation` are all written. |
| `fugue/switch` | `condition` (boolean, required), `on_true` and `on_false` (any one type, lazy, optional) | `value` | Passes on the value of `on_true` when `condition` is true, else of `on_false`. Both sides are lazy ([3.2](#32-inputs)): the hook asks only for the chosen one, so what feeds the other never starts ([3.4](#34-what-runs), [9.2](#92-scheduling)). The output has the type of the first linked side (`"follows"` in its manifest: an output of type `any`, which only built-in nodes may have; the compiler gives it the type of the input it follows, so a text feeds a text input and a number a number one; both sides linked with different types is `type_mismatch`, and a Switch with neither side linked has no type yet). On the canvas the output has that type too (`followedOutputType`, `engine/followed_type.h`, from the node's own links, a chain of Switches included): the link check, the wire's color and node search from a Switch value use it, so an image-fed Switch feeds image inputs only and search offers only nodes that take an image; a side takes the other side's type (`followedInputType`), so with a text on one side only a text can be dragged into the other, and a list only into a side whose other side is a list (the compiler refuses a list and a single value); with nothing linked both stay open, as `any` is a wildcard. Relinking a side so that a link out of the value no longer fits leaves that link in place (nothing is deleted silently; the canvas does not mark it, the compiler reports `type_mismatch`), and a new link of it is refused. The chosen side unconnected or empty is a `switch_empty` error on that input, never a silent empty output; a linked empty list is a value on a `takes_list` input, so a chosen side that is an empty list passes through as an empty list. The wire out of a Switch value has the resolved type's color when edited as when loaded, and follows a side linked or unlinked upstream (also through a chain of Switches). A side may carry a **list** (`takes_list` on the input, [manifest](nodes/manifest.md)): the list passes through as it is and the output is that list type, so a list input takes it and a single-value input does not; one side a list and the other a single value is `type_mismatch` (#592). A literal written on a side (only the CLI or hand-edited JSON can: the canvas has no widget for an `any` input) would type nothing and pass unchecked, so it is a `type_mismatch` on that input; link an output. A muted or bypass-emptied source of a side empties that side only: the Switch runs when the other side is chosen and is skipped `upstream_muted` (or `upstream_empty`) when the empty one is (#590). The cache key is the condition and the chosen value only. Read as a document (readiness, paired placement) a side is not a requirement: a model held only by nodes that lazy inputs alone reach (`conditionalNodes`, `engine/lazy_reach.h`) is left out of what a run is refused on, in one place (`modelsToHave`: the readiness request, so `fugue app run`, the Needs panel, Get ready and the app launcher agree) and is not placed on a paired host. Decision (2026-11-26): such a model is not listed or downloaded by Get ready either, rather than listed as "only if that branch is chosen" (a label would add a row state to the panel); if its side is chosen the node fails with its own `model_missing`, and `fugue model` and the download actions still name it. A model a node that is certain to run also holds, or one no compiled node holds (a muted node, a graph that does not compile), is still required. A bypassed Switch passes on `on_true`, the first input of its type whatever the condition says (bypass, 3.5, takes the first matching input; following the condition is not built). Soft Switch (one side left unconnected) is this node with that side empty, as the issue (#557) asks: an unconnected chosen side is the error. Decisions (2026-11-26): category Basic (the utilities one); `condition` has no default, so an unset one is `missing_input`; a Switch inside a group takes its type from what its sides hold as the node runs: an exposed input of the group replaces a link written inside, and is resolved in the scope it was written in. |
| `fugue/apply-lut` 1.0.0 (#548) | `image`, `file` (a `.cube`, the file picker; read like Load Image's), `strength` (0 to 1, default 1) | `image` | Colour grading from a 1D or 3D `.cube` table (Adobe's Cube LUT Specification 1.0: `TITLE`, `DOMAIN_MIN`, `DOMAIN_MAX`, DaVinci Resolve's `LUT_1D_INPUT_RANGE` and `LUT_3D_INPUT_RANGE` (one min and max for all channels), `LUT_1D_SIZE` or `LUT_3D_SIZE`, rows of three floats, red index fastest). Each channel is mapped through the domain to the table's index range and clamped; 1D is linear per channel, 3D trilinear; `strength` mixes with the original (0 is the picture). Alpha is kept. A malformed file is `input_invalid` on `file`, the message naming the line (`line 7: ...`; a file short of rows names its last line); a missing file `file_not_found`. Differences from ComfyUI_essentials' `ImageApplyLUT+` (cubiq, MIT, which does not read DOMAIN keywords and takes the LUT by name from a folder): any path, the domain honoured. Worked-number tests, no parity (`tests/nodes/test_lut.cpp`; code in `src/nodes/lut.*`). **Also reported before a run** (the kernel's `check`, a warning on the node under validate, [9.1](#91-compile)): a `file` that is not there or cannot be opened (`file_not_found`), and a bad `.cube` header read from the first 8 KB (`input_invalid` on `file`: a data row before the size, a bad or second size, an unknown keyword, a domain whose max is not above its min; a short file is parsed whole). The rows of a long file are read by the run only. |
| `fugue/desaturate` 1.0.0 (#551) | `image`, `amount` (0 to 1, default 1) | `image` | Each color channel moves toward the Rec. 709 luma (0.2126 R + 0.7152 G + 0.0722 B, taken on the values as they are, as ComfyUI_essentials' `ImageDesaturate+` does) by `amount`; 0 is the picture, 1 gray. Alpha is left alone. |
| `fugue/posterize` 1.0.0 (#551) | `image`, `levels` (2 to 256, default 4) | `image` | Each color channel is rounded to the nearest of `levels` evenly spaced values from 0 to 1 (round half up). Alpha is left alone. |
| `fugue/pixelize` 1.0.0 (#551) | `image`, `block_size` (1 to 4096 px, default 8) | `image` | Every block_size square (the ones at the right and bottom edges are smaller) takes the average of its pixels in every channel, alpha too; the picture keeps its size. Plain block averaging: `PixelOEPixelize+`'s contrast-aware method is out of scope (the issue). Worked-number tests for the three nodes: `tests/nodes/test_tone_ops.cpp`; code in `src/nodes/tone_ops.*`. |
| `fugue/draw-text` 1.0.0 (#549) | `image`, `text` (multiline), `anchor` (nine points, default top-left), `x`, `y` (default 16, 16), `size` (px of a line, 4 to 2000, default 32), `color` (default white), `max_width` (0 = no wrap), `align`, `outline` (0 to 16 px) with `outline_color` (shown when outline > 0), `box` with `box_color` and `box_padding` (shown when Box is on) | `image` | Writes text on a picture. **No GUI text**: the run worker and the CLI run under a `QCoreApplication`, where `QFontDatabase` and `QPainter` text are unavailable, and the AppImage's libvips is built without pango (so `vips_text` is not there), so glyphs are drawn by `stb_truetype` (public domain, `src/nodes/third_party/`) from a bundled Noto Sans (Latin subset, SIL OFL 1.1, `share/fonts/`); there is no font choice yet: stb_truetype does no range checking and must not read untrusted font files (a shared workflow or app can bring files), so a font input waits for FreeType (#558). Layout is `src/nodes/text_raster.*` (`rasterizeText`: text to a coverage mask at a size, newline and `max_width` word wrapping, a word too long breaks between letters, left, center or right lines; kern table, no shaping, so Latin text), reusable by other label-writing nodes. `anchor` names the point of the text block put at (x, y). Drawing order is box, outline (the coverage grown by a disc), text, each blended over with its color's alpha times coverage; an RGBA picture gets "over" alpha, an RGB one stays RGB; text off the picture is clipped. Blank text gives the picture. Errors: `invalid_font`, `value_out_of_range`. Tests under a `QCoreApplication` (`tests/nodes/test_draw_text.cpp`). |
| `fugue/image-grid` 1.0.0 (#552) | `images` (list, 1 to 64, in link order), `columns` (0 = about square: the square root of the count, rounded up; default 0), `labels` (multiline, one line per picture, written under it), `column_labels`, `row_labels` (advanced; one line per column above it, per row left of it), `gap` (px between pictures and around the sheet, default 8), `background` (color, default `#1e1e1e`), `label_size` (px of a line, advanced; 0 = 3% of the cell height, at least 10) | `image` | Lays pictures out as one RGB sheet (first slice of the sweep design in #552: the sweep itself, `planSweep` and `--sweep`, comes after). The cell is the largest width and height among the pictures; a smaller picture is centered in its cell, never scaled or cropped (a picture with alpha is blended over the background). Labels use `rasterizeText` (the GUI-free Noto Sans of Draw Text, centered, wrapped at the cell width), light on a dark background and dark on a light one; a label band is as tall as the tallest label of the sheet, so rows stay even. **`labels` is a deviation from #552's design** (which had only column and row labels): a set of views of one picture (front, back, close-up) is a list of names, not a table, so per-picture labels are the plain case. Extra label lines are ignored, missing ones leave the cell unlabelled. **Limits:** over 64 pictures is a `value_out_of_range` on `images` (the manifest's `max_items` stops it too); a sheet over 16384 px on a side is a `value_out_of_range` on `images` that names the size (an error, not a silent scale-down: the caller picks what to give up); no pictures is `missing_input`. Test: `tests/nodes/test_image_grid.cpp`. |

The mask ports of Pad (`mask`), Grow Mask, Invert Mask, Image to Mask and Solid Mask carry `"role": "mask"` since 1.0.1 ([14.4](#144-masking-and-inpainting-167)); nothing else about them changed.

### 14.3 M5: component level

The spike passed ([spikes/02](spikes/02-sdcpp-split.md)). Shipped (#79), each stage a job of the
helper's stage capabilities: Load Models (diffusion model, text encoder and VAE picked from disk,
one `fugue/model`, [2.6](#26-data-values-latents-and-conditioning)), Apply LoRA (strength,
stacking; the result is a derived handle, [2.5](#25-resource-handles)), Text Encode (prompt and
negative prompt, one conditioning; optional reference pictures for an edit, #88), Empty Latent, Sampler (seed, steps, guidance, sampler,
scheduler, denoise, and the advanced optional flow shift, Sampler 1.3.0, #341), VAE Decode and VAE Encode. A text-to-image graph of them gave the same
pixels as one `generate` run of the same helper for Z-Image Turbo and FLUX.2 Klein 4B at guidance
1, checked by hand on an RTX 3090 (SDXL's size conditioning differs: Appendix A);
the templates `z-image-turbo`, `flux2-klein-4b` and `flux2-klein-9b` are ready to open (11.10).
Video (#105): the same nodes on a video model, with Empty Video Latent (width, height, frames, rate)
in place of Empty Latent, VAE Decode Video in place of VAE Decode, and Load Models' optional
high-noise diffusion model for Wan 2.2 14B. Text Encode and Sampler are unchanged: the latent says
it is a video, and Text Encode takes an optional First frame and Clip (the Empty Video Latent) for an
image-to-video model (#130): the picture is cut to the clip's size and goes to the encode, the conditioning carries it
(`meta.first_frame`, `meta.clip`), and the Sampler sends it again and refuses a latent of another size
or length; the template `wan22-i2v` (fugue-workflows) is this graph on Wan 2.2 I2V A14B Q4_K_S, checked through the CLI on an RTX 3090 (640 x 384, 33 frames, 20 steps: three minutes). Video to video (#130): VAE Encode Video (video, frames, rate, optional size) fits a clip to the model's size, rate and 4 n + 1 length with ffmpeg and encodes it, and the Sampler redraws from that latent by Denoise (a sampled video latent works the same way); the helper is [8.10](#810-gpu-memory-and-the-helper)'s. A latent of the other kind at a decode is `node_failed` naming the right node. Checked on an RTX 3090 with Wan 2.2 T2V A14B (Q4_K_S, both passes, UMT5 fp8, Wan 2.1 VAE; 320 x 192, 9 frames, 4 steps): the three stages as separate helper processes give a video byte for byte the same as one `generate` run, and the graph above gives the same through Fugue, locally and through `--serve-http` (conditioning and latent files identical). Measured on the split (#105, #130, 2026-11-22; Wan 2.2 T2V A14B Q4_K_S, 640 x 384, 33 frames, 8 steps, `--serve-http` on a GB10 with the aarch64 CUDA build from this patch, the RTX 3090 local, the host's helper kept running): text encode, VAE decode and the sampling loop on the GB10 or the 3090, and the stage files passed as the run folder carries them. One clip, encode on the GB10 (0.7 s with UMT5 loaded; 126 s the first time, hashing and reading the models), sample on the 3090 (76 s), decode on the GB10 (9.4 s): 86.5 s; all on the 3090 (its helper starts with the run, so UMT5 is read each time: 8 s encode, 75 s sample, 4.8 s decode): 89 s; all on the GB10 (0.8 s, 126 s, 9.3 s): 136 s. The GB10 samples at 0.6 of the 3090's speed and decodes at half of it; its UMT5 encode with the model resident is faster than a cold start on the 3090, so the split pays only when the text encoder or VAE would otherwise be read again. Two clips at once in one graph, one wholly on each host: 136 s for both (the slower clip), against 222 s one after the other. Two *runs* do not overlap (the engine runs one at a time), and a host's helper takes one job at a time, so clip A's decode on the GB10 waits for clip B's sampling there: put each clip's three stages on one host. Video to video checked the same day (Wan 2.2 T2V A14B Q4_K_S): on the 3090 (320 x 192, 9 frames, 4 steps, strength 0.6) `vae_encode`, `encode`, `sample` with `init_latent` and `decode` as four processes give the same bytes as one `generate` with `init_video`, different from the source clip; with no start video the T2V stages still equal one `generate`. Through Fugue (a 33-frame 640 x 384 clip made by the text-to-video graph, 8 steps, Denoise 0.6, so only the low-noise model runs): VAE Encode Video 3.0 s on the 3090 and 6.4 s on the GB10 (ffmpeg here, the encode on the host), sample 28 s on the 3090 and 48 s on the GB10, decode 4.7 s and 9.7 s; the three arrangements (all on the 3090, encode and decode on the GB10 with the sampling on the 3090, all on the GB10) take 45 s, 45 s and 59 s, and the pictures keep the clip's scene and motion with a PSNR of 28 dB against the source. The template `video-to-video` (fugue-workflows, 2026-11-22) is that graph on Wan 2.2 T2V A14B Q4_K_S: Load Video of a short fox clip beside it, VAE Encode Video (640 x 384, 33 frames, 16 fps), Text Encode, Sampler 1.2.0 (Denoise 0.6, 20 steps, guidance 3.5, random seed), VAE Decode Video, Save Video. Run through the CLI on the RTX 3090 (a fresh `FUGUE_HOME`, the CUDA helper): text encode 48 s (UMT5 read cold), VAE Encode Video 3.3 s, sample 65 s (only the low-noise model runs at Denoise 0.6), decode 4.7 s; GPU memory peaked near 23.5 GB, so it needs the 24 GB card. The output keeps the clip's scene, fox and motion and redraws the details.
Image editing (#88), as ComfyUI's FLUX.2 Klein 9B templates do it: Load Image, Resize to
Megapixels (1 MP), Text Encode's references, an Empty Latent of the resized size, Sampler, VAE
Decode; the templates `flux2-klein-9b-edit` (distilled: 4 steps, guidance 1),
`flux2-klein-9b-base-edit` (base: 20 steps, guidance 5, empty negative prompt) and
`flux2-klein-9b-edit-2-refs`, each with sample pictures. Checked on an RTX 3090: staged equals one `generate`
run with the same reference, byte for byte. Not built: ComfyUI's KV-cache edit (FluxKVCache) and
its outpaint template (pad, mask, color match). Then text
encoders other than an LLM, Wan's UMT5 and FLUX.1's T5 with CLIP-L (SD3), ControlNet,
hires fix as a group. A coarse Generate Image can later be rebuilt as a group of
these.

Masks (#173, [14.4](#144-masking-and-inpainting-167)): VAE Encode 1.1.0 takes an optional `mask` and
the latent carries it with the picture (`meta.mask`, `meta.image`, media handles); the Sampler sends
them (`mask` as an 8-bit gray PNG scaled by the nearest pixel to the latent's size, `image` as the
picture, which the library scales as it does at the encode) and a latent it samples carries neither.
An inpaint model's input width reads as the latent it makes (SD and SDXL Inpaint 9 channels in, 4
out; FLUX.1 Fill 96 and Flex.2 49 in, 16 out), so FLUX.1 Fill with `ae` and SDXL Inpaint with its VAE are no
longer `model_mismatch`. Checked on an RTX 3090 (512 x 512): VAE Encode with the mask, Sampler and
VAE Decode as separate jobs give the same PNG, byte for byte, as one `generate` with the same
`init_image` and `mask`, for FLUX.2 Klein 4B (Q8_0, 4 steps), FLUX.1 Fill (`fluxFillFP8_v10`, 6
steps) and SDXL Inpaint (RealVisXL V4.0 with the SDXL Inpaint UNet, 8 steps); without the hook the
FLUX.1 Fill staged picture differs from the single run's.

**Flow shift and ComfyUI's schedules (#341, 2026-11-23; the engine at 3b12781f).** The Sampler's advanced
`flow_shift` (a number, empty by default) is sent as `sample.flow_shift` and only then; empty, the request is
the same as before. The engine applies it in the model's time-to-sigma map, so it changes the `discrete` and
`simple` schedules and not the one FLUX.2 Klein defaults to (`flux2`, which takes its own mu from the image size
and the step count and ignores the shift). Klein's engine default is shift 1.0 (the weights have no guidance
input). The map is ComfyUI's `flux_time_shift`, so ComfyUI's Klein value 2.02 means the same number here: at
denoise 1, `simple` with shift 2.02 equals ComfyUI's `simple` to 5e-4 and `discrete` with shift 2.02 equals its
`normal` except the last sigma above zero (0.0075 against 0.0056, because ComfyUI's table has 10000 points and
the engine's 1000). Denoise below 1 does not match: ComfyUI cuts the last `steps + 1` sigmas of an
`int(steps / denoise)`-step schedule; the helper starts at the noise level `denoise` on the `steps`-step
schedule (strength_args.h, #244), and on a 4 to 6 step schedule, crowded near 1, that leaves one or two steps. The table
is on #341. The opt-in reading is the Sampler's `denoise_mode` (below).

**Reference size (#342, 2026-11-23; the engine at 3b12781f).** Text Encode 1.1.0 has an advanced `reference_size`:
`auto` (the default) or `exact`. At the pin the engine sizes every reference itself before the VAE, in
`src/api/generation_image.cpp` (`reference_pixels`): to at most 1 MP and the output's area, aspect kept, each side
rounded to a multiple of 16 (32 for Qwen-Image). The option `ref_image_args` that the older engine had
(`image.cpp`, `resize_before_vae`) is gone with the DGL move, so the fork adds it back as the one setting
`ref_image_args = "resize_before_vae=false"` (anything else but `resize_before_vae=true` is `input_invalid`): the
reference then keeps the size the graph gave it, whatever the model. Text Encode sends it with `ref_images` when
`exact`, and puts `reference_size` on the conditioning's descriptor, so the Sampler sends the same value (a request
with `auto` or unset carries no `ref_image_args`, so it is as before). A side that is not a multiple of the VAE's (16, 32 for Qwen-Image) is cut from the
middle down to the multiple (`crop_only` in the fork's `reference_pixels`: 330x190 reaches the VAE as 320x176), the rule
of ComfyUI's VAE Encode, which crops the pixels to a multiple of the VAE's factor; without the cut the engine's VAE would
rescale the picture to the nearest multiple (330x190 to 336x192, a slight stretch). Checked on the 3090 with 330x190 and 250x250
(clean result, 320x176 and 240x240); aligned references are unchanged byte for byte. The helper logs each reference's size before and after at
the VAE (`reference N: WxH -> WxH`). Checked on the RTX 3090 (FLUX.2 Klein 9B Q8_0, 256 x 256, 4 steps, references
320x192, 256x256 and 192x320): `exact` reached the VAE as 320x192, 256x256 and 192x320, in the encode and the
sample; `auto` gave 656x400, 512x512 and 400x656 at the encode (no output size, so a megapixel) and 336x192, 256x256
and 192x336 at the sample. **Edit format (#574, 2026-11-25).** Text Encode 1.2.0 has an advanced `edit_format`: `default` or `Moss-Image-Edit`, for MOSS-Image edits only (other models ignore it; `shown_when` reads only the node's own inputs, so the row is always shown and its description says so). `Moss-Image-Edit` sends the engine option `moss_edit_format = "moss-image-edit"` with `ref_images` (and puts `edit_format` on the conditioning's descriptor, so the Sampler sends the same): ComfyUI's TextEncodeMOSSImageEdit format of that name, the references enter the DiT through the VAE only (no vision tokens even when the encoder has a vision tower), RoPE text_start anchor, references isolated at t=0. `default` sends nothing (the engine's ai-toolkit layout, as before). With `exact` reference size the MOSS references keep the graph's size (the engine at the #573 commit: a 1 MP 3:2 reference is no longer capped to 1024 on its long edge), and the engine strips the prompt as ComfyUI does. The engine logs `MOSS-Image edit layout moss-image-edit: N reference(s)...` and `reference N: WxH -> WxH before the VAE`.

**Reference indices.** FLUX.2 (`sd_version_is_flux2`) sets `ref_index_mode = increase`
on the predictor (`generation_image.cpp`, the `increase` list in `image_references`, and `flux.hpp` forces it again
for the model), the target takes time index 0, the references start at 1 (`rope.hpp`, `gen_flux_ids`, `x_index + 1`)
and each takes `index * ref_index_scale` with the scale 10 for FLUX.2 (`flux.hpp`, the flux2 config), so the
references get 10, 20, 30 in the order they are sent, as ComfyUI's ReferenceLatent does. No change was needed.

**Denoise mode (#359, 2026-11-23; the engine at 3b12781f).** The Sampler's advanced `denoise_mode` (1.4.0) is `noise_level`
or `schedule_cut`, unset by default; unset or `noise_level` sends what it always did. With `schedule_cut`, a start latent
and 0 < denoise < 1, the kernel sends ComfyUI's reading (`src/nodes/denoise_cut.h`, shared with Detail Regions later):
N = int(steps / denoise) in double (6 steps at 0.6 give 10; a float gives 9), `sample.steps` = N, `strength` =
(steps - 0.5) / N and `sample.args` = `strength_as_noise_level=false`. The engine's `slice_sigmas_by_strength`
(`image_helpers.dgl`) keeps the last `min(floor(N * strength) + 1, N)` steps, exactly `steps` of them; the half step keeps
the cut off an integer so the helper's float strength cannot drop one. `strengthSampleArgs` leaves a request that names the
reading alone. An Empty Latent keeps its steps; denoise 0 in this mode is an input error; a video latent refuses the mode
(Wan's high-noise split counts steps). The live preview is paced by the steps that run, not by N (`sampledSteps` in
`strength_args.h`). The fork logs the schedule that runs (`sampling sigmas: ...`). Checked on an RTX 3090 (FLUX.2 Klein 4B
Q8_0, 256 x 256, 6 steps, `flow_shift` 2.02, euler), schedule_cut, sigmas as logged:

| Scheduler | Denoise | Fugue (logged) | ComfyUI (#341) |
|---|---|---|---|
| simple | 0.5 | 0.8829, 0.8436, 0.7908, 0.7153, 0.6018, 0.4087, 0 | 0.8829, 0.8434, 0.7904, 0.7153, 0.6013, 0.4068, 0 |
| simple | 0.75 | 0.9577, 0.9263, 0.8829, 0.8189, 0.7153, 0.5185, 0 | not measured |
| discrete | 0.5 | 0.8629, 0.8120, 0.7394, 0.6275, 0.4325, 0.0075, 0 | 0.8629, 0.8119, 0.7392, 0.6272, 0.4319, 0.0056, 0 |
| discrete | 0.75 | 0.9497, 0.9097, 0.8500, 0.7516, 0.5585, 0.0075, 0 | not measured |

The differences are those of denoise 1 (the engine's 1000-point table against ComfyUI's 10000; last discrete sigma 0.0075
against 0.0056).

### 14.4 Masking and inpainting (#167)

Built: everything in #167 is built. The pieces are the issues at the end of this section, and each says here what shipped
(#171 to #177). The research and each choice with its confidence are in Appendix A (the rows marked
#167); the person's guide is [masking.md](masking.md).

**Shipped (#171):** the `role` manifest field (6.1; `src/engine/node_type.*`) and the nodes Composite
Masked, Feather Mask, Threshold Mask, Combine Masks, Fill Masked and Overlay Mask (`src/nodes/mask.cpp`
for the kernels, `src/nodes/mask_ops.*` for the arithmetic, plain functions on float buffers that
Stitch and Generate Image call as well; `pixel_io.*` is the reading and writing of buffers that
`pixel.cpp` and `mask.cpp` share). The rows of the table below that are not marked as shipped are
still to come, except those marked shipped below. Parity tests against ComfyUI's own outputs cover Composite Masked, Threshold Mask and
Combine Masks (`tests/nodes/golden/make_mask.py`, `mask.json`); Feather Mask and Fill Masked have
property tests (`tests/nodes/test_mask_nodes.cpp`).

**Shipped (#176):** Crop to Mask and Stitch (`src/nodes/crop_stitch.cpp` for the nodes,
`src/nodes/crop_stitch_ops.*` for the arithmetic: plain functions on float buffers), the declared
type `fugue/stitch`, and the group `fugue/inpaint-crop-stitch`. Tests: `tests/nodes/test_crop_stitch.cpp`
(the box rule, the sizes, crop then stitch, the handle's hash, the errors, the group on the fake
helper). No ComfyUI golden: the size rule is Resize to Megapixels' (checked in #72's tests), the
resampling and the feathering are the shared kernels, and the community Inpaint Crop and Stitch node
is GPL, so its behaviour was taken from its README only; what is new is the box and the stitch,
which the tests state as properties (an untouched crop gives the original back, outside the box is
byte for byte the original's).

**Shipped (#177):** the two templates `inpaint-region` and `outpaint` in fugue-workflows (category `edit`,
"Image editing"; the `templates` submodule), and the finished [masking.md](masking.md) with its pictures
(`docs/images/mask-*.png`, `inpaint-*.png`, `outpaint-graph.png`, made by the offscreen tests of
`test_readme_shots`). Both were run through `fugue run` on an RTX 3090 with FLUX.2 Klein 9B Q8_0 by the
staged path, from a helper built on the fork's pin. `inpaint-region`: the 1024 px sample (a reading
nook made with Fugue, shared with the `flux2-klein-9b-edit-2-refs` template) with an armchair painted
over (`inputs/reading-nook-mask-<hash>.png`), the group with prompt, seed random, Steps 4, Guidance 1,
Denoise 1, Padding 64, Megapixels 1, Blend 16; Save Image and Preview. The armchair became a yellow
velvet one with its legs, in the lamp's light, and the result equals the original at every pixel more than 20 px
from the mask (the 16 px Blend is the only mixing; 0.4 of 255 at 16 px); about 25 s a run after the first (the models load once).
`outpaint`: Load Image (the cat of the edit templates, 768 px square, so the padded one is 1280 x 768,
0.98 megapixel), Pad 256 left and right with feathering 40, Text Encode, VAE Encode (padded, Pad's
mask), Sampler (euler, 4 steps, guidance 1, denoise 1), VAE Decode, Composite Masked (padded, result,
Pad's mask), Save Image and Preview; about 18 s a run after the first. Both borders are drawn from the
prompt, with no visible seam (checked at 2x). Text Encode does not take the padded picture as a
reference here: with it Klein at 4 steps reproduced Pad's gray border (the old content kept, Appendix A);
the inpaint group keeps its reference, where the same test without it gave a chair with no legs, off the
old one's place. Decision: reference on for the crop, off for the padded picture. Load Image 1.1's
Paint mask row made its node 28 px taller, which put it over the prompt in the layouts of `image-to-video`
and `wan22-i2v` after a run (`test_template_overlap`); their prompts moved down 40 px. `scripts/check_doc_links.py`
checks the links and headings of the README and docs.

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
- Face (shipped, #174): the picture slot shows the picture with the mask over it (`MaskNodeView.qml`,
  made at full detail for a node whose mask is painted; the mask is read at no more than 1024 px a
  side), and the `mask` input's own row is the Paint mask button: the full 24 px row, the label
  "Paint mask", the state ("Painted" or "None") and a brush glyph at its right, a click opens the
  editor on the node's picture. A row, not a 28 px button under the picture, so the node's layout and
  height rules stay as they are. Without a picture (Load Image with no file, Paint Mask whose input
  has not run) it says so in a notice. On Paint Mask a double click on the picture opens the editor,
  as Crop's opens the crop window; on Load Image a double click keeps opening the large view. A
  node with a `mask` input over an image input shows that input's picture, as Crop does.
- Workflow files: the file a `mask` input names is an input file, as a `file` input's is. Saving
  copies it into `inputs/` (`relocateInputs`), a package carries it (11.5) and a template's is
  taken in (11.10). One predicate says which inputs hold such files, for all three
  (`InputSpec::holdsInputFile`, shipped #174: widget `file`, `mask` or (#181) `paint`; `forEachFileLiteral`, the
  scheduler's path resolution, read-root check and content hash, and the share copy's path kind use
  it too, so a mask is keyed by its bytes like a picture). A group instance's literal input counts as an input
  file when its exposed input feeds one (through nested groups; an unwired input, a missing definition or a
  cycle is not one), so a picture given to a group's `file` input is saved, packaged and taken in like a Load
  Image's (#372).
- Widget field: an input of widget `mask` names, in `over`, the image input it is painted over
  (6.1); Load Image's paints over its own file.

**Nodes.** Each small; the formula shared with ComfyUI has a parity test against ComfyUI's own
outputs (`tests/nodes/golden/`, made by running ComfyUI's nodes as #29 did; its code is not read
or copied). Mask outputs carry the role.

| Type id | Inputs | Outputs | Notes |
| --- | --- | --- | --- |
| `fugue/load-image` 1.1.0 (shipped, #174) | adds `mask` (widget `mask`, optional) | adds `mask` | The painted mask; else 1 − alpha when the file has alpha (ComfyUI's Load Image); else all 0. A mask painted on another size is scaled to the picture (bilinear) with `mask_resized`. |
| `fugue/paint` 1.0.0 (shipped #181: the window opens from the `paint` row) | `image` (optional), `width`, `height` (optional, 1 to 8192), `paint` (widget `paint`, over `image`, optional) | `image`, `mask` | The layers of the `.ora` file the `paint` input names, over the picture or over a blank canvas. The canvas is the picture's size, else Width and Height (both), else the file's own; a file painted on another size is scaled to it (bilinear) with the warning `mask_resized`. `image` is the picture with the visible paint and picture layers over it (straight RGBA, transparent where nothing is); with nothing painted over it, the input picture itself, exactly. `mask` is the visible mask layers (the largest value, times their opacity), all 0 for none. Without a picture or a size, `missing_input`. **Also reported before a run** (the kernel's `check`, a warning on the node under validate, [9.1](#91-compile)): a `paint` file that is not there (`file_not_found`). Whether a picture or a size is given depends on a link, which a check does not see. |
| `fugue/paint-mask` 1.0.0 (shipped, #174) | `image`, `mask` (widget `mask`, over `image`, optional) | `mask` | A mask painted over any picture, a generated one too. Unpainted: all 0. **Also reported before a run** (the kernel's `check`, a warning on the node under validate, [9.1](#91-compile)): a painted `mask` file that is not there (`file_not_found` on `mask`). |
| `fugue/composite-masked` 1.0.0 (shipped) | `destination`, `source`, `mask` (optional), `x`, `y`, `resize_source` (advanced) | `image` | ComfyUI's ImageCompositeMasked: the source at x, y over the destination, out = d × (1 − m) + s × m; no mask is all of the source; `resize_source` stretches the source to the destination first; the mask is resized to the source (bilinear). Shipped: `x` and `y` run 0 to 16384, ComfyUI's range (its nodes fail on a negative one, so there is no parity case for it); the arithmetic function also takes a negative offset, cutting what lies before the edge, and a source wholly outside leaves the destination as it is. A source with an alpha channel onto a destination without drops it; without one onto a destination with it gives alpha 1. |
| `fugue/feather-mask` 1.0.0 (shipped) | `mask`, `radius` (0 to 512 px, default 16), `direction` (outward, inward, centered; default outward) | `mask` | A soft edge `radius` wide: b = the mask blurred (three box passes, close to a Gaussian of sigma radius / 2); outward max(m, b), inward min(m, b), centered b. Outward keeps all that is painted and fades into the rest; inward never reaches a pixel outside the paint. Not ComfyUI's FeatherMask, which fades from the picture's borders. Shipped: each of the three box passes has the variance radius^2 / 12, so together they are a Gaussian of sigma radius / 2, and the box's width is continuous in the radius (the two pixels beyond its ends count by a fraction, so a radius of 1 still softens). The picture's edge repeats beyond it, so a mask that reaches the edge stays full there. Time grows with the pixels, not the radius: 512 px on 4 MP takes about 40 ms. |
| `fugue/mask-from-color` 1.0.0 (#550) | `image`, `color` (default #00ff00), `tolerance` (0 to 1, default 0.1), `soften` (0 to 512 px, default 0) | `mask` (role mask) | 1 where a pixel is within `tolerance` of `color`, else 0; alpha is ignored. The distance is the Euclidean distance in RGB divided by sqrt(3), so 0 is the exact color and 1 takes everything (ComfyUI_essentials' `MaskFromColor+` takes a per-channel threshold, which is a cube, not a distance; its RGBCMYBW variant is this node with a color picked). `soften` is Feather Mask's centered blur, so the edge is as soft as that node's of the same radius. The output is a one-channel mask like Image to Mask's, so every mask input takes it. Worked-number tests (`tests/nodes/test_mask_from_color.cpp`; arithmetic `maskFromColor` in `src/nodes/mask_ops.*`). |
| `fugue/threshold-mask` 1.0.0 (shipped) | `mask`, `value` (default 0.5) | `mask` | ComfyUI's ThresholdMask: 1 where m > value, else 0. Shipped. |
| `fugue/combine-masks` 1.0.0 (shipped) | `destination`, `source`, `operation` (multiply, add, subtract, and, or, xor), `x`, `y` | `mask` | ComfyUI's MaskComposite. Shipped: `x` and `y` run 0 to 16384; the result is held to 0 to 1; and, or and xor round half to even first, so 0.5 counts as off. |
| `fugue/fill-masked` 1.0.0 (shipped) | `image`, `mask`, `mode` (gray, blur; default blur) | `image` | What lies under the mask before a redraw below denoise 1 (A1111's masked content "fill", Krita's pre-fill): gray is 0.5; blur spreads the colors around the mask into it (push-pull: averages of the kept pixels, coarse to fine). Where m = 0 nothing changes. Property tests, no parity (no permissive reference). Shipped: a mask of another size is stretched to the picture (bilinear). The blur weights each pixel by 1 - m, so a half-masked pixel counts half, and a fill is a convex mix of kept colors; with nothing kept it is gray. Where 0 < m < 1 the result is image x (1 - m) + fill x m. Gray gives alpha 1 on a picture with alpha; 4 MP takes about 130 ms. |
| `fugue/overlay-mask` 1.0.0 (shipped) | `image`, `mask`, `color` (default `#ff3b30`), `opacity` (default 0.5) | `image` | The mask drawn over the picture, to check or save what is masked. A mask alone already shows as a gray picture in Preview. Shipped: out = image x (1 - a) + color x a with a = m x opacity; `color` is `#rrggbb`, `#rgb` or `#rrggbbaa` (the alpha multiplies the opacity); the picture's alpha channel is left alone. |
| `fugue/color-match` 1.1.0 (shipped, #180; Method added in 1.1.0, #555) | `image`, `reference`, `mask` (optional), `strength` (0 to 1, default 1), `method` (reinhard, match-color; default reinhard), `luminance` (0 to 2, default 1), `color_intensity` (0 to 2, default 1), `neutralize` (0 to 1, default 0); the three sliders show only with Match Color (`shown_when`, 6.1.1) | `image` | Moves the picture's colors to the reference's, so a redrawn region does not show a color shift at its seam (ComfyUI's Klein Image Extend template color-matches; Acly's Color Match (Masked) is the same idea). Reinhard, Ashikhmin, Gooch and Shirley, "Color Transfer between Images" (2001), written from the paper (the color-matcher library and the ComfyUI nodes are GPL and were not read): linear RGB to the paper's LMS, log10, then l-alpha-beta; each axis of the image is moved to the reference's mean and scaled by the ratio of the standard deviations; `strength` mixes the result with the picture in that space, so 0 is the picture itself. With a `mask` the statistics are weighted by 1 - m: the pixels the mask leaves are measured, the masked ones (the redraw) are not; the same weights apply to the reference when it has the picture's size (the two then describe the same place), otherwise the reference is measured whole. The transfer is applied to every pixel, so a Composite Masked or a Stitch after it still decides what is kept. An axis with no spread in the picture is moved only (scale 1). Alpha is kept. Warnings: `mask_resized` (a mask of another size is stretched), `mask_full` (a mask of all 1 leaves nothing to measure: the whole picture is measured). Errors: `value_out_of_range` on `strength`, `luminance`, `color_intensity` and `neutralize`; `value_not_in_choices` on `method`. **Method (#555, 1.1.0)**: `reinhard` is everything above and the default, and a node saved as 1.0.0 (no `method`) runs it, giving the same pixels. `match-color` follows Photoshop's Image > Adjustments > Match Color as cubiq's `ImageColorMatchAdobe+` (ComfyUI_essentials, MIT) reads it; Photoshop's formula is unpublished. Both pictures go to CIELAB (D65), and with the same mask weights as above: l' = (l - mean_l(image)) * (std_l(ref) / std_l(image)) * `luminance` + mean_l(ref); a and b are scaled together around the image's own means by sqrt(var a + var b) of ref / that of image, moved to (1 - `neutralize`) times the reference's mean a and b, and the whole of a and b is then multiplied by `color_intensity` (0 is gray, as Photoshop's lowest Color Intensity), so the colors go to the reference's and Neutralize 1 gives a picture with no cast. Deviation from cubiq (owner asked for Photoshop's behaviour; coordinator, medium-high): his code subtracts `neutralize` times the image's own mean and adds no reference mean, so the picture kept its own hue and only its saturation was matched, which is not what Photoshop's Match Color does. The result is mixed with the picture in RGB by `strength` (the fade) and clamped. A ratio whose image spread is 0 is 1. Alpha is kept; the mask warnings are the same. The sliders are hidden, not greyed, with Reinhard: the rows that do not apply are hidden by the existing `shown_when` rule (6.1.1), and the engine still gets their values. Tests (`tests/nodes/test_color_match.cpp`): a darker copy is brought to the reference's brightness, a picture matched to itself is untouched, a warm picture matched to a cool one turns cool, Color intensity 0 is gray, Luminance 0 flattens brightness, Neutralize draws the channel means together, strength fades, a flat picture gives no NaN, masked pixels stay out of the statistics, and a node with no `method` equals Reinhard. Property tests, no parity (no permissive reference): a known shift is undone, masked pixels do not change the statistics, strength 0 is the identity (`tests/nodes/test_color_match.cpp`; kernel and arithmetic in `src/nodes/color_match.*`). |
| `fugue/detail-regions` 1.0.0 (#345, epic #339) | `image`, `model`, `conditioning`, `boxes` (text, one `x1 y1 x2 y2` a line: Find by Text's; only a blank line is skipped, any other line that is not four finite numbers is `input_invalid` on `boxes` naming the line), `padding` 32, `target_size` 1024, `denoise` 0.3, `denoise_scale` (`fixed`, `smaller_less`, `smaller_more`), `steps` 4, `guidance` 1, `sampler` euler, `scheduler` simple, `feather` 24, `seed`, `seed_shift` | `image` | Refines every box in turn, the detailer of DemonAlone's DA_RefineTaskNode and DA_MasterRefiner (MIT; read for the numbers). A graph cannot loop over a variable number of boxes (2.3, 9.9), so one node runs the model itself: a `ProviderKernel` of several jobs (`next()`, as Upscale Video with Model), three a box, each the very request the VAE Encode, Sampler and VAE Decode kernels make (they are called, not copied). Per box: corners rounded, grown by `padding` and cut at the picture's edge, skipped (warning `box_skipped`) under 16 px on a side; a crop whose short side would be under 64 px once the long side is `target_size` is grown on its short side inside the picture (not stretched; in a picture too small for that it is stretched); the crop is resized (bicubic) so its long side is `target_size` and the short side keeps the shape, both in steps of 16 (the VAE of every model Fugue runs needs 16; the reference's 8 only fits ComfyUI's cropping VAE); encoded, sampled with no noise mask and decoded; resized back and pasted over the whole padded crop through weights with a linear ramp on each edge (`f = min(feather, h/2, w/2)`, ramps multiply in a corner) except an edge that lies on the picture's own border, which has no seam to hide: the reference ramps all four edges, so there a face touching the frame keeps its original edge strip, which this node redraws. Each box is cut from the picture as the one before left it (kept at 16 bits in the node's folder between jobs). Seed of a box `seed + seed_shift x 100000 + index` (the index counts skipped boxes); denoise scaling `ratio = clamp(box_area / image_area, 0.01, 0.5) / 0.5` (the part of the box inside the picture, as given, not padded), `smaller_less` x(0.4 + 0.6 ratio), `smaller_more` x(1 - 0.6 ratio), held to 0.05..1 and rounded to three places. A denoise below 1 is always ComfyUI's schedule cut (`denoise_cut.h`, #359: `int(steps / denoise)` steps, the last `steps` run), as the reference runs it. Only colour is pasted: an alpha channel is kept as it was. Coordinates are held to a billion before they are rounded. No box: the picture passes through and no job is asked (`JobRequest` with an empty capability: the scheduler goes straight to `next()` and `outputs()`). Cancel is checked between jobs. Known and left: a node with no box still needs a provider to be selected, and an out-of-memory retry restarts from the first box. Tests: `tests/nodes/test_detail_regions.cpp` (arithmetic in `src/nodes/detail_ops.*`, kernel in `components.cpp`) **Also reported before a run** (the kernel's `check`, a warning on the node under validate, [9.1](#91-compile)): a `boxes` text that is not one `x1 y1 x2 y2` a line (`input_invalid` on `boxes`). |
| `fugue/crop-to-mask` | `image`, `mask`, `padding` (px, default 64), `megapixels` (default 1; 0 keeps the size), `multiple` (advanced, default 16) | `image`, `mask`, `stitch`, `width`, `height` | The box around every pixel with m > 0, grown by `padding` on each side, moved inside the picture where it sticks out and cut where the picture is smaller; then resized to Resize to Megapixels' size for the box (lanczos for the picture, bilinear for the mask), so a small region is redrawn at the model's resolution and a large one is not drawn above it. Shipped (`fugue/crop-to-mask` 1.0.0): padding 0 to 4096, megapixels 0 to 16, `multiple` 1 to 256. The box is the bounds of the pixels above 0 (a faint stroke counts); grown by the padding it is moved inside the picture, and cut when the grown box is wider or taller than the picture, so a mask over half the picture, or a 1 px mask in a corner, still gives a box inside it. Its size is `sizeForMegapixels` of the box's aspect, no side above 4096 (a 1 x 4000 box at 1 MP would be 16 x 65536); with megapixels 0 it is the box's own size and nothing is resampled. A mask of another size than the picture is stretched to it (bilinear) with the warning `mask_resized`; an empty one is `mask_empty`. The crop keeps the picture's channels; the mask output is one channel in 0 to 1. |
| `fugue/stitch` | `stitch`, `image`, `blend` (px, default 16), `mask` (optional) | `image` | The redrawn crop put back: resized to the box (lanczos) and blended through the handle's mask (or `mask`, at the picture's size) feathered outward by `blend` (Feather Mask's kernel), cut at the box. Outside the box the original's pixels are kept byte for byte. Shipped (`fugue/stitch` 1.0.0): blend 0 to 512. The mask is feathered over the whole picture and then cut at the box, so a mask that reaches the box's edge fades as it does anywhere; where the feathered mask is 0 the original is returned exactly (out = d x (1 - m) + s x m with m = 0), and a crop that was not resized and not changed gives the original back within float rounding. A mask of another size than the original is stretched to it. The redrawn picture must have the crop's shape to within 2% of its aspect ratio, any size: another size is resized to the box (an edit model may return a larger picture). A handle whose original or mask file has changed since Crop to Mask read it is `file_not_found` on `stitch`; a value that is not a stitch handle is `input_invalid`. |

**The stitch handle.** A declared data type `fugue/stitch` (a diamond port, 2.2), a descriptor
alone as an Empty Latent is (2.6; shipped: the file is `stitch.json` in Crop to Mask's folder and holds the `meta` as compact canonical JSON and nothing else, so what the scheduler hashes is the meta; the media handles it names are the inputs' own files, a mask of another size included, and Stitch checks their hashes): `{"data": "fugue/stitch", "hash", "meta": {"image": <the
original's media handle>, "mask": <the mask's, at the original's size>, "box": {"x", "y",
"width", "height"}, "size": {"width", "height"}}}`, the hash that of the canonical `meta`. So a
Stitch cannot be fed a picture by mistake, and it needs no second wire from the original.

**The group.** `fugue/inpaint-crop-stitch`, "Inpaint (crop and stitch)", shipped by the `fugue`
pack as a group (6.1 `runtime.kind: "group"`). The built-in pack can carry one: `pack.json` lists
the folder, whose `node.json` has `runtime: {"kind": "group", "definition": "group.fugue"}` and whose
`group.fugue` (a document with an empty graph and the definition in `groups`, as the library writes
it, 5.3) is compiled in as a resource beside it (CMakeLists.txt globs `group.fugue`);
`loadLibraryGroup` and `embedGroupTypes` read it through the resource path as they read a file, so
the template (#177) adds the group like any other and the workflow embeds the definition. Its nodes: Crop to Mask, Text Encode (the crop as its reference), VAE Encode
(the crop and its mask), Sampler, VAE Decode, Stitch (inner ids `crop`, `encode`, `vae_encode`, `sample`, `decode`, `stitch`). Exposed: `model`, `image`, `mask`,
`prompt`, `seed`, `steps`, `guidance`, `denoise` (default 1), `padding`, `megapixels`, `blend`;
out: `image`.

**Keep unchanged areas (#621).** `fugue/keep-unchanged` 1.0.0, "Keep unchanged areas", `Image/Mask`, a second group
of the `fugue` pack, shipped as `fugue/inpaint-crop-stitch` is (`src/nodes/manifests/keep-unchanged`, test
`tests/nodes/test_keep_unchanged.cpp`). After an edit model (FLUX.2 Klein and the like) redraws a whole picture, it keeps the
original everywhere the edit did not really change it. In: `original`, `edited` (any size or aspect ratio), and the knobs
`blur` (4, range 1 to 31, as Blur's radius), `threshold` (0.12), `speck` (12), `close` ("Fill gaps", 24), `margin` (16), `feather` (16); out: `image` (the composite)
and `mask` (the change mask, 0 to 1). The chain, inner ids in brackets: fit the edit to the original's size (Composite Masked with
`resize_source`, no mask: `fit`; an edit model returns another aspect ratio); a rough change map (Blur both, Blend `difference`,
the summed channel difference as below, Threshold Mask at the same `threshold` as the final one, bound to the same knob: a stricter rough cutoff left a faint change in Color Match's
statistics and matched it away) grown 20 px; Color Match (`match`) of the fitted edit onto the original,
measured only where that map is 0, so the global drift is measured on unchanged pixels (a single pass is pulled by the changed
area); Blur both (`blur`), Blend `difference`, the three channels of the difference as masks (Image to Mask red, green, blue) added by
Combine Masks `add` (held to 1), so the magnitude is the sum of the channel differences and a change in blue or in hue alone
counts (Desaturate's luma weights, blue 0.072, needed a blue-only change near 0.55; Combine Masks `or` rounds to 0 or 1 and is
no maximum), Threshold Mask (`threshold`; the sum is about 3 times the gray value, hence 0.12 where the Desaturate version used
0.04, and on the same test pair the mask covers the same 5.7% of the picture); an open (Grow Mask by -`speck`, then +`speck`; Number Math negates) that drops specks; a close
(Grow +`close`, then -`close`) that fills gaps; Grow +`margin`; Feather Mask outward by `feather`; Composite Masked, destination the
original, source the fitted edit, through that mask. Where the mask is 0 the output is the original pixel for pixel (tested
exactly, with a 16-bit mask). Blend `difference` stands in for Compare Images, which hangs inside a group (#623). Exposed inputs
bind to inner literals (`blur` to three Blurs, `speck` to Number Math and the regrow, `close` likewise), as the Inpaint group's do.
The paste uses the fitted edit, not the colour-matched one (Color Match only decides where the change is), so a global drift of
the edit shows as a soft transition inside the feather band: intended, the edit is kept as the model made it. Alpha: Composite
Masked keeps the destination's channels, so an RGBA original keeps its alpha where the mask is 0; where the mask is above 0 the
alpha mixes toward the edit's alpha, or toward 1 (opaque) when the edit has no alpha. Resizing the fit is bilinear (Composite
Masked offers no other filter), so an edit much larger than the original is aliased a little; it is detection and paste alike,
not a seam. Contract: the edit has the original's framing and is only resized. The fit stretches each axis to the original's size (Composite Masked
`resize_source`), so a cropped or padded edit has its details displaced, and the group does not detect that. The exposed knobs carry the
manifest's `min` and `max` in `group.fugue`, so an embedded instance keeps them (the face takes `step` and `integer` from the first target, so
the integer inner input is listed first). Known limit: a dark-to-dark change (an old dark sleeve against a dark background) stays under the threshold and leaves dark
fragments at margin 16; margin 28 clears most of them. What the group cannot do (a relative or structural difference for
low-contrast changes, round morphology where Grow Mask leaves diamond facets, removal of small regions by area) is the case for a
Change Mask node, optional and not built.

**Generating with a mask, staged (14.3).**

- VAE Encode 1.1.0 takes an optional `mask`. The latent's `meta` carries `mask` (its media
  handle) and `image` (the picture encoded), as a conditioning carries a first frame (#130). An
  all-0 mask is `mask_empty`; an all-1 mask is the warning `mask_full` (that is plain
  image-to-image).
- The Sampler, given such a latent, sends the job inputs `mask` (one channel, scaled nearest to
  the latent's size) and `image` (the picture, which the library scales to the request's size as
  it does at the encode), with `denoise` as the strength. Text Encode does not change. The mask
  must be the size of the picture (else `node_failed` on `mask`); a latent the Sampler makes carries
  no mask. The Sampler's advanced `soft_mask` (boolean, default off, Sampler 1.1.0) adds the helper option
  `fugue.soft_mask: true` for such a latent (#179, 8.10); the mask file keeps its gray either way. The face shows `soft_mask` only when its latent comes from a VAE Encode with a mask linked, and `denoise_mode` only while Denoise is below 1 (#429, 6.1.1); a hidden value is kept and sent.
- The Sampler's advanced `variation_strength` (0 to 1, step 0.01, default 0) and `variation_seed` (Sampler 1.5.0, #553; the seed shows only above strength 0) are variation seeds: the start noise is the spherical blend of the Seed's noise toward the Variation seed's by the strength (ComfyUI_essentials' KSamplerVariationsWithNoise). Above 0 the Sampler adds the helper options `fugue.variation_strength` and `fugue.variation_seed`; at 0 it sends neither, so a workflow without it keeps its request and cache key; above 0 on a video latent it is the error `node_failed` on `variation_strength` (the engine varies pictures only), never a silent plain run. Strength 1 is a plain run at the Variation seed; a Variation seed equal to the Seed counts as Seed plus 1. Images only (not the hires, layered, HiDream-O1 or video programs). Needs this build's helper.
- Helper (8.10): `sample` and `generate` take `"mask": IN` (job input `mask`), read as one channel
  at the request's size; the three-channel reader used for pictures would break FLUX.1 Fill's
  packing of the mask, and stable-diffusion.cpp's own CLI reads it as one channel. An image
  model's `sample` with a mask also takes `"image"`. For an inpaint model (the library's version
  names `SD 1.x Inpaint`, `SD 2.x Inpaint`, `SDXL Inpaint`, `Flux Fill` and `Flex.2`, from
  `sd_get_model_version_name`) the helper passes the picture as `init_image` and no start latent,
  which the fork refuses for them; for any other image model it passes the start latent and not
  the picture. A video model with a mask is `input_invalid`.
- Fork: where a sample reads its conditioning (`fugue.cond_in`), the program keeps the statements
  that build an inpaint model's `concat` from this call's picture and mask, as a single run does,
  because the encode had neither. No change to sampling code.
- Fork, opt-in: `fugue.soft_mask` (a boolean image option, off by default), differential diffusion
  (arXiv 2306.00950, written from the paper, Algorithm 1). On: `src/api/fugue_stage.cpp` removes the
  threshold on the mask (kept for an inpaint model, which reads it as a channel it was trained on),
  and the blend that puts the start latent back (`latent x m + base x (1 - m)`, `blend_mask`)
  uses a mask of 0 or 1 made for each step: a latent cell (scaled by "nearest max" as before) is redrawn
  at step i of k when `m > (k - 1 - i) / k`, held to the start latent before. The paper's map `mu` is 1
  for no change and keeps the cell from the model when `mu < (k - t) / k`; with `m = 1 - mu` that is this
  inequality (strict, `t = k - 1 - i`). So 1 is redrawn from the first step, 0 never, 0.25 for the last
  quarter of the steps. A hard mask gives the same result on or off. Off, the one changed expression is
  the same code as before, so the output is byte-identical to upstream. The Sampler's `soft_mask` input
  sets it (a latent with a mask only). Not an option of the library's API: the owner proposes it upstream
  (the description is in [differential-diffusion-upstream.md](differential-diffusion-upstream.md)).
- What the library does with a mask, read at the pin (3b12781, `src/api/templates/image_inpaint.dgl`
  and `blend_mask`): rounds it to 0 or 1, so a soft mask is hard in the latent; scales it
  to the latent by "nearest max", so a latent cell is redrawn when any of its pixels is masked
  (blocks of 8 or 16 pixels); grows it one more cell for an inpaint model; and at each step puts
  the start latent back outside it (`denoised × m + init × (1 − m)`). Every pixel still passes
  through the VAE, so a graph puts the original back with a feathered mask (Composite Masked or
  Stitch). That is where the soft edge is made.
- Models at the pin. With a mask input of their own: FLUX.1 Fill dev, SDXL Inpaint, SD 1.5 and
  2 Inpaint, Flex.2. By the per-step replacement (any other image model): FLUX.2 Klein 4B and 9B
  (with the picture as a reference, as the edit templates do), Qwen-Image Edit, FLUX.1 Kontext,
  FLUX.1 dev, Z-Image, SDXL, SD 1.5. Soft masks (differential diffusion, #179: `fugue.soft_mask`, the Sampler's
  Soft mask) on the models of the per-step replacement; an inpaint model reads its mask as a channel and
  gets it rounded. Not: video models.
- `model_shape.cpp` must read an inpaint model's input as it reads Wan's 36 channels: SDXL
  Inpaint's 9 input channels and FLUX.1 Fill's 384 (96 per patch) do not match their VAEs' 4 and
  16, so today Fugue calls them `model_mismatch` and a stage refuses them.
- Guard: staged equals one `generate` run, byte for byte, on an RTX 3090, for FLUX.2 Klein 4B
  (masked sampling, and a VAE that encodes without drawing noise) and for FLUX.1 Fill when its
  file is at hand. The SD-family VAEs draw their latent from the encoder's distribution with the
  request's seed, so SDXL from a VAE Encode (which has no seed) can differ from one `generate`
  run; image-to-image from a latent always could, so this is not a mask matter.

**Generate Image** 1.1.0 (shipped, #175) takes an optional `mask` (role `mask`, only with `image`): the
standard parameter `mask` of `image.generate` (8.1), an input, an 8-bit gray PNG of the start image's
size, white redrawn. A provider says it takes one with `"mask"` in its `inputs` property (8.1); a provider
that does not is `mask_unsupported` on `mask` before the job starts, never a silent full redraw.

- Which providers. The `omarchy` provider does when the model's server lists `mask_image` in its
  capabilities (8.7): sd-server's native API, one channel, sent as `mask_image`. The `fugue-helper`
  provider does not offer `image.generate` at all (8.7): its way to a masked redraw is the staged path
  above (VAE Encode with a mask, a Sampler), and `mask_unsupported` says so. The `http` module and
  third-party provider modules declare `inputs: ["mask"]` in their `describe` (8.8) when they take one.
  The message names the providers and models that can (the selection's `preferredOffers`) and, when
  there are none, what to do (an Omarchy model whose sd-server lists `mask_image`, or the staged nodes).
- Routing. With a mask and no model chosen, a ready model that takes one wins over an earlier one that
  does not (8.3, `Requirement::preferred`), so a graph with a mask runs on the machine that can. A model
  named on the node, or a pinned provider, is that one, and fails with `mask_unsupported`.
- The mask is read as one channel and must be the start image's own size (else `node_failed` on `mask`,
  as VAE Encode's); it is cut the way the start image is (scaled to cover the size the node sends, cut
  from the middle, `image_cover`), so the two line up, and written as `mask.png` (8-bit gray) in the
  node's folder. A mask that is already that size goes as it is. A mask with nothing painted is
  `mask_empty`; one that covers everything is the warning `mask_full` (plain image to image). A mask without a start image is
  `missing_input` on `image`.
- Put back. The result is composited over the start image as it went to the model through the same
  mask, with Composite Masked's kernel (`compositeMasked`, `mask_ops.h`): out = start x (1 - m) + drawn
  x m, so what is outside the mask is the start image's, to the pixel, and the part that the model
  redraws in whole latent blocks beyond the mask is put back too. The mask is used as given: a soft
  edge is the person's Feather Mask in front of it (the library rounds the mask to 0 or 1 for the redraw
  itself, so only the blend is soft). When the provider returns another size than asked, the start image
  is cut to that size the same way. With no mask the node's request and result are what they were.
- Cache: the mask is an input, so it is in the key.

**Templates** (fugue-workflows, 11.10), both on the FLUX.2 Klein 9B files of the edit templates
(Q8_0, Qwen3-8B, flux2-vae: the same hashes and sources), 4 steps, guidance 1, denoise 1, each
checked on an RTX 3090 before it ships, with a preview and its sample picture:

- `inpaint-region`, "Inpaint: change a region": Load Image (a sample picture and a painted
  sample mask), the group "Inpaint (crop and stitch)", Save Image and Preview. The prompt says
  what should be there.
- `outpaint`, "Outpaint: extend a picture": Load Image, Pad (256 px left and right, feathering
  40), Text Encode (the prompt alone: the padded picture as a reference made Klein keep the gray border, Appendix A), VAE Encode (the padded picture and the
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

Built (#178, first pack): **Remove Background**, `examples/packs/io.github.bmaltais.fugue-remove-background`
(guide: [masking.md](masking.md#remove-a-background)). One persistent command node, `remove-background` 1.0.0, in
`Image/Mask`: inputs `image`, `model` (optional; the pack's resource type `.../birefnet-model`, folder `birefnet`),
`invert`, and the advanced `threshold` (0 keeps BiRefNet's soft edge) and `device` (`auto`, `cpu`, `cuda`); output `mask`,
an 8-bit gray PNG of the picture's size with `role: mask`. Its Python environment is made by the pack's `setup`
(`uv sync --no-dev --extra cpu`: ONNX Runtime, NumPy, Pillow); `--extra cuda` swaps in `onnxruntime-gpu`. The models come
from the pack's own catalog (`catalog/birefnet.json`, 8.9), so the picker offers them under "Download" with the licence
question.

Built (cutout, pack 1.1.0): the same pack ships the group **Remove Background**, `io.github.bmaltais.fugue-remove-background/cutout`
(`runtime.kind: "group"`, folder `cutout/`; the node it wraps is now named **Remove Background Mask**, 1.0.1, name only). Inner
nodes `find` (Remove Background Mask), `soften` (Feather Mask, direction inward) and `alpha` (Apply Mask as Alpha). Exposed: `image`,
`model` (optional), and in the Advanced section `threshold` and `feather` ("Edge shrink", default 0 = the model's edge); out: `image`
(the cutout, RGBA) and `mask` (the feathered mask, `role: mask`). The template `remove-background` (fugue-workflows) carries the pack
(`packs/<id>/`, 11.10) so the window offers to install it; without the pack the embedded group's `find` node is unmet and the
usual missing-pack banner names it.

Built (#245, G5): **Remove Object**, `examples/packs/io.github.bmaltais.fugue-remove-object` (guide:
[masking.md](masking.md#remove-an-object)), LaMa (big-lama, Apache-2.0) through ONNX Runtime. One persistent command node,
`lama-fill` 1.0.0, in `Image/Mask`: inputs `image`, `mask` (`role: mask`, white is what to remove), `model` (optional;
`.../lama-model`, `.onnx`, folder `lama`) and the advanced `device`; output `image`, the picture with the masked pixels
filled and every other pixel unchanged (alpha kept). The ONNX export (Carve/LaMa-ONNX `lama_fp32.onnx`, Apache-2.0, pinned
to a commit and a sha256 in `catalog/lama.json`) has a fixed 512 x 512 input, so the picture is mirrored out to a square
(the margin is never a hole), run, and the output taken back at the picture's size and mixed in by the mask's weight; a
hole is the mask at or above 128. The group `remove-object` 1.0.0 is Grow Mask (`grow`, default 8, Advanced) then LaMa
Fill. It is the pre-fill of the Remove action (`inpaint-workflows/remove-lama`: LaMa Fill where Fill Masked was, the same
Klein pass after). MAT (Places512, "research purposes only") is not offered. `onnx_node.py` is now copied in three packs.

Built (#347, Klein DA 8): **Depth Estimate**, `examples/packs/io.github.bmaltais.fugue-depth` 1.0.0, Depth Anything V2 through ONNX Runtime
(guide: the pack's README). One persistent command node, `depth-estimate` 1.0.0, in `Image/Analyze`: inputs `image`, `model`
(optional; `.../depth-anything-model`, extension `.onnx`, folder `depth-anything`), `resolution` (default 832, the shorter
side the picture is looked at, rounded to a multiple of 14 as Depth Anything V2's own `lower_bound` resize does) and the advanced
`device`; output `depth`, an 8-bit RGB PNG of the picture's size, min-max stretched, **near light** (the model's value is
largest for the nearest part; ControlNet's depth preprocessors give the same), a flat scene black. The catalog offers only
**Depth Anything V2 Small** (Apache-2.0; ONNX export `onnx-community/depth-anything-v2-small`, pinned to a commit, sha256 and
size checked 2026-11-23). Base, Large and Giant are CC-BY-NC-4.0 (the ComfyUI workflow's default is Large): never fetched, a
user may put their own ONNX file in the models folder. The template `flux2-klein-9b-da-edit-depth` (a sibling of the pack-free `flux2-klein-9b-da-edit`, so that template does not need the pack and a 99 MB model) carries the pack and the Small model and uses the node to replace Image 1 by its depth picture as the first reference (what the workflow's AnySwitch does); bypass the node (it then gives its input through, 3.5) to use Image 1 itself.

Built (#178, second pack): **Segment by Points or Box**, `examples/packs/io.github.bmaltais.fugue-segment` (guide:
[masking.md](masking.md#select-an-object-by-points-or-a-box)), SAM 2.1 through ONNX Runtime. One persistent command node,
`segment` 1.0.0, in `Image/Mask`: inputs `image`, `model` (optional; `.../sam2-model`, extension `.zip`, folder `sam2`), the
text inputs `positive` (Include points), `negative` (Exclude points) and `box`, `invert`, and the advanced `soft` and
`device`; output `mask`, an 8-bit gray PNG of the picture's size with `role: mask`. The prompts are plain text in the
picture's pixels (x right, y down, one `x y` per line; the box `x1 y1 x2 y2`), so any node can wire them; a clicking widget
on the node's face is a later slice and needs a new widget kind in 6.2 (no pack ships interface code). Its model is one
`.zip` per size with the encoder and the decoder inside (the file the catalog downloads and checks by sha256; the node reads
the parts in memory); the catalog (`catalog/sam2.json`) offers SAM 2.1 tiny and base+. The node keeps the sessions and the
embedding of the last picture while it is loaded, so only the first run of a picture pays for the encoder. Both packs share
`onnx_node.py` (kept identical by hand; extract it to a library with a third ONNX pack).

Built (#178, text to mask): a second node in the Segment pack, **Find by Text** (`find-by-text` 1.0.0, `Image/Mask`; guide:
[masking.md](masking.md#select-by-words)), GroundingDINO tiny through ONNX Runtime. Inputs `image`, `model` (optional;
`.../grounding-dino-model`, extension `.onnx`, folder `grounding-dino`), `query`, `threshold` (default 0.3) and the advanced
`max_results` and `device`; outputs `boxes` (text, one `x1 y1 x2 y2` line per match, best first, in the picture's pixels) and
`preview` (the picture with each box, phrase and score). It does not make a mask: `boxes` is wired to the `box` input of
`segment`, which now takes one box per line, one object each, joined in one mask (points then are refused). Text to mask is
two small nodes, not one, so each is useful alone (boxes for a crop or a script, Segment for clicks).

**Paint window (#181 slice 2, shipped).** The `paint` row of the Paint node opens `MaskEditor.qml` with `layers: true`:
the same frame, pointer, brush (Size, Hardness), Lasso and Rectangle, undo, zoom and keys as the mask editor, driving a
`PaintLayer` item (`canvas/paint_layer.*`, over `PaintDocument`; the 256 px texture cells are `canvas/cell_grid.*`, shared with
`MaskLayer`) instead of a `MaskLayer`. The bar gains the brush color (a swatch that opens the shared color picker) and the
brush Opacity; a layers panel at the right lists the layers top first, each with an eye, a kind glyph and its name (a double
click renames), the picture's placeholder as the bottom row ("The input picture" or "No picture: a blank canvas"), the active
layer's Opacity, add a paint layer, add a mask layer, move up, move down and remove (the picture layer cannot be removed).
The paint and picture layers are drawn as the result will be; the visible mask layers are the mask editor's tinted overlay (H shows them
alone, white on black); a canvas with no picture has a checkerboard under it. A mask layer is painted white whatever the color; painting
while the picture layer is active does nothing, and the footer says to pick another layer. The canvas is the input picture's size
(the file of the node arriving at `image`, so a picture that has not run gives the notice "Run the node above first"), else the node's Width
and Height, else the stored painting's own size. OK saves the layers as one `.ora` (`storeImage`: `<stem>-paint-<10 hex>.ora`, atomic, the same
layers one file) and `CanvasModel::setPaintPath` makes it the input's literal as one undo step; an editor with nothing painted
(only the picture, whole and visible) saves no file and empties the literal; Cancel, the close button and Escape change nothing. A stored
painting on another size is scaled and the window says so (the `mask_resized` text). `CanvasModel::paintState` is
{input, over, path, editable, linked, width, height}; `CanvasRuns::inputPicture` gives the picture arriving at an input.

**Blend modes and layer masks (#226, shipped in the document, the `.ora` and the canvas store).** Every layer of a `PaintDocument` has a **blend mode**, stored as its OpenRaster `composite-op` (`LayerInfo::compositeOp`; empty is `svg:src-over`), and a paint or picture layer can own a **layer mask**: one channel at the canvas's size (a `MaskPaint`, the buffer a mask layer has, so the mask brush can paint it later), white shows the layer, black hides it, grey in between. A layer mask is not a layer of the stack: it belongs to its layer, so it moves, is duplicated and is deleted with it and has no position of its own; it can be turned off (`maskEnabled`) without being lost. A *mask layer* (`LayerKind::Mask`, the paint window's) is the other thing, a selection-like mask flattened to the `mask` output, and does not take a layer mask. `flattenArea` (and so `flatten`, the history and compare pictures, PNG export, Canvas Region's composite and the Paint node) composites bottom up in straight RGBA: a layer's alpha is `αs = pixel alpha × opacity × mask` (mask only when enabled), then, with `B` the blend function per channel, `Cs' = (1 − αb)·Cs + αb·B(Cb, Cs)` and source-over: `αo = αs + αb(1 − αs)`, `Co = (αs·Cs' + αb·Cb·(1 − αs)) / αo` (the W3C Compositing and Blending formulas; Normal is computed as before, bit for bit). The blend functions, for channels in 0 to 1: Normal `Cs`; Multiply `Cb·Cs`; Screen `Cb + Cs − Cb·Cs`; Overlay `Cb ≤ 0.5 ? 2·Cb·Cs : 1 − 2(1 − Cb)(1 − Cs)`; Soft light `Cs ≤ 0.5 ? Cb − (1 − 2Cs)·Cb(1 − Cb) : Cb + (2Cs − 1)(D(Cb) − Cb)` with `D(Cb) = Cb ≤ 0.25 ? ((16Cb − 12)Cb + 4)Cb : √Cb`; Darken `min`; Lighten `max`; Add `min(1, Cb + Cs)`. Test values (`tests/engine/test_paint_document.cpp`), a 0.4 grey backdrop (102) under an opaque 0.6 layer (153): Normal 153, Multiply 61, Screen 194, Overlay 122, Soft light 114, Darken 102, Lighten 153, Add 255; Multiply at opacity 0.5 gives 82; Screen through a full mask at opacity 0.5 gives 148; over a transparent backdrop every mode shows the layer as it is. The `composite-op` names are `svg:src-over`, `svg:multiply`, `svg:screen`, `svg:overlay`, `svg:soft-light`, `svg:darken`, `svg:lighten` and `svg:plus` (Add: OpenRaster defines `svg:plus` as the Porter-Duff plus operator; Fugue draws it as Krita's and GIMP's Addition, which is what they write it for [U: not opened in Krita or GIMP]). Any other op a file has (`svg:color-dodge`, `krita:...`; at most 64 characters of `[A-Za-z0-9:._-]`, else `media_unreadable`) is read, drawn as Normal and written back unchanged. In the `.ora`, OpenRaster has no layer mask, so a masked layer degrades to what other programs show: its `src` is the layer as it looks (alpha times the mask, rounded per byte), and Fugue keeps the layer's own pixels in `data/layer-N-raw.png` (`fugue:raw`) and the grey mask in `data/layer-N-mask.png` (`fugue:mask`, `fugue:mask-enabled`); a mask that is off is not baked (`src` is the layer's own pixels, no raw copy). Krita, GIMP and MyPaint show the baked layer and ignore Fugue's entries and attributes; a file they save again has the baked layer and no mask, which looks the same [U: assumed from the OpenRaster rule that readers ignore what they do not know; not tested]. A layer's origin (who made it and when) is `fugue:producer` and `fugue:created`. The same layers still give the same bytes.

**Not in this design:** a paint canvas window with layers and colors, its own issue (#181; built: `engine/paint_document.*`, `MaskPaint` with 4 channels, the `.ora` file `engine/paint_file.*`, the `paint` widget and the Paint node, the paint window). Not built in the paint window: blend modes (the document has them, #226, and the Canvas view's layers panel sets them), groups, lock and alpha lock, duplicate and merge down, a bucket or color select, text and shapes, a picture layer you add or remove (the one picture layer is the node's input), layers renamed or restyled by undo (only adding, removing, moving and the strokes are undo steps). Soft masks (differential
diffusion, #179) are built as an opt-in change in the fork (8.10).

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
| #178 | Later: automatic masks as packs (Remove Background, Segment by Points or Box and Find by Text built) | #174 | |
| #179 | Soft masks (differential diffusion): the `fugue.soft_mask` engine change and the Sampler's Soft mask | #173 | |
| #180 | Color Match (shipped; the Outpaint template does not use it yet, see the row below) | #171 | |
| #181 | Later: a paint canvas (built: layers, color, undo, flatten; the `.ora` file, the `paint` widget and the Paint node, slice 1; the paint window, slice 2) | #174 | |
| #208 | Canvas 1 (epic #206): the revision store, `fugue/canvas` handle and `fugue canvas` commands (built; the window and template are the later slices) | #181 | |
| #209 | Canvas 2 (epic #206): Canvas Region and Canvas Write, head named in `compileForRun` (built) | #208, #176 | |

### 14.5 The canvas revision store (#206, #208)

Slice 1 of the canvas epic (#206, #208) is the store, the handle and the command line; slice 2 (#209) is the two nodes and the compile step, described at the end of this section. No window yet; later slices (the history panel, the generative fill template) build on them. The research and its measurements are in [canvas-research.md](canvas-research.md); the person's guide is [canvas.md](canvas.md).

A **canvas** is a chain of immutable, content-hashed **revisions**, and one of them is the **head**. Nodes are pure functions of their inputs (2.3), so a canvas is never edited in place: a change makes a revision whose parent is the one it started from, and the old ones stay.

**Layout** (`engine/canvas_store.*`), under `<data>/canvases` (`dataDir()`, so a project's own under `FUGUE_HOME`):

| Path | Holds |
| --- | --- |
| `blobs/<aa>/<hex>.png` | One tile: a 256 px square of one layer as a PNG (level 1: about 3x faster than level 6 for a few percent more bytes), named by the hash of its raw pixels. Shared by every canvas, so equal tiles are stored once. |
| `trees/<hex>.json` | A revision's content: `{"format": 1, "width", "height", "layers": [{"name", "kind", "visible", "opacity", "tiles": {"<x>,<y>": "sha256:..."}}]}`, layers bottom first. Since #226 a layer may also have `"blend"` (its `composite-op`, when not `svg:src-over`), `"mask": {"enabled", "tiles"}` (its layer mask, one-channel tiles) and `"origin": {"producer", "time"}` (who made the layer and when); each is written only when it is not the default, so a tree from before has the hash it had. |
| `canvases/<id>/canvas.json` | `{"format", "id", "name", "created"}`. The id is `canvas-` and 12 hex digits (`[a-z0-9][a-z0-9_-]{0,63}` is valid). |
| `canvases/<id>/commits/<hex>.json` | The commit record: `{"format": 1, "tree", "parent" (null for the first), "producer", "time"}`. |
| `canvases/<id>/head` | The head revision's hash, one line. |

**Tiles** (`engine/canvas_tiles.*`). A layer is cut into 256 px squares (edge tiles are smaller). A tile's hash is the SHA-256 of a header (`fugue-tile 1`, then its width, height and channel count) and its raw pixel bytes, so equal pixels are the same name whatever file or canvas they are in, and a one-channel tile can never equal a four-channel one. A tile whose bytes are all 0 (a mask's black, a layer's transparent) is not stored: a layer lists only its other tiles. A 512 px edit therefore adds the four tiles it touched and nothing else (measured in the research: 1.06 MB against 65 MB for a whole `.ora`).

**Two hashes.** The **tree hash** is the canonical-JSON hash (1.5) of the tree: pixels and layer properties (name, kind, visibility, opacity, blend, layer mask and its state, origin) only. It does not change with who made the revision, when, or in which canvas, and it is what a cache key uses (a later slice); the same pixels always give the same tree hash. The **revision** is named by the canonical hash of its commit record, so two commits of one tree are two revisions. Layer ids, the undo history and the active layer are not saved. A picture layer is a placeholder, as in an `.ora` (14.4): the picture is a node's input, never part of a revision.

**Immutable and checked.** Every object is written once, whole or not at all (a partial file, then a rename), and never changed. Reading one checks it against its name (a tile is decoded and re-hashed, a tree and a commit re-hashed); a mismatch is `canvas_corrupt`, never wrong pixels.

**The head** is moved only by compare-and-swap: `moveHead(canvas, expected, revision)` takes a per-canvas lock (`head.lock`), moves the head when it is `expected` now and says `{moved: true}`; when it is not, it leaves the head alone and says `{moved: false, head: <what it is>}`. That is a result, not an error: the caller rebases onto the new head or keeps both, and the store does neither. A revision added but not moved to stays stored (a **candidate**, which a later slice accepts or drops). `commit` is `addRevision` then `moveHead` from the head the caller last saw. A revision is named by `head`, a full hash, or the start of one (8 hex digits at least, unique).

**`.ora`.** `fugue canvas import` reads a file with `readCanvasFile` (an `.ora` through the paint file reader, or a PNG, JPEG or WebP picture as one layer) and `export` writes with `paintToOra`, so a canvas imported and exported again is the same bytes, and the same pixels give the same tree hash. The `.ora` is the exchange format; the store is where a canvas lives.

**The `fugue/canvas` handle** (`engine/canvas_handle.*`, declared in `pack.json` like `fugue/stitch`). A data value (2.6) `{"data": "fugue/canvas", "path": <work folder>/canvas.json, "meta": {"canvas": <id>, "revision": <hash>}}`: a descriptor, never pixels, and nothing in `meta` is a path. `canvas.json` holds the same two keys as canonical JSON, so the same descriptor is the same bytes. The revision is in the value, so a different revision is a different cache key. Reading a handle that has other keys, a bad id or a bad hash is `input_invalid`.

Tests: `tests/engine/test_canvas_tiles.cpp`, `test_canvas_store.cpp`, `test_canvas_handle.cpp`, and the command line in `tests/cli/test_cli_canvas.cpp`.

#### Canvas Region and Canvas Write (#209)

Both are built-in nodes in the `Image/Canvas` category (`nodes/canvas_nodes.*`; the arithmetic is pure functions in `nodes/canvas_region_ops.*` and Crop to Mask's `cropAt`, the stitch handle's reading and writing is `nodes/stitch_handle.*`, shared with Crop to Mask and Stitch).

**Canvas Region** (`fugue/canvas-region` 1.4.0; 1.4.0 made an empty `canvas` the Active canvas, #246; 1.1.0 added `layer`, #226; 1.2.0 added `selection`, #230; 1.3.0 added `target_megapixels` and the padding floor, #242): inputs `canvas` (default empty, the Active canvas, 14.5; or an id, a plain `line` text: not a `file` widget, which would hash the whole `.ora` and ignore the revision), `revision` (advanced, default `head`), `layer` (advanced, default empty: the composite, the visible layers with their opacity, blend modes and layer masks, as the Canvas view shows them; a name: that layer alone, its own pixels without its opacity, blend or mask, the top one of that name, `input_invalid` naming the layers when there is none, or when it is a mask layer), `mask` (optional), `selection` (advanced, text, default empty: a selection painted in the Canvas view and stored by content in the canvas store, named by its `sha256:` hash; with one it is the part to change and `mask` and the rectangle are not used, read from `selections/<hex>.json`; `canvas_not_found` for a hash the store has none of, `input_invalid` for text that is not a hash, `mask_empty` when nothing is selected, `mask_resized` when it is another size; the hash is a literal, so it is in the cache key and `compiled.json` and a replay is exact, docs/canvas-actions-design.md 3), `x`, `y`, `width`, `height` (the rectangle, used without a mask; 0 for a width or height runs to the edge), `padding` (px, default 64; raised to `feather` + 16 with the warning `padding_raised`, because Canvas Write's blend stops at the region's box and a smaller padding ends in a hard edge, #242), `scale` (0.25 to 4, default 1), `max_megapixels` (the cap, default 1.5; 0 for none), `target_megapixels` (default 0 = use `scale`; above 0 the size is Crop to Mask's `cropSizeFor`, up or down to about that many megapixels, `scale` unused, the cap still only brings it down; 0 to 16, else `value_out_of_range`; #242), `feather` (px, default 0) and `multiple` (advanced, default 16). Outputs `image`, `mask`, `stitch`, `canvas` (a `fugue/canvas` handle at the revision read), `width`, `height`. The region is the rectangle, or the bounds of the mask (an empty one is `mask_empty`, a mask of another size is stretched with `mask_resized`), grown by `padding` and moved inside the canvas as Crop to Mask does (`paddedBox`), redrawn at `regionSize`: the box times `scale`, brought down to the cap keeping its shape, each side a multiple of `multiple` (rounded down when the cap bites, so it holds), no side above 4096 (Krita's resolution multiplier and pixel cap). `image` is the composite of the visible layers at that revision, with the canvas's transparency; `mask` is the rectangle (or the mask) cut and resized the same, softened outward by `feather` canvas pixels (Krita's denoise zone). The stitch is Crop to Mask's `fugue/stitch`: its `image` is the whole composite and its `mask` the unfeathered rectangle or mask, both written to the node's folder, so it works for Stitch as it is (an unchanged region stitched back is the canvas) as well as for Canvas Write. Also reported before a run (`check`): a `canvas` that is typed and is not a canvas id (`input_invalid` on `canvas`); an empty one is the Active canvas, named when a run compiles, and the store is not read.

**Canvas Write** (`fugue/canvas-write` 1.7.0, `sink: true`; 1.7.0 added `onto_pixels` (#314): a paint layer's name (the top one of that name at the top level) and `image` goes into that layer's own pixels instead of a new layer, inside the stitch's box and through the feathered mask, `old*(1-m)+picture*m` in premultiplied terms (`layerWithPicture`), ONE revision through `CanvasStore::setLayerPixels` (the layer's mask, name, opacity and blend stay, a placed layer's `source` is dropped), which Undo takes back; it is refused as `input_invalid` for a locked layer, a missing one, a group, picture or mask layer, and when `onto` is also set. **Head moved:** the picture is composed once over the layer as read; when the head moved meanwhile the write is made again from the new head (up to four times) if the layer is found there with the same name and the very same tiles, so layers added, moved or masked elsewhere do not matter, else the run fails with `canvas_head_moved` and writes nothing (a person's edit to that layer is never overwritten by a stale result; a new layer, which only adds, can always be placed). Region's `layer` and this input resolve the name with one function (`topLayerNamed`: the top layer of that name that is not a picture layer, a group counts), and a group or mask layer is `input_invalid`; the same pixels as the layer holds write nothing and keep its source; the errors say whether the store could not read the new head (`canvas_unreadable`), the layer really changed, or the canvas kept changing (`canvas_head_moved`). An action whose layer role targets `onto_pixels` is checked at planning (the layer must exist and have pixels) and refuses Count above 1 (`act_refused`), since variations would race for the same layer. Version 1.6.0 and older workflows replay unchanged (the input defaults to empty, so a compiled.json that does not name it runs as before); 1.6.0 added `onto` and `layer_mask` and made `image` optional (#231 slice 6): with `onto` (a layer's name, the top one of that name that is not a picture layer) no layer is added and `layer_mask` becomes that layer's layer mask through `setLayerMask`, inside the stitch's box and through the mask (`old*(1-m)+new*m`, the whole layer without a stitch), as one revision; a locked layer, a missing layer or a missing `layer_mask` is an `input_invalid`/`input_missing` error and a head that moved meanwhile is `canvas_head_moved`, writing nothing; 1.5.0 added `visible` and `suffix` for the task bar's Count, #231; 1.2.0 added `bake`, the layer mask and the names, #226; 1.3.0 lands runs directly and rebases onto a moved head, #229; 1.4.0 removes the `candidate` input, #239): inputs `canvas` (the handle Region emitted: the revision the work started from), `image`, `stitch` (optional), `mask` (optional, at the canvas's size), `blend` (px, default 16, 0 to 512), `bake` (advanced, default off), `name` (the layer's, default `Fill`), `visible` (advanced, default on: off lands the layer hidden) and `suffix` (advanced, default empty: put after the name with a space, "(2 of 3)"); outputs `canvas` (the new revision), `accepted` and `head`. **The layer and its mask (#226).** As Photoshop's Generative Fill does, the new layer is the redrawn picture resized to the stitch's box, whole and with its own alpha, and the blend (the mask feathered outward by `blend`, 0 outside the box) is its **layer mask** (`placedLayerWithMask`), so the person can turn the mask off or (later) paint it to show more or less of the fill; the composite is the old baked layer's within rounding (half a byte of alpha, tested). `bake` on gives the layer of #209 below, with the blend in its alpha and no mask. **The name.** `name` left at `Fill` or empty names the layer from the prompt upstream: the first `prompt` text met walking the Write's links back through the workflow as run (`NodeContext::document`, the Text Encode that made the picture), one line, cut at a word to 40 characters with an ellipsis; with none, `Fill n`, one more than the largest `Fill n` of the revision read. Any other name is kept. The layer's origin is `canvas-write:<node>` and the time it was made (`LayerInfo::producer`, `created`), which the layers panel shows. The rest of this paragraph is the placement of #209, which `bake` keeps and the layer mask splits in two: The layer is built from Stitch's math, not the full composite: `image` is resized to the stitch's box, its alpha is the mask (the stitch's own, or `mask`; all 1 without a stitch) feathered outward by `blend` (Krita's blend, apart from Region's feather) times the picture's own alpha, and everything outside the box, or where the alpha is 0, is empty, so the revision adds only the tiles the box touches. Without a stitch the picture covers the whole canvas (resized to it). A picture whose shape differs from the region's by more than 2% is `stitch_mismatch`. The node never edits a revision: it loads the revision Region read, adds the layer, stores the result as a revision with that revision as parent, and moves the head by compare-and-swap from that revision (`moveHead`): by default a run **lands directly on the head** as a layer, with no review step (the Photoshop model, #229: the person deletes the layer if they do not like it, and Undo removes it too). When the head is still there `accepted` is true. When something else moved it meanwhile (the compare-and-swap lost), the node does not demote the run to a candidate: it **adds the run's layer onto the head as it is now**, as a new revision (the same `accept()` placement as 14.5: layers only add, producer `accept:<short hash>`; a write that races this one again is retried against the newer head, four times), `accepted` is true, `head` and the `canvas` output name that new revision, and nothing is warned. Only when that is impossible (the canvas has another size now) the run **fails with `canvas_head_moved`, in plain words, and writes nothing** (#239): the size is checked before anything is stored, and in the rare race where it changes between the check and the write the revision is hidden (`drop`) before the error is returned, so no candidate waits for review. `accepted` is therefore always true on a result. **Candidate review is not a product concept** (#239; the Photoshop model: a run is a layer, delete it or Undo): the `candidate` input of 1.1.0 to 1.3.0 is gone from the node and its UI, and a saved `candidate` value in an older workflow is ignored with no error and no notice, by the manifest's `retired_inputs` (3.6; the compiler skips those names). The Canvas view has no new-candidate banner, no pending chip, no Accept or Drop and no candidate overlay, and settings have no `canvas_show_new_results`; the store's candidates, `fugue canvas candidates|accept|drop|diff` and the History window stay for scripts and film teams (the History window is to be retired later). The Write is an ordinary cached sink: the same inputs are a cache hit, so re-running a compiled graph does not write twice. Also reported before a run (`check`): `onto` and `onto_pixels` both set (`input_invalid` on `onto_pixels`).

**Head, named once** (`engine/canvas_revisions.*`, called from `compileForRun`, 9.1). After the compile, `resolveCanvasRevisions` replaces the `revision` literal of every `fugue/canvas-region` node (`head`, a short prefix, or nothing at all) with the full revision hash, through the setup's `resolveCanvas` callback (`sessionCanvasResolver()` is the store; `runCompileSetup` sets it, and a test passes a fake). A node whose `canvas` or `revision` is a link is left as it is and its kernel refuses a revision that is not a full hash (`input_invalid`). The hash is then an ordinary literal in `compiled.json` and in the node's cache key (10.1), which gives the rules the epic asks for: the same revision gives the same bytes, a re-run of the same compiled graph is a cache hit, a run can be replayed with `fugue run compiled.json`, and a later compile sees the head as a Write moved it (a different key, so Region runs again). The key is the revision (the commit), which determines the tree; keying by the tree hash alone, so an identical picture made by another commit is a hit, needs a kernel `keyExtra` for node kernels and is left for later. A partial run after a Write compiles again and sees the new head; freeze Region, or give it a revision hash, to keep reading the old one.

Tests: `tests/nodes/test_canvas_nodes.cpp` (the geometry, both kernels, and a workflow through the scheduler: head named once, same revision same bytes, the re-run a cache hit that writes nothing twice, the CAS conflict) and `tests/engine/test_canvas_revisions.cpp` (the compile step with a fake resolver).

**Canvas New** (`fugue/canvas-new` 1.0.0, `sink: true`, `deterministic: false`; #231 slice 8): inputs `image` and `name` (default `Upscaled`, empty is `Untitled`; a name another canvas has gets ` 2`, ` 3`, ... after it); outputs `canvas` (a `fugue/canvas` handle at the new canvas's one revision) and `id`. It makes a canvas of one paint layer, named as the canvas was asked to be (without the number of a taken name: `Upscaled`; `Layer 1` when the name is empty, #311), holding the picture whole at its size (`canvasFromPixels`, producer `canvas-new:<node>`), without reading any canvas. `deterministic: false` puts the run id in the cache key, so every run makes a canvas (a cached result would name a canvas that may be gone). A picture over the canvas limit is `canvas_too_big` on `image` (nothing is made). A canvas action whose workflow ends in Canvas New instead of Canvas Write (`checkAction` collects them as `ActionGraph::news`; the action then needs a Canvas Write or a Canvas New) has its result as a new canvas: `CanvasActions` reads the new id from the node's `canvas` output and emits `canvasMade(source, made)`, which the Canvas view that shows `source` answers by opening `made`. The block's `selection` may be `none` (slice 8): the action takes the whole picture; `bindAction` leaves the rectangle, the stored selection, the padding and the scale out and the Region keeps the workflow's own, and Generate ignores any selection. The templates `upscale` (Real-ESRGAN x4plus through Upscale with Model) and `upscale-no-model` (Lanczos) are Region (whole picture, scale 1, no cap, multiple 1) -> [Upscale with Model] -> Resize to (Region width and height times `scale`, two Number Math nodes) -> Canvas New; `scale` is an Advanced input with `choices` 2x and 4x and `choice_values` 2 and 4 bound to both multipliers.

**The Active canvas (#246).** An empty `canvas` on a Canvas Region means the Active canvas: the canvas shown in the Canvas view (dialog or area) that was used or focused last, else the newest canvas of the root. Each Canvas view reports the canvas it shows to the window's `ActiveCanvas` (an App-level object, one per window; `canvas/active_canvas.h`), so the engine has no dependency on any view. `compileForRun` names it with the head (`RunCompileSetup::activeCanvas`, `resolveCanvasRevisions`): the id is written into the compiled node as a literal, so it is in the cache key and in `compiled.json` and a replay is exact; Canvas Write takes the Region's handle and follows. A headless run (`fugue run`, `compile`, `validate`: no window) names the newest canvas of the root; with no canvas at all the input stays empty and the Region fails when it runs with `input_invalid` on `canvas`: "Choose a canvas" in plain words, and what to do. A pinned canvas (a literal id) is never changed: workflows saved with one keep it, and the Generative fill template and bundled action ship with the empty canvas. In the window the input's picker lists **Active canvas** first (checked when empty); a pinned input shows "<name> · pinned" and the first row takes it back to the Active canvas in one step; the Canvas view's notice "reads another canvas" appears for pinned nodes only and offers Show, Use this canvas for the node, and Use the active canvas. Editing the rectangle or selection in the view leaves an empty `canvas` empty.

**Candidates, diff and fill (#210).** A **candidate** is a stored revision that is not in the head's history (the ancestors of the head, found by walking parents) and has no resolution. Nothing new is stored for a candidate: it is a commit nobody points at, which is what `addRevision` and a lost `moveHead` already make. Since #239 no node makes one on purpose (Canvas Write 1.4.0 has no `candidate` input and always lands on the head); the command line (`addRevision`, `fugue canvas`) and a lost race can still leave one. `CanvasStore::candidates()` lists them newest first with `onHead` (the parent is the head now). `accept()` takes one: made from the head, the head moves to it (compare-and-swap, so a head that moved meanwhile is `moved: false` and nothing changes); made from an older revision, it is **rebased** when the candidate's tree is its parent's layers unchanged plus more layers on top and the canvas has the same size: the new layers are placed on the head as a new revision (producer `accept:<short hash>`), and that revision becomes the head. Anything else (an edited existing layer, a different size) is `canvas_conflict`, and the person drops it and runs again. `drop()` hides one. Both leave a one-line file `canvases/<id>/resolved/<hex>` (`accepted <revision>` or `dropped`) because the objects are immutable and nothing is deleted; a dropped candidate stays loadable and `accept()` still takes it, and a revision that is already history or accepted is `canvas_not_candidate`. `engine/canvas_diff.*` compares two trees (no pixels read): layers are paired by name in order, then added, removed or changed, with the tiles that differ, the rectangle they cover and changed properties (`visible`, `opacity`, `kind`); the command line prints it and the viewer draws it. A Canvas Region whose `canvas` is blank compiles and is refused when it runs (`input_invalid`: no canvas is chosen), so a template can ship without one.

The template **`generative-fill`** (fugue-workflows; category `canvas` since #231 slice 2, which also moved it to FLUX.2 Klein 4B and gave it an `action` block, see Canvas actions below; the description that follows is of its first form): Canvas Region (a rectangle, padding 64, about a megapixel), Text Encode, VAE Encode (the region's picture and mask), Sampler (FLUX.2 Klein 9B, 4 steps, guidance 1), VAE Decode, Canvas Write (the fill lands on the head as a layer, #229; #239: the template never mentions candidates), and Preview, as plain nodes, with the canvas id left blank for the person to fill. The loop: `fugue canvas new` (or import), set the template's canvas and rectangle, run: the fill is a new layer on the head, to hide, edit or delete (`fugue canvas undo` removes it). For a review step, turn Canvas Write's Keep as candidate on, then use `fugue canvas candidates`, `diff`, and `accept` or `drop`; running again from the same head gives another candidate to compare.

**The revision history** (#210, #214; `canvas/canvas_history.*`). The work of the former File > Canvas history… window (retired into the Canvas view by #228, D1: `CanvasHistoryDialog.qml`, `file.canvas-history` and its test are gone) is `CanvasHistory`: the revisions and candidates of a canvas (candidates first, then the history from the head back, each with its short hash, producer and a tag: Head, Candidate or Dropped), `accept`, `drop`, `exportPicture` (the shown revision flattened with `flattenToPng`, never replacing a file) and a summary of what differs (`diffTrees`: layer added, removed or changed, tiles and the rectangle they cover, a size change). The Canvas view uses it for its History panel (All revisions lists the candidates, open or dropped, read only), its Compare (a candidate against the head, any other revision against its parent) and its Export. The window and the command line use the same store functions, so either sees the other's change after `reload()`.

**The Canvas view** (#221, epics #216 and #217; `canvas/canvas_view_model.*`, `canvas/CanvasView.qml`, `canvas/CanvasViewDialog.qml`). File > Canvas… (`file.canvas-view`) opens the picture of a canvas. `CanvasView.qml` is a self-contained item: its whole context is a canvas id (empty: the newest canvas of the store), the store root (empty: the session's) and an optional **follow target**, a document's `WidgetEdit` and a Canvas Region node id; it needs no dialog or area around it, so the workspace areas (#217) can host the same item as the `canvas` view type (the dialog is the first host, `LargeViewDialog`; with the Graph's selection of exactly one Canvas Region as the follow target, which also moves the view to that node's canvas). `CanvasViewModel` is `CanvasHistory` (the same store calls, so the History window, the command line and this view agree) with a head picture, a region and a refresh. The model's `choices` and `earlier` (the strip's lists until #228) remain for tests and the History dialog; the view shows the History panel instead (below). The picture is one revision alone at full size (`CompareView` in `single` mode: wheel zoom, drag to pan, Fit and 100% at the end of the tools or on Home or `0` and `1`, `+` and `-`, the keymap of every picture and the Graph, 12.4). A revision kept apart (a candidate made by the command line) is only looked at (#239): it is listed under **All revisions** in the History panel, shown alone and read only, with **Back to latest** in the layers panel; the view has no new-candidate banner, no "N pending" chip, no Accept or Drop and no overlay toggle. Export… saves the shown revision (never replacing a file). **Compare** opens the five modes of Compare Images (`CompareModeButtons.qml`: Slider, Slider horizontal, Side by side, Flip, Difference; the one last picked in a canvas surface comes back, `compareMemory.js`), against the **head** for a candidate and against the parent for any other revision. With a follow target the node's rectangle (solid) and its padded context box (dashed) are drawn over the picture in canvas pixels, and a line says what the node reads only when it is pinned to a step other than the latest version (#234; `canvasViewRegionNote`, "Canvas Region reads “<step>”": the node by its title, `CanvasModel::nodeName`; with **Use for region**); a node that follows the latest version, the normal case, says nothing. **The rectangle is edited on the picture** (#224): it is a `CropFrame` (the Crop window's and the region picker's frame, with `dimAlpha` 0 so the picture stays whole), with eight handles; a drag inside it moves it, a drag on a handle resizes it (Shift keeps the shape, Alt works from the center), and a drag on empty picture draws a new one from that point in any direction (`crop::drawn` in `engine/crop_geometry.h`, also `CropGeometry.drawn`; Shift makes it square, Alt draws from the center, a press that does not move draws nothing, a press outside the picture is not taken). **Snap to <multiple>** in the tools row (the node's Multiple of; off at first) is a `constrain` function on the frame, so the frame, the numbers and the written values are the snapped ones; a move or a held ratio snaps the top left only. The status line's selection (`canvasViewStatusSelection`, "456 × 777 at 0, 253"), the context box's small label (`canvasViewContextLabel`, "Context 456 × 777") and the frame's tooltip (the context and the size the model gets, with the cap) are computed live from the drag by the node's own functions (`CanvasViewModel::previewRegion`, over `canvas/region_pick.h`). Releasing writes x, y, width and height as ONE undo step through `WidgetEdit::setRegion` (`CanvasViewModel::setRegion`; no Apply button, direct manipulation); the node's `revision` is not touched, so drawing on a candidate does not repin it (that is **Use for region**). Escape during a drag (`CropFrame::cancel`) leaves the node as it was and the dialog open; with no drag it closes the dialog as before. The wheel zooms, the middle button or a left drag with Space held pans (the frame hands the press to the viewer, `yieldPress`), and neither writes anything. The frame is drawn and not edited when X, Y, Width or Height is linked to another node (`region.editable`) or while Compare is on (a left drag there moves the divider), each with a hint under the picture (`canvasViewRegionHint`; there is no how-to line: the tool strip's tooltips teach the drag, #234); with no follow target a drawn rectangle says what to do with it. **The status line** (#234, the owner: "Not sure this is needed either"; Photoshop's is the reference): the only text under the picture in the normal case is one thin row (`canvasViewStatusLine`): `canvasViewStatusZoom` ("32% · 2000 × 2000", the zoom and `pictureSize`), `canvasViewStatusSelection` (the dragged or set rectangle, "W × H at X, Y", canvas pixels) and **Advanced details** (`canvasViewAdvancedDetails`, a toggle, off at first) which adds `canvasViewStatusDetails`: the context and model size, what the node reads, the revision shown and the store diff of the shown step against its parent (`CanvasHistory.summary`, no longer a line of its own). `CompareView.showSize` (off here) drops the size chip over the picture; a size mismatch of a compare is still said. Notes, banners and errors stay for what a person must act on. **Use for region** writes the node's `revision` input as one undo step ("head" when the head is shown, so the node keeps following it, else the full hash); it is shown only while the node is pinned to a step other than the latest version, disabled when the node reads the shown one already or when the Revision input is linked to another node, and absent when the node reads another canvas. The view watches the store's folders (the canvases folder and the canvas's own with `commits` and `resolved`), so a head moved by a run or `fugue canvas accept`, a new candidate or a new canvas shows within a moment; a view on the head moves to the new head, one on another revision stays on it. **No candidate review** (#239; #225's new-result banner and its `canvas_show_new_results` setting are gone): a revision that arrives for the shown canvas (a candidate made by the command line) is only listed; the view never moves to it, shows no banner and has no Accept or Drop. A run's layer lands on the head (Canvas Write 1.4.0) and a view on the head goes along with the short notice below. A read of the store that fails (a run is writing, a slow disk) is tried again up to three times, 400 ms apart. **One bar, one look** (#228, the cohesion batch): the File > Canvas… dialog turns its own bar off (`LargeViewDialog.showBar`) and puts its close in the view's `trailing` slot, so the bar reads, as the Mask editor's and the Crop window's: the canvas, then the tools in chunks 4u apart (Layers; Compare and the modes; Snap; Export…; Fit and 100%; the host's close). Toggles and compare modes are the quiet marking. Each revision is named once (the step in words, or Latest), on the picture's labels and History. The canvas button opens the canvas picker's own rows (`canvasMenu.js`: name, size and last change, the id) with **New canvas…** (`NewCanvasDialog.qml`, the picker's dialog, which the view shows the made canvas of: `CanvasHistory::useCanvas`) and **Import picture…** under a hairline (`CanvasHistory::importPicture`: the canvas is made and shown); an empty store shows "No canvases yet" with the same two buttons in the picture's place. Keys: Enter is the host's (a dialog's confirm); while the view has the focus, a key of the Graph (`CommandRegistry::ownerOfPress` and `actsOnGraph`: Home, `.`, Delete, Ctrl+A, Tab and the rest of the Graph's edit, node, group and view keys) and the bare Q (kept for a quick mask) do not reach the window, Escape is the window's (the run's, or the dialog's close) unless a rectangle drag is on, and Ctrl+Z stays the document's (the region's undo). Right to left the bar, the notes and the layers panel mirror; the picture and the region frame never do. The area host draws the dialog's surface color behind the view and its 2u inset. **A run's layer on the Head** (#228, the owner's decision: the review step is not the normal flow): when the Head moves by itself while the view is on it, the view goes along (as before) and `CanvasViewModel::headNotice` says which layers came ("Layer Fill 1 added", "2 layers added": the layer names of the new Head against the old one; nothing when none were added), shown as a short notice over the picture's foot (`canvasViewHeadToast`, 10 s, a close) with **Undo** (`CanvasViewModel::undo`, the Head back one step); anything else shown ends it. **The follow target and the free selection** (#228, owner reports): the view takes the Graph's `selection` and follows the selected Canvas Region, else the one last followed or chosen while it reads the shown canvas, else the only Region node that reads it (`WidgetEdit::regionNodes`; a node with no canvas counts, `region.adopts`, and `setRegion` or `useForRegion` writes the shown canvas to it in the same undo step); the follow is applied through a delayed `Binding`, since following moves the view to the node's canvas, and setting the same node again never moves it back. With no node followed a left drag draws a view-only rectangle (`CropFrame` with a `rect` of no size draws nothing until then): with several Region nodes on the canvas a `WidgetMenu` (`canvasViewRegionChooser`, the nodes by `CanvasModel::nodeName`) asks which one it is for and writes it as one undo step, remembered as the target; with none, **Add a Canvas Region node** (`WidgetEdit::addRegionNode`: the node at the middle of the graph's view, reading this canvas, with the rectangle, one undo-stack macro). A node on another canvas (`region.problem` `otherCanvas`, `known`, `nodeCanvasName`) or a linked canvas is a notice in plain words (`canvasViewCanvasMismatch`) with **Show <its canvas>** (`showNodeCanvas`, when this root has it) and **Use <this canvas> for this node** (`useCanvasForNode`, one undo step); following never moves the view to an id this root lacks. **The layers panel read only** (another revision than the Head shown): the reason by case (a new candidate, a dropped one, an earlier revision) and **Go to Head**, and for a candidate Accept (not the accent fill: the view's bar has the primary) and Drop, hidden while the new-candidate banner shows its own (`decisionElsewhere`). **History instead of the strip** (#228, the owner's decision: "all of this needs to disappear"; Photoshop's History panel is the reference): the default view shows no revision chips and no hashes. The bar starts with the canvas button and, only while candidates wait, one chip `N pending` (`canvasViewPending`, `CanvasViewModel::pendingCount`) that shows the newest for its review; then **History** (`canvasViewHistory`, closed at first) and **Layers**. The History panel (`canvasHistoryPanel`, 56u at the start side of the picture) lists `CanvasViewModel::steps`: the latest version's revisions newest first, each named in words from its tree against its parent's (`stepName`: Layer added: <name>, Layer deleted: <name>, Renamed: <name>, Layers reordered, Shown/Hidden: <name>, Opacity N%: <name>, Blend mode: <name>, Mask added/deleted/on/off: <name>, Painted: <name>, Canvas size w × h, Canvas made, Picture imported; cached per revision), the time at the right ("Latest" on the newest), the hash only in the tooltip; a click or Up and Down shows that step read only, **Back to latest** returns; **All revisions** (`allSteps`) puts the candidates first ("Candidate: …", "Dropped: …"), which is where the earlier strip's Earlier menu and the dropped candidates live now. The picture's labels are words (Latest, Candidate, the step's name); the region line says "reads the latest version" or the step's name; user text says "the latest version" where it said Head (the History dialog and the command line keep their words). Undo and Redo stay the Layers panel's. It is also the `canvas` view type of the window's areas (12.16, `workspace/CanvasArea.qml`), with the same context and its canvas kept in the area's state. Not here yet: painting, pinning a revision in the strip. Tests: `tests/canvas/test_canvas_view_model.cpp` (the model: strip, overlay, region, preview and snap, one undo step, refresh, new results), `tests/workspace/test_area_split_window.cpp` (the area host shows a new result and its banner decides), `tests/engine/test_crop_geometry.cpp` (`drawn`) and `tests/canvas/test_canvas_view_window.cpp` (the real window, offscreen, QTest events only).

**The New canvas dialog** (#232, epics #206, #216 and #217; `engine/new_canvas.*`, `canvas/new_canvas_model.*`, `canvas/NewCanvasDialog.qml`). Photoshop's New Document, on a `Question`. **File > New canvas…** (`canvas.new`, Ctrl+Alt+N, app context, before Canvas…) opens it anywhere; a `canvas` input's **New canvas…** opens the same dialog (one per picker, `NewCanvasDialog`) and puts the new id in the input as one undo step. The fields: **Name** (the first free `Untitled <n>` of this root, never a name a canvas of the root has; a name left blank is the next free one); **Start from** Blank, Clipboard or A file…; for a blank canvas **Preset** (Square 1024 × 1024, Landscape 4:3 1024 × 768, Wide 16:9 1280 × 720, Portrait 3:4 768 × 1024, HD 1920 × 1080, 4K 3840 × 2160, Custom: a typed size that is a preset's lights it, any other lights Custom), **Size** (width × height in px) and **Background** (Transparent, White, Black or Color: a hex field and the color picker); a preview of the shape in the background (a checkerboard for transparent) and one line: the size, the megapixels, and the cap (at most 16384 px a side and 67 megapixels, the engine's `paintFileMaxSide` and `paintFileMaxPixels`, also `fugue canvas new`). **Clipboard** is on only while `QGuiApplication::clipboard()` holds a picture: opening the dialog then starts on it (Photoshop's way), the size fields show the picture's size and cannot be typed in, and Create makes a canvas of one layer, `Layer 1`, with the picture's pixels (straight, not premultiplied alpha). **A file…** opens the file dialog (PNG, JPEG, WebP, `.ora`: Import picture…'s `readCanvasFile`); the line shows the file and its size and the name becomes the file's unless one was typed. A reason a canvas cannot be made (a size over the cap, a field that is not a whole number, a bad color, a clipboard without a picture, a file that is no picture) is the engine's sentence in the error color and **Create** waits; Cancel then Create sit at the right, Create the one highlighted button; Enter in a field creates. A blank canvas of a background other than Transparent has one opaque layer named `Background` (Photoshop's), else one empty `Layer 1`. **The last settings come back**: preset or size, background and color are kept in `<state>/new-canvas.json` (the project's own under a project) once a canvas was made, never the name or the source. **Paste as new canvas** (`canvas.paste-new`, File, no key) makes a canvas named `Pasted <n>` from the clipboard's picture at once; it is off while the clipboard has none. What a command shows afterwards: the active area when it is a Canvas area is pointed at the canvas, else the Canvas view's large view opens on it; a canvas input uses it and opens nothing. Tests: `tests/engine/test_new_canvas.cpp` (backgrounds, caps, layers), `tests/canvas/test_new_canvas_model.cpp` (presets, sizes and refusals, names, remembered settings, clipboard, file, paste), `tests/canvas/test_new_canvas_window.cpp` (the real window, offscreen, QTest events only, the clipboard of the test process), `tests/canvas/test_canvas_picker.cpp`, `tests/cli/test_cli_canvas.cpp` (`--background`).

**The region picker** (#213; `canvas/region_picker.*`, `canvas/region_pick.*`, `canvas/RegionDialog.qml`). The **Pick region…** entry of Canvas Region's `canvas` drop-down (after Import picture…; disabled with no canvas, or one this root does not have) opens a large view (`LargeViewDialog`) on the canvas at the node's `revision`: `head` is resolved when the window opens (as a run names it when it compiles), a hash or prefix is read as given. The picture is the revision flattened (`PaintDocument::flatten`), kept as a PNG of at most 2048 px on its longest side in the window's own temporary folder, so a large canvas draws and pans fast while every number stays in the canvas's own pixels. The viewer and the rectangle are the Crop window's (`CropView`, `CropFrame`, `engine/crop_geometry.h`): zoom and pan, a rectangle moved by dragging inside it and resized by its eight handles, Shift keeps the ratio, Alt resizes from the center, the ratio lock (Free, Original, 1:1, 4:3, 3:2, 16:9, Turn); X, Y, Width and Height are numbers under the picture, in step with the rectangle. **Snap to N** (N is the node's `multiple`, off when it is 1) puts every edge of the rectangle on a multiple of N when a drag ends, a number is entered or the button is pressed (the canvas's own far edge counts as one, so the region can reach it); with a ratio lock only the top left snaps and the size is left, so the lock is never broken. The **dashed outer box** is the padded area the node will cut and the line under the picture gives its size and the **model size** (width by height, and "at the N MP cap" when the cap, not the scale, decided it): both come from the node's own functions (`paddedBox`, `regionSize` of `nodes/canvas_region_ops.h`), called by `region::context` and `region::modelSize`, and a test compares them with the node's for many rectangles, so they cannot drift from what the node computes. The numbers come from the node's inputs (`WidgetEdit::regionState`): the stored value or the default; an input that is linked has no known value before a run, so its default stands (a linked X, Y, Width or Height makes **OK** unavailable and says so; a linked mask is said to win over the rectangle). **OK** (or Enter; the Crop window's and the mask editor's word for the commit that closes) writes `x`, `y`, `width` and `height` as ONE undo step ("Pick region", `WidgetEdit::setRegion`; four explicit numbers, never a 0 that runs to the edge) and only when one changed; Cancel, the close button and Escape change nothing. Opening and dragging write nothing. A canvas that cannot be read says why in the window instead of the picture.

**The Canvas view's menu bar (#232 slice B).** `canvas/CanvasMenuBar.qml` is the first row of the view in the File > Canvas… dialog, and the Canvas area's header (#262 slice 3) in an area: Canvas (was File), Edit, Select, Layer and View, the registry's `canvas-view` menu tree (`menu/menus.cpp`; each row a `canvas-view.*` command, or a `planned` placeholder that is no command: greyed, no key, tooltip "Not built yet (#N)", resolved by `CommandRegistry::menu`). The view gives each row its state (`CanvasView.menuState`: enabled, checked) and runs it (`windowRequested`, live while the pointer is over the view, the focus is in it or one of its menus is open: `menuOpen`). The store gained `rename`, `duplicate` (the whole history under a new id), `remove` and `clear` (one new revision of the same size with one empty layer; producer `canvas-view:clear`), and the export writes .jpg, .webp and .ora by extension. Escape clears the selection when no drag or run wants it (`escapeDeselects`). Tests: `tests/engine/test_canvas_store.cpp`, `tests/canvas/test_canvas_view_model.cpp`, `tests/commands/commands_tests.cpp`, `tests/canvas/test_canvas_view_window.cpp`.

**Canvas resize (#240; `engine/canvas_resize.*`, `engine/edge_fill.*`, `CanvasStore::resize`, `fugue canvas resize`).** The store grows, shifts or cuts a canvas as ONE new revision: the new canvas is a rectangle `x,y,w,h` of the old one, which may reach past any side. Unlike a layer edit it re-places pixels, so every layer is cut and moved into the new size (tiles re-cut; those that are the same pixels keep their hash, the rest are new blobs); a layer mask is white in a new area so a grown layer is not hidden; a picture layer stays a placeholder. The pre-fill of the new area is the benchmark's winner (docs/inpaint-outpaint-research.md 4.6): the nearest picture pixel stretched outward and blurred (24 px), on a new BOTTOM layer `Expanded area` that is transparent over the old picture, so it can be hidden or deleted and the old pixels are never touched. `edge_fill` is a plain-pixel component (float buffers, a hole mask, a mode: gray, edge, edge-blur, transparent) that Pad and Extend Edges (#243) use too. Choices made: the fill reads the flattened visible layers (what the person sees), not one layer; the nearest-pixel search is two sweeps of the 8 neighbours (exact outside a rectangle, which is every outpaint); the blur premultiplies alpha so a transparent pixel's colour does not bleed; gray and edge are offered beside edge-blur and transparent because the code is shared. Not in this change: the window's Expand action and the Canvas Size/crop tool (#231, #237) call `CanvasStore::resize`.

**Layer edits, undo and redo (#226).** A layer operation is an edit of a revision's tree, never of pixels (`engine/canvas_layer_ops.*`, pure functions on `Tree`): set visible, opacity or blend, rename, delete, move (a block of layers, kept in order, so the lowest ends at a position among the others), duplicate (each copy above its layer, named `<name> copy`, `copy 2`, ..., sharing its tiles and mask), turn a layer mask on or off, delete a layer mask. One edit can take several layers (a selection) and is still one revision. Layers are named by position, bottom first, at the top level of the tree (groups, #252, are one layer each; see below). `CanvasStore::editLayers(canvas, base, edit, producer)` applies an edit to the head, which must be `base` (the revision the caller shows): it stores the new tree (its tiles are the base's, checked: an edit adds none, so nothing is encoded and no blob is written), a commit whose parent is `base`, and moves the head by compare-and-swap; when the head is not `base` nothing is written and the answer says what it is; an edit that changes nothing writes nothing. Only the head is edited, so an edit never forks the history. The producer is `canvas-view:<op>` from the Canvas view and `canvas-cli:<op>` from `fugue canvas layer`, `<op>` one of `visible`, `opacity`, `blend`, `rename`, `delete`, `move`, `duplicate`, `mask-enabled`, `mask-delete`. **A layer mask painted or added (#230)** is the one edit that brings tiles: `CanvasStore::setLayerMask(canvas, base, position, mask, producer)` cuts the one-channel mask (the canvas's size) into tiles, writes only the blobs the store has not got (a stroke in one corner of a large mask adds that corner's tile, the rest are the base's hashes), and applies the `SetMask` op (`mask-set`: the layer's `maskTiles`, the mask on; refused for a mask layer or more than one layer) through the same compare-and-swap commit as `editLayers`; a mask equal to the layer's writes nothing. The Canvas view's producers are `canvas-view:mask-add` (Add mask: all white), `canvas-view:mask-paint` (a group of strokes), `mask-invert`, `mask-fill` and `mask-clear`; History names them "Mask added: <layer>" and "Painted: <layer>". **Selections** (#230): `CanvasStore::storeSelection(mask)` keeps a one-channel selection by content, its tiles in `blobs/` and `selections/<hex>.json` (`{format, width, height, tiles}`, named by its canonical hash, written once), and `selection(hash)` reads it back, checked against its name; nothing is deleted yet, as for revisions. Showing a layer that was hidden is a new revision with the old tree: same pixels and properties, same tree hash, another revision. **Undo** (`CanvasStore::undo`, `fugue canvas undo`) moves the head to its parent by compare-and-swap and writes `resolved/<hex>` = `undone` for the revision it left, which keeps it out of the candidates (it is not a result waiting for review); **redo** moves the head to the newest `undone` revision whose parent is the head. Any head can be undone, a Canvas Write's accepted layer included; the first revision cannot (`canvas_no_step`). An edit that lost the head's race is marked `undone` too. Tests: `tests/engine/test_canvas_layer_ops.cpp` (each op, the tree hash rules, one revision per op with no new tile, the stale base, the undo and redo chain, a rebased accept that keeps the added layer's blend and mask) and `tests/cli/test_cli_canvas.cpp`. `engine/canvas_diff.*` also pairs a layer left over after the name pairing with one of the same kind and pixels (a rename) and names the changed properties `name`, `blend` and `mask` besides `visible`, `opacity` and `kind`.

**The Layer menu's operations and groups (#252).** `engine/canvas_layer_ops.*` also has `New` (an empty paint layer, above one layer or on top, `Layer N`), `Group` (the chosen layers, in order, become the children of a new group at the place of the highest of them, `Group N`), `Ungroup` (a group is replaced by its children) and `Properties` (a list of `steps`, each one of the edits above, applied as ONE edit: the Layer properties dialog's name, opacity, blend and mask switch or delete in one revision and one undo). `engine/canvas_layer_merge.*` has the two that touch pixels, `MergeDown` and `Flatten`: they composite the layers as the picture shows them (`PaintDocument::flatten`: hidden and mask layers do not draw, opacity, blend and layer mask do), cut the result into tiles through the store's blobs and replace the layers in the tree; `CanvasStore::editLayers` sends them there (`commitTree`, the same compare-and-swap commit, with new tiles allowed). Merge down keeps the lower layer's place, name, blend and origin and bakes its opacity and mask into its pixels (the merged layer has opacity 1 and no mask); Flatten makes one `Background` of every shown layer, drops hidden layers and keeps a mask layer above it. **A group is a layer record with `group: true` and `children`** (bottom first, nested groups allowed to depth 8; the kind stays Paint and `tiles` is empty); its tree JSON is `"kind": "group"` with a `"children"` array, written only for a group, so a tree with none keeps its hash and a store from before groups reads one as an unknown kind (`canvas_corrupt`) rather than as an empty layer; `storeFormat` stays 1. A group's own opacity, blend, visibility and layer mask apply to its children's composite. A `PaintDocument` has no groups: `documentOf` (so `load`, the nodes, the exports and the view's picture) turns a group into the one paint layer its children composite to, with the group's name, opacity, blend and mask, so every reader sees the picture as drawn. **Writers keep groups (#274):** the three writers that used to go through a document now build their revision at the tree level, so nothing is refused for a group. A Canvas Write puts one new layer record on top of the base revision's tree (`CanvasStore::storeLayerTiles` stores the layer's and its mask's tiles, `addLayerRevision` adds the revision; the head's move and the race rules are unchanged; a head that moved to a grouped revision is fine, only a changed canvas size still fails the run); `CanvasStore::resize` is `commitTree` of `resizedTree` (`engine/canvas_tree_resize.*`): every layer, every child of a group at any depth and every layer mask is read, moved by the rectangle's corner and cut again (a new area is transparent, a mask is white there), and the optional pre-fill bottom layer is made from the picture the old tree shows (a group as its children draw it); a tree without groups gets the tree, and so the hash, the document path gave (tested). Resizing to the same rectangle still makes a revision (`commitTree`'s `keepSame`). The rebase of `accept` onto a newer head appends the layers the candidate added (compared with its parent as before: changing any existing layer, group children included, is `canvas_conflict`) to the head's tree with `commitTree`: no pixel is read or encoded. What stays refused, and why: a candidate that changed or removed a layer it was made from, or a canvas that has another size now, cannot be placed on a newer head (`canvas_conflict`, `canvas_head_moved`): their layers no longer fit. Other `PaintDocument` writers: making a canvas, importing (`create`, `createWithEdits`, PSD and .ora import) and Clear write a fresh document, so no group exists to lose; the batch, Export and the exports read a revision (a group is flattened into the picture, or one layer per group) and write no revision; the paint window and its widget edit a `PaintDocument` in memory and never write the store. Group rules: groups nest at most `maxGroupDepth` (8) deep (Group refuses the ninth level, the store refuses to write a deeper tree); only paint layers and groups can be grouped, and a stored group holding a mask or picture layer is `canvas_corrupt`; Ungroup refuses a group that is hidden or has its own opacity, blend or layer mask (it would lose them: set them to default in Layer properties first); Merge down refuses a layer whose mask is turned off (turn it on, which bakes it in, or delete it). The layers panel lists a group as one row of kind `group` (its children are not listed; Ungroup takes them out); a tree-level edit (rename, opacity, blend, visibility, mask, duplicate, move, delete) works on it as on any layer. `canvas_diff` names a changed group's children `contents`. The Canvas view's Layer menu (`canvas-view.new-layer`, `.merge-down`, `.flatten`, `.group`, `.ungroup`, `.layer-properties`) and the panel's right-click menu call `CanvasViewModel::newLayer`, `mergeDown`, `flatten`, `groupLayers`, `ungroup` and `setProperties`; History names them "Layer added: Layer 4", "Merged down: <upper>", "Flattened", "Grouped: <name>", "Ungrouped: <name>", "Properties: <name>". Producers are `canvas-view:<op>` and `canvas-cli:<op>` with `<op>` `new`, `group`, `ungroup`, `merge-down`, `flatten`, `properties`. Tests: `tests/engine/test_canvas_layer_ops.cpp` (the pure ops, merge and flatten keep the picture up to 8-bit rounding, a group in the store, the hash rule), `tests/canvas/test_canvas_layers_model.cpp`, `tests/cli/test_cli_canvas.cpp`.

**Groups in the layers panel (#278).** `LayerEdit::parent` (engine/canvas_layer_ops.h) names a group by the indexes from the top level; `layers`, `to` and a new layer's place are then among that group's children, so every layer edit (visible, opacity, blend, rename, lock, delete, duplicate, move, new, group, ungroup, mask switch) works inside a group, nested at most `maxGroupDepth`. A locked group refuses what adds, removes, moves or masks its layers; showing, opacity, blend and names stay open. Merge down and Flatten stay at the top level. `engine/canvas_layer_tree.h` (`LayerOutline`) is the panel's tree: rows with ids (a top-level layer's id is its position), open state kept by names, a hidden group's `shown` and a locked group's `lockedAbove` for its layers, the selection a closing group takes over, and the drop line resolved within a group. `fugue canvas layer` still names top-level layers only. Not built: dragging a layer into or out of a group.

**A picture placed as a layer, and the layer's original (#236).** `engine/canvas_layer_source.*` is the optional `LayerRecord::source` (`"source"` in a layer's tree JSON, written only for a layer that has one: a tree without one keeps its hash, `storeFormat` stays 1): `{width, height, tiles, mask: {tiles}?, transform: {cx, cy, width, height, angle, flipX?, flipY?, filter}, rendered}`. The tiles are the original's own four-channel (and one-channel mask) tiles at the original's size, blobs like any other; `transform` says where the original lands on the canvas (it is scaled to width x height, turned by `angle` degrees clockwise about its center (cx, cy), the numbers kept to a thousandth); `rendered` is `renderedKey` of the layer's tiles and mask the transform produced. The layer's own `tiles` stay what a revision SHOWS, so the composite, the nodes, the flattened export, .ora and .psd read the shown layer and never the original (a file leaves with the transformed pixels, canvas sized, its layer mask; the original is kept in the store and a `.fuguepack`, whose tile lists walk it). `treeFromJson` reads it as untrusted (size, tile keys inside the original's own size, finite numbers within `maxTransformSide` 32768 and `maxTransformArea` 16384 x 16384, a known filter, a hash for `rendered`); the tile caps count it; a group, mask or picture layer cannot have one. A layer is *current* while `renderedKey(tiles, mask)` equals `rendered`; a layer painted on, or given another mask, since then is not, and its next transform starts from the layer as it is (the old original is ignored, then replaced). `CanvasStore::pasteLayer` (engine/canvas_transform.h `placeableProblem`: at most 16384 px a side and 64 megapixels) keeps a pasted picture whole as the original and shows it centered on whole pixels, cut at the canvas's edge, as ONE revision, named "Pasted N" (the lowest N no layer has) unless a name is given, above one layer or on top; producer `canvas-view:paste`, History "Layer pasted: <name>". `CanvasViewModel::pasteLayer` and `placeFile` read the clipboard or a file through `canvas/picture_import.*` (a size cap read from the header before a decode, a file at most 256 MB and named as a picture). Commands: `canvas-view.paste-layer` (Ctrl+V, context `canvas-view`, which is more specific than the Graph's `canvas`, so the Graph's Ctrl+V still pastes nodes), `.paste-canvas`, `.place-picture`. Tests: `tests/engine/test_canvas_paste.cpp`, `tests/canvas/test_canvas_paste_model.cpp`, `tests/canvas/test_canvas_view_window.cpp`, `tests/commands/commands_tests.cpp`. **Transforms (#236).** `engine/canvas_transform.*` draws an original with a `LayerTransform`: flipped, scaled to `round(width) x round(height)` (separable per axis on premultiplied float alpha; Lanczos a = 3 or Catmull-Rom bicubic, support widened by the shrink factor, edges repeat their pixel; nearest picks the pixel the new one's center falls in), then turned about the center of that box when the angle is not a multiple of 90 (the same filter at the turned positions, a 6 x 6 or 4 x 4 neighbourhood, a soft one pixel edge, hard for nearest; a quarter turn is exact and hard-edged). Only the canvas's window, and the rows and columns of the original it needs, are computed. A layer mask is drawn through the same placement as a gray picture (255 where there is no picture). `transformedTree(tree, position, transform, read, consume)` is the pure edit, `CanvasStore::transformLayer` stores it as ONE revision (`commitTree`, new tiles allowed; the same placement writes nothing; a group, mask or picture layer, a position that is not there, an empty layer and a placement outside the limits are `input_invalid` in words). A layer without a current original takes its content bounds (`originalOf`: the tiles it has, which are not rewritten) as the original at the identity placement. `CanvasStore::resize` shifts the placement with the layer and re-keys `rendered`. `canvas-view:transform` History names come from comparing the layer's placement with its parent's. `CanvasViewModel::layerBox`, `transformLayer`, `flipLayer`, `turnLayer` and `moveLayerBy` are what the window calls (positions are the tree's, bottom first). Storage decision: the shown tiles stay the picture, so every reader, `.ora` and `.psd` included, writes the transformed canvas-sized layer with its mask and never the original, which a `.fuguepack` and a duplicate keep. **Memory and races (review fixes):** a render is drawn in 256 px tiles of the canvas (`RenderOptions`), each from the window of the scaled picture it needs, with the source rows added in one at a time, so the float buffers are a tile's whatever the sizes (a test bounds `peakBytes`, and any tile size draws the same pixels); it still runs on the calling thread (the window's, for a commit: a very large transform can pause it, not yet moved to a worker). `CanvasStore::transformLayer` takes the `SeenPlacement` the caller read and refuses ("changed since it was looked at") when the layer is not that; the model stores a pending mask stroke first, then reads the placement. `CanvasStore::resize` draws a placed layer again from its original on the new canvas. `MaskPaint::fromPicture(bytes, maxPixels)` and `pictureSize` read the size from the decoder's header, so a picture file is read once and a too large one is refused before it is decoded. Ctrl+V over the view with no picture on the clipboard, or on an earlier step, is the Graph's paste. Tests: `tests/engine/test_canvas_transform.cpp`, `tests/canvas/test_canvas_paste_model.cpp`. **Move and Free Transform gestures (#236).** `canvas/transform_math.*` is the geometry (pure; `TransformGesture`, a QML type in `canvas/transform_gesture.*`, wraps it over placement maps): a handle drag is computed from the placement the drag began with and the two pointer points, in the box's own turned frame (the opposite side holds, Alt holds the center, Shift keeps the proportions, the size is at least 1 px and never flips), a drag outside a corner turns by the angle the pointer sweeps about the center (Shift: 15° steps), a move shifts by the pointer's travel (Shift: one axis). `snap::snapFloating` (canvas/snap.h, `GuidesModel::snapFloating`) is the snap for a rectangle that may leave the canvas; `snapRect` clamps into the canvas and rounds, which a Region needs and a layer must not have. `CanvasViewModel::beginTransform` writes two pictures once per revision and layer (the canvas without the layer, and the layer's original) so a drag draws the original at the draft placement over the canvas; `pictureSource` is the first while a box is open; nothing is stored until `transformLayer`. `TransformFrame.qml` draws the box, handles and turn rings; `CanvasView.qml` owns the session (`xfPlacement`, `freeTransform`; Enter, Escape and the arrows are Shortcuts that only act while it is on). Tests: `tests/canvas/test_transform_gesture.cpp`, `tests/canvas/test_snap.cpp`, `tests/canvas/test_canvas_transform_window.cpp`.

**The layers panel (#226; `canvas/CanvasLayersPanel.qml`, `canvas/DragNumberField.qml`, `CanvasViewModel`'s layers).** The Canvas view has a panel at the right of the picture (56u, 2u from it; **Layers** in the tools hides it; it is not shown while comparing) with the shown revision's layers, top first, as an outliner: compact 32px rows of a 24px thumbnail (the layer's own pixels on a checkerboard), a second 24px thumbnail for its layer mask (dim and crossed when off), the name (dimmed when hidden) and the eye at the right, as an outliner's toggles. A click selects a layer (Ctrl adds or takes one away, Shift a range from the last one picked; a layer is always current, the top one to start); a double click on the name or F2 renames it in place; a drag moves the selected layers, an accent line showing the gap they land in; a right click opens a menu of the same operations with their keys (Hide or Show, Show this layer alone, Rename…, then Delete under its own heading, then Duplicate, Move up, Move down, then the layer mask's Delete the mask and Turn off or on; no delete is the last row). The eye shows or hides the layer (the whole selection when the row is in it); **Alt+click shows that layer alone** (solo), a view-only state that makes no revision, says "Showing <name> alone" with a button to end it, and ends when another revision is shown. Shift+click on the mask turns it off or on. Under the list, the current layer's properties: **Opacity** as a drag-number field (drag to scrub over 200 px for the whole range, Shift ten times finer, Ctrl on 10% steps; a click types it, a double click puts 100% back; the value follows the pointer and one revision is made on release), **Blend** (a menu of the eight modes, the current one checked), for a layer with a mask **Mask on/off** and **Delete mask**, the layer's origin ("From Canvas Write write, 2026-11-22 10:00", also the row's tooltip), then **Duplicate**, up and down, and after a gap twice the others **Delete**. The header has **Undo** and **Redo** of the canvas. Every edit is `CanvasStore::editLayers` on the shown revision when it is the head, so it is one revision (producer `canvas-view:<op>`) and the view moves on to it at once; an edit that changes nothing makes none; on any other revision (an older one) the list reads only and says "Layers are changed on the head: go to Head to change them." The selection follows an edit (the copies after Duplicate, the moved block, the layer under a deleted one). Rows carry `depth` (0 now) so groups can be a nested list later without a new model. Keys: the `canvas-layers` commands of 12.4. Tests: `tests/canvas/test_canvas_layers_model.cpp` (the model) and `tests/canvas/test_canvas_layers_window.cpp` (the real window offscreen, QTest events only).

**The tool strip and the mask brush (#230; `canvas/CanvasView.qml`, `canvas/BrushPen.qml`, `CanvasViewModel`'s mask brush, `canvas/mask_layer.*`).** A narrow column of tools at the start of the picture (7u, scrolling in a short area, never wrapping), Photoshop's order and keys: Hand (H; Space held is a moment's Hand with any tool; a left drag pans), Zoom (Z; a click zooms in by 1.25 at the pointer, Alt+click out, or the other way with Zoom out in the options), then Rectangle region (M or R; the frame of the followed Canvas Region or the free selection, as above; the default tool, and the only one that edits the frame: with another tool it is drawn only), then the Mask brush (B) and eraser (E; X swaps them), then Quick Mask (Q) at the foot. Tooltips name each tool and its key; the current tool is the quiet marking. An options bar between the bar and the picture holds the current tool's settings (it wraps in a narrow area): Snap for the region; Zoom in or out; for the brush what it paints, Size (canvas pixels, `[` and `]` by a quarter, Alt+drag on the picture: left and right size, up and down hardness), Hardness and Opacity as drag-number fields, and the target's own: a layer mask's Overlay (\\) and menu (Invert, Ctrl+I; Reveal all; Hide all), Quick Mask's Use selection and Clear selection. **What the brush paints:** with Quick Mask on, the view's selection; else the layer mask the panel targets: Add mask (the panel's button under the list, its row menu, Ctrl+Shift+M) gives the selected layer a fully revealing mask as one revision and targets it, and a click on a mask's thumbnail targets that one (the thumbnail gets a 2px accent frame and the options bar says "Mask of <layer>"); selecting another layer, or showing a revision that is not the head, ends the target; with neither, the options bar offers Add mask and Quick Mask and a press paints nothing. On a layer mask the brush hides (towards 0) and the eraser shows (towards 255), and the hidden part is drawn in red (`look.error` at 50%) while Overlay is on, which it is to start; with Overlay off the picture shows the result, revision by revision. The strokes go into a `MaskLayer` at once (only the 256 px cells a dab touched are uploaded) and are stored after the brush has rested 400 ms as ONE revision (`setLayerMask`, `canvas-view:mask-paint`); a tool change, another layer, Undo or Redo store them first. A head moved by a run meanwhile takes the strokes when the layer is still at its position under the same name. Ctrl+Z over the view goes back one revision while a mask is painted and the mask is read again from it; Invert, Reveal all and Hide all are one revision each. **Quick Mask:** the same brush paints the selection, in red where selected, kept in the view (not a revision; Ctrl+Z undoes its strokes one by one; a canvas change clears it). **Use selection** stores it by content (`storeSelection`) and writes its hash into the followed Canvas Region's `selection` input and its bounds into x, y, width and height, as one undo step of the document ("Use selection"; a node with no canvas yet takes the shown one in the same step), so the frame, the padded box and the size the model gets read right and the next run redraws the painted part and lands as a layer on the head. A rectangle drawn on the picture afterwards empties `selection` in the same step as its numbers (a new selection replaces the old). The pointer is `BrushPen.qml`, the Mask editor's and the paint window's own (one stroke code: dabs, Alt+drag sizing, the ring, tablet pressure, the lasso and rectangle there). Keys: the `canvas-view` commands of 12.4. Tests: `tests/engine/test_canvas_store.cpp` (setLayerMask, the shared tiles, selections), `tests/nodes/test_canvas_nodes.cpp` (Region's `selection`), `tests/canvas/test_canvas_mask_brush.cpp` (the model: Add mask, batching, undo, invert, fill, clear, the target's end, Use selection) and `tests/canvas/test_canvas_tools_window.cpp` (the window offscreen, QTest events only, and its `FUGUE_SHOT_DIR` pictures).

**Marching ants (#313; `canvas/selection_ants.*`, `canvas/selection_contour.*`, `CanvasView.qml` `canvasViewAnts`).** The selection's edge is drawn as a black and white dashed line that moves 8 px a second (a 125 ms timer that runs only while there is an outline). `SelectionAnts` is a `QQuickPaintedItem` over the picture's view with `rect` (picture pixels: the free rectangle or the Region node's area) and `mask` (a `MaskLayer`: `selectionMask`, which the lasso, Quick Mask, Modify and a layer's mask or alpha all write); it never takes the pointer. The mask's outline is `selectionContour`: marching squares at 127.5 on a grid whose step is the smallest power of two that is at least 2 screen pixels (`contourCell`), aligned to multiples of the step, over the part of the picture the item shows plus a cell, with a linear crossing on each edge; the polylines (a closed one ends on its first point) are cached until the mask (`edited`, `historyChanged`, `imageSizeChanged`), the zoom, the origin or the size changes, rebuilt on the next event loop turn. The cost is the view's node count, not the picture's. The ants show when a rectangle exists (only while no mask selection exists) or a mask selection exists, with Quick Mask on or off, not over Compare or the difference view; the red tint (`canvasViewSelection`) shows only with Quick Mask on and `selectionOverlay` (\\, off to start), or by itself when the mask has no 50% contour (`ants.outlined` false). **A brush release is the selection (#313):** in Quick Mask `BrushPen.pressedAt` paints a rectangle into the mask (`selectionToMask`) before the first stroke, a stroke adds (the eraser subtracts), `onPainted` sets `selectionLast` so Ctrl+Z reaches the selection after Q, Deselect sets it too, Modify and Transform no longer switch Quick Mask on, Select all fills the mask when one exists, and `canvas-view.use-selection` is enabled whenever the mask is non-empty (the Graph's Region is still written only by Use selection). Tests: `tests/canvas/test_selection_ants.cpp` (contour and item, no window) and `tests/canvas/test_canvas_tools_window.cpp` (`theSelectionHasMarchingAnts`, `aQuickMaskStrokeIsTheSelectionOnRelease`; `FUGUE_SHOT_DIR` pictures `ants-rect.png`, `ants-lasso.png`, `brush-selection.png`). Not built: a reduced-motion setting.

**Not here yet (later slices of #206; for layers, #226; for the tool strip, #230):** lasso, Move layer and Eyedropper tools, painting a layer's own pixels, merge down, lock (and alpha lock), groups, a layer mask added from a selection, the "use in region" toggle per layer, Canvas Write onto an existing layer or with a blend mode of its own, layer transforms; a **Select mask…** that opens the paint window on the canvas as its backdrop, so a mask can be painted over the real picture (#213's second half; the paint window draws over the node's input picture or a blank canvas, a canvas backdrop needs a source other than a node's), a canvas mask layer as Region's selection, a position on Write, a revert and the changed rectangle drawn over the picture in the window, retention and garbage collection (nothing is ever deleted now), a canvas editing window, and a canvas in a share package. Decisions are in Appendix A (rows marked #208 and #209).

**The canvas action router (#233; `engine/canvas_router.*`, a pure function with no Qt).** `routeAction(input, tuning)` decides which generative action the task bar runs and on what region, from the canvas size, the selection's bounds (canvas pixels; past the edge is allowed) and how much of them is selected, the crop frame (the canvas bounds the crop tool asks for), whether there is a prompt and a strength below 1. The rules, in order: no canvas, none; a crop frame past the canvas, **Expand** (wholly inside it is a crop: none); no selection, **Refine** of the whole canvas with a prompt, else none ("Select an area first"); a selection past the edge, **Expand** to the union of canvas and selection; fewer than 64 selected pixels, none (`mask_empty`); a strength below 1, **Refine**; 90% of the canvas or more selected, **Refine** with a prompt, else none; no prompt, **Remove**; else **Generative fill**. The route also gives the Region's `padding` (half the selection's longer side, held to 64..256; Remove at least 128), the region box (grown by the padding and moved inside the canvas, as Crop to Mask does), the Region's `scale` (up to 1 megapixel when the box is below 0.8 of it, at most 4), the strength (Refine 0.7: a noise level, light 0.55, restyle 0.9), whether the selection touches the edge, and for Expand the new canvas bounds, the fill (each grown strip plus 32 px of overlap into the old picture), the region (the fill plus 256 px of the old picture, cut at the canvas) and the number of passes (a side grown by more than half the old side is split). Every route carries its reasons in words. The numbers come from the benchmark in [inpaint-outpaint-research.md](inpaint-outpaint-research.md#6-the-router) and are fields of `RouteTuning`. Tests: `tests/engine/test_canvas_router.cpp`.

**Canvas actions and the task bar (#231 slice 1; `engine/canvas_action.*`, `canvas/canvas_actions.*`, `canvas/CanvasTaskBar.qml`; design: docs/canvas-actions-design.md).** A canvas action is a workflow with a top-level `action` block (design 1.2: `format`, `name`, `description`, `glyph`, `selection` required, optional or bounds, `region`, `inputs` with a `role` of prompt, negative_prompt, strength, seed, model or layer, or a plain `name`, and `targets`, `label`, `section`, `default`, `min`, `max`, `choices`, `choice_values`, `optional`, `fallback`; slice 4: `choice_values` is a list of numbers, one per choice, so a word stands for a number the binding puts in the targets, which must take a number, with a `default` that is one of the words). The block is kept as an unknown top-level key (4.1) and is outside `sourceHash`, so it changes no cache key. `parseAction` and `actionOf` read it (`action_invalid` in words for a malformed block, `unsupported_format_version` for a later format); `checkAction` finds the Canvas Region that takes the selection (`region`, or the only one) and the Canvas Writes, and checks every target: a top-level node's input or a group instance's exposed input, not linked, of the role's type (text for prompt, negative_prompt and layer, number for seed and strength, a resource for model), one type per input; every Write must take its canvas from that Region or from a Region with a blank canvas, else "This action writes to another canvas". `bindAction` makes the run's copy: the Region gets the shown canvas, the **full hash** of the head that was shown, the rectangle (or a stored selection's hash and its bounds; a rectangle removes any selection the file held) and the router's `padding` and `scale`; another Region with a blank canvas gets the same canvas and revision; each bound input's value goes into its targets, held to its `min` and `max`; an input with no value keeps its block default, else the workflow's own; an empty prompt takes its `fallback`, and is refused for a prompt that is not optional. `CanvasActions` (App.canvasActions) lists the actions of its roots (the bundled ones under `:/fugue/actions`, from `share/actions/*.fugue`), each with its broken reason and its missing models (`Workflows::missingModels`, the template cards' count), and **Generate** binds and queues the copy through `RunService` with the action's file as the workflow (so the queue panel names it): a random seed unless one is given; Auto takes `routeAction`'s pick (only Generative fill exists in this slice, so the other routes say what to do instead); a selection that is too small, a missing prompt, missing models or a broken action is refused in words before anything is queued. It follows its runs' events (`CanvasRuns` ignores runs of no tab): progress of the running node, Cancel (the waiting runs first; a cancelled run writes nothing and says nothing), and the error as one plain line (design 4: `mask_empty`, the errors of 8.5, `out_of_memory`, `stitch_mismatch`, `canvas_conflict`, else the error's own sentence; a `canvas_head_moved` warning says the result is kept in History). **A fully cached Write** (design decision 5): the same prompt, seed, selection and head after an undo hits the cache to the end, Write included, so nothing would land; when the run ends, the revision the cached Write landed (its `canvas` output) is placed on the head again with `CanvasStore::accept` (the head moves to it when it was made from the head, else its added layers are placed: layers only add; an `undone` revision is accepted, only an `accepted` one is refused); when it is still in the head's history the bar says "Same prompt, seed and selection as “<layer>”. Change the seed to get another." Prompt history: per action, newest first, at most 200, `canvas-prompts.json` in the root's state folder (atomic, never inherited). **The task bar** floats in the Canvas view's picture under the selection (the rectangle, or the Quick Mask selection while Quick Mask is on: stored by content at Generate), 2u clear, above it when there is no room, 4u inside the picture; across the foot of the picture when the picture is narrower than the bar (a small area); it follows a pan or zoom. **Moving it (#381; `canvas/task_bar_place.*`, `App.canvasActions.barPlace`):** a grip at its left end (tooltip "Drag to move", the `MoveGrip` of the run toolbar) drags it anywhere over the picture, 4u from the edges; the place is kept (`canvas-task-bar.json` of the state folder: `{"place": {"x", "y"}, "pinned"}`, fractions of the room the bar has, as `engine/float_place.h`), so a resize keeps it inside the area (`positionIn`); `⋯` has **Pin position** (the grip no longer moves it) and **Reset position** (the bar floats under the selection again); a narrow, docked bar ignores the place. Test: `tests/canvas/test_task_bar_place.cpp`. It shows while there is a selection, or while its runs go or its last word stands. Simple state: the action menu (Auto, then the actions; a broken one disabled with its reason; its glyph alone in a narrow bar), the prompt (Enter generates; Ctrl+Enter stays Run of the Graph; Up and Down step through the history in an empty field; the clock lists the last 20), **Generate** (the one primary; quiet while the candidate banner's Accept shows; disabled with the reason as its tip) or **Cancel** while its runs wait or go (Escape in the field or the view too), `⋯` with **Clear prompt history…** (a Question, Keep highlighted). One status line under them only when there is something to say: "Waiting for the GPU…", "Generating… 45%", the download's progress, the error in `look.error`, the notice, "Generate works on the latest version." with **Back to latest**, or "<action> needs N models, <size>." with **Download** (`Workflows::downloadModels`, the template Download's path). While a run goes, the selection's outline shimmers (accent dashes marching on a dark halo). Tests: `tests/nodes/test_canvas_action.cpp` (parse, check, bind, the bundled action compiles), `tests/canvas/test_canvas_actions.cpp` (the runner with `FakeProvider`: one press, one layer; cancel; plain errors; refusals; the cached Write placed again; a Quick Mask selection by hash; history) and `tests/canvas/test_canvas_actions_window.cpp` (the real window offscreen: draw, type, Enter, progress, the layer, Escape, the error line, Download, a Canvas area; `FUGUE_SHOT_DIR` pictures). **Slice 2 (actions from the library, Edit workflow).** `CanvasActions` lists first the templates of the library (`Workflows::templateList()`, the person's own first) whose workflow has an `action` block, then the files of its roots (the ones compiled into Fugue); the first of an id wins, so a template replaces the compiled file of the same id, while a template with no block is not an action and hides nothing; a block that does not fit its graph is listed as broken. The list is read again on `templatesChanged` and when the action menu opens. The `generative-fill` template (fugue-workflows, category `canvas`, "Canvas actions") carries the block and is the same workflow as `share/actions/generative-fill.fugue` (a test compares their `sourceHash` and block). `CanvasActions::editWorkflow(id)` ("auto" is Generative fill) opens the action as a tab with no file: a library action through `Workflows::openTemplateById` (sample files and packs as for any template), a compiled one through `Workflows::openCopyOf(file, title)`; Save asks where, no bundled file is written. The bar's `⋯` > Edit workflow calls it, and the host shows the Graph (an area: `Workspace::active` is the Graph's; the dialog: it closes). Not here yet (later slices): the advanced popover, count and variations, generate-again pinning, the bar running the Edit workflow tab's unsaved edits, saving an edited bundled action as the person's own (`saveOwn`), the bar's hide, Details into the queue panel, `fugue canvas act`. **Slices 5 and 7 (Refine, Remove, Expand).** Three templates carry blocks (category `canvas`): `refine` (strength 0.7, an optional prompt with a fallback), `remove` (no prompt role; Fill Masked `blur`; padding held at 128 or more by Generate for this action) and `expand` (`selection: "bounds"`). For a `bounds` action Generate ignores the rectangle and reads `request.expand = {side, percent}` (or `pixels`; one of left, right, top, bottom; the amount a share of the picture's side in multiples of 16, at least 16); after the model and prompt checks it calls `CanvasStore::resize` (edge-blur pre-fill, producer `action:<id>`; `canvas_head_moved` in words when the head is not the shown one), routes the grown canvas with `RouteInput::resize` and binds the Region to the new head's full hash, the strip plus the overlap as the rectangle, the router's padding (256) and scale. The bar adds a "Grow the canvas" row for such an action and shows with no selection while it is the chosen action. **Slice 4 (the Advanced popover, Count, `choice_values`).** The bar's `⋯` keeps only **Clear prompt history…**; **Advanced** (`CanvasTaskAdvanced.qml`) is a popover with the action's inputs the bar has no field for (every bound input except the prompt, seed and layer), **Count** and **Edit workflow**. `CanvasActions::advanced(id)` puts the action's workflow, with the remembered values and the block defaults in, into `form`, a `CanvasModel` of its own (the window's downloads set on its `WidgetEdit`), and returns the rows; each row is drawn with `InputWidget` over `form.widgets` and `CanvasModel::inputPort(node, input)` (the face's port map), so the model picker with Download and the drag number are the Graph's. `CanvasModel::edited` is read back (`syncForm`): what differs from the action's own value (block default, else the file's) is kept per action in `canvas-actions.json` of the state folder (`{"format": 1, "actions": {"<id>": {"values": {"<key>": json}, "count": n}}}`, atomic, never inherited); `setChoice`, `resetAdvanced`, `variations`/`setVariations` are the rest. Generate puts the remembered values in `ActionBinding::values` (a stored word that is no longer one of the choices is dropped), checks the missing models and offers the download for the models the action would run with. An input with `choice_values` is a word: `bindAction` maps it to its number (`action_invalid` for a word that is none of the choices); `parseAction` refuses `choice_values` that are not numbers, have no `choices`, are of another length, repeat a word, or a `default` that is none of the words, and `checkAction` a target that does not take a number. **Count N** (1 to 4, `CanvasActions::maxVariations`; 1 for an action with no seed role): one Generate binds N copies of the same head and selection and queues N runs, seeds random each or `s + n`; each Canvas Write gets `suffix` "(n of N)" and, for n above 1, `visible` off; `runNumber` and `runTotal` say which run goes; Cancel ends all of them, a failed run cancels the rest (one error), the newest step frame wins; the prompt is remembered once. Tests: `test_canvas_action` (choice_values), `test_canvas_actions` (the popover rows and remembered values reach the run; Count 3: seeds, three layers, the first shown; cancel and failure) and `test_canvas_actions_window` (the popover in the real window offscreen). Not here yet: the `‹ 2 of 3 ›` strip, generate-again pinning, the licence line in the popover, Make action…. **Slice 6 (Remove background).** The template `remove-background-action` (carries the Remove Background pack) has `selection: "optional"` and the `layer` role on `region.layer` and `write.onto`: Generate takes the view's current layer (`request.currentLayer`, from `CanvasViewModel::currentLayer`; refused in words when there is none), keeps Region's own padding and scale, and the Write puts the pack's subject mask on that layer as its layer mask (Canvas Write 1.6.0 `onto`). An action whose nodes need a pack that is not installed is listed with `packMissing`, `packText` and `packInstallable` (not broken: the block's inputs that point into the missing node wait), Generate refuses it with `pack_missing`, and `CanvasActions::installPack` asks the window to install the pack its template carries (Settings > Packs asks the person); batches refuse it. Tests: `test_canvas_nodes` (Write onto), `test_canvas_action` (the template with its pack loaded), `test_canvas_actions`, `test_canvas_actions_window`.

**Not here yet (later slices of #206; for layers, #226):** merge down, lock (and alpha lock), groups, painting a layer mask (the mask brush of the tool strip), a layer mask added from a selection, the "use in region" toggle per layer, Canvas Write onto an existing layer or with a blend mode of its own, layer transforms; a **Select mask…** that opens the paint window on the canvas as its backdrop, so a mask can be painted over the real picture (#213's second half; the paint window draws over the node's input picture or a blank canvas, a canvas backdrop needs a source other than a node's), a canvas mask layer as Region's selection, a position on Write, a revert and the changed rectangle drawn over the picture in the window, retention and garbage collection (nothing is ever deleted now), a canvas editing window, and a canvas in a share package. Decisions are in Appendix A (rows marked #208 and #209).

---

## 15. Milestones

| Milestone | Contents | Issues |
| --- | --- | --- |
| Spec | This document. | #1 |
| **M0** engine, CLI and spikes | Spikes first: component-level go or no-go, libvips benchmark, GPU checks, canvas frame rate, plugin-interface research. Then the document model, compiler, scheduler and result cache, run folder and media handles, the provider interface with the `omarchy` provider, and the CLI skeleton. Driven by tests with fake nodes; no canvas. | #2 to #12, #34 |
| **M1** first nodes | The twelve nodes of [14.1](#141-m1-the-first-twelve). | #13 to #15 |
| **M2** canvas | The canvas of [section 12](#12-the-graph) on the M0 engine: wires, search, copy and paste, undo, run, per-node state, save, open, recovery, import, tabs, keymap, theme. | #16 to #22 |
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
| ~~Soft masks in the sampler (differential diffusion)~~ Decided (#179): carried in the fork as the opt-in `fugue.soft_mask`, byte-identical when off ([8.10](#810-gpu-memory-and-the-helper)); the upstream PR to stable-diffusion.cpp is the owner's | soft inpainting, [14.4](#144-masking-and-inpainting-167) | #179 |
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
| Conditionals are lazy inputs: a node asks, once its other inputs are in, which lazy inputs it needs, and only those are fed (ComfyUI's `check_lazy_status`); a node nothing demands is skipped as `not_chosen`, sinks are always roots, validate takes both sides as needed (#557) | 3.2, 3.4, 9.2, 9.9 | medium-high |
| An output of type `any` (built-in nodes only) takes the type of the input it `follows`; a Switch output is not a wildcard that any input accepts unchecked (#557) | 2.3, 14.1 | medium-high |
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
| Expression, Not, And, Or (#557, part 1; the lazy Switch is part 2): ComfyUI's Math Expression runs simpleeval over `a` to `z`; here the formula is a small native parser over six fixed slots (as Text Template), with Python's precedence (`**` right to left and tighter than a unary minus, `not` below comparisons, chained comparisons). Deviations, each chosen on purpose: `and`, `or` and `not` give a boolean, not an operand; a boolean is 1 or 0 in arithmetic; every result is capped at 1e15 and an exponent at 1024 so a value never reaches a widget or the cache as inf or NaN; a division by zero or a log of 0 or less is an error, not inf or NaN; `round` rounds halves to even as Python does. Not, And and Or are separate small nodes (And and Or take a `list` input, so any number of links) rather than one node with a mode | 14.1 | medium-high |
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
| The embedded `run` section also holds `prompt` (the picked text) for a Dynamic Prompt upstream of the saving node, so an image says which prompt it used even when the wildcard files change later; it is removed when the Save node embeds without prompts (#556) | 11.7, 14.1 | medium-high |
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
| Where a model lives, for templates and model pickers (#164). Research: today a card's "Models missing" comes from `ModelLibrary::missing` over the template's `requires.models` handles (name and hash, headers never read, 11.9, #85, #89); the picker lists `listOfferedModels` (providers, by instance) and `listDiskModels`. The omarchy provider names a served model by its recipe, without a hash (`OfferedModel::handle`, 8.7), and a paired host is its own provider instance with an `address` (`defaultProviderInstances`); an offline paired host is kept, its models `unavailable`. omarchy-local-ai says nothing more about files. Other tools: LM Studio marks local and "downloadable" models, Ollama tags `:cloud` models, Jan separates local and remote engines, Stability Matrix and ComfyUI Manager badge installed against not installed; none shows a paired host's models next to local ones, so this is Fugue's own. Decided: the badge is the glyph plus a tooltip, never color alone (informational: no press, so no 28 px floor, 16 px glyph); `monitor` for this machine and `server` for a host (Lucide, added to the subset); a model on a host counts for a template only by the same hash (else resource and name, as provider selection does), so a recipe-named host model (no hash) does not mark a template whose file models carry hashes; the host's models are read when the dialog opens and the cards update when they arrive; a template with no model files is "ready" with no badge; ready-first is a stable partition inside each category, not a new sort key | 11.10, 8.7, 2.5 | medium (the matching is the doubtful part: a host that reports component hashes would light the badges; recipes alone do not) |
| Text-field focus (12.4): widget fields are text inputs, so while one has focus its keys are the field's; a press on the canvas or on another widget takes the focus from the field first, ending its edit. Menus are modal, so the window's keys sleep while one is open | 12.4 | high |
| Widgets are made with a node's rows (in view at a readable zoom); at rest a widget is a well, a text and a mouse area, and editors, menus and pickers are made on first use | 12.1 | high |
| Canvas runs (#19) run in the window's process (9.8: no daemon) on a thread of their own that owns everything a run touches: its own registry (node types, and providers.json read again at each run's turn, as a worker does), the kernels, the result cache and the Run objects. The UI thread hands over a copy of the document and gets the events back, so compiling, hashing, moving outputs and waiting for providers or the queue never block it. A run goes through `fugue run`'s steps and code: compile (run to here is the compiler's target), run folder, an entry in the shared queue, its turn, the scheduler. A run that does not compile gets no folder, as with `fugue run` | 9.8, 12.1 | high |
| The window's queue entries record `"owner": "window"`, its pid and that process's start time (`/proc/<pid>/stat`, field 22); such an entry is live while that very process lives, whoever looks, and a process never discards its own entries, so the window can look for dead ones while it holds the lock. `fugue runs cancel` of an active window run leaves the file `cancel` in its run folder, which the window looks for every 50 ms; a waiting one is cancelled by taking its entry, as for a worker's | 9.8, 9.6 | medium-high |
| Canvas runs carry no file roots (13.6): the roots guard against agents and scripts writing where nobody looked, while on the canvas a person sets every path on the node face, often through the portal, which can be anywhere (a USB disk, `/mnt`); limiting them would refuse the user's own choice. A workflow opened from elsewhere shows its paths on its nodes before anything runs. Revisit if the canvas ever runs workflows an agent wrote without the user looking (11.8) | 13.6 | medium |
| A node added on the canvas starts its seeds on randomize, as ComfyUI's control-after-generate does by default: Run makes a new picture each time, the loop people come from ComfyUI with. The manifests keep `0` as the default, so `fugue run` and agents stay reproducible without asking, and no manifest default needs `{"auto": "random"}`. Leaving randomize keeps the seed of the result shown, so a liked image can be iterated on downstream from the cache | 6.2, 10.1, 14.1 | medium |
| Per-node state on the canvas (10.3): between runs it is `fugue validate`'s (checkGraph on the run thread, 300 ms after the last edit and when a workflow is shown, only while no run is active); during a run it follows the events. A node's error stays until the workflow is edited or run again; its last values (time, output values, thumbnail) stay through edits, at 50% while it runs again. A node inside a group shows on the group's node, its error with the inner path id (`g7/first: ...`), and every error of the run is listed by path id from the run pill | 10.3, 12.7, 9.5 | medium-high |
| Node pictures (#263): three levels, as above. The mode is a setting because it is a taste (one person wants every picture, another a clean chain); the override is in the layout because it is about one node of one workflow, so it travels with the file; the role is in the manifest, not a list in the code, so a pack says it for its own types. Crop and Paint Mask are `result`, not `never`: the picture is their editing surface. Not done: a control inside a node's Advanced section (its rows are the type's inputs, the override is not one; the menus carry it) | 12.7, 3.1 | medium |
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
| The mask widget and its files (#174, #167). A `mask` input is `text` holding the path of an 8-bit gray PNG in the inputs folder; `over` names the image input it is painted over (none: the node's own `file`, as Load Image). The picture's file and hash never change: painting only sets the literal, as one undo step (`CanvasModel::setMaskPath`). One predicate, `InputSpec::holdsInputFile` (widget `file` or `mask`), says which inputs hold a file that travels with the workflow, and `forEachFileLiteral`, `relocateInputs`, `makeInputsAbsolute`, `takeInputsFrom`, the share package, the scheduler (relative path, read roots, content hash) and the share copy all use it. Load Image 1.1.0 gives `mask`: the painted mask, else 1 minus alpha when the file has alpha, else all 0 at the picture's size; a painted mask of another size is scaled bilinear with the warning `mask_resized`; a mask file that is gone is `file_not_found` on `mask`. Paint Mask has the `image` and the mask and gives it, all 0 unpainted. Decided: the Paint mask button is the `mask` input's row, not a 28 px `ViewButton` under the picture (a row keeps the face's layout and height rules and is already a full-width 24 px target; the spec text said button under the picture); a mask port is a hollow square with a 3 px frame, wired or not (the ring on the square), because the type color and shape already say image; the face overlays the mask at no more than 1024 px a side so a big picture does not cost a big texture; a node with a `mask` input over an image input shows that input's picture as Crop does, so any pack node can use the widget | 14.4, 6.1, 6.2, 11.2, 11.5, 11.10, 12.6 | medium-high |
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
| Compare Images (#62, `fugue/compare-images`). Researched: rgthree-comfy's Image Comparer (MIT; a Slide mode where the divider follows the pointer, a Click mode, A and B inputs), ComfyUI's core Compare Images (`comfy_extras/nodes_image_compare.py`, GPL, one draggable slider, no modes or labels documented), community packs with slider, side-by-side, toggle and difference modes (Deno Image Compare, ComfyUI-EsesImageCompare), Lightroom's `\` before/after toggle, img-comparison-slider (MIT: hover option, arrow keys move the divider) and a Qt forum QML slider (no license: the clipped top image drawn at a negative offset, which the viewer does too, so B never bleeds through). Matched: the slider is the primary and default mode, and the divider follows the pointer as well as a drag (rgthree's Slide); a flip mode (rgthree's Click, Lightroom's key); the same viewer on the node and in a larger view; A and B labels; arrow keys move the divider (img-comparison-slider). No code was copied: the concepts are used and the viewer is Fugue's own QML, built on the Preview slot (the thumbnail's place on the face, `shown` for the inputs) and Blend's difference formula and fit. Improved: side by side, a horizontal divider and a difference view (|A - B| times a gain, black when identical, made by the node and so saveable); one linked zoom and pan for both pictures, nearest-neighbour past the file's own pixels, 1:1 on a key (#420: 100% follows the larger picture, `engine/compare_scale.h`: when B is larger than A, as a SeedVR2 restore is, 100% is one pixel of B per screen pixel and A is magnified, the notice says "100% of B · 3072 × 3072", and a second press of `1` or of the 100% button toggles "100% of A" and "100% of B" while the sizes differ; B the same size or smaller keeps A's 100%; the view state's `actualB` says which, and Fit clears it; the detail rule above sees 100% of B as one file pixel per screen pixel, so it loads the files; the node's face shows small Fit and 100% chips at its bottom corner while the pointer is over it, in the labels' chip style, on a face at least 24u wide, doing what `0` and `1` do; **hold Z** (the selected node on the Graph, or the large view) peeks: 100% of the larger picture centered on the point under the pointer, and releasing gives back the view it had; a tap under 200 ms is `1`; autorepeat is ignored and a lost focus or window gives the view back; `CanvasModel::comparePeekBegin/End`. Nothing else owns Z in the canvas and compare contexts (the Canvas view's Z is its zoom tool, a context of its own); pressing the viewer on a face hands the keyboard focus to the Graph); a notice with both sizes when they differ (B is cropped to A's shape and resized, as the difference is made); the large view reads the full files, the face the 512 px copies while they are sharp enough (#371: once one copy pixel of the wider picture covers more than 1.1 screen pixels, at any zoom including the node's own fit (a large face stretches a copy too), the face loads the files for A, B and the difference and draws them over the copy as each is ready, so nothing flashes; it lets them go below 0.9, so a graph of small compare nodes holds no full-size pictures it does not need; the wider of A and B decides, since a SeedVR2 restore is several times its original; the rule is `engine/compare_detail.h`, tested in `tests/engine/test_compare_detail.cpp`); the view state is the node's and survives the face's items being dropped, and is not an edit; Copy A, Copy B and Copy view; the viewer is made only at full detail. Outputs: the difference and the composite, so they can be saved; the composite is the node's own Mode and Position widgets (a cached result is never made again to flip), while flipping on the face is session state. Keys (when a Compare Images is the selected node): `\` flip, `M` next view, `0` fit, `1` 100% | 14.1, 12.3, 12.7 | medium |
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
| Settings panel (#51): pages (Cache, Folders, General, Keymap, Models, Packs, Providers, Run, Theme: listed alphabetically by their translated title, locale-aware, and named in code by a stable id, never by position; it opens on General) instead of ComfyUI's category tree, since Fugue has about a dozen settings. Each change is written at once (no Apply), checked in C++; QML only asks | 1.4 | high |
| Packs, trust, templates and prompts of a project (#204 step 2 slice 5, 2026-11-22): the Default's packs are a read-only base layer searched after the project's own, trust is honoured only for the exact inherited id and version, an own pack of the same id and major hides the inherited one, refusals name where to change it, and the pack's write risk is a stated warning, not enforced. Templates: only the Default's own folder, not Fugue's bundled ones (every project has those); prompts: an own file replaces. The cross-root wait is a time stamp, not a lock, so a dead waiter blocks nobody; the helper stops when it idles and a stamp is under 3 s old | 1.4.2 | medium |
| Reusing the Default's remembered hashes (#204 step 2 slice 7, 2026-11-22; decided without the owner, each to confirm). (1) It is part of `models`, not a new inherits word: a project that shares the model folders shares the fact that those files were hashed, and a strict project (no `models`) reads nothing; (2) only entries under the inherited model roots, by canonical path with a `/` boundary, so inputs and pictures the Default saw stay private (this keeps the research's rule that `hashes.json` is not inherited, narrowed to model files); (3) the validity rule is the project's own (size and mtime in ns), so a changed file is never trusted; (4) nothing from the Default is copied into the project's store and the Default's file is never opened for writing (a test compares the Default's folders before and after); (5) the layering is in `FileHashes`' constructor over `defaultPath()`, so every caller (run, CLI, window, downloads) gets it with no change, and any other store (authoring's scratch) stands alone; (6) the helper's own `model-hashes.json` is not inherited (only the helper reads it; unchanged). Not verified: a real run in a project against a tens-of-GB Default store | 1.4.2 | medium-high |
| Use what you have (#605, 2026-11-26; decided without the owner, each to confirm). (1) One piece (`findOnDisk`) decides for every surface, and applying is by surface: the graph edits the document with one undo step, the App view keeps the choice in the tab's own state, never in the workflow. (2) A file with the wanted file's own name and quantization from another publisher is a match, marked "from another source" (same family, stem and quantization; the bytes differ): offered because a ComfyUI folder's Qwen3 Q8_0 is the same model as the one a template names, and refusing it would push people into a download; said plainly because it is not verified identical (medium-high). (3) A file with no readable header or that the engine cannot load is never offered, even alone (high). (4) A single file is offered without a Recommended mark; several: the Recommended one, else the best quality (high). (5) In the App view a model that is an app input takes the file as the field's value (so Reuse inputs and the takes record it), any other is a tab override that lives for the session (medium). (6) The node's "Use it" shows on the warning card once it is kept up with a click, not on hover (medium). (7) `validate` takes `--use-on-disk` too: it answers "would this run with what I have" (medium-high). (8) The Get ready sheet and the banner in the graph apply to the document (undo), in the App view to the app's state; the sheet lists the offer only while reviewing | 2.5, 8.9, 11.10 | medium-high |
| Use my file automatically (#618, owner decision 2026-11-26, the rest decided without the owner, each to confirm). (1) The setting is `use_on_disk`, `use` (default) or `ask`, a preference like `new_seed` (medium-high). (2) One check (`Workflows::autoUse`) is the choke point: a tab opened, shown or refreshed applies its covered models once per tab and model, so Get ready, the Needs panel and the node card have nothing covered to show; a model whose swap was undone or whose file could not be read is not asked again (high). (3) A same-quantization file from another source stands in before a Recommended other quantization (#624): the file the workflow's own name asks for is the closest to it (medium-high). (4) The CLI follows the setting for `run`, `app run` and `validate`, `--no-use-on-disk` opts out, both flags are a usage error (medium-high). (5) Run reads no file itself: it relies on the swap done at open (medium) | 2.5, 13.1 | medium-high |
| Recommended model file (#370, 2026-11-24; decided without the owner, each to confirm). (1) Only a mark: nothing is picked or swapped by itself. (2) The same weights are one family, the same header figures and the same name stem, so a distilled Klein, its base and the kv file are three groups; a file with no header or family is never grouped. (3) Quality tiers are a table in code read from the name, no hash list; a .safetensors file with no quantization word is taken as bf16. (4) Speed is measured from real runs (no benchmark of its own): seconds per step per megapixel at guidance 1, the median of the last five per device and file; the default without a measurement is INT8 on CUDA with compute capability 8.0 or more and GGUF elsewhere; a measurement counts only when every file in the tier has one. (5) The memory check uses the device's total memory (the unified-memory figure on a GB10) against the weights of the node's other picked models and the file, plus a fifth. (6) A remote helper is never recorded and never recommended for here. (7) Text-encoder INT8 files get the same default as diffusion models: no encoder run is measured. | 2.5, 8.9 | medium |
| Layered settings (#204 step 2, 2026-11-22): `models` and `preferences` only, each key in one class; the output, inputs and download folders, pack paths and cache limits never cross, because they name places a project must own. A project sets a preference back to the built-in value by writing it when the Default's differs; a list cannot remove an inherited entry (it only adds); the keymap is one value, not a merge of diffs. The cross-root GPU lock is a second flock after the engine lock, a file in the real runtime folder, so the helper's VRAM is not shared by design but serialized by runs | 1.4.2 | medium-high |
| settings.json keeps keys it does not know; a known key with a bad value is listed as a problem and its default used, the value left as written; a file that is not a JSON object is never overwritten. A value equal to the default leaves the file, so a later default reaches users who never chose | 1.4 | high |
| `new_seed`: random (default, as ComfyUI's control_after_generate) or fixed (a random number that stays) for seed widgets of nodes added on the canvas; increment stays a per-node choice. `embed_workflow` sets what new Save Image nodes start with (11.7) | 6.2, 11.7 | medium |
| `language`: follow Omarchy (default) or a shipped catalog, live. `output_folder` and `inputs_folder` are read afresh by every run and the CLI | 1.4, 12.11 | high |
| Providers page: the effective instances in preference order; with no providers.json the defaults show as "default, not saved", and the first change writes them all (mode 0600, atomically). Keys and secret module fields reach the panel only as set or not set | 8.2, 8.8 | medium |
| ComfyUI settings not taken: custom navigation mix, zoom and pointer tuning, link render modes, node opacity and badges, palettes and background images (the Omarchy theme), auto save (recovery, 11.3), server flags (each provider owns its engine's), accounts. Later: minimap, snap to grid, side panel layout, reduce motion, keys edited in place, pack enable/disable. Full table in the #51 pull request | 12 | medium |
| Command-node previews (#65). ComfyUI (ComfyUI_frontend `app.ts`, `nodeOutputStore.ts`, read at main on 2026-11-20) sends a sampler's latent preview as a binary websocket message (`b_preview_with_metadata`); the frontend revokes the node's last preview and shows the new one in its image slot; nothing clears it when the node finishes or a new run starts, so a KSampler keeps its last preview, and previews are drawn before output images. We match: one slot, the newest preview, full strength while it is this run's, an image result taking its place, and a result with no image leaving the last preview (as a sampler's preview stays on ComfyUI's node). We differ: a node sends a file path, not bytes (the protocol is lines of JSON, and the file stays in the run folder for Open Image); previews queue at most one deep, so a fast node cannot hold up the run thread; a bad preview is a warning, never a failure; the last run's picture is dimmed while the node runs again, until the new preview comes, and a provider's sampling preview (8) follows the same rule, so it is no longer drawn dimmed while it streams | 7.2, 9.4 | medium-high |
| Image Size (#72): ComfyUI's core has no such node (Empty Latent Image takes a width and a height); people install one (Comfyroll's CR Aspect Ratio, resolution-calculator nodes). Built in here, as it is how a size is picked for every image model, and the shipped text-to-image example sets its size with it. Aspects are written `w:h`, landscape and portrait as separate choices so one pick sets both; the width is the nearest multiple to the exact one and the height the nearest to the area left (2:3 at 1 MP is 832 × 1280) | 14.1 | medium-high |
| Context menus offer what applies (#72): a command for a group, a port or an image is left out of a node's menu when it does not apply, and commands not built yet are left out of context menus; the menu bar keeps every command, dimmed when off (40%), so it can be found. As ComfyUI, whose node menu shows a subgraph's entries only on a subgraph and image entries only on a node with an image. Rules: Run to here needs exactly one node, Make unique a definition other instances share, Paste Fugue's nodes, a JSON object or an image, Run and the exports a node | 12.3 | high |
| Renaming a group's port (#72) sets its label, the name on the face, and keeps its `name`, which links and `--set` use, as ComfyUI's Rename Slot sets a slot's label; so outputs gained an optional `label` (an older Fugue keeps it as an unknown key). Reached from the port itself: a double-click on a group card's port or on an inner port's exposed mark, or Rename on its context menu. Node titles (#55) and Frame titles are edited in place, on the header or the Frame (#74, as the owner asked): the title selected, Enter keeps, Esc drops, a click elsewhere keeps; a port keeps the small question, its label not being drawn as one line to edit | 5.4, 12.4 | medium-high |
| Side tab strip (#159). Researched from public documentation (VS Code's user interface page, JetBrains' tool windows page, ComfyUI's interface overview, read 2026-11-21) and, for what those pages do not say, from general knowledge of the products, to recheck before quoting (Obsidian's ribbon, Blender's editor-type and area edge controls, Krita and GIMP docker tabs, Figma and Photoshop panel tabs); no source was read, ComfyUI's, Blender's, Krita's and GIMP's being GPL. Found in the docs: ComfyUI's left sidebar is a vertical icon rail (Job History, Assets, Node Library, Model Library, Workflows, Apps) and its page says nothing of tooltips, keys or one-at-a-time; VS Code's Activity Bar sits on the far left, switches views, carries badges, offers a right-click menu to move it and to show or hide views, views reorder by drag, and the Primary Side Bar moves right while the bar's own place is set separately; JetBrains' stripes sit on the window edges, a click shows or hides the tool window, icons drag to reorder, each tool window has a shortcut (Alt+1 and so on), and Shift+Escape hides the active one. From general knowledge: the icon of the open view is marked (VS Code and JetBrains), a second click collapses it, hover labels carry the key, VS Code's Activity Bar is 48 px wide with 24 px icons, and a narrow window loses the side bar before the bar. Matched: a left vertical rail of icons, one per panel (ComfyUI, VS Code), a click shows and a second click hides (JetBrains, VS Code), the open one marked, a tooltip with the name and the key, the same key as the strip does the same thing. Improved: the strip is built from the panel registry (a new panel adds a tab with no strip change), it is keyboard reachable (Tab, Enter, Space) with a focus ring and screen-reader names with a checked state, and it stays in step with N, W and the View menu through the one toggle (ComfyUI's page does not say); the canvas keeps 160 px when a narrow window cannot hold the panel; the strip hides in focus mode with the rest. Chosen: the left edge, because the panels are navigation (library, workflows) and ComfyUI, VS Code and Obsidian put navigation left; inspectors belong right, and the strip stays left even when a panel is docked right (VS Code does the same); 44 px wide, a 24 px glyph and a 44 px hit area (VS Code's 48/24, above the 36 px floor of the issue); no badges, reordering, right-click menu or right strip (follow-ups); the Lucide glyphs `boxes` (nodes), `workflow` (workflows) and `layout-template` (Templates), ISC, added to the bundled font by the recipe in `share/fonts/lucide-fugue.txt`; a registry row has a kind, `panel` (a docked panel, toggled, marked open) or `action` (#192: a tab that launches a command, here Templates = `file.templates`, never marked open or checked, in step with the menu command and its key through the one command; a row also has an `edge`, `top` or `bottom`, so Templates sits directly under Workflows and Settings = `app.settings` (Ctrl+, , the Lucide `settings` gear, added by the same recipe) alone at the lower end past a hairline); the run queue (a floating panel of the run toolbar) and the media results (a view in the workflows panel) are not docked panels, so they have no tab yet | 12.14, 12.15, 16 | medium |
| Auto-hide menu bar (#160). **Researched from the products' public documentation and general knowledge of them, not re-read this session (no web fetch was made; recheck the numbers before quoting them)**; no source was read (Blender, GNOME Shell and KDE Plasma are GPL). Firefox and Chrome in full screen: the toolbar (and tabs) are hidden and slide down when the pointer touches the top edge of the screen, and slide up when it leaves, with a short delay; F6 or Ctrl+L also bring the toolbar in; the page does not move while it is shown (it overlays). Windows taskbar auto-hide: the bar hides to a 1-2 px sliver at the screen edge and slides in on hover, and stays while a menu or flyout of it is open; macOS auto-hidden menu bar: it appears when the pointer reaches the top edge after a short dwell, over the windows, and a full-screen app can show it always or on hover. GNOME Shell and KDE Plasma panels: the KDE panel's "auto-hide" and "windows go below" modes reveal on the screen edge and offer a visible sliver or handle; the delay is configurable. VS Code: the menu bar can be hidden (`window.menuBarVisibility`: classic, visible, toggle, hidden); in "toggle" a tap of Alt shows it, a mnemonic opens its menu, and Alt+F in "hidden" opens a menu without a visible bar. Blender: a hidden area header leaves a small arrow tab on the area that restores it by a click. Figma and Photoshop: Tab hides every panel; Photoshop shows them again for a moment when the pointer goes to the edge. Common to all: a hot edge at the window or screen edge (Fitts: the edge is infinitely deep, so the target is as long as the edge and as deep as the pointer can push), a dwell before revealing so a pass does not trigger, a longer delay before hiding so the bar does not flicker, the bar over the content and not pushing it, the bar kept while a menu of it is open, a visible cue for touch and for discovery (the arrow tab, the sliver), and a key (Alt, F10, Tab) for keyboards. Screen readers: the hidden bar is normally out of the accessibility tree and the key reveals it; native bars keep F10. Hover delays used in practice are roughly 100-300 ms in and 300-1000 ms out; touch has no hover, so a tap reveals and a tap elsewhere hides. Matched: an edge that reveals on hover, the bar overlaying the content, staying while a menu is open, Escape and a command hiding it, F10 and mnemonics working while hidden, a visible handle (Blender's arrow, KDE's sliver). Improved: the edge is a real 4 px layout strip, not a 1 px pixel, so it is a long target that is never under the content, the handle has a name for screen readers, a 28 px floor, and moves clear of the side strip and the breadcrumb; the remembered mode is in a word so a later "pinned elsewhere" mode needs no new format; one container, tested, for any top bar. Chosen: 4 px edge, 36 x 28 px handle at the left (menu glyph), in after 120 ms, out after 400 ms, 170 ms OutCubic slide of `y` (not the layout), Escape slides out only when nothing else wants the key, a tap holds 4 s; the tab bar and the run toolbar are not converted: Firefox and Chrome reveal tabs with the toolbar, which needs two bars stacked under one reveal, and the run toolbar floats and moves, so an edge reveal does not fit it (follow-ups); the `AutoHide` component is generic so the tab bar costs one wrapper | 12.15 | medium |
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
| Checked on the owner's folders (2026-11-20), after their runs crashed the helper: a file in a quantization stable-diffusion.cpp has no kernels for (GPTQ; SVDQuant; AWQ; and the ComfyUI `comfy_quant` formats not listed next) fits nothing and is `model_unsupported` before the helper. ComfyUI quantization is allowed by format, read from each `comfy_quant` tensor's JSON payload, and a file only when every format in it is allowed; the message names the refused format, and a payload that cannot be read is refused cleanly (#340, 2026-11-23). Allowed after a real run on the RTX 3090 gave a correct picture: `int8_tensorwise` (a Qwen-Image 2.1 INT8 ConvRot diffusion model with a Qwen3-VL INT8 text encoder). Still refused: `nvfp4` and so fp8mixed files (141 `float8_e4m3fn` plus 85 `nvfp4`: the helper aborts on a scale-size assertion, ggml_block.hpp, the 4-bit packed weights read as 8-bit), `convrot_w4a4` and `asym_w4a8_int8` (the pin reads them; not run), and plain `float8_e4m3fn` (not run alone). The Klein INT8 and SeedVR2 INT8 files were not at hand, so they are untested; an LLM encoder's family (Qwen3, Qwen3.5, Qwen2.5) must be the one its model reads, so the 4096-wide Qwen3.5-9B no longer fits Klein 9B. LoRAs are judged by every naming they come in (PEFT, kohya, LoKr, LoHa): a native-named LoRA with none of its layers in the model, a UNet's on a transformer, another dominant width, or a block index the model lacks (FLUX.1's on the as-wide Klein 4B) does not fit. The fitting files are one "Fits <model>" group. On the owner's 532 LoRAs: Klein 9B 27 fit, 495 do not, 10 untold; Klein 4B 7, 394, 131 (FLUX.1 LoRAs in other namings, as wide); Z-Image 8, 517, 7 | 2.5, 8.10, 13.3 | medium-high |
| A model path that is one kind's folder is told by its files when the settings do not say (#86, the owner chose both): a path with any type's folder inside keeps ComfyUI's layout, else up to 8 files are read and the kind more than half share is the path's; a mixed folder holds nothing until set. A LoRA fits a model when the layers it changes that the model has are as wide (names matched without prefixes and separators, so kohya's and PEFT's meet); no layer found tells nothing (diffusers names, an SDXL UNet's), so it is not dimmed. What a folder holds is its own key, `model_folder_kinds` by path, not `{"path", "holds"}` entries in `model_paths` as #86 first said: model_paths stays a plain list an older Fugue reads, and a kind for a path no longer listed is ignored. A LoRA's fit is not checked by the canvas (its model comes from a node that has not run then), only by the stage | 8.9, 2.5, 6.4 | medium-high |
| Which text encoder and VAE fit a diffusion model is read from the files' headers (tensor shapes, and the tensor names that tell a T5, CLIP or LLM encoder and an image or video VAE apart), never from a table of file names or hashes: it holds for any finetune, quantization or renamed file with no list to keep (#85, the owner asked whether a table would help). An encoder fits when the model's text width is 1 to 3 times its hidden size (FLUX.2 Klein stacks three Qwen3 layers). Files that do not fit stay pickable, dimmed under "Other": a family the reader does not know must not be hidden. Load Models' text encoder takes an LLM encoder or the T5 of Wan and FLUX.1 (`t5xxl_path`); a CLIP encoder fits only the CLIP-L input (`clip_l_path`, as wide as the model's pooled CLIP vector, `vector_in`, 768 for FLUX.1), and SD3's T5 fits nowhere yet (#241). Headers are remembered in memory per file (size and time), not on disk as hashes are: a read takes 0 to 21 ms (the owner's folder). Known limit: two LLMs of one width (Qwen3-8B, Qwen3.5-9B) both read as fitting. A NodeKernel may say before a run what its inputs would fail on (`check`, the canvas check's warnings) | 2.5, 14.3 | medium-high |
| The component nodes take one `fugue/model`, made by Load Models from the three files, not ComfyUI's separate MODEL, CLIP and VAE wires (#79): stable-diffusion.cpp fails to load a context missing any component ("model metadata validation failed"), so every stage needs all three, and one handle keeps one helper context warm. VAE Decode takes only the latent, which carries the model | 2.6, 14.3, 8.10 | high |
| Text Encode takes the prompt and the negative prompt and gives one conditioning (#79, the owner's choice over ComfyUI's positive and negative wires, which would need an engine change): stable-diffusion.cpp encodes both sets in one call. It always makes the negative set (guidance other than 1 at encode), the Sampler's guidance being unknown to it; sampling at 1 skips it, so the pixels match a single run, at the cost of a second, short encode of the (often empty) negative prompt | 14.3 | high |
| Load Models' text encoder is the model's LLM encoder (`llm_path`: Qwen3 for Z-Image and FLUX.2 Klein, Qwen2.5-VL for Qwen-Image) or its T5 (`t5xxl_path`: Wan's UMT5, FLUX.1's T5-XXL); FLUX.1 also takes an optional CLIP-L input (`clip_l_path`) and is a `model_mismatch` blamed on it when it is empty. SD3 (T5, CLIP-L and CLIP-G) and SD 1.x/SDXL (CLIP from a checkpoint) are not covered yet. A plain `fugue/model` is a checkpoint (`model_path`, the `checkpoints` folder) | 14.3 | medium-high |
| MOSS Image (#542) reads a Qwen3.5 encoder (its `text_embedder.proj` is 4096 wide, as Qwen3 8B is, so the LLM family tells them apart: a Qwen3 encoder is a `model_mismatch` on Text encoder) and the engine refuses to build its text encoder without the official Qwen3.5 `tokenizer.json`, which no model file holds (the context option `tokenizer`). The tokenizer is a companion file of the model, never a person's choice (#544, owner 2026-11-25: "was not obvious I needed that"): Load Models has no Tokenizer input since 1.2.0 (1.1.0 had one; `tokenizer` is a `retired_inputs` name, so a workflow saved with a value opens and runs, the value ignored without a notice; the `fugue/tokenizer` type, the `tokenizers` folder and the catalog entry stay). A diffusion model whose header says it reads the tokenizer (`needsQwen35Tokenizer`) is given the catalog's `qwen3.5-tokenizer` (`qwen3.5_tokenizer.json`, 12.8 MB, Apache-2.0, from Qwen/Qwen3.5-9B at a pinned commit, sha256 checked), found by hash; other models get none. It is listed missing like a model file: Load Models' check warns `model_missing` on the Diffusion model input, and `ModelLibrary::missing` adds the catalog's entry (with its source and size) to the models of any workflow whose diffusion model or checkpoint on this machine reads it, so the open banner, the Get ready sheet and `prepare` offer the download (a catalog entry, 6.7: nothing is fetched silently). A run that still lacks it fails `model_missing` (Text Encode's `model`, as for any model file, with the hint to use Get ready). A paired host is sent the handle {name, sha256} and finds the file in its own `tokenizers` folder; when the host's engine says it has none ("pass --tokenizer FILE") the error is `model_missing` in Fugue's words naming the host's tokenizers folder. Limit: the model's header is read on this machine, so a model file that is only on the host does not show its tokenizer in Get ready or send it. The helper appends the engine's own last error line to a failed component load ("cannot load component from … (the engine said: …)"), so a reason the engine logs is not lost | 14.3, 8.9, 8.10 | medium-high |
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
| A model is offered for every mode its server's `supported_modes` lists, not only its row's kind: no hard-coded list of "image-capable video models", so a recipe that gains img_gen needs no Fugue change. H3 as Omarchy runs it lists `vid_gen` only, so it stays a video model | 8.7 | high |
| Every LoRA a video server lists is sent with every job (Fantasi's rule; the server applies none, and MiniMax H3's few-step LoRA is how it is meant to run). A user-chosen LoRA for video is not offered: Omarchy's runtime maps none | 8.7 | high |
| Load Video and Load Audio are separate from Load Image, one kernel for the three: an `any` file loader would need a type chosen by content, and compatibility is by type (2.3). ComfyUI also has separate LoadVideo and LoadAudio (core, GPL-3.0: read as documentation only, no code taken; its audio loader also takes video files and decodes whole into memory, its video type is an opaque object; Fugue keeps files and probes). Dropping or pasting a video or sound onto the canvas is not built | 14.2 | medium-high |
| Save Video and Save Audio are two nodes modelled on Save Image, sharing its code for name, folder, embedding and numbering (`nodes/save_files`), not new naming logic. Formats are few: mp4 and webm for video, flac, mp3 and opus for sound, default mp4 and flac. A file already in the format is copied, anything else goes through ffmpeg (already a dependency, run as a program as the probe does). No codec, CRF or `auto` choices, which ComfyUI's SaveVideo has (format auto, mp4, mkv, webm, mov; codec auto, h264, av1, prores; H.264 and AV1 CRF); the defaults are the ones a person would pick. No separate Preview Audio or MP3 and Opus Save nodes (ComfyUI has SaveAudio, SaveAudioMP3, SaveAudioOpus, PreviewAudio): Preview takes any type, and the format is an input | 14.2 | medium-high |
| Research for #116: ComfyUI's SaveVideo (inputs `video`, `filename_prefix` default `video/ComfyUI`, `format`, `codec`; embeds the prompt and the workflow in the file; outputs the video and a UI preview), SaveAudio (`audio`, `filename_prefix` `audio/ComfyUI`) and its audio preview were read as documentation only (docs.comfy.org); ComfyUI is GPL-3.0 and none of its code was read or taken. Kept: the workflow travels in the saved file, so dropping it onto the canvas can restore it; the node passes its file on. Changed: Fugue's `name` is a plain name (no `%date%` formatting), the folder is its own input, a file never overwrites (ComfyUI counts `_00001_`), the output is the file as written, and the player plays on the node face where ComfyUI's preview shows in its sidebar or a widget. Dropping a saved video or sound onto the canvas to restore its workflow is not built (the tag is written; reading it back is a later ticket) | 14.2, 11.7 | high |
| The player uses Qt Multimedia (QML `MediaPlayer`, `AudioOutput`, `VideoOutput`; LGPL-3.0, as the rest of Qt), the FFmpeg backend. This is a new runtime dependency: `qt6-multimedia` and `qt6-multimedia-ffmpeg` in the PKGBUILD's `depends` (CI installs from that list, so it follows). The alternative was no player or a second app opened per file; Qt Multimedia is the one piece of Qt that plays, with the same API on every theme and no process per node. It is a QML plugin loaded when the player is first made, so a window with no video or sound, and the command line, load nothing more. Qt's FFmpeg backend logs a stream dump to stderr for every file; `main.cpp` turns that one category off (`QT_LOGGING_RULES` still wins) | 12.7, 14.2 | medium-high |
| The player is `MediaView.qml`: a source, a kind and a label in; play state, position and signals out; it knows no canvas, node or run. The face and the large view each make one. A video holds a decoder from the moment its source is set (it shows its first frame paused), so the face sets the source only while the node is in view. No volume slider (mute only; the system's volume is the level), no loop, no speed, no fullscreen, no waveform (canvas-look 4.5 sketched one; it needs peaks the engine does not report). A Preview or Save Video of a video with pending trim or crop plays the whole file, not the edit: the edits are applied only in a saved file | 12.7, 14.2 | medium |
| Research for #28: ComfyUI's video nodes (LoadVideo, SaveVideo, CreateVideo, GetVideoComponents, trim) and audio nodes (LoadAudio, PreviewAudio, VAE audio) were read as documentation and in the workbench note `map/research/14-comfyui-media-handling.md`; Fantasi (the Omarchy image app) and omarchy-local-ai, stable-diffusion.cpp `api.md` and acestep.cpp `docs/ARCHITECTURE.md` (MIT) for the servers. Fantasi's code was read for the protocol only; Fugue's provider is written for Fugue | 14.2, 8.7 | high |
| Video stages (#105) reuse the image stage capabilities, the stage file format, the nodes (Text Encode, Sampler) and the provider and transports as they are: the helper picks the video call from the context, a latent's descriptor says it is a video, and only two small nodes are new (Empty Video Latent, VAE Decode Video), because an output port has one type. ComfyUI does the same: one KSampler and CLIPTextEncode for image and video, EmptyHunyuanLatentVideo / Wan22ImageToVideoLatent for the empty latent, VAEDecode giving frames that CreateVideo/SaveVideo assemble | 8.10, 14.3 | high |
| The video leaves the helper as YUV4MPEG2 (`video.y4m`), not an encoded file or PNG frames: no codec in the helper (its build stays the pinned fork), one file for the transport, half the bytes of RGB, and ffmpeg (already required) reads it. Lossless FFV1/Matroska afterwards, as the engine keeps every intermediate video (2.4) | 8.10, 14.3 | medium-high |
| Wan 2.2 14B's two diffusion models are one more optional part of Load Models (`high_noise`), not a second model input on the Sampler: stable-diffusion.cpp loads both in one context and switches at its `moe_boundary` itself, where ComfyUI chains two KSamplerAdvanced nodes by hand. Wan's UMT5 goes in as `t5xxl_path`, told by the file's tensors | 14.3, 8.10 | medium-high |
| Research for #105: stable-diffusion.cpp (MIT) `docs/models/wan.md` and `docs/guides/c_api.md` at the pin (Wan 2.1 and 2.2 T2V, I2V, TI2V, VACE, S2V; `sd_generate_video`; `video_frames`, `fps`, `high_noise_*`); `src/pipeline/video.cpp` read to place the seams, as the image ones were. ComfyUI's Wan example workflows and node docs read for how it splits encode, sample and decode (its code is GPL-3.0 and was not read or copied) | 8.10, 14.3 | high |
| Enhance Prompt's styles are prompt templates, data (#140, the owner's principle: modular parts), one JSON schema read like the model catalog (shipped files compiled in, the person's in the data folder, same id replaces, invalid entries left out and said). Style stays a plain choice input whose list the registry fills at start-up (`Registry::setInputChoices`), so the CLI, the window and the run thread all see it, a workflow stores the id, and a group can narrow it; a live reload on a dropped-in file and picking a template from the connected model's family are not done (a model handle carries no family) | 14.2, 8.9 | medium-high |
| Describe Image (#194) is a node of its own, generic (not food), and not a new capability: Text Generate already sends an optional image on `text.generate` (#87), so it is Enhance Prompt's kernel with the picture as the input and a `describe` family of prompt templates (#140's data) for the style, instead of a second system-prompt field to fill in by hand. Four styles at first: a text-to-image prompt, a caption, a long description, SD-style tags. Not built: a custom question input (Text Generate does that), a food style (a template file in the prompts folder does it), a thinking switch | 14.2, 8.1 | medium-high |
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
| Video to video (#130, 2026-11-22) is one new node, VAE Encode Video, and no new Sampler: the Sampler already redraws from any latent by Denoise, and its refusal of a sampled video is lifted. The node does what ComfyUI does with a Load Video, a resize and a frame cap before VAE Encode, in one place, because a video latent is only right for one size, rate and length (the clip must be a multiple of 16 on each side and 4 n + 1 frames, and the helper reads a file of exactly that), so fitting it is part of encoding it; trimming to a part stays a Trim Video's job (pending trim and crop are applied first). Sizes: unset, the video's own in steps of 16; the picture is scaled to cover and cut from the middle, as the first frame is. The helper's input is YUV4MPEG2 in the layout `decode` writes (full-range BT.601 4:2:0), so a clip goes out and back with no codec in the helper and no colour drift, and a paired host takes it as data. In the patch the start latent replaces `init_latent` and `strength` drops the first sigmas, moving the high/low-noise switch with them, as image to image trims; no change to the sampling code. The helper takes the size, rate and length from the file, so the other stages must be given the latent's (the Sampler and the decode send the latent descriptor's). Chosen, not asked: full-range BT.601 in the y4m; a frame count that is not 4 n + 1 is cut, not padded; Wan 2.2's I2V and TI2V models take a clip too (the first-frame conditioning is made as for image to video) | 8.10, 14.3 | medium-high |
| Text Encode takes an edit's reference pictures and carries them on its conditioning (`meta.references`) to the Sampler, which sends them again (#88). stable-diffusion.cpp's flux2 preset gives references to the model, not the text encoder, but every stage of one generation must send the same options (the guidance sets an edit makes are chosen at encode), so both stages send `ref_images`. ComfyUI attaches reference latents to the conditioning (ReferenceLatent) on the positive and negative branch, after a VAE Encode of each: one wire here does all of it. The Sampler's cache follows the pictures, since the conditioning's descriptor holds them. The encode VAE-encodes them too, unused (a few hundred ms) | 14.3 | high |
| Templates (#83) are a separate repo, `bmaltais/fugue-workflows` (private, like Fugue), bundled as a submodule at a pinned commit: the owner's call (2026-11-20), so templates can be published and contributed apart from the app. ComfyUI ships its templates as a pip package pinned by version | 11.10 | high |
| A template is a folder with no index: ComfyUI's `index.json` must be kept in step with the files by a sync script and CI. A template's models and node versions come from its workflow, not from fields written again (ComfyUI repeats the models in the index and on each node) | 11.10 | high |
| Templates carry sample pictures (`inputs/`), as ComfyUI's repo does (`input/`); opening one takes them into the inputs folder, so an edit template runs as it opens (owner, 2026-11-20); this includes a picture given to a group instance's input (#372). Packs are not carried: a copy would replace the installed version of the pack; side-by-side versions are #90 | 11.10 | high |
| A template made by a newer Fugue is left out, judged by its nodes' versions (3.6), not by `requires.fugue`: a node at the same major and the same or older minor runs as made | 11.10 | medium-high |
| The person's own templates folder is `~/Documents/Fugue/templates`, beside where workflows are suggested; there a plain `.fugue` is a template too. ComfyUI has none (custom nodes' `example_workflows` only) | 11.10 | medium-high |
| A template's sources are exact: each URL's file has the recorded sha256 (Hugging Face's LFS oid). The FLUX.2 Klein 9B templates name unsloth's current Q8 file, not the older build first checked on the 3090, whose bytes only unofficial mirrors still hold (owner, 2026-11-20) | 11.10 | high |
| A model's source is, in order: the creator's own repo, then Comfy-Org, then another publisher, and an 8-bit file (fp8, INT8, Q8_0) where one fits a 24 GB card. Every URL is pinned to a commit. The Qwen3 text encoders come from Qwen's own GGUFs (the same weights as unsloth's; the file is named `qwen3/…` so it never meets unsloth's file of the same name), Klein 4B from BFL's fp8, and Z-Image Turbo from Comfy-Org's INT8. The rest stay with the next source down: BFL's Klein 9B repos are gated, the 8-bit Qwen-Image-Edit 2511 (20.5 GB) and Wan 2.2 (2 × 14.3 GB) don't fit without offload, and the depth and Grounding DINO creators publish no ONNX (owner, 2026-11-25) | 11.10 | high |
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
| Restore Image (#346): the ComfyUI workflow's last stage is `ImageScaleBy` lanczos x3, SeedVR2 preprocess (zero-pad bottom and right to 16), tiled VAE encode and decode (512, overlap 64), one euler step (cfg 1, denoise 1, the model's stored embeddings) and the `lab` post-processing. The engine's `vid_upscale` call is gone from the pinned stable-diffusion.cpp (3b12781f): SeedVR2 runs only through DGL, with no tiling or colour fix in its script, so the helper builds that script (tiling added) and the colour fix is Fugue's own, written from the description (ComfyUI is GPL: documentation only). Checked on the RTX 3090 with the workflow's own `seedvr2_7b_int8_convrot` and `ema_vae_fp16` (both hashes equal Hugging Face's LFS oids): 341 x 341 to 1023 x 1023 recovers the soft picture's books and lamp, and 1024 x 1024 to 3072 x 3072 (9.4 MP, 32 s end to end with the colour fix, 18.5 GB of card) brings back the velvet's grain and the wood's, with the colours of the lanczos picture. Catalog entries (Apache-2.0, Comfy-Org/SeedVR2 at a pinned revision): only the 7B INT8 ConvRot and the VAE, both hashed on this machine and equal to Hugging Face's LFS oids; the 3B and the 7B Sharp files are left out until someone downloads and hashes them | 14.1, 8.1, 8.9 | medium-high |
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
| Mask nodes as built (#171): the names and defaults are the owner's-taste picks to look at: Feather Mask outward 16 px, Combine Masks as the name of ComfyUI's MaskComposite, Overlay Mask color `#ff3b30` at 0.5. Choices made while building: `role` is read on outputs as well as inputs and only `mask` on an `image` port is accepted (a node with another role is disabled, so a typo shows instead of being ignored); ComfyUI's nodes fail on a negative `x` or `y`, so the manifests keep its range of 0 to 16384 and the negative case is the plain function's, tested with worked numbers; Feather Mask's box passes are sized so their variance is exactly that of a Gaussian of sigma radius / 2 and the width is continuous (a first version with integer boxes gave a variance 18% over at radius 4 and did nothing at radius 1), the picture's edge repeats beyond it (a mask reaching the edge stays full, as Crop to Mask and Stitch want), and the running sums are doubles; Fill Masked weights each kept pixel by 1 - m and mixes a partly masked one with the fill, and with nothing kept it is gray; a mask of another size is stretched (bilinear) for Fill Masked and Overlay Mask, and Composite Masked stretches it to the source as ComfyUI does; Overlay Mask's color takes `#rgb` and `#rrggbbaa` as the `color` widget does, the alpha multiplying the opacity; Composite Masked's output is the destination's channels, with alpha 1 for a source that has none; the pixel reading and writing the #29 nodes held privately moved to `pixel_io.*` so these nodes and Stitch share them; search keywords avoid the word blend, which belongs to Blend (a search test types it) | 14.4, 6.1 | medium-high |
| The painted mask is a separate 8-bit gray PNG in the inputs folder, named by its bytes, not the picture's alpha as ComfyUI keeps it: the picture's file and hash stay as they were (so its cache and its embedded workflow too), white is plainly the part to change, and saves, packages and templates carry it as they carry pictures. A widget `mask` holds its path, so a pack's node can have a painted mask | 6.2, 11.5, 14.4 | high |
| The brush paints with max and erases with min, sizes in picture pixels, undo as the 64 px tiles a step changed, at least 100 steps; defaults size 64, hardness 0.8 (owner's taste). The editor is the crop window's frame (`LargeViewDialog`, `CompareView` single) with a `QQuickItem` layer, so zoom and pan are the viewer's. Load Image's double click keeps the large view (learned place); Paint Mask's opens the editor, as Crop's opens its window | 12.7, 14.4 | medium-high |
| The mask enters the staged graph at VAE Encode (the latent carries it, as ComfyUI's Set Latent Noise Mask does), not at Text Encode or the Sampler: one place for every model. An inpaint model's masked picture lives in the conditioning in stable-diffusion.cpp, so one patch hook sets `c_concat` from the sample's own latents when it reads a conditioning, as a single run does; the alternative, giving Text Encode the picture and mask as well, would ask for the same wire twice | 8.10, 14.4 | medium-high |
| The helper, not Fugue, decides how an image model uses a mask, by the library's version name (inpaint models: the picture; others: the start latent): it owns stable-diffusion.cpp's behaviour, and the names are checked when the pin moves. Fugue's `model_shape` only learns the inpaint models' wider input so it stops calling them mismatched | 8.10, 14.4 | medium |
| Staged masks as built (#173, 2026-11-21): the Sampler sends the picture as VAE Encode did (the library scales both the same way) rather than pre-scaling it, and the mask pre-scaled by the nearest pixel, which the helper checks against the request's size; a mask of another size than the picture is `node_failed` on `mask`, not scaled; `mask_empty` is an error at VAE Encode and `mask_full` a warning there, not flags on the latent; an inpaint model ignores the start latent it is sent (its sample re-encodes the picture, so the VAE's noise draw is the one a single run makes); the hook sets `c_concat` only where a single run would (the unconditional and image-unconditional when the conditioning has them). Staged equals one `generate`, byte for byte, on an RTX 3090 for FLUX.2 Klein 4B, FLUX.1 Fill (fp8) and SDXL Inpaint; the FLUX.1 Fill staged picture without the hook differs, which is what the hook is for. The diffusers SDXL Inpaint UNet is read for its 9 channels from `conv_in.weight` only when it has 9 and `down_blocks.` (a VAE and a 4-channel UNet are not) | 8.10, 14.3, 14.4 | high |
| Generate Image's mask as built (#175, 2026-11-21): the capability is a model property `inputs: ["mask"]` (not a new capability: the same models serve `image.generate` with or without a mask); the node checks it in `request()` from the selection and does not make it a hard routing requirement, because a hard one answers a named model that cannot with `model_missing` and gives no list of the ones that can, while a preference (`Requirement::preferred`, `Selection::preferredOffers`) picks a capable ready model for "any model" and lets the node say `mask_unsupported` with the names. The omarchy provider reads `features_by_mode.img_gen.mask_image` (the mirror `features` when there is no mode-aware object); it refuses a mask for a model without it rather than drop it. The mask must be the start image's size, as VAE Encode's (no `mask_resized` scaling here: a mask of another shape cut by cover would not line up with the picture). The put-back is over the picture as it went to the model, at the result's size. Not checked on a live sd-server (none on this machine) and not on a GPU (the helper has no `image.generate`) | 8.1, 8.3, 8.7, 14.4 | medium |
| Composite back is a node in staged graphs (Composite Masked, Stitch), and built into Generate Image, the coarse node whose promise is that the outside is unchanged. Soft edges come from Feather Mask and Stitch's blend, in pixels, because the library rounds the mask in the latent | 14.4 | medium |
| Crop and stitch as built (#176): the group ships from the built-in pack (a `group.fugue` resource beside its `node.json`; the loader, `loadLibraryGroup` and `embedGroupTypes` took it with no change), so #177's template only adds it. Choices made while building: the handle's `meta` names the picture and mask as the inputs' own media handles, so the cache and the hash follow them, and a mask of another size is kept as given and stretched in Stitch (a resized mask written to the node's folder would move into the cache's store and leave a stale path); the shape check compares the picture with the handle's own `size`, not with the box, so the rounding to a multiple of 16 never causes `stitch_mismatch`; no side of a crop passes 4096 (a sliver box would otherwise be 16 x 65536 at 1 MP); a crop that is not resized takes no resampling, so megapixels 0 with blend 0 returns the original byte for byte; the mask is feathered over the whole picture and cut at the box, not feathered inside the box, so the edge fades alike at the box's border; the box is the bounds of the pixels above 0, so a faint stroke counts and an 8-bit mask's 1/255 is not ignored; no ComfyUI golden (the formulas are the shared resize and feather kernels; the GPL Inpaint Crop and Stitch node was not read beyond its README); `mask_full` is not a warning of Crop to Mask (the box is the picture) | 14.4 | medium-high |
| Keep unchanged areas (#621): a group first, built from existing nodes (Blur, Blend difference, Image to Mask and Combine Masks add for the summed channel difference, Threshold, Grow, Feather, Color Match, Composite Masked); a Change Mask node is optional and waits until the group shows what it cannot do (low-contrast changes, round morphology, removal by area). Two-pass Color Match, a fit step and a close step were needed, found on real Klein edits; defaults tuned there (blur 4, threshold 0.12, speck 12, fill 24, margin 16, feather 16) | 14.4 | medium-high |
| Names and defaults the owner may change: Feather Mask (`fugue/feather-mask`, a blur of the mask outward by default; ComfyUI's FeatherMask, which fades from the picture's borders, would convert to another node), Combine Masks for MaskComposite, Crop to Mask's padding 64 px and 1 megapixel, Stitch's blend 16 px, the overlay in the accent at 50% | 14.4 | medium |
| Both templates run FLUX.2 Klein 9B, the files the edit templates already use, by masked sampling with the picture as a reference: the owner has the files and they are checked staged; Klein 4B (Apache-2.0) is the alternative with a licence for commercial use, FLUX.1 Fill the dedicated model once its file is at hand. Whether the reference helps or makes Klein keep the old content was checked on the 3090 (#177, same seed, both ways): for the inpaint crop the reference helps (without it the chair lost its legs and its place), so the group keeps it; for the outpaint it makes Klein keep Pad's gray border, so the template gives Text Encode no reference | 11.10, 14.4 | high |
| Color Match as built (#180, 2026-11-21): one node, not a group (nothing in it splits further; Feather Mask and Composite Masked stay the nodes that decide the seam). Statistics are weighted by 1 - m rather than cut at a threshold, so a half-masked pixel counts half, as Fill Masked does. The mask describes the picture; the reference shares its weights only at the same size, because for an outpaint the reference (the original, smaller) and the redraw do not line up, and measuring a different-sized reference whole needs no offset input. The pictures go to linear light before the paper's LMS matrix (the paper leaves the RGB unspecified; sRGB values are not light). Strength mixes in l-alpha-beta, which equals mixing the mean and the scale. A mask of all 1 measures the whole picture with a warning rather than failing. `mask_full` and `mask_resized` are reused, so there is no new error code. The Outpaint template is unchanged: whether the node visibly helps on the 3090 is for the owner to check (before and after on #180), then it goes before Composite Masked | 14.4 | medium-high |
| Differential diffusion (#179, owner decision 2026-11-22: option a): carried in the fork (`bmaltais/stable-diffusion.cpp`, branch `fugue`) behind `fugue.soft_mask`, not proposed first, so it ships with the next build; the hunks leave upstream byte-identical when off and are listed in helper/README.md and checked at each pin move, which is why 8.10's rule allows opt-in hunks. Written from the paper (its repository has no licence). The control is the Sampler's `soft_mask` input, not a VAE Encode option: the Sampler is the node that sends the mask to the helper and already owns the other per-run options, and the latent still carries the mask as before. An inpaint model's mask stays rounded (it is a conditioning channel). The threshold is applied to the denoised prediction at each step as the library's own replacement is, with k the steps the schedule runs (so Denoise below 1 counts its shorter run) | 8.10, 14.4, 17 | medium-high |
| Automatic masks are optional packs of command nodes, not built in: each brings a model and a runtime (ONNX Runtime or Python) that Fugue's binary should not link, and their weights' licences vary (BRIA's are non-commercial) | 14.4 | medium-high |
| Remove Background cutout (2026-11-22): the group and Apply Mask as Alpha, not one node. Composite Masked cannot cut out (it mixes the destination's color into a soft edge, tested), so one small builtin node, `fugue/mask-to-alpha`, puts a mask in alpha; the group is `find` -> `soften` -> `alpha` and lives in the pack, not in the fugue pack, because the built-in pack must not name a node of an optional pack. Alpha is the image's alpha x the mask, so a picture that is already partly transparent stays so and an opaque one gets exactly the mask. Colors are not premultiplied by the mask, so a soft edge keeps the subject's own colors (what PNG and WebP expect). `feather` is Feather Mask inward at 0 by default (off): the model's mask is already soft, and inward shrinks away a fringe of the old background. Both outputs, no `invert` (the mask node has it). The pack node was renamed Remove Background Mask so a search for the group's name finds one thing | 14.4, 5.3, 6.1 | medium-high |
| Remove Background (#178, #167, 2026-11-22): BiRefNet through ONNX Runtime in the pack `io.github.bmaltais.fugue-remove-background`. **Licences, checked 2026-11-22**: BiRefNet's code is MIT (GitHub licence file, "Copyright (c) 2024 ZhengPeng"); its weights are MIT too (the Hugging Face model cards of `ZhengPeng7/BiRefNet` and `BiRefNet_lite` say `license: mit`, the repository's README shows the MIT badge, and the author's ONNX files are assets of the repository's release `v1`, converted from his `.pth` files); ONNX Runtime is MIT; NumPy is BSD-3-Clause and Pillow is MIT-CMU (HPND). The model cards do not speak for the training sets (DIS5K, DUTS and others), whose own terms are research-oriented: not verified, so the catalog says "MIT" and links the licence, not "for any use". The pack's `pack.json` says it in `description` (an unknown field would only warn) and `license` is the pack's own code, MIT; each catalog entry carries `license`, `license_url`, `commercial` and `attribution` | 14.4, 6.4, 8.9 | high (code), medium-high (weights) |
| Paint canvas core (#181, 2026-11-22). **Research** (documentation, from memory of the pages and not re-fetched this session, so unverified in detail; no GPL code read): **Krita** (docs.krita.org: layers, undo): paint, group, vector, filter and file layers in a stack; each has visibility, opacity, blend mode, alpha lock, inherit alpha and lock; masks are separate layers (transparency, selection) attached to a layer; one undo history for the document, with layer operations as steps; the brush engine has size, hardness (fade), opacity and flow, and a stroke does not build up past its opacity unless flow is below 100%. **ComfyUI's mask editor** (docs.comfy.org/interface/maskeditor; its frontend is GPL and was not read): three layers, mask, paint and image, a paint pen with a color, size, opacity and hardness, an eraser, undo and redo; the paint layer is saved as a picture beside the mask. **Photopea and Photoshop** (docs): a layer list bottom to top, the eye, an opacity slider, drag to reorder, new, delete and duplicate, a picture as a layer, flatten; layer operations undo like strokes. **InvokeAI** (Apache-2.0; docs and the unified canvas's layer model): raster, control, inpaint-mask and regional-guidance layers, each with a visible toggle, opacity and reorder; one undo history; "merge visible" to a picture, and a layer's picture is what a control model takes. **Matched**: a bottom-to-top list of layers of kinds paint, picture and mask, each with a name, a visible toggle and an opacity; one brush and one mode shared by all layers; one undo history over strokes, shapes and layer operations (add, remove, move); a picture layer is not painted on; flatten to one picture; the mask kept separate from the color. **Improved**: it is the mask editor's own core, not a second one: `MaskPaint` takes 4 channels (straight RGBA) as well as 1, with the same dabs, shapes, tiles and undo, and the mask editor's bytes and behaviour are unchanged (its tests are the same); undo is bounded by the tiles a step touched, for color as for masks, and a removed layer is kept whole by its step so undoing the removal brings back its paint; a color stroke does not build up over itself (the pixel as the step began, with the color over it by the stroke's largest coverage), which Krita's flow and a mask's `max` already do; the mask is a layer kind and a separate output (`flattenMask`), as it is a separate port in Fugue, so one canvas gives a picture and its mask. **Decided** (core only; each medium-high unless said): (1) the class stays `MaskPaint` and gains `channels` (1 or 4), not a new class, so the mask editor shares the code with no copy (the name is now narrower than the thing: renaming touches the mask editor and was left for a change that does only that); (2) straight, not premultiplied, 8-bit RGBA, as PNG and `Image` are in Fugue (2.1); scaling interpolates the straight channels, which darkens a soft edge a little, as the mask editor's `scaled` is used only to match sizes; (3) one canvas size; a picture of another size is scaled to it (bilinear); (4) history: each layer keeps its own tiles and the document keeps the order, so a step is the tiles of one layer, not a copy of the canvas (medium); a layer's visibility, opacity and name are not steps (the person toggles them to look, and they are cheap to redo); (5) invert on a color layer flips the colors and keeps the alpha, clear makes it transparent, fill covers it with the brush color times opacity; (6) `flatten` composites in floating point, bottom up, mask layers left out; (7) the color comes from the caller (`Brush::color`), so the shared color picker stays the one place a color is chosen (12.2). **Not done, the next slice**: the window (a `PaintLayer` item per layer like `MaskLayer`, the layer list panel, the Paint canvas dialog opened from an image input's row or a node; no obvious seam to open it from was found without a decision on which widget it is), blend modes, layer groups, lock and alpha lock, duplicate and merge down, a bucket fill, a color select, text and shapes, saving a canvas as a project file with its layers (only the flattened picture and mask exist), and Fugue nodes that take the picture and the mask | 14.4 | medium-high |
| Canvas store (#208, 2026-11-22; decided without the owner, each to confirm). (1) Two kinds of object, as git does: a **tree** (content) shared across canvases and a **commit** per canvas; the revision hash is the commit's, the cache key will use the tree hash. Medium-high. (2) A tile's hash covers a small header (size and channels) as well as the raw bytes: "raw-pixel SHA-256" alone would let a mask tile equal a color tile with the same bytes. Medium-high. (3) All-zero tiles are not stored, so a mostly empty layer costs its content only. High. (4) Blobs are shared by every canvas of the store (`blobs/`), so a fork or a second import costs nothing. High. (5) The store is at `<data>/canvases`, which is the project's own under `FUGUE_HOME` and the session's otherwise. Medium-high. (6) CAS uses a `QLockFile` per canvas around read-compare-rename; a lost race is a result (`moved: false`), not an error. High. (7) Picture layers are placeholders, as in the `.ora`. Medium-high. (8) Nothing is deleted (no GC, no retention) until a later slice decides the policy. High. (9) `fugue canvas list` is not built (not in the issue); ids are printed by `new` and `import`. Medium. (10) `export` never replaces a file. High. | 14.5, 13.1 | medium-high |
| Canvas nodes (#209, 2026-11-22; decided without the owner, each to confirm). (1) `head` is named by `compileForRun` after the compile, as a literal on the node, not by a kernel at run time: the hash must be in the compiled graph for `compiled.json`, the key and a replay. Medium-high. (2) The cache key holds the revision, not the tree hash alone (a kernel `keyExtra` for node kernels would be needed); an identical picture from another commit misses once. Medium. (3) Region's stitch is Crop to Mask's `fugue/stitch`, with the whole composite and the region's mask written as files, so Stitch accepts it; the price is one composite file per Region run. Medium-high. (4) Write takes the canvas handle as an input (Region's `canvas` output), so the parent revision is explicit and the Region and the Write of one workflow agree by wiring; a Write on its own canvas id with `head` is not offered. Medium-high. (5) Write is cached like any sink and not `deterministic: false`: re-running a compiled graph does not write twice. Medium-high. (6) A lost compare-and-swap is a warning and `accepted: false`, with the candidate kept, never an error. High. (7) Region's `feather` softens the mask output only and Write's `blend` is the seam, as Krita splits them; Region's image is the composite with the canvas's transparency (not flattened onto a color). Medium. (8) Region defaults: padding 64, scale 1, cap 1.5 MP, multiple 16, feather 0; Write: blend 16, layer name `Fill`. Names and numbers the owner may change. Medium. (9) The source is the composite or a rectangle or mask only: a named layer, a canvas mask layer as the selection, positions and blend modes are later. Medium-high. | 14.5, 9.1, 10.1, 14.4 | medium-high |
| Canvas 3 (#210, 2026-11-22; decided without the owner, each to confirm). (1) A candidate is any stored revision outside the head's history that has no `resolved/` file; nothing is stored differently, so a lost race and a deliberate keep are the same thing. High. (2) Resolutions are small files beside the commits, not a change to the immutable objects; dropping hides, never deletes (GC stays a later slice). High. (3) Accept of a candidate made from an older revision places only its added layers on the head (a rebase of the simplest kind) and refuses anything else with `canvas_conflict`; no pixel merge. Medium-high. (4) Canvas Write gets a `candidate` input (1.1.0) rather than a second node, so the template keeps its result for review and the default behavior of #209 is unchanged. Medium-high. (5) `accept` and `drop` with no `@revision` take the one open candidate and are `usage` when there are none or several, so a person cannot accept the wrong one by habit. Medium. (6) `diff` with one argument compares to the parent; two arguments may be of two canvases (trees are global). Medium. (7) The template has no canvas chosen and the compile step leaves a blank canvas alone (the kernel refuses at run time), so a template can ship; the owner may prefer a Canvas New node later. Medium. (8) The template is flat nodes, not a group, so Region and Write stay visible (the epic's answer to dynamic templates). Medium-high. (9) The history is a File menu dialog on the existing Compare viewer, not a panel in a canvas window (there is none yet); it picks a canvas from `CanvasStore::list`, a new read-only call, with `fugue canvas list` for the same. Medium-high. (10) The viewer compares flattened pictures, not tile overlays; the summary text carries the rectangles. Medium. | 14.5, 13.1 | medium-high |
| Canvas view and export (#214, 2026-11-22; decided without the owner, each to confirm). (1) The history viewer shows one revision alone by default and Compare is a toggle in the bar, not a fifth mode: the question is usually what the canvas looks like; the comparison keeps the mode last used. High. (2) Ctrl+click on a row (the A side) turns Compare on, since it only means something compared. High. (3) The window exports a PNG only (the issue); the command line writes png, jpg and webp. Medium-high. (4) The extension decides the format on the command line; an unknown or missing extension is a `usage` error, where before any name wrote an `.ora`. Medium-high. (5) JPEG is flattened onto white (quality 92), said in `data.background` and the text; WebP is lossless (it keeps alpha and exact pixels), at most 16383 px (its format limit). Medium-high. (6) The PNG is `flattenToPng`, the history window's own flatten, so a file and the picture shown are the same pixels. High. (7) The window's Export never replaces a file and says so; the file dialog may already have asked about an overwrite, in which case Fugue still refuses. Medium. (8) The first revision (no parent) has Compare disabled with a tip. High. | 14.5, 13.1 | medium-high |
| Canvas view region editing (#224, 2026-11-22; decided without the owner, each to confirm). (1) The frame is the existing `CropFrame` extended (`draw`, `constrain`, `yieldPress`, `cancel()`, `dimAlpha`) and `crop::drawn` added to the shared geometry, not a second frame; the region picker window keeps working unchanged and can adopt the same options. High. (2) A left drag on empty picture draws, so pan is the middle button or Space-drag (Shift-drag, the viewer's other pan, is the frame's ratio key); at Fit the picture does not pan anyway. Medium-high. (3) Snap is a toggle, off at first, as in the region picker; it is applied inside the drag so the frame shows what is written. Medium-high. (4) No ratio buttons here: Shift (square for a new one, the start ratio for a resize) and Alt cover the window's lock for a quick pick; the picker window keeps the named ratios. Medium. (5) The numbers are in the line under the picture and are not typed here; the picker window has the fields. Medium. (6) Editing is off while Compare is on, since a left drag there moves the divider. High. (7) The dim outside the frame is off so the candidate can be judged while it is framed. Medium-high. (8) The rectangle is held to the shown picture's size; a pinned older revision of another size is not special-cased. Medium. | 14.5, 14.4 | medium-high |
| Canvas picker (#211, 2026-11-22; decided without the owner, each to confirm). (1) A new widget kind `canvas`, not a general "resource list" widget: the model picker is the only list there is and it is bound to resource handles and providers, so a general list would have one user and a manifest field (`source`) nothing else needs; the pieces are general where it pays: `CanvasChoices` (rows, makers, watch) knows no widget, and the menu is the existing `WidgetMenu`. Medium-high. (2) Only Canvas Region's input is typed: Canvas Write takes Region's `fugue/canvas` handle, so there is no second id to pick; any later node with a canvas id takes the widget with one line in its manifest. High. (3) The value is the id string; the name, size and time are looked up for display and never stored. High. (4) The list is the store of the session (`CanvasStore::forSession`, which is the project's own under FUGUE_HOME): a canvas of another root is "missing" by design (that is the bug that prompted this), shown by its id with the list beside it, not hidden and not an error until the run. Medium-high. (5) New canvas… takes a name and a size (default 1024 x 1024, the limits of `fugue canvas new`, refused in the dialog with the engine's sentence) or a picture or `.ora` (`readCanvasFile`, as `fugue canvas import`); Import picture… is the same picture path without the dialog, named after the file. Making the canvas is not part of the undo step (nothing is deleted, spec 14.5): undo takes the id out of the input and the canvas stays. Medium-high. (6) The list refreshes by a folder watch on the store (the folder, and each canvas's, where a head is renamed into place), settled over 120 ms, and is re-read each time the menu opens; no polling. The row is read from disk each time (a few small files per canvas). Medium. (7) The same rows serve the history dialog's canvas list. High. | 6.2, 14.5, 12.4 | medium-high |
| Region picker (#213, 2026-11-22; decided without the owner, each to confirm). (1) The action is an entry of the existing canvas drop-down, not a new input: a widget-only input would be a row in every saved workflow and the manifest, and the picker needs the canvas chosen first, so the entry sits where the canvas is chosen; the price is that it is found by opening the list (the node's context menu may also offer it later). Medium. (2) The Crop window's viewer, frame, handles, ratio lock and `CropGeometry` are reused whole (only `CropView` gained `changing`): one rectangle behavior in the app. High. (3) The pure part (`region_pick`: snap, context, model size) calls the node's own functions rather than restating the rules, and a test compares them with the node's for many rectangles. High. (4) Snap rounds each edge to the nearest multiple (the far canvas edge counts as one) and with a ratio lock only moves the top left, so the lock outranks the snap; snapping is applied when a drag ends, not during it (the frame shows the raw drag, the context box and numbers the snapped one). Medium. (5) The picture is a PNG of at most 2048 px on its longest side, made once on open (a 16384 px canvas is a one-time cost of reading and flattening it, the window waits); a tiled viewer is not needed until a canvas editor is. Medium-high. (6) Apply writes four explicit numbers, so a width of 0 (to the edge) becomes a number; and nothing when nothing changed. Medium-high. (7) A linked input's value is unknown before a run: its default stands in the dialog, with the note; a linked X, Y, Width or Height forbids Apply. High. (8) **Select mask…** (paint window on the canvas) is not built: the paint window's picture comes from a node's input picture, and a canvas backdrop needs a flattened revision as a source and a mask that is saved at the canvas's size, which is a change to the paint window, not a use of it; left as a follow-up. Medium. **Not verified**: the real window, the dashed line's look on a bright picture, a 16384 px canvas's open time | 14.5, 6.2, 12.7 | medium |
| Copy a selection (#327, 2026-11-23; decided without the owner, each to confirm). (1) The logic is `engine/canvas_copy.*` (pure: cut to the selection's bounds, alpha = picture alpha x selection, the mask form, a layer's pixels with its layer mask and without its opacity: also what Export layers as files uses), the window part `CanvasViewModel::copyPicture`; the clipboard gets a QImage, so other programs read a PNG and the Graph's existing image paste (`ImageInputs`, a Load Image node) takes it, with no new Graph code. (2) The mask is the whole canvas's size, not cropped, so it lines up as a mask for Canvas Region and the other mask nodes; it is opaque gray, so it is a picture to every node and app. (3) Ctrl+C in the Canvas view copies the picture and Ctrl+Shift+C is Copy merged (Photoshop's keys); the Graph's Ctrl+C is the fallback when no layer can be copied. Copy selection as mask has no key. (4) Nothing selected copies the whole layer, as Photoshop. (5) A layer inside a group cannot be picked for Copy yet (Copy merged works). (6) From the owner (2026-11-23): one paste should give the image and the mask. A copy with a painted selection also offers the selection, cut to the same bounds, as a gray PNG under `application/x-fugue-mask` (`clipboardMaskType`); the Graph's paste (`ImageInputs::take`) stores it in the inputs folder (`selection-mask-<hash>.png`) and sets it as the new Load Image's painted `mask` input, so Image is the pixels and Mask is the selection (white = to change, as a painted mask). A Load Image with no painted mask still gives 1 - alpha. A rectangle or no selection offers no mask (all white). |
| New canvas dialog (#232 slice A, 2026-11-22; decided without the owner, each to confirm). (1) The dialog is its own `Question` item, `NewCanvasDialog`, with its logic in `NewCanvasModel` (a QML type, tested with no window), because three entry points need it (File menu, the canvas picker, later the Canvas view's File menu in slice B) and each only decides what shows the new canvas. (2) Presets are Square, Landscape 4:3, Wide 16:9, Portrait 3:4, HD, 4K and Custom, 4:3 and 16:9 being 1024 × 768 and 1280 × 720 (about a megapixel, what the models are tuned to) and HD and 4K the screen sizes; Custom is not a size but the state "typed", so it keeps the numbers. (3) Photoshop's choices are followed: a clipboard picture is preselected with its size shown (Photoshop's Clipboard preset), a filled background is a layer called Background, a transparent one an empty layer. The risk, a Create meant for a blank square that makes the clipboard's picture, is met by showing the choice (Clipboard lit, the size fixed, the line "the picture on the clipboard") and by a one-click Blank. (4) Last-used settings are the preset or size, background and color only, in the state folder (so a project has its own), written only when a canvas was made; the name is always the first free `Untitled <n>`, the source always Clipboard-when-present. (5) Errors are the engine's sentences (`canvasSizeProblem`), shown live, with Create waiting, rather than a refusal on press: a person sees the cap while typing. (6) The cap is 67 megapixels (8192 squared), the figure the engine prints; the docs said 64. (7) The key is Ctrl+Alt+N (free in every context; Ctrl+N is New workflow); Paste as new canvas has none, since Ctrl+Shift+V is Paste with connections and the command is rare. (8) A new canvas made by a command is shown in the active Canvas area, else in the Canvas view's large view, because a person who made a canvas wants to see it; `CanvasView.qml` is not touched (slice B will add the view's own File menu and may replace the large-view fallback). |
| Workspaces 0: areas and view types (#220, epic #217, 2026-11-22; decided without the owner, each to confirm). (1) An `AreaNode` is not an item: every view goes straight into one host item at its rectangle and a view the window already has is adopted, so an area costs no item and the default window is item for item what it was; a nested item per area would have cost one item each and a reparent of the Graph on every split. High. (2) The window's one area has no host: the canvas stays in the column, and a host for the body is made only while the tree has a split (slice 1), so the default window never pays for areas. Medium-high. (3) The saved axis is `"axis": "row" \| "column"` (start to end, top to bottom), not the research's `"orientation": "horizontal"`, whose meaning differs between Blender, Qt and VS Code. Medium-high. (4) No workspace store and nothing on `App` yet: nothing changes a layout in slice 0, so a store would be code with no caller; the registry and the layout helper are QML types the window and tests use directly. Medium. (5) The Graph is one per window, and its minimum is 160 x 120 px (what the side panel already leaves the canvas). Medium. (6) A new area's id is one more than the largest, so a gone area's id is used again; ids only mean something inside one tree. Medium. (7) Redundant shapes are normalized (a branch of one child, a row in a row, shares not summing to 1), damage is refused whole (the default tree); at most 64 areas and 16 levels, and a split that would go deeper is refused so whatever is made reads back. Medium-high. (8) Minimums that do not fit shrink every part in proportion to its minimum rather than overflowing the window. Medium. (9) The same shape with new sizes moves the views; another shape makes the views again (an adopted one stays), which slice 1 may refine so a split keeps the other views. Medium. (10) The default window is pinned by a file of its item count and item names (`tests/workspace/data/default-window.txt`), not only by the budget bound in test_canvas_window. Medium. **Not verified**: the real window (offscreen tests only) | 12.16 | medium-high |
| Canvas view shows a new result (#225, 2026-11-22; decided without the owner, each to confirm). Cause found: the strip already refreshed from the folder watch (tested with a candidate written from another thread, in the dialog and in an area); what the owner saw was the by-design head view. (1) The view moves to the newest arrival only from the head or from a result it showed itself, never from a revision the person chose. High. (2) Compare on and a rectangle drag lock the view (`viewLocked`); a result that came during the lock is shown when it lifts if still on the head, so a drag is never interrupted and the result is not lost. Medium-high. (3) The setting is `canvas_show_new_results`, a `preferences` key on by default, in Settings > General after Reopen tabs at start (no page of its own for one switch). Medium-high. (4) Arrivals are the open candidates not in the last successful read; the first read of a canvas, and a failed read, count none. High. (5) Several arrivals: the first row (newest) is shown and `n` more are counted; a later one while looking at the shown result replaces it and adds to the count. Medium-high. (6) The banner has the bar's Accept and Drop (same calls) and **Keep looking at head** (since #228 **Back to Head**, first, with Drop last and the bar's pair hidden while the banner shows), and a one-line hint that Canvas Write with Keep as candidate off moves the head by itself (the node's input is "Keep as candidate", default off). Medium-high. (7) A failed read is retried up to three times, 400 ms apart; no spinner. Medium. (8) The producer is shown as the node id of a Canvas Write (`canvas-write:<node>`), not a time. Medium. **Not verified**: the real window and a real run of the template (the tests write the candidate as Canvas Write does, from another thread) | 14.5, 12.16 | medium-high |
| Candidate review removed (#239, 2026-11-22; owner's decision, the details decided without the owner, each to confirm). Candidate review is not a user concept (Photoshop's model: a run is a layer; delete it or Undo). (1) Canvas Write 1.4.0 drops the `candidate` input; a new manifest field `retired_inputs` (a list of names) makes the compiler skip a saved value for one with no error and no notice (the 3.6 older-minor warning would have said "its value is not used"). High. (2) An unplaceable result (the canvas changed size) is the error `canvas_head_moved` in plain words and nothing is written: the size is checked before storing, and in the race after the check the stored revision is hidden with `drop` (the store has no delete, so the hidden revision is only in History's All revisions). Medium-high. (3) The Canvas view loses the banner, the pending chip, Accept and Drop (bar and layers panel), the overlay toggle and `canvas-view.show-candidate`; `canvas_show_new_results` leaves settings (a file that has it is read without a problem and keeps it). High. (4) Kept: `CanvasStore` candidates, accept and drop, `fugue canvas candidates\|accept\|drop\|diff`, the History dialog and the History panel's All revisions (a candidate is shown alone, read only, with Back to latest). High. (5) `Banner.decision` and the task bar's and panel's `decisionElsewhere` had no other user and are gone. High. **Not verified**: a real run of the template in the real window | 14.5, 12.16, 6.1, 3.6 | medium-high |
| Workspaces 1: split, resize, area headers, the Canvas view, the kept layout (epic #217 slice 1, 2026-11-22; decided without the owner, each to confirm). (1) The Graph always has exactly one area: its area cannot switch away, Graph chosen elsewhere trades the two areas' views and states, and closing its area gives it to the area that takes the place; a window without the Graph would leave the run toolbar, node search and the dialogs with no home and every canvas key acting on a hidden graph. Medium-high. (2) A split's new area shows the split area's view when it may be shown twice, else the first registered one that may (splitting the Graph gives a Canvas), as Blender duplicates the editor. Medium. (3) Keys: `Ctrl+\` (VS Code's split editor) and `Ctrl+Alt+\`, `Ctrl+Alt+W` (Ctrl+W closes the workflow), `Ctrl+Alt+G` and `Ctrl+Alt+C` for the two views, none for Reset layout; all free in every context, none a text field's; Ctrl+Alt is used because Linux reports AltGr apart from it. Medium. (4) The commands sit in a View > Areas submenu after Focus mode, so the menus keep their order (learned place) and the default window shows nothing new until it is opened. Medium-high. (5) Headers only with two or more areas, 7u tall with 24 px buttons, the view menu at the start and split, split, close at the end; focus mode hides them, the borders stay. Medium. (6) A border is a 24 px target centered on it, under the headers so their buttons win; a drag puts the border under the pointer (an absolute rule, so a clamped drag does not drift), a double click or Enter shares equally, the arrows move 2u (Shift 10u). Medium-high. (7) The active area follows a press (a C++ host that sees presses before the item under them and never takes them) and the keyboard focus, not hover; it is not kept, and the Graph's area is active at start. Medium-high. (8) `workspaces.json` (format 1 of the tree), 400 ms after a change and at quit; a layout of one Graph area is never written and reaching it removes the file; a damaged file (or one over 1 MiB) gives the default window, is kept as `.bad` and is said once on the notice line; a layout with no Graph gets it in its first area, a second Graph shows a Canvas. Medium-high. (9) A rebuild keeps each made view whose area id and type stay, so a split or a close leaves the other areas' zoom and scroll. Medium-high. (10) A Canvas area keeps `{"canvasId"}` as its state, records the canvas it opened on once, and opens on the newest when the kept one is gone; Canvas is `many` (two areas may show one canvas: the view writes only the region's revision input, never the head). Medium. (11) Minimums are the area's, header included: Canvas 240 x 180 px. Medium. (12) On the first split the column gets a host in the canvas's place with its margins and the canvas moves in; the side strip, side panel and menu-bar handle are placed from the body (host or canvas), the run toolbar, queue panel, search, context menu and dialogs stay hung from the canvas. High. **Not verified**: the real window (offscreen tests only), the look of the headers and borders, a touch or pen drag of a border | 12.16 | medium |
| Paint file, widget and node (#181 slice 1, 2026-11-22; the owner said OK and an Opus subagent recommended, the design note on the issue). **Decided** (each medium-high unless said): (1) a new widget `paint` and a thin node `fugue/paint`, not a change to `mask` (a mask literal is one 8-bit gray PNG by design; mixing brings back the polarity confusion #167 removed), and not an "edit in paint canvas" entry on a port (a later menu entry that creates this node); (2) the file is OpenRaster (`.ora`), a zip any painting program can open: `mimetype` first and stored, `stack.xml`, `data/layer-N.png` per layer by `MaskPaint::toPng`, plus the attribute `fugue:kind` (paint, mask, picture) that other programs ignore; everything is stored, not deflated (the PNGs are compressed already); (3) the same layers give the same bytes: one order (N is the position, bottom first), every entry's time is 1980-01-01 and its owner 0, no thumbnail, merged image, program name or date; read and written again it is the same file (a test); (4) a picture layer is a placeholder: its pixels are never stored (the file does not hold or change with the input picture, so the content-hash key moves only when the painting does), its PNG is one transparent pixel, and at run time every picture layer takes the node's picture; (5) reading is for hostile files: a zip's central directory is required (a cut file is refused), the first entry must be the mimetype, an entry with an absolute name, `..`, `\`, `:` or a double slash is refused, an entry no paint file has is refused rather than skipped (skipping still unpacks it), at most 200 entries, 64 layers, 16384 px a side and 64 million pixels a canvas (8192 x 8192), each entry at most 256 MB unpacked and all layers 1 GB decoded, a PNG's size is read from its header and must be the canvas's before anything decodes it, a DTD, a group, an offset or an opacity outside 0 to 1 in `stack.xml` is refused, and nothing is written to disk; (6) an input picture is an 8-bit straight RGBA layer at run time (a 16-bit or float picture is quantized only when paint is over it; with nothing painted over it the picture itself is the output); (7) Width and Height are optional with no default: a node with no picture and no size is `missing_input` rather than a guessed 1024 square, which shows at once on a linked picture that has not run; (8) `mask_resized` is the code for a canvas of another size too, so one warning means one thing in Load Image, Paint Mask and Paint; (9) a package, a template and the inputs folder take an `.ora` as they take a picture (`sniffInputFile`: an image, or a zip whose first entry is the mimetype), after reading it whole with its caps; a paste and a drop of a picture still take images only. Not done: the window (built in slice 2, below), thumbnails and merged images, other programs' offsets, groups and blend modes (a file from another program is refused), the active layer and the history in the file | 6.2, 11.5, 14.4 | medium-high |
| Paint window (#181 slice 2, 2026-11-22; decisions made without the owner, each to confirm). **Decided** (each medium-high unless said): (1) one frame, not a fork: `MaskEditor.qml` takes `layers: true` and `canvasLayer` is the `MaskLayer` or the `PaintLayer`, so the pointer, brush ring, shapes, keys, zoom and undo are one code (`test_canvas_mask_editor` and `test_canvas_mask_node` pass unchanged; the editor's `layer` alias still is the mask's layer); the bar's slider became its own file (`ValueSlider.qml`) so the layers panel reuses it; (2) the cell grid of `MaskLayer` (256 px textures, only the touched cells upload) moved to `CellGrid`, which `PaintLayer` shares; (3) `PaintLayer` draws the stack itself, picture layers included, so a layer under the picture is covered by it as the node will do; the viewer under it is empty and gives only the zoom and pan; (4) the input picture's file is the one the linked node shows (`CanvasRuns::inputPicture`), not the Paint node's own result (which has the paint on it), and a Paint node's slot keeps showing its own result: the face needs no overlay as the mask nodes do (medium); (5) the `paint` row is the whole button, as the `mask` row is (spec 14.4), not a thumbnail row with an Edit button: the picture slot already is the flattened result, and a taller row would change the node's layout rules (medium; the issue text asked for a thumbnail and an Edit button); (6) double click on the Paint node's picture opens the window, as for Paint Mask; a blank canvas has no picture to double click, so its row is the way; (7) the picture layer cannot be removed (only hidden or moved), and a window with only the picture visible and whole saves no file; (8) no new eye-off glyph (the font is a subset and adding one needs lucide-static): a hidden layer's eye is flat and dimmed; (9) a layer's visibility, opacity, name and the active layer are not undo steps (core rule 4 of the first row) and the panel says nothing of it; (10) the saved name is `<picture stem>-paint-<10 hex>.ora`, `paint-` for a blank canvas; (11) the color picker is `ColorPickerPopup` as is, its OK sets the brush color, and the recent colors are the app's. **Not verified**: the real window, a pen tablet's pressure on a color stroke, a 4K canvas's speed (a stroke re-uploads only its cells; an undo of a layer step and the eye re-upload every visible cell) | 14.4, 12.2 | medium |
| The first pack lives in `examples/packs/`, not a repository of its own (#178; the owner's question): it is the only place packs live today, it is tested and documented with Fugue, and the folder is self-contained (its own `pyproject.toml`, `uv.lock`, tests and catalog), so `git subtree split` makes a repository of it with no change. A pack that is its own repository is the way for the other two (heavier dependencies, their own release pace). To confirm | 14.4, 6.6 | medium |
| The weights are not bundled and not fetched by the node. The pack's catalog (`catalog/birefnet.json`, read by `ModelCatalog::load` from every pack folder, #178) lists two author-released ONNX files from the release `v1`, with the sha256 the checks need (computed here from the downloaded files, the release publishes none): **general Swin-T** (224 MB, default, about 10 s for 1 megapixel on a 12-thread CPU) and **general Swin-L** (973 MB, about 16 s). The same download line as every model (8.9) shows the licence, the size and the host and asks first. Other BiRefNet variants (portrait, matting, HR, lite 2K, fp16) work if their `.onnx` is put in the `birefnet` folder, since the node reads the input size from the model; they are not listed because each would need to be checked | 8.9, 6.7 | high |
| A pack brings its own catalog: `<pack folder>/catalog/*.json` is read after Fugue's own and before the person's (#178). Without it the model picker of a pack's resource type could offer files on disk only, and the licence question (8.9) would not exist for packs. Data only; reading it fetches nothing | 8.9, 6.4 | high |
| Segment by Points or Box (#178, #167, 2026-11-22): SAM 2.1 through ONNX Runtime in the pack `io.github.bmaltais.fugue-segment`. **Licences, checked 2026-11-22**: SAM 2's README says "The SAM 2 model checkpoints, SAM 2 demo code (front-end and back-end), and SAM 2 training code are licensed under Apache 2.0" (the Inter font and Noto Color Emoji of the demo are OFL-1.1, not used); the model cards `facebook/sam2.1-hiera-*` carry `license: apache-2.0`; the ONNX files are Viet-Anh Nguyen's export (`vietanhdev/segment-anything-2.1-onnx-models`, one zip per size, dated 2026-02-21), whose card says Apache-2.0, "license unchanged from upstream"; the training data's terms (SA-V, SA-1B) were not read, so the catalog says "Apache-2.0" and links the licence. The files are the third party's export of Meta's checkpoints: a run on a real picture gives an intersection over union of 0.997 for a square found from one point and from a box (tiny and base+, `tests/test_segment.py`, opt-in), which shows the weights work, not that they equal the checkpoints. Other exports found and not used: `onnx-community/sam2.1-hiera-*-ONNX` (the card is missing, the files have external-data sidecars), `Suhas-G`, `akiyamanx` and `feeldx-dxliving` (a few uploads, no downloads); `vietanhdev/segment-anything-2-onnx-models` is SAM 2, not 2.1 | 14.4, 8.9 | high (code and checkpoints), medium (the export) |
| Segment by Points or Box: a new pack, not a second node of Remove Background (#178). The dependencies are the same (ONNX Runtime, NumPy, Pillow), which argues for one pack, and one environment saves a second 200 MB of ONNX Runtime. Against: the pack is the unit a person installs, trusts and reads the licences of (Remove Background is "BiRefNet, MIT", this one "SAM 2.1, Apache-2.0, a third party's export"), its id and name say what it is for, renaming a published pack id breaks installs, a pack that grows nodes needs a version for each, and 6.8 asks for an environment per pack so a fault in one cannot touch another. So two packs, each with its own `uv.lock`, and the 100 lines they share are one file, `onnx_node.py` (the command protocol, execution providers, the picture as a tensor), copied into each and kept identical by hand; the first pack now uses it too. A pack cannot import from another (each is installed on its own), so the extraction is a small library each depends on, which is worth it at the third ONNX pack, not before. To confirm | 14.4, 6.8 | medium-high |
| The SAM 2.1 model is a bundle: a `.zip` with `*.encoder.onnx` and `*.decoder.onnx`, as one resource file, not two resources or two inputs. A model is one file in the catalog (8.9, one sha256, one hash for the handle), and the encoder and decoder are useless apart; the exporter publishes them zipped. The download machinery checks the whole zip; the node reads the two members in memory (`zipfile`, bytes to ONNX Runtime, no unpacking, member sizes capped, names with `..` ignored), so nothing is extracted next to the model and a user's own bundle works if it names its parts the same. Two catalog entries per model (encoder, decoder) would have needed the picker to pick one file and find its twin | 8.9, 6.4 | medium-high |
| MobileSAM is not offered (#178; to confirm). The issue names it for CPU. Measured on this machine (12 threads): MobileSAM's encoder 0.38 s against SAM 2.1 tiny's 1.05 s for the same 1024 px input, and 1 GB for either. SAM 2.1 tiny already makes the first run of a picture a second long, and every later run (new points) 0.05 s, because the embedding is kept. Its costs: a second adapter (SAM 1's inputs are not SAM 2's: raw 0 to 255 pixels with the longest side to 1024 and padding, `orig_im_size`, a padding point labelled -1, one output map), a download of two files (Acly's `mobile_sam_image_encoder.onnx` and `sam_mask_decoder_single.onnx` are separate, no zip), and a licence that has to be explained: the ONNX card says MIT, MobileSAM's code and weights are Apache-2.0 (ChaoningZhang/MobileSAM), so the ONNX card is the exporter's, not the weights'. Two weights at most were asked: tiny and base+, the same code. MobileSAM is a third adapter in `segment.py` if the owner wants it for slow CPUs | 14.4 | medium |
| Segment by Points or Box as built (#178): the prompts are text inputs in the picture's pixels. The simplest existing seam: a `multiline` text for points (one `x y` per line, any of space, comma, semicolon; `#` lines are notes) and a `line` for the box, which any node can also wire, and no new widget; a picker on the node face (click to add a point, drag a box) is a later slice that needs a widget kind in 6.2 whose value is such a text, so this node would not change. A point outside the picture is `prompt_outside` with the size, not clamped (a wrong coordinate system is the likely mistake); something to segment is a Box or one Include point (`prompt_missing`); a box with no area is `prompt_invalid`. Box corners go first with the labels 2 and 3, then the points (1 include, 0 exclude), as SAM 2's predictor does. The decoder gives three maps and their predicted IoUs; the node takes the best-scored one (SAM 2's own multimask choice for a single point; with more prompts it is the usual pick too). Its logits are scaled up bilinear to the picture and cut at 0 after, so the edge is as fine as the picture, not 256 px blocks; Soft edge gives the sigmoid instead. A transparent picture is flattened on white as Remove Background does. A persistent node keeps the sessions and the embedding of the last picture (keyed by the picture file's sha256 and the model), measured on a 1200 x 900 picture, 12 threads: tiny 2.6 s then 0.05 s for each change of prompts, base+ 4.9 s then 0.05 s; peak 1.1 GB and 2.0 GB, so `needs.memory_mb` is 4000. Not tried on a GPU (none used: never the gx10), not clicked in the real window | 14.4, 6.2, 6.7 | medium-high |
| The environment is made by `uv sync --no-dev --extra cpu` and `--extra cuda` swaps the wheel (#178): the CPU and GPU builds of ONNX Runtime share one module name, so they are two mutually exclusive extras (uv `conflicts`), and the default setup installs the CPU one, about 27 MB, so the trust prompt stays honest and the install works everywhere. `device: auto` takes CUDA when that provider is there, else the CPU; `cuda` with no provider is the error `gpu_unavailable` that says what to run. Not declared: `needs.gpu` (a CPU install uses none, so declaring it would make the scheduler hold the node for a GPU it does not use); the cost is that a CUDA install's VRAM is not scheduled against the helper. Persistent with `idle_timeout_s` 60 so it frees memory soon. `needs.memory_mb` 8000 is measured (6.7 GB peak for Swin-T, 7.7 GB for Swin-L at 1024 px, ONNX Runtime's arena and pattern off: with them on, a second run grew Swin-T to 12 GB). To confirm | 6.8, 6.7, 8.10 | medium |
| The mask is BiRefNet's output as it is: sigmoid of the logits, resized bilinear to the picture's size, white the subject, soft at the edge; `threshold` above 0 makes it hard and `invert` gives the background. The picture's alpha is composited on white first (what the model sees is what it was trained on). No refinement, guided filter or matting: a Grow Mask or Feather Mask node after it does that, as with every other mask | 14.4 | medium-high |
| A node test cannot give a resource, so `fugue node test` of the pack names a model by the environment variable `FUGUE_BIREFNET_MODEL`, which the node also takes when its `model` input is empty; the tests use a 176-byte stand-in ONNX with the same interface (`tests/fixtures`, made by `tests/make_fixture.py`) because the real model is 224 MB to 1 GB. Fugue's ctest does not run the pack's Python tests (they need the pack's environment and the network to make it); `uv run --extra cpu pytest` in the pack folder does | 6.11, 14.4 | medium-high |
| Find by Text (#178, 2026-11-22): GroundingDINO tiny, then SAM 2.1 (the Segment node) as the mask maker. Chosen because it is what ComfyUI's segment-anything custom node uses (GroundingDINO Swin-T or Swin-B `.pth` in `models/grounding-dino`, then SAM, the instance masks joined: the node's README was not re-read here, this is from the `/mnt/windows/models` layout and memory), it has an Apache-2.0 ONNX export with a complete card, and it worked on the first real pictures. Not tried here: CLIPSeg (a 352 px heat map, so coarse edges; the Xenova ONNX card names no licence), OWL-ViT and OWLv2 (Apache-2.0 boxes; a model with a similar job, not measured against this one). Matched: text to boxes to SAM masks, a threshold of 0.3 (Hugging Face's default for this model), every instance joined in one mask, the `grounding-dino` models folder. Improved: no PyTorch (the pack stays ONNX Runtime, CPU or GPU), a preview with the scores to tune the threshold by eye, and the boxes are a text output that a script, a crop or the Segment node can all take. **Licences, checked 2026-11-22**: GroundingDINO code Apache-2.0 (github.com/IDEA-Research/GroundingDINO, LICENSE); `IDEA-Research/grounding-dino-tiny` and `onnx-community/grounding-dino-tiny-ONNX` cards say `license: apache-2.0`; `bert-base-uncased` (the word list `vocab.txt`, byte-identical to the Hugging Face file) says `apache-2.0`. The training data's terms (O365, GoldG, Cap4M) were not read, so the catalog says "Apache-2.0" and links the licence. The local `.pth` files in `/mnt/windows/models/grounding-dino` (the original checkpoints) are not read: a `.pth` is a pickle, and the ONNX file is the one that runs without PyTorch | 14.4, 8.9 | high (code), medium-high (weights) |
| Find by Text: the model is one `.onnx` file, the tokenizer is in the pack (#178). The catalog downloads one file per entry, so a model must be one file; the 231 KB BERT word list (`find-by-text/vocab.txt`) is shipped with a 40-line wordpiece, checked against the model's own `tokenizer.json` by a test, not a dependency on `tokenizers` or `transformers`. Two catalog entries: the half-precision file (360 MB, default: the same boxes as full precision to a pixel, 1 s on an RTX 3090, 4 s on a 12-thread CPU) and the full-precision file (719 MB, needs about 4 GB of GPU memory). The 8-bit file (204 MB) is not offered: on a clown picture its box is 35 px lower than full precision's, which is outside any tolerance for a box that feeds a mask | 8.9, 6.4 | high |
| Find by Text runs the model once per phrase (#178). With "glove . red balloon ." in one prompt the second phrase scores 0.14 to 0.2 where it scores 0.87 alone, and in the other order the glove is lost, so the ONNX export does not keep the phrases apart as the PyTorch model does (the sub-sentence attention mask). One run per phrase is correct and costs one model run each (a second on a GPU); the matches are merged best first and a box found for two phrases is kept once, under the better. The picture is scaled to fit 800 x 800 and padded with a mask of the real part, not squashed: boxes are fractions of the real part (checked: the same boxes either way to a few pixels, the padded form is what the model trained on) | 14.4 | high |
| Segment takes several boxes (#178): one object per line, each decoded alone (the decoder costs 0.05 s) and the logits joined by their maximum, which is the union of the masks. Points with several boxes are refused (they would apply to which object?), so the rule is one box with its points, or any number of boxes with none. A mask of everything the words name is the useful default, and the person can still narrow by `max_results` | 14.4, 6.4 | high |
| Pack secrets (#402, 2026-11-25; decided without the owner, each to confirm). (1) A pack's `settings` are secrets only (`secret: true` required), in provider modules' shape, names lowercase; a bad entry disables the pack. (2) The store is its own module and file, `pack-secrets.json` in the config folder of the root (so per project like providers.json), keyed by pack id, 0600, atomic, and `pack remove` of the last version forgets the values. (3) Hand-over is a `secrets` object in the request on stdin, not the environment: it works for a persistent program (fresh values per request), is not inherited by what the program starts, and is not in /proc/<pid>/environ. (4) The runner hands a node only the names its own pack declares, and masks values of 4 or more characters (and their percent-encoded form) in errors, hints, the stderr tail, progress and text outputs. (5) CLI: `fugue pack secret list|set|clear`, value on stdin, never printed. (6) HTTP Request declares `token_1` to `token_3`, accepts `{secret:NAME}` in headers and the URL and refuses it in the body, so a server's echo of the body cannot carry it; the node masks too. (7) The trust and Get ready questions list secret names, and a yes covers those names. (8) Review 2026-11-25: a key is sent only to the origin the person set with it (required for the HTTP pack; refused by the CLI and the page without it, not handed over when stored without one), and the node refuses placeholders in the address; values are keyed by pack id and source so a look-alike pack gets none; isolation between packs is advisory and the prompt says so. | 6.4.1, 6.7, 7.2, 6.13 | medium-high |
| Privacy consent (#404, 2026-11-25; decided without the owner, each to confirm; reviewed and changed the same day: placement by the address really used, a fugue-helper judged by address, the question opens on No). (1) Three placements: this machine, an own host (paired omarchy and fugue-helper hosts, and any address `isLocalHost` accepts: private, link-local, CGNAT, `.local`, `.internal`) and off-machine (everything else); only off-machine asks. A LAN or tailnet box counts as the person's own because they typed its address in and the `http` provider already trusts the same zone; a stricter rule would ask for every home server. (2) The question comes from the check (stale nodes routed to the instance), before anything is queued, with the run's id kept, so no new id reaches the run bar, the App view or the batch; a node the check could not route (an input it does not know yet) is caught by the scheduler's refusal at the node, which fails it with `consent_needed` before the job is submitted. (3) One question per run with a row per service, one yes for all; the yes is per instance and kept in `remote-consent.json` next to `trust.json` (the root's state folder, 0600) with the instance's type and address, so a changed address asks again; removal prunes it. (4) CLI: `--allow-remote <instance>` is for that run only and remembers nothing (a script's flag is not a standing decision); a yes given in the window is honoured by the CLI of the same root. (5) A network pack's trust already names `network`, so it is not asked again. (6) What leaves is read from the node type's inputs that are written: image (masks when `role: mask`), audio, video, and text that is not a choice, for the nodes routed there; numbers and choices are settings, not listed. Cost and which instance is chosen are #403 and #405, not this | 8.2, 12.17 | medium-high |
| Canvas layers (#226, 2026-11-22; decided without the owner, each to confirm). (1) A layer mask belongs to its layer (a one-channel `MaskPaint` in the layer, `LayerInfo::hasMask`, `maskEnabled`), not a Mask-kind layer linked by id: it then moves, is duplicated and is deleted with its layer with no ordering rule to keep, the tree stores it inside the layer record, and the mask brush can still paint it (the same buffer type). Mask-kind layers stay the paint window's selection-like masks. Medium-high. (2) `.ora`: a masked layer's `src` is baked (alpha times mask) so Krita, GIMP and MyPaint show the right picture, and Fugue's raw pixels and mask ride as `fugue:raw` and `fugue:mask` entries; rejected: a sibling layer with `svg:dst-in` in an isolated group (Fugue reads no groups, and GIMP's handling of dst-in is unknown), a hidden sibling mask layer (other programs would show the unblended crop). Medium. (3) The blend mode is stored as the `.ora` composite-op string itself (empty for src-over), so an op Fugue does not draw round-trips unchanged; Add is `svg:plus` drawn as Addition [U]. Medium-high. (4) New tree keys (`blend`, `mask`, `origin`) are written only when not default, so every existing tree keeps its hash. High. (5) A layer's origin (producer, time) is in the tree, so it follows renames, moves and duplicates; the cost: two Writes of the same pixels have different tree hashes (cache keys use the revision, so nothing depends on that). Medium. (6) Layer edits are tree edits only (no pixel read or written), only on the head, one revision per edit even for a selection of layers; `fugue canvas layer set` with several properties makes one revision each. Medium-high. (7) Undo is the head moved to its parent, and the revision it leaves is marked `undone` so it never shows as a candidate (the new-result banner would otherwise pop it); redo is the newest undone child of the head, found in the store, so the command line and the window share it. Any head can be undone. Medium-high. (8) Canvas Write's layer is the whole redrawn crop with the blend as its layer mask by default (Photoshop's Generative Fill), `bake` keeps the old placement; the name comes from the first `prompt` upstream in the workflow as run, else `Fill n`, and `Fill` (the old default) counts as "no name given". Medium. (9) Canvas Region's `layer` reads a layer's own pixels (no opacity, blend or mask); empty is the composite. Medium. (10) The panel is a new outliner-style component, not PaintLayersPanel (that one is bound to PaintLayer and has the eye at the left); it reuses ViewButton, ThemedTextField, WidgetMenu and the look. Opacity is a new `DragNumberField` (InputWidget's scrub is bound to WidgetEdit). Medium-high. (11) Keys: a new `canvas-layers` context, the most specific, on only while the pointer is over the panel or its list has the focus, so the region's document undo on the picture is not taken; Ctrl+Z there is the canvas's. Solo has no key (Alt+H would clash with the Help menu's mnemonic). Medium. (12) Taken from Blender: outliner rows with toggles at the right, Ctrl/Shift multi-select, F2 and double-click rename, hover-owned keys (H, X/Delete, Ctrl+D), drag-number fields (drag, click to type, double-click to reset, Ctrl to snap), Alt+click solo as a view state. Left out for now: the render toggle, groups (the model has `depth`; lock and groups came later, see 13), Alt+H unhide-all, the number field's arrow buttons, box select in the list, Blender's Ctrl+click on the eye to isolate (Alt+click is Photoshop's and is what was asked). Photoshop kept: the mask thumbnail beside the layer's, Shift+click to disable it, a current layer always. (13, 2026-11-23, each to confirm) Lock is Photoshop's "lock all" as a layer property (`LayerRecord::locked`, stored as `"locked": true` only when set, so no existing tree changes its hash; an `.ora` carries it as `fugue:locked`; a `.psd` has no place for it here and drops it): a locked layer refuses what changes its pixels or takes it out of the stack (delete, merge as either side, flatten, move or transform, a mask paint, replace or delete, ungrouping a locked group) and nothing else, in the store (`applyLayerEdit`, `applyPixelEdit`, `transformedTree`) so the command line and the window share it; the panel disables the controls instead of waiting for the refusal; a duplicate of a locked layer is locked; rejected: locking opacity, blend and visibility too (Photoshop's separate "lock position/pixels/transparency" is three more states, and showing or fading a layer is not a destructive edit). Medium. (14) "Generate reads this layer only" is a view state of the Canvas view (`CanvasViewModel::regionLayer`, as Show alone), sent as the Layer input of the action's Canvas Region by name (`ActionBinding::layer`); an action that fixes its own Layer keeps it when none is chosen; a name two layers share reads the top one, so only that one can be chosen. Medium-high. | 14.4, 14.5, 12.4, 13.1 | medium |
| HTTP Request (#406) is a command node in a first-party pack that declares `network` (not a built-in, which the trusted `fugue` pack would run with no prompt): Python 3 standard library, no setup. Inputs `a` to `d` fill `{a}` to `{d}` (fixed names, because a manifest has a fixed port list); a JSON body is parsed first and placeholders fill its strings, so a value cannot break the JSON; a status of 400 or more is an output, `fail_on_error_status` makes it an error; redirects are not followed; the JSON path is dotted keys, `[n]`, `[-n]` and `["key"]` only; `{secret:NAME}` is the header and URL-path syntax of #402 ([6.13](#613-http-request-first-party-pack)), filled only for the origin the person set the key for; the node is a sink so a webhook with nothing downstream still runs; errors keep the node's own codes (the engine reports them as `node_failed`), with the hint in the message | 6.13, 6.3, 7.2 | medium-high |

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
| #194 Describe Image (image to prompt, caption, tags) | 14.2, 8.1 |
| #140 Enhance Prompt styles as prompt templates (video, MiniMax H3, music) | 14.2, 8.9, Appendix A |
| #28 Load Video, Load Audio, Generate Video, Generate Song | 14.2, 8.1, 8.7 |
| #88 Klein 9B edit: references, Resize to Megapixels | 14.1, 14.3 |
| #83 workflow templates, missing models on open | 11.10, 4.3, 13.1 |
| #89 model downloads | 8.9, 4.3, 13.1, 13.3 |
| #91 export for sharing | 11.5, 4.3, 13.1 |
| #90 node versions side by side | 3.6, 6.5, 6.6, 10.1 |
| #29 more pixel and mask nodes | 14.2, 2.1 |
| #167 masking and inpainting: mask editor, mask nodes, staged inpaint, crop and stitch, templates | 14.4, 2.1, 6.1, 6.2, 8.1, 8.10, 13.3, 17, Appendix A |
| #208 canvas 1: revision store, `fugue/canvas` handle, `fugue canvas` (epic #206) | 14.5, 1.4, 13.1, 13.3, Appendix A |
| #209 canvas 2: Canvas Region and Canvas Write, head named in `compileForRun` (epic #206) | 14.5, 9.1, 10.1, 13.3, Appendix A |
| #210 canvas 3: candidates, accept and drop, diff, `generative-fill` template (epic #206) | 14.5, 13.1, 13.3, Appendix A |
| #30 agent skill | 13.4, 13.5 |
| #31 providers as modules | 8.8 |
| #32 component-level helper | 8.10, 2.6 |
| #33 component nodes | 14.3, 2.5, 2.6, 8.9 |
| #34 CI recipe | 17 |
| #105 video stages in the helper and nodes | 8.10, 8.1, 2.6, 14.3 |
| #133 media viewer: the media browser and the workflows panel's Results view | 12.14, 9.7, 11.7, Appendix A |
| #159 side tab strip | 12.14, 12.15, Appendix A |
| #160 auto-hide menu bar | 12.15, Appendix A |
| #220 workspaces 0: the area tree, the view registry, the window's one Graph area (epic #217) | 12.16, Appendix A |
| epic #217 slice 1: split, resize, area headers with the view-type menu, the Canvas view type, workspaces.json | 1.4, 12.4, 12.16, 14.5, Appendix A |
| #141 cache management | 10.5, 10.6, 13.1, 12.7, Appendix A |
| #134 upscaling: Upscale Image and Video, Upscale with Model, the model catalog and picker downloads | 8.1, 8.9, 14.1, Appendix A |
| #138 Upscale Video with Model: frames streamed through the helper's upscaler | 8.1, 9.2, 14.1, Appendix A |
| #164 template and model picker icons for models on this machine and on paired hosts, Ready to run | 11.10, 8.7, 2.5, Appendix A |
| #384 app block: binding, check, prune | 4.9, 4.10, 11.10 |
| #385 App view: a workflow as one simple screen | 12.17, 12.5 |
| #386 App view: one control per input type | 12.17 |
| #387 apps in the catalog: the Apps chip, the `app` flag | 11.10, 12.17 |
| #388 apps shelf | 12.17 |
| #389 takes from History, Run again | 12.17 |
| #471 Run again: Off, On change, Repeat | 12.17 |
| #392 `fugue app list`, `info`, `run` | 13.3, 4.9 |
| #393 Make an app: the builder | 12.19, 12.5 |
| #394 `.fugueapp` bundles, `fugue app export`, Keep this app | 11.5, 12.18, 13.3 |
| #398 an app as its own application (`fugue --app`) | 12.18, 13.3 |
| #450 App view as a view type of an area | 12.16, 12.17 |
| #484 optional pictures (a branch bypassed when left empty) | 4.9, 12.17 |
| #396 Apps docs and tests | 4.10, [docs/apps.md](apps.md), GLOSSARY |
| #406 HTTP Request node in a first-party network pack | 6.13, Appendix A |
| #402 secrets for pack nodes | 6.4, 6.4.1, 6.7, 7.2, 6.13 |
