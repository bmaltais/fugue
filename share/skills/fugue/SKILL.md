---
name: fugue
description: >
  Build, validate, run and inspect Fugue workflows (node graphs for local AI image, video,
  audio and text generation) and write Fugue nodes, all through the `fugue` command line with
  no window. Use when the user asks for a Fugue workflow or `.fugue` file, to run or debug one,
  to see what nodes Fugue has, to fetch a run's results, or to make a Fugue node or pack.
  Triggers: fugue, .fugue, workflow graph, node pack, fugue run, fugue validate.
---

# Fugue

Fugue is a node-based app for local AI. A workflow is one JSON file (`.fugue`); the `fugue`
command reads, checks and runs it with no display. Add `--json` to any command: stdout is then
exactly one object, `{"ok": true, "data": ...}` or `{"ok": false, "errors": [...]}`. Exit code 0 is
success, 1 any failure. Read an error's `message`, `valid` (the full list of options) and `hint`
before guessing a fix.

## The loop

1. Discover node types: `fugue node list` (`--type <port type>` filters), then
   `fugue node describe <type id>` for the exact manifest: inputs with type, default, range,
   choices, outputs, and whether it is a sink. Never guess an input name or a type; describe it.
2. Write the `.fugue` file (format below), or start from `fugue workflow new <file>` or
   `fugue template list` and `fugue workflow new <file> --template <id>`.
3. `fugue validate <file> --json`. Nothing runs. Fix every error it names, then re-validate.
   `fugue workflow describe <file>` summarizes nodes, links and sinks.
4. `fugue run <file> --wait` streams events (JSON lines) and ends with `run.done`; without `--wait`
   it prints a run id and returns. `--set <node>.<input>=<value>` overrides one value for that run.
   `--to <node>` runs only up to a node.
5. `fugue runs result <run-id> --json` gives each sink's values and output file paths. Use
   `fugue runs status <run-id>` for per-node state and `fugue runs events <run-id>` to replay events;
   `fugue runs list` shows recent runs; `fugue runs cancel <run-id>` stops one.

A run executes the sinks (Save, Preview) and what they need; a node no sink depends on is skipped
(`not_part_of_output`).

## The workflow file

```json fugue-example
{
  "fugue": 1,
  "graph": {
    "nodes": {
      "subject": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": "a lighthouse"}},
      "prompt": {"type": "fugue/text-concat", "version": "1.0.0",
                 "inputs": {"a": {"from": "subject.text"}, "b": "oil painting", "separator": ", "}},
      "show": {"type": "fugue/preview", "version": "1.0.0", "inputs": {"value": {"from": "prompt.text"}}}
    }
  }
}
```

- `graph.nodes` is an object keyed by node id: lowercase letters, digits, `-` and `_`, starting with
  a letter, at most 32 characters. Choose readable ids. Each node has `type`, `version` (copy it
  from `node describe`) and optional `inputs`; an input left out takes its default.
- An input value is a literal (string, number, boolean), a link `{"from": "<node id>.<output name>"}`,
  `{"auto": "random"}` for a seed input, or an array for a list input. `null` is not a value:
  leave the input out. An object is a link only if it has the key `from`.
- Links must match port types exactly; there are no implicit conversions. A single value feeding a
  list input is wrapped; a list cannot feed a single input.
- Leave `layout` out: the canvas places nodes that have no position. Write the file yourself;
  an open Fugue window picks up changes to it.
- Optional per node: `mode` (`"bypass"` or `"mute"`), `provider` (pin a provider instance).
- A model input left unset means any ready model of that type. Run `fugue model list` to see what
  is on disk, and `fugue model get <file|template id>` to download the models a workflow names.

## What needs a person

An agent cannot do these. They fail with `needs_confirmation` when there is no terminal, and no flag
answers for the human: `fugue pack add|update|remove|trust`, running a pack's setup, adding a
provider or key, starting or stopping a model server. A run that reaches an untrusted pack fails
with `pack_untrusted`. If a workflow needs a pack or a model server, say what to install or start
and ask the user to do it; do not work around it. Fugue reads anywhere under the home folder and
writes only under the current directory, the output folder and `~/Documents/Fugue`; other paths
fail with `permission_denied`.

## Commands

