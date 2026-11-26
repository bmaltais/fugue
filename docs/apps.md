# Apps

An app is a workflow shown as one simple screen: the few inputs worth changing, a Run button and the results. Anything in
Fugue that you can build as a graph can become one, and the graph is still there behind it. The design is in
[spec 4.10](spec.md#410-apps-at-a-glance-396-epic-397).

## Use one that comes with Fugue

Open **File > All apps…** (Ctrl+Shift+A) and pick a card. Fill the form (a picture, a prompt, a few options under
**Advanced**) and press **Run** (Ctrl+Enter). Each run is a **take** in the strip under the result; click an earlier one to see it
again. **Run again** under the form can repeat the run when you change an input (On change) or keep going (Repeat).

If the app needs a model or a pack you do not have, a **Needs** block at the top of the form says so and offers to fetch it.

## Make an app from a workflow

1. Open the workflow and press **Ctrl+Alt+M** (File > Make app…).
2. Give it a title and, optionally, a description.
3. Add the inputs the form should show: choose the node inputs (a Load Image's picture, a prompt, a seed, a number), give each a
   label, and mark the rarely used ones **Advanced**. An input can say "optional": a picture left empty skips its branch.
4. Choose the output: the Save or Preview node whose result is the app's result.
5. **Apply**. The workflow now shows as the app. Ctrl+Z takes the block out again; File > Edit app… changes it later.

## Open it in the graph

**Open in Graph** (the Graph item in the rail, or Ctrl+Alt+A) shows the same workflow as the ordinary graph; **App | Graph** at the
end of the window bar switches back. Fix a node in the graph and the app uses the fix: the form only lists inputs, it does not copy
them. The values in the form belong to the run, so the saved workflow is never changed by running the app.

## Share it

**File > Export for sharing…** on a workflow with an app writes one `.fugueapp`. It carries the packs and the sample pictures; models are
kept as references the other person downloads. Opening a `.fugueapp` asks once on the Get ready sheet, and the app opens in a
window of its own, ready to run; **Keep this app** adds it to your apps.

## Run it from the command line

```sh
fugue app list                       # the apps on this machine
fugue app info restyle               # what it takes and gives
fugue app run restyle --input image=photo.png --input prompt="a red fox" --out results/
fugue app export restyle restyle.fugueapp
fugue --app restyle                  # its own window
```

When an app's models are not on this machine and a paired helper is set up, `app info` says "Runs on gx10" if it answers, or "gx10 is
not answering" if not (the window says the same above Run); a node no helper can take is named with why, before anything runs.

`app info` and the form describe an input the same way (`src/engine/app_input.h`): the kind (picture, mask, model, seed, choice, toggle,
number or text; `--json` says `image`, `mask`, `model`, `number` for a seed, `choice`, `boolean`, `number`, and the port's type or `any`
for text), the default (the block's, else what the workflow holds, else the target's own), whether a run needs it, its range and its
choices. `--input` takes a picture or mask as a file, a model as a file name or a handle as JSON, a number, `true` or `false`, a
choice word or text; `@<file>` reads text from a file, and a seed also takes `auto` (or `random`), the engine's own.

`app run` prints the files the app's output made, one per line (`--json` for the data). An id is a template's name or the path to a
workflow file. See [spec 13](spec.md#13-cli-and-agent-interface) for every option.