| Command | Does |
| --- | --- |
| `fugue run <file>` | Start a run (`--wait`, `--first`, `--to`, `--provider`, `--set`) |
| `fugue validate <file>` | Compile and report errors and warnings; runs nothing |
| `fugue compile <file>` | Print the compiled graph |
| `fugue workflow new <file>` | Write an empty workflow, or a template's (`--template <id>`) |
| `fugue workflow describe <file>` | Nodes, links, sinks, what is missing |
| `fugue workflow export <file>` | Package to share (`--share`, `--out`, `--no-prompts`, `--no-layout`) |
| `fugue runs list` | Recent and queued runs |
| `fugue runs status <run-id>` | State and per-node progress |
| `fugue runs events <run-id>` | The event stream (`--follow` to keep reading) |
| `fugue runs result <run-id>` | Sink values and file paths |
| `fugue runs cancel <run-id>` | Cancel a run |
| `fugue node list` | Installed node types (`--type`) |
| `fugue node describe <type id>` | Full manifests; `<id>@<major>` picks a major |
| `fugue node new <name>` | Scaffold a pack with a node (`--id`) |
| `fugue node test <path>` | Run a node's tests |
| `fugue pack list` | Installed packs |
| `fugue pack add <folder or git-url>` | Install a pack (a person confirms; `--ref`) |
| `fugue pack update [id]` | Update packs (a person confirms; `--ref`) |
| `fugue pack remove <id>` | Remove a pack (a person confirms) |
| `fugue pack trust <id>` | Trust a pack (a person confirms) |
| `fugue model list` | Models on disk and offered by providers (`--type`, `--hash`) |
| `fugue model get <file or template id>` | Download the missing models a workflow or template names (`--into`) |
| `fugue template list` | Templates, with the models and packs each needs |
| `fugue cache status` | What the caches hold (results, thumbnails, runs): bytes, files, oldest, and the results per workflow and node |
| `fugue cache clear` | Clear a cache, results unless `--kind results\|thumbnails\|runs\|all`; `--workflow`, `--node`, `--older-than 30d`, `--dry-run` first. Saved outputs, frozen results and a run going or queued are never removed |
| `fugue skill install` | Install or update this skill (`--target`, `--dry-run`, `--force`) |
| `fugue skill uninstall` | Remove it (`--target`, `--dry-run`, `--force`) |

Plurals work for `node`, `pack`, `provider`, `model` and `template` (`fugue nodes list`).

## Errors

Every failure has a stable `code`. Fix by code, not by message text:

- Files: `file_not_found`, `invalid_json`, `invalid_document`, `unsupported_format_version`,
  `permission_denied`, `media_unreadable`, `download_failed`, `download_mismatch`
- Graph: `unknown_node_type`, `node_version_mismatch`, `node_newer_major`, `unknown_input`,
  `missing_input`, `unknown_link_source`, `unknown_output`, `type_mismatch`, `value_out_of_range`,
  `value_not_in_choices`, `cycle`, `group_recursion`, `unknown_provider`
- Warnings (the command still succeeds, in a top-level `warnings` array): `not_part_of_output`,
  `model_missing`, `model_mismatch`, `model_unsupported`, `frozen_result_missing`,
  `pack_update_available`, `workflow_not_embedded`, `node_bad_preview`. A model warning becomes an
  error when a run reaches that node.
- Trust: `pack_untrusted`, `needs_confirmation`
- Packs: `pack_invalid`, `pack_exists`, `unknown_pack`, `fetch_failed`, `setup_failed`
- Groups: `group_invalid`
- Providers: `capability_unavailable`, `model_not_running`, `provider_unreachable`
- Running: `node_failed`, `node_bad_output`, `node_timeout`, `out_of_memory`, `cancelled`,
  `node_test_failed`
- Masks: `mask_empty` (paint the part to change), `mask_unsupported` (the provider or model cannot take a mask: Generate Image's `mask` needs a model whose provider lists `mask` in its `inputs`; the error names the ones that can), `stitch_mismatch` (Stitch got a picture of another shape than its crop); warnings `mask_full` (everything is masked: plain generation), `mask_resized` (the mask was stretched to the picture)
- Cache: `cache_busy` (a warning: a run is going, so `cache clear` left its results and previews alone)
- Skill: `skill_modified`
- Other: `unknown_run`, `usage`, `internal`

A run that ends `failed` or `cancelled` under `--wait` exits 1; `runs status` names the node and
the error. `out_of_memory` means a smaller size or a lighter model, not a retry.

## Writing a node

A node is any program that reads JSON on stdin and writes JSON on stdout, declared by a
`node.json` manifest in a pack folder.

1. `fugue node new <name>` makes a pack with a working node and a test to copy (`--id` names the
   pack, `io.example.<name>` by default; run it inside a pack folder to add a node to that pack).
2. Edit the node's `node.json` (inputs, outputs, types: copy the shape of `fugue node describe`
   for a built-in) and its program.
3. `fugue node test <path>` runs its tests; `fugue validate` on a workflow that uses it checks the
   wiring. The pack must be installed, which a person confirms (`fugue pack add`).

The manifest and protocol references are in the Fugue docs: `docs/nodes/` in the repository, from
the quickstart to publishing.
